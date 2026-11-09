# Crowd-controller preparation firmware

This image prepares the fresh ESP32-S3 for the VL53L1X and LD2410C sensors without assigning sensor pins before their wiring is confirmed.

- Bonjour hostname: `crowdcontroller.local`
- Rotary encoder: GPIO4/GPIO5, common to GND
- Pushbutton: GPIO6 to GND
- Onboard RGB LED: GPIO48
- Detection-distance range: 500–4000 mm
- Encoder step: 50 mm
- Default OSC address: `/controls/global/slider/1`
- Sensor inputs: intentionally unassigned

The encoder currently emits a normalized distance-configuration value over OSC and serial JSON. The pushbutton locks/unlocks the threshold. The onboard RGB LED uses `brightness,red,green,blue` status values: unlocked with presence `255,255,0,255` (flashing magenta), unlocked without presence `255,0,0,255` (flashing blue), and locked `25,255,0,0` (steady dim red). Sensor sampling will be added after the LD2410C UART wiring is confirmed.

Build:

```sh
pio run -d firmware/crowd-controller
```

Flash only after confirming the fresh board's serial port:

```sh
pio run -d firmware/crowd-controller -t upload --upload-port <crowd-controller-serial-port>
```
