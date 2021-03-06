/*
 * ps2spi.c
 *
 *  This program is the interface code for AVR with a PS2 keyboard.
 *  It implements a PS2 keyboard interface and an SPI serial interface.
 *  The AVR connects directly into the Raspberry Pi's input interface and
 *  supplies make/break ASCII code for a Dragon 32 emulation running
 *  in the Raspberry Pi (https://github.com/eyalabraham/dragon).
 *
 *  The SPI interface is used for communicating keyboard ASCII codes
 *  and for in-circuit programming of the AVR device.
 *
 *  +-----+               +-----+            +-------+
 *  |     |               |     |            |       |
 *  |     +----[ MOSI>----+     |            |       |
 *  |     |               |     |            |       |
 *  |     +----< MISO]----+     +--< Data >--+ Level |
 *  | RPi |               | AVR |            | shift +---> PS2 keyboard
 *  |     +----[ SCL >----+     +--< CLK ]---+       |
 *  |     |               |     |            |       |
 *  |     +----[ RST >----+     |            |       |
 *  |     |               |     |            |       |
 *  +-----+               +-----+            +-------+
 *
 *
 * ATtiny85 AVR IO
 *
 * | Function  | AVR  | Pin | I/O               |
 * |-----------|------|-----|-------------------|
 * | Reset     | PB5  | 1   | RPi GPIO22        |
 * | PS2 clock | PB3  | 2   | in/out w/ pull up |
 * | PS2 data  | PB4  | 3   | in/out w/ pull up |
 * | DI        | PB0  | 5   | RPi MOSI          |
 * | DO        | PB1  | 6   | RPi MISO          |
 * | SCLK      | PB2  | 7   | RPi SCL           |
 *
 * Port B bit assignment
 *
 *  b7 b6 b5 b4 b3 b2 b1 b0
 *        |  |  |  |  |  |
 *        |  |  |  |  |  +--- 'i' DI
 *        |  |  |  |  +------ 'o' DO
 *        |  |  |  +--------- 'i' SCLK
 *        |  |  +------------ 'i' PS2 clock
 *        |  +--------------- 'i' PS2 data
 *        +------------------ 'i' ^Reset
 *
 * Note: all references to data sheet are for ATtine85 Rev. 2586Q–AVR–08/2013
 *
 * Data transfer schema uses a double buffer: input buffer from PS2 holding scan codes,
 * output buffer holding converted keyboard make/break codes for the Dragon 32 input
 * The main loop reads from the PS2 buffer, processes the scan codes, and stores the into
 * the output keyboard buffer.
 *
 * Dragon 32 keyboard: http://archive.worldofdragon.org/index.php?title=Keyboard
 *
 * TODO:
 * 1) keyboard error handling and recovery
 *
 */

#include    <stdint.h>
#include    <stdlib.h>

#include    <avr/io.h>
#include    <avr/interrupt.h>
#include    <avr/wdt.h>
#include    <util/delay.h>

// IO port B initialization
#define     PB_DDR_INIT     0b00000010  // Port data direction
#define     PB_PUP_INIT     0b00000000  // Port input pin pull-up
#define     PB_INIT         0b00000000  // Port initial values

// Pin change interrupt setting
#define     GIMSK_INIT      0x20        // Enable pin change sensing on PB
#define     PCMSK_INIT      0b00001000  // Enable pin change interrupt on PB3
#define     PCINT_PB3       PCMSK_INIT  // Pin change interrupt on PB3

// USI
#define     USICR_INIT      0b01011000  // 3-wire, external clock, positive edge, interrupts enabled
#define     USICR_USIOIE    0b01000000  // Counter Overflow Interrupt Enable

#define     USI_CNTR_OVRF   0b01000000  // Counter overflow
#define     USI_COUNTER     0b00001111  // USI mask counter bits

// PS2 control line masks
#define     PS2_CLOCK       0b00001000
#define     PS2_DATA        0b00010000

// Buffers
#define     PS2_BUFF_SIZE   32          // PS2 input buffer
#define     KEY_BUFF_SIZE   32          // Key code output buffer

// Host to Keyboard commands
#define     PS2_HK_LEDS     0xED        // Set Status Indicators, next byte LED bit mask
#define     PS2_HK_ECHO     0xEE        // Echo
#define     PS2_HK_INVALID  0xEF        // Invalid Command
#define     PS2_HK_ALTCODE  0xF0        // Select Alternate Scan Codes, next byte Scan code set
#define     PS2_HK_INVALID2 0xF1        // Invalid Command
#define     PS2_HK_TMDELAY  0xF3        // Set Typematic Rate/Delay, next byte Encoded rate/delay
#define     PS2_HK_ENABLE   0xF4        // Enable
#define     PS2_HK_DISABLE  0xF5        // Default Disable
#define     PS2_HK_DEFAULT  0xF6        // Set Default
#define     PS2_HK_SET1     0xF7        // Set All Keys - Typematic
#define     PS2_HK_SET2     0xF8        // Set All Keys - Make/Break
#define     PS2_HK_SET3     0xF8        // Set All Keys - Make
#define     PS2_HK_SET4     0xFA        // Set All Keys - Typematic/Make/Break
#define     PS2_HK_SET5     0xFB        // Set All Key Type - Typematic, next byte Scan code
#define     PS2_HK_SET6     0xFC        // Set All Key Type - Make/Break, next byte Scan code
#define     PS2_HK_SET7     0xFD        // Set All Key Type - Make, next byte Scan code
#define     PS2_HK_RESEND   0xFE        // Resend
#define     PS2_HK_RESET    0xFF        // Reset

#define     PS2_HK_SCRLOCK  1           // Scroll lock - mask 1 on/0 off
#define     PS2_HK_NUMLOCK  2           // Num lock   - mask 1 on/0 off
#define     PS2_HK_CAPSLOCK 4           // Caps lock  - mask 1 on/0 off

#define     PS2_HK_TYPEMAT  0b01111111  // 1Sec delay, 2Hz repetition

// Keyboard to Host commands
#define     PS2_KH_ERR23    0x00        // Key Detection Error/Overrun (Code Sets 2 and 3)
#define     PS2_KH_BATOK    0xAA        // BAT Completion Code
#define     PS2_KH_ERR      0xFC        // BAT Failure Code
#define     PS2_KH_ECHO     0xEE        // Echo
#define     PS2_KH_BREAK    0xF0        // Break (key-up)
#define     PS2_KH_ACK      0xFA        // Acknowledge (ACK)
#define     PS2_KH_RESEND   0xFE        // Resend
#define     PS2_KH_ERR1     0xFF        // Key Detection Error/Overrun (Code Set 1)

#define     PS2_SCAN_CAPS   0x3a        // Caps lock scan code
#define     PS2_SCAN_SCROLL 0x46        // Scroll lock scan code
#define     PS2_SCAN_NUM    0x45        // Num lock scan code
#define     PS2_LAST_CODE   0x50        // Last (largest scan code)

/****************************************************************************
  Types
****************************************************************************/
typedef enum
{
    PS2_IDLE,
    PS2_DATA_BITS,
    PS2_PARITY,
    PS2_STOP,
    PS2_RX_ERR_START,
    PS2_RX_ERR_OVERRUN,
    PS2_RX_ERR_PARITY,
    PS2_RX_ERR_STOP,
} ps2_state_t;

typedef enum
{
    I2C_IDLE,           // Idle
    I2C_START,          // Start condition detected
    I2C_ADDR_ACK,       // Address received ACK sent
    I2C_READ,           // Peripheral address received + read
    I2C_READ_ACK,       // Wait for ACK after read data sent
    I2C_WRITE,          // Peripheral address received + write
    I2C_WRITE_ACK,      // Wait for ACK after write data sent
} i2c_state_t;

/****************************************************************************
  Function prototypes
****************************************************************************/
void    reset(void) __attribute__((naked)) __attribute__((section(".init3")));
void    ioinit(void);

int     ps2_send(uint8_t);
int     ps2_recv_x(void);       // Blocking
int     ps2_recv(void);         // Non-blocking

void    kbd_test_led(void);
int     kdb_led_ctrl(uint8_t);
int     kbd_code_set(int);
int     kbd_typematic_set(uint8_t);

int     read_key(void);
int     write_key(uint8_t key_code);

/****************************************************************************
  Globals
****************************************************************************/
// Circular buffer holding PS2 scan codes
uint8_t      ps2_scan_codes[PS2_BUFF_SIZE];
int          ps2_buffer_out = 0;
volatile int ps2_buffer_in = 0;
volatile int ps2_scan_code_count = 0;

// Variable maintaining state of bit stream from PS2
volatile ps2_state_t ps2_rx_state = PS2_IDLE;
volatile uint8_t  ps2_rx_data_byte = 0;
volatile int      ps2_rx_bit_count = 0;
volatile int      ps2_rx_parity = 0;

// Key code output buffer
uint8_t      key_codes[KEY_BUFF_SIZE];
volatile int key_code_count = 0; //KEY_BUFF_SIZE;
volatile int key_buffer_out = 0;
volatile int key_buffer_in = 0;

volatile uint8_t command_in = 0;

// Variables maintaining state of I2C (USI in TWI mode)
volatile i2c_state_t    i2c_state = I2C_IDLE;

// Keyboard status
volatile uint8_t    kbd_lock_keys = 0;;

/* ----------------------------------------------------------------------------
 * main() control functions
 *
 */
int main(void)
{
    int     scan_code;
    uint8_t kdb_lock_state = 0;

    // Initialize IO devices
    ioinit();

    // Wait enough time for keyboard to complete self test
    _delay_ms(1000);

    // light LEDs in succession
    kbd_test_led();

    // set typematic delay and rate
    kbd_typematic_set(PS2_HK_TYPEMAT);

    // change code set to "1" so code set translation does not needs to take place on the AVR
    kbd_code_set(1);

    sei();

    /* Loop forever. receive key strokes from the keyboard and
     * accumulate them in a small FIFO buffer to be read by the emulation
     * code running on the Raspberry Pi.
     */
    while ( 1 )
    {
        scan_code = ps2_recv();

        /* Only pass make and break codes for keys in range 1 to 83
         * Handle 'E0' modifier for keypad by removing the 'E0' which
         * will effectively reduce any keyboard to one that is equivalent to an 83 key keyboard.
         * Discard PrtScrn E0,2A,E0,37 and E0,B7,E0,AA; no support print screen.
         * Discard E1 sequence of Pause/Break
         */
        if  ( scan_code != -1 )
        {
            // Handle 'E1' scan code case for Pause/Break key
            if ( scan_code == 0xe1 )
            {
                // Get the next byte
                scan_code = ps2_recv_x();

                // Get the next byte and discard sequence
                if ( scan_code == 0x1d )
                {
                    ps2_recv_x();
                    continue;
                }
                else if ( scan_code == 0x9d )
                {
                    ps2_recv_x();
                    continue;
                }
            }

            // Handle 'E0' scan code cases
            if ( scan_code == 0xe0 )
            {
                // Get the next byte
                scan_code = ps2_recv_x();

                // Keep only wanted sequence pairs and discard the rest
                if ( scan_code == 0x50 ||
                     scan_code == 0xd0 ||
                     scan_code == 0x4d ||
                     scan_code == 0xcd ||
                     scan_code == 0x4b ||
                     scan_code == 0xcb ||
                     scan_code == 0x48 ||
                     scan_code == 0xc8    )
                {
                    // Do nothing. The 'keep'list is shorter then the 'discard' list
                }
                else
                {
                    continue;
                }
            }

            // Remove unwanted scan codes
            switch ( scan_code & 0x7f )
            {
                case 15:        // Tab
                case 27:        // ]
                case 29:        // L-Ctrl, R-Ctrl
                case 41:        // '
                case 40:        // ' and " signs
                case 43:        // Back slash
                case 55:        // Keypad *
                case 56:        // Alt keys
                case 58:        // Caps lock
                case 69 ... 71: // Keypad
                case 73 ... 74:
                case 76:
                case 78 ... 79:
                case 81 ... 83:
                case 85:        // Special keys
                case 133:
                case 134:
                case 91 ... 93:
                    continue;

                case 54:
                    scan_code = (scan_code & 0x80) + 42;
                    break;
            }

            if  ( ((uint8_t)scan_code & 0x7f) > PS2_LAST_CODE || scan_code == 0 )
            {
                continue;
            }

            /* Store processed scan code in the key code output buffer 'key_codes[]'
             */
            write_key(scan_code);

            // TODO: Handle commands from the host Raspberry Pi
            //       Test variable 'command_in'
        }

        /* Update indicator LEDs if required
         * do this only if there is no pending scan code in the buffer
         * so that host-to-keyboard comm does not interfere with scan code exchange
         */
        else if ( kdb_lock_state != kbd_lock_keys )
        {
            kdb_led_ctrl(kbd_lock_keys);
            kdb_lock_state = kbd_lock_keys;
        }
    }

    return 0;
}

/* ----------------------------------------------------------------------------
 * reset()
 *
 *  Clear SREG_I on hardware reset.
 *  source: http://electronics.stackexchange.com/questions/117288/watchdog-timer-issue-avr-atmega324pa
 */
void reset(void)
{
     cli();
    // Note that for newer devices (any AVR that has the option to also
    // generate WDT interrupts), the watchdog timer remains active even
    // after a system reset (except a power-on condition), using the fastest
    // prescaler value (approximately 15 ms). It is therefore required
    // to turn off the watchdog early during program startup.
    MCUSR = 0; // clear reset flags
    wdt_disable();
}

/* ----------------------------------------------------------------------------
 * ioinit()
 *
 *  Initialize IO interfaces.
 *  Timer and data rates calculated based on the default 8MHz internal clock.
 *
 */
void ioinit(void)
{
    // Reconfigure system clock scaler to 8MHz
    CLKPR = 0x80;   // change clock scaler (sec 8.12.2 p.37)
    CLKPR = 0x00;

    // Initialize general IO PB pins
    DDRB  = PB_DDR_INIT;
    PORTB = PB_INIT;
    PORTB = PB_INIT | PB_PUP_INIT;

    // Initialize USI for 3-Wire mode (SPI)
    USISR &= ~USI_COUNTER;
    USISR |= USI_CNTR_OVRF;
    USIDR = 0;
    USICR = USICR_INIT;

    // Pin change interrupt setting
    GIMSK = GIMSK_INIT;
    PCMSK = PCMSK_INIT;
}

/* ----------------------------------------------------------------------------
 * ps2_send()
 *
 *  Send a command to the PS2 keyboard
 *  1)   Bring the Clock line low for at least 100 microseconds.
 *  2)   Bring the Data line low.
 *  3)   Release the Clock line.
 *
 *  4)   Set/reset the Data line to send the first data bit
 *  5)   Wait for the device to bring Clock low.
 *  6)   Repeat steps 5-7 for the other seven data bits and the parity bit
 *  7)   Wait for the device to bring Clock high.
 *
 *  8)   Release the Data line.
 *  9)   Wait for the device to bring Data low.
 *  10)  Wait for the device to bring Clock  low.
 *  11)  Wait for the device to release Data and Clock
 *
 *  param: command byte
 *  return: -1 transmit error, 0 ok
 */
int ps2_send(uint8_t byte)
{
    int     ps2_tx_parity = 1;
    int     ps2_tx_bit_count = 0;
    uint8_t ps2_data_bit;
    int     result;

    // Disable pin-change interrupts and reset receiver state so receive ISR does not run
    cli();
    //PCMSK &= ~PCINT_PB3;

    ps2_rx_state = PS2_IDLE;
    ps2_rx_data_byte = 0;
    ps2_rx_bit_count = 0;
    ps2_rx_parity = 0;

    // Follow byte send steps
    DDRB |= PS2_CLOCK;
    PORTB &= ~PS2_CLOCK;
    _delay_us(100);

    DDRB |= PS2_DATA;
    PORTB &= ~PS2_DATA;

    DDRB &= ~PS2_CLOCK;
    PORTB |= PS2_CLOCK;

    while ( ps2_tx_bit_count < 10 )
    {
        // This will repeat 8 bits of data, one parity bit, and one stop bit for transmission
        if ( ps2_tx_bit_count < 8 )
        {
            ps2_data_bit = byte & 0x01;
            ps2_tx_parity += ps2_data_bit;
        }
        else if ( ps2_tx_bit_count == 8 )
        {
            ps2_data_bit = (uint8_t)ps2_tx_parity & 0x01;
        }
        else
        {
            ps2_data_bit = 1;
        }

        do {} while ( (PINB & PS2_CLOCK) );

        if ( ps2_data_bit )
            PORTB |= PS2_DATA;
        else
            PORTB &= ~PS2_DATA;

        do {} while ( !(PINB & PS2_CLOCK) );

        ps2_tx_bit_count++;
        byte = byte >> 1;
    }

    // Restore data line to receive mode
    DDRB &= ~PS2_DATA;
    PORTB |= PS2_DATA;

    // Check here for ACK pulse and line to idle
    do {} while ( (PINB & PS2_CLOCK) );
    result = -1 * (int)(PINB & PS2_DATA);

    // Wait for clock to go high before enabling interrupts
    do {} while ( !(PINB & PS2_CLOCK) );

    sei();
    //PCMSK |= PCINT_PB3;

    // Allow keyboard to recover before exiting,
    // so that another ps2_send() is spaced in time from this call.
    _delay_ms(20);

    return result;
}

/* ----------------------------------------------------------------------------
 * ps2_recv_x()
 *
 *  Get a byte from the PS2 input buffer and block until byte is available
 *
 *  param:  none
 *  return: data byte value
 *
 */
int ps2_recv_x(void)
{
    int     result;

    do
    {
        result = ps2_recv();
    } while ( result == -1 );

    return result;
}

/* ----------------------------------------------------------------------------
 * ps2_recv()
 *
 *  Get a byte from the PS2 input buffer
 *
 *  param:  none
 *  return: -1 if buffer empty, otherwise data byte value
 *
 */
int ps2_recv(void)
{
    int     result = -1;

    if ( ps2_scan_code_count > 0 )
    {
        result = (int)ps2_scan_codes[ps2_buffer_out];
        ps2_scan_code_count--;
        ps2_buffer_out++;
        if ( ps2_buffer_out == PS2_BUFF_SIZE )
            ps2_buffer_out = 0;
    }

    return result;
}

/* ----------------------------------------------------------------------------
 * ps2_test_led()
 *
 *  Simple light test for keyboard LEDs.
 *  The function discards the keyboard's response;
 *  if something is wrong LEDs will not light up in succession.
 *
 *  param:  none
 *  return: none
 */
void kbd_test_led(void)
{
    kdb_led_ctrl(PS2_HK_SCRLOCK);

    _delay_ms(200);

    kdb_led_ctrl(0);
    kdb_led_ctrl(PS2_HK_CAPSLOCK);

    _delay_ms(200);

    kdb_led_ctrl(0);
    kdb_led_ctrl(PS2_HK_NUMLOCK);

    _delay_ms(200);

    kdb_led_ctrl(0);
    kdb_led_ctrl(PS2_HK_CAPSLOCK);

    _delay_ms(200);

    kdb_led_ctrl(0);
    kdb_led_ctrl(PS2_HK_SCRLOCK);

    _delay_ms(200);

    kdb_led_ctrl(0);
}

/* ----------------------------------------------------------------------------
 * kdb_led_ctrl()
 *
 *  Function for setting LED state to 'on' or 'off'
 *
 *  param:  LED bits, b0=Scroll lock b1=Num lock b2=Caps Lock
 *  return: none
 */
int kdb_led_ctrl(uint8_t state)
{
    int temp_scan_code;

    state &= 0x07;

    ps2_send(PS2_HK_LEDS);
    temp_scan_code = ps2_recv_x();

    if ( temp_scan_code == PS2_KH_ACK )
    {
        ps2_send(state);
        temp_scan_code = ps2_recv_x();
    }

    return temp_scan_code;
}

/* ----------------------------------------------------------------------------
 * kbd_code_set()
 *
 *  The function requests the keyboard to change its scan code set,
 *  Legal values are 1, 2 or 3.
 *
 *  param:  scan code set identifier
 *  return: PS2_KH_ACK no errors, other keyboard response if error
 */
int kbd_code_set(int set)
{
    int temp_scan_code;

    if ( set < 1 || set > 3 )
        return PS2_KH_RESEND;

    ps2_send(PS2_HK_ALTCODE);
    temp_scan_code = ps2_recv_x();

    if ( temp_scan_code == PS2_KH_ACK )
    {
        ps2_send((uint8_t)set);
        temp_scan_code = ps2_recv_x();
    }

    return temp_scan_code;
}

/* ----------------------------------------------------------------------------
 * kbd_typematic_set()
 *
 *  The function sets the keyboard typematic rate and delay.
 *
 *  Bit/s   Meaning
 *  ....... ...................................................
 *  0 to 4  Repeat rate (00000b = 30 Hz, ..., 11111b = 2 Hz)
 *  5 to 6  Delay before keys repeat (00b = 250 ms, 01b = 500 ms, 10b = 750 ms, 11b = 1000 ms)
 *     7    Must be zero
 *
 *  param:  typematic rate and delay
 *  return: PS2_KH_ACK no errors, other keyboard response if error
 */
int kbd_typematic_set(uint8_t configuration)
{
    int temp_scan_code;

    configuration &= 0x7f;

    ps2_send(PS2_HK_TMDELAY);
    temp_scan_code = ps2_recv_x();

    if ( temp_scan_code == PS2_KH_ACK )
    {
        ps2_send(configuration);
        temp_scan_code = ps2_recv_x();
    }

    return temp_scan_code;
}

/* ----------------------------------------------------------------------------
 * read_key()
 *
 *  Get a byte from the keyboard output buffer
 *
 *  param:  none
 *  return: -1 if buffer empty, otherwise data byte value of key code
 *
 */
int read_key(void)
{
    int     result = -1;

    if ( key_code_count > 0 )
    {
        result = (int)key_codes[key_buffer_out];
        key_code_count--;
        key_buffer_out++;
        if ( key_buffer_out == KEY_BUFF_SIZE )
            key_buffer_out = 0;
    }

    return result;
}

/* ----------------------------------------------------------------------------
 * write_key()
 *
 *  Write a byte to the keyboard output buffer
 *
 *  param:  key code to write
 *  return: -1 if buffer is full, otherwise data byte value of keycode
 *
 */
int write_key(uint8_t key_code)
{
    int     result = -1;

    if ( key_code_count < KEY_BUFF_SIZE )
    {
        key_codes[key_buffer_in] = key_code;
        result = (int) key_code;
        key_code_count++;
        key_buffer_in++;
        if ( key_buffer_in == KEY_BUFF_SIZE )
            key_buffer_in = 0;
    }

    return result;
}

/* ----------------------------------------------------------------------------
 * This ISR will trigger when PB3 changes state.
 * PB3 is connected to the PS2 keyboard clock line, and PB4 to the data line.
 * ISR will check PB3 state and determine if it is '0' or '1',
 * as well as track clock counts and input bits from PB4.
 * Once input byte is assembled is will be added to a circular buffer.
 *
 */
ISR(PCINT0_vect)
{
    uint8_t         ps2_data_bit;

    if ( (PINB & PS2_CLOCK) == 0 )
    {
        ps2_data_bit = (PINB & PS2_DATA) >> 4;

        switch ( ps2_rx_state )
        {
            /* Do nothing if an error was already signaled
             * let the main loop handle the error
             */
            case PS2_RX_ERR_START:
            case PS2_RX_ERR_OVERRUN:
            case PS2_RX_ERR_PARITY:
            case PS2_RX_ERR_STOP:
                break;

            /* If in idle, then check for valid start bit
             */
            case PS2_IDLE:
                if ( ps2_data_bit == 0 )
                {
                    ps2_rx_data_byte = 0;
                    ps2_rx_bit_count = 0;
                    ps2_rx_parity = 0;
                    ps2_rx_state = PS2_DATA_BITS;
                }
                else
                    ps2_rx_state = PS2_RX_ERR_START;
                break;

            /* Accumulate eight bits of data LSB first
             */
            case PS2_DATA_BITS:
                ps2_rx_parity += ps2_data_bit;
                ps2_data_bit = ps2_data_bit << ps2_rx_bit_count;
                ps2_rx_data_byte += ps2_data_bit;
                ps2_rx_bit_count++;
                if ( ps2_rx_bit_count == 8 )
                    ps2_rx_state = PS2_PARITY;
                break;

            /* Evaluate the parity and signal error if it is wrong
             */
            case PS2_PARITY:
                if ( ((ps2_rx_parity + ps2_data_bit) & 1) )
                    ps2_rx_state = PS2_STOP;
                else
                    ps2_rx_state = PS2_RX_ERR_PARITY;
                break;

            /* Check for valid stop bit
             */
            case PS2_STOP:
                if ( ps2_data_bit == 1 )
                {
                    if ( ps2_scan_code_count < PS2_BUFF_SIZE )
                    {
                        ps2_scan_codes[ps2_buffer_in] = ps2_rx_data_byte;
                        ps2_scan_code_count++;
                        ps2_buffer_in++;
                        if ( ps2_buffer_in == PS2_BUFF_SIZE )
                            ps2_buffer_in = 0;
                        ps2_rx_state = PS2_IDLE;
                    }
                    else
                        ps2_rx_state = PS2_RX_ERR_OVERRUN;
                }
                else
                    ps2_rx_state = PS2_RX_ERR_STOP;
                break;
        }
    }
}

/* ----------------------------------------------------------------------------
 * This ISR will trigger when the USI counter overflows indicating
 * 8-bits have been received/transmitted into/from the data buffer
 *
 */
ISR(USI_OVF_vect)
{
    int     data;

    // Get byte received as command
    command_in = USIDR;

    // Load new keyboard ASCII, if one is available
    if ( ( data = read_key() ) == -1 )
        USIDR = 0;
    else
        USIDR = (uint8_t) data;

    // Reset for next byte sequence
    USISR &= ~USI_COUNTER; // TODO: may not be needed
    USISR |= USI_CNTR_OVRF;
}
