# Crowd Controller Wiring and Bill of Materials

This is the current single-sensor crowd-controller build. Each DevKit has one LD2410C; the VL53L1X is not part of this controller.

## Wiring

Use the ESP32-S3 Mini board headers labeled `TX` and `RX` for the radar UART. On this board variant, those headers are GPIO43 (TX) and GPIO44 (RX).

| ESP32-S3 Mini | LD2410C / accessory | Purpose |
| --- | --- | --- |
| 5V / VBUS | VCC | Radar power |
| GND | GND | Common ground |
| RX header / GPIO44 | TX | Radar serial data into ESP32 |
| TX header / GPIO43 | RX | Radar serial data to sensor |
| GPIO7 | OUT | Binary presence output |
| GPIO4 | Encoder A | Rotary input |
| GPIO5 | Encoder B | Rotary input |
| GPIO6 | Encoder switch | Lock/unlock button, active low |
| GPIO48 | Onboard RGB LED | Status indicator |

The LD2410C serial signals are 3.3 V logic. Cross the UART connections: sensor TX goes to board RX, and sensor RX goes to board TX. Do not connect the radar to both the labeled UART pins and GPIO8/9.

The encoder selects the radar's maximum detection gate. The firmware starts at 500 mm, which maps to the LD2410C's lowest supported gate, and sends the range configuration over UART. The pushbutton locks the selected range.

## LED status

The onboard RGB LED uses `brightness,red,green,blue` values:

- unlocked with presence: `255,255,0,255` — flashing magenta;
- unlocked without presence: `255,0,0,255` — flashing blue;
- locked: `25,255,0,0` — steady dim red.

The module's `OUT` signal is also exposed through the live device endpoint:

```sh
curl http://crowdcontroller.local/api/status
```

The response includes `presence`, `distance_mm`, and the configured OSC address.

## Bill of materials

| Qty | Item | Notes |
| ---: | --- | --- |
| 1 | ESP32-S3 Mini / ESP32-S3FH4R2 development board | Crowd controller board; onboard RGB LED on GPIO48 |
| 1 | HLK-LD2410C 24 GHz mmWave presence sensor | 5 V power; 3.3 V UART; use VCC, GND, TX, RX, and OUT |
| 1 | Rotary encoder with integrated pushbutton | Five-pin mechanical encoder; A/B/switch wired to GPIO4/5/6 |
| 1 | USB cable | Programming and 5 V power for the ESP32 board |
| 1 set | Jumper wires / hookup wire | Sensor, encoder, power, and ground connections |
| 1 | Breadboard or prototyping carrier | Optional, for bench testing |
| 1 | External LED and series resistor | Optional; only needed if the onboard RGB LED is not used for bench indication |

No separate 5 V supply is required for the bench build when USB powers the ESP32 and its 5V/VBUS pin is available to the radar. Confirm the USB source and wiring can supply the ESP32 plus the LD2410C.

## Current limitations

- The LD2410C reports binary presence through `OUT`; the API does not expose a precise measured target distance.
- Range selection is quantized to the sensor's supported distance gates.
- The LD2410C does not provide target X/Y position.
