# PS2 keyboard interface to SPI for Dragon 32 emulator on Raspberry Pi

This program is the interface code for AVR with a PS2 keyboard. It implements a PS2 keyboard interface and an SPI serial interface. The AVR connects with the Raspberry Pi's SPI interface. The code configures the keyboard, accepts scan codes, converts the AT scan codes to ASCII make/break codes for the [Dragon 32 emulation](https://github.com/eyalabraham/dragon) running on the Raspberry Pi.
The AVR buffers the key codes in a small FIFO buffer, and the emulation periodically reads the buffer through the SPI interface.

## Resources

- [wiki](https://en.wikipedia.org/wiki/PS/2_port)
- [AVR interface to PS2](http://www.electronics-base.com/projects/complete-projects/108-avr-ps2-keyboard-key-readout)
- [PS2 protocol](http://www.burtonsys.com/ps2_chapweske.htm)
- [PS2 keyboard command protocol](https://wiki.osdev.org/PS/2_Keyboard)
- Dragon 32 computer [keyboard](http://archive.worldofdragon.org/index.php?title=Keyboard)

## Raspberry Pi interface

### Connectivity

Raspberry Pi and AVR running on 3.3v, and level shifter to/from 5.0 logic is connected between AVR and keyboard lines. ** WILL THIS WORK? PS2 lines and open collector **

```
 +-----+               +-----+            +-------+
 |     |               |     |            |       |
 |     +----[ MOSI>----+     |            |       |
 |     |               |     |            |       |
 |     +----< MISO]----+     +--< Data >--+ Level |
 | RPi |               | AVR |            | shift +---> PS2 keyboard
 |     +----[ SCL >----+     +--< CLK ]---+       |
 |     |               |     |            |       |
 |     +----[ RST >----+     |            |       |
 |     |               |     |            |       |
 +-----+               +-----+            +-------+
```

### ATtiny AVR IO

 | Function  | AVR  | Pin | I/O               |
 |-----------|------|-----|-------------------|
 | Reset     | PB5  | 1   | RPi GPIO22        |
 | PS2 clock | PB3  | 2   | in/out w/ pull up |
 | PS2 data  | PB4  | 3   | in/out w/ pull up |
 | DI        | PB0  | 5   | RPi MOSI          |
 | DO        | PB1  | 6   | RPi MISO          |
 | SCLK      | PB2  | 7   | RPi SCL           |

## Scan code processing

- Only pass make and break codes for keys in range 1 to 84
- Handle 'E0' modifier for keypad by removing the 'E0' which will effectively reduce any keyboard to one that is equivalent to an 83 key keyboard.
- Discard PrtScrn E0,2A,E0,37 and E0,B7,E0,AA and other codes for key that the Dragon does not support.
- Convert E1 sequence of Pause/Break to scan code 54h/84
