# Controller Wiring and Bill of Materials

## Hue controller wiring

The hue controller uses the VL53L1X for hand-distance control. The encoder sets the operating range; the pushbutton locks the current hue. The VL53L1X `INT` and `SHUT` pins are intentionally unused.

| ESP32-S3 Mini | VL53L1X / encoder | Purpose |
| --- | --- | --- |
| 3V3 | VIN | Sensor power |
| GND | GND / encoder common | Common ground |
| GPIO10 | SDA | I²C data |
| GPIO11 | SCL | I²C clock |
| GPIO4 | Encoder A | Range control |
| GPIO5 | Encoder B | Range control |
| GPIO6 | Encoder switch to GND | Hue lock/unlock |
| GPIO48 | Onboard RGB LED | Hue feedback |

The firmware uses VL53L1X long-distance mode and a selectable 500–4000 mm operating range in 50 mm steps. A hand within the selected range maps to the Synesthesia hue value; a hand outside the range leaves the last hue unchanged. Configure the ESP32's OSC host as the Mac's LAN IP, not `127.0.0.1`.

This is the current single-sensor crowd-controller build. Each DevKit has one LD2410C; the VL53L1X is not part of this controller.

## Wiring

Use the ESP32-S3 Mini board-labeled RX/TX pins for the radar UART.

| ESP32-S3 Mini | LD2410C / accessory | Purpose |
| --- | --- | --- |
| 5V / VBUS | VCC | Radar power |
| GND | GND | Common ground |
| Board-labeled RX / GPIO44 | TX | Radar serial data into ESP32 |
| Board-labeled TX / GPIO43 | RX | Radar serial data to sensor |
| GPIO7 | OUT | Binary presence output |
| GPIO4 | Encoder A | Rotary input |
| GPIO5 | Encoder B | Rotary input |
| GPIO6 | Encoder switch | Lock/unlock button, active low |
| GPIO48 | Onboard RGB LED | Status indicator |

The LD2410C serial signals are 3.3 V logic. Cross the UART connections: sensor TX goes to board RX, and sensor RX goes to board TX. Do not connect the radar to both the board-labeled UART pins and GPIO8/9.

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

The response includes `presence`, `activity`, `distance_mm`, and the configured OSC address. `activity` is a `0.0–1.0` value made from the two strongest movement-energy gates, with faster attack and slower release smoothing. This same activity value is sent to the configured OSC address.

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

- The LD2410C reports binary presence through `OUT`; the API does not expose a precise measured target distance. Engineering mode is enabled at startup so the UART includes movement-energy values for activity smoothing.
- The OSC activity value is not a crowd count; it is smoothed movement energy from two distance gates.
- Range selection is quantized to the sensor's supported distance gates.
- The LD2410C does not provide target X/Y position.
