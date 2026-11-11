# TODO

## Crowd sensor firmware

- [x] Flash a stable sensor-independent preparation image to the fresh crowd-controller board.
- [ ] Confirm VL53L1X breakout wiring and I2C address.
- [ ] Confirm LD2410C UART pins, voltage, and protocol settings.
- [ ] Create a separate PlatformIO environment with hostname crowdcontroller.
- [ ] Implement VL53L1X distance sampling and filtering.
- [ ] Implement LD2410C presence and distance-gate parsing.
- [ ] Combine both sensors into a configurable crowd-distance state.
- [ ] Use the crowd rotary encoder to set the shared detection threshold.
- [ ] Add pushbutton lock/unlock for the crowd threshold.
- [ ] Add calibration and smoothing controls.
- [ ] Add hardware-in-the-loop tests once sensors are connected.

## Hue controller

- [x] Add VL53L1X hand-distance hue control.
- [x] Use the rotary encoder for sensor-range adjustment.
- [x] Use the pushbutton to lock/unlock the current hue.
- [x] Verify pull-away behavior and live OSC output.

## OSC and scenes

- [ ] Add a named scene-profile list to the configurator.
- [ ] Add one-click scene address selection.
- [ ] Add validation for scene-specific OSC paths.
- [ ] Verify behavior when Synesthesia changes scenes rapidly.
- [ ] Add a reconnect/retry indicator for network OSC configuration.

## Reliability

- [ ] Persist host-side configuration in a local ignored file.
- [ ] Add graceful serial reconnect after USB unplug/replug.
- [ ] Add structured logging and a configurable log level.
- [ ] Test overnight Wi-Fi and OSC operation.
- [ ] Add firmware watchdog and explicit Wi-Fi reconnect behavior.

## Documentation and release

- [ ] Add a wiring diagram after the sensor pinout is confirmed.
- [ ] Add a short demo video or GIF.
- [ ] Add a hardware bill of materials.
- [ ] Add a tagged first public release.

