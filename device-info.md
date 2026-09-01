# Device info

Name: FF380 Force Feedback Racemaster

Model number: 12933

ID: 06D6:005F

# Communication info

Reversed engineered from Windows XP driver that came with the wheel. Observed the behavior of the hardware.

## URB INTERRUPT IN
- Byte 0–1 : padding / unused
- Byte 2   : steering axis
- Byte 3   : shared pedal base / ADC common channel
- Byte 4   : right pedal axis
- Byte 5   : left pedal axis
- Byte 6   : Button group B (bitfield) (D-pad hat switch, bits 0x0 to 0x07 clockwise, 0xFF = neutral)
- Byte 7   : Button group A (bitfield) (bits 0x00 to 0x07)

## Force motor

### INFO
- Wheel is latched, single-axis torque device
- No continuous stream needed
- Explicit commit required
- No true HID PID support
- Pure vendor protocol
- The wheel is 1D force device (only X axis)

### URB INTERRUPT OUT
- Byte 0   : command (0x11 = set force effect)
- Byte 1   : effect slot / mode (0xFF = active/default slot)
- Byte 2   : subcommand / flags
- Byte 3   : unsigned force (0..255)
- Byte 4–7 : padding / unused

### FORCES BITMAP
- Full force left:    11 FF FF FF 00 00 00 00
- Medium force left:  11 FF FF BF 00 00 00 00
- Zero Force:         11 FF FF 00 00 00 00 00
- Medium force right: 11 FF FF 41 00 00 00 00
- Full force right:   11 FF FF 7F 00 00 00 00
- Reset (to middle):  05 01 00 00 00 00 00 00
- Commit:             01 11 00 00 00 00 00 00

Commit must follow every force command.

### FORCE STRENGTH VALUES (byte on index 3)
- Force zero strength: 0
- Force left increasing strength:  80 to FF (weakest to strongest)
- Force right increasing strength: 01 to 7F (weakest to strongest)
