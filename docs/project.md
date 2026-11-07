# Synesthesia Controller

## Status

**Last documented:** 2026-11-07  
**Status:** Hue controller operational; live Synesthesia OSC routing and Bonjour configuration verified. Crowd-sensor firmware is not yet implemented because the sensors have not arrived/been wired.

## Purpose

An ESP32-S3 controller for Synesthesia Pro. The current controller uses a rotary encoder to set the Synesthesia hue value and a pushbutton to lock/unlock the control. A Mac-hosted configurator/API sends values to Synesthesia over OSC and can reconfigure the destination address during a performance.

The next stage is a separate crowd sensor using a VL53L1X time-of-flight sensor and an LD2410C mmWave sensor. Its rotary encoder will set the shared crowd-detection distance/threshold rather than directly changing the Synesthesia control value.

## Hardware identity and safety

There are two ESP32-S3 boards. Never interchange their firmware targets.

| Device | USB serial port | Chip MAC | Role |
|---|---|---|---|
| Hue controller | platform-specific USB serial port | not published | Synesthesia hue/OSC controller |
| Etherbulb | separate platform-specific USB serial port | not published | Etherbulb lighting controller; do not touch |

The hue controller is an ESP32-S3 with 4 MB flash and 2 MB PSRAM. It advertises the configurable Bonjour/mDNS hostname `huecontroller.local`. Network credentials and addresses are deployment-specific and are not committed here.

The ESP32 destination should normally be the Mac's LAN IP for reliable OSC delivery; mDNS hostname resolution from the ESP32 is not assumed.

## Hue controller wiring

The current rotary controller is wired as follows:

| Function | ESP32-S3 GPIO |
|---|---:|
| Encoder A | GPIO4 |
| Encoder B | GPIO5 |
| Encoder common/center | GND |
| Encoder pushbutton | GPIO6 and GND |
| Onboard RGB status LED | GPIO48 |
| External strip/sensor output | GPIO13 reserved and not driven by hue firmware |

The encoder inputs and switch use internal pull-ups. Rotation changes hue in 0.02 increments on the 0.0–2.0 Synesthesia range. The pushbutton toggles the lock state. While locked, rotation does not change hue. The status LED tracks hue, with red at the low end and green at the high end.

## Current OSC configuration

Synesthesia Pro:

- OSC input port: `6000`
- Mac OSC destination: the local Mac LAN IP on port `6000`
- Current address is scene-dependent and can be changed live.

Examples:

```sh
# Set the controller to Synesthesia's hue control.
curl -X PUT http://127.0.0.1:8765/api/config \
  -H 'content-type: application/json' \
  -d '{"device_host":"huecontroller.local","osc_host":"192.168.1.100","osc_port":6000,"osc_address":"/controls/meta/hue"}'

# Route the same controller to a scene-specific control.
curl -X PUT http://127.0.0.1:8765/api/config \
  -H 'content-type: application/json' \
  -d '{"device_host":"huecontroller.local","osc_address":"/controls/scene/zoomfactor"}'
```

The API updates its own OSC output, sends the new configuration over USB serial, and attempts to send it to `http://huecontroller.local/api/osc` over Wi-Fi. The ESP32 stores the OSC destination in NVS, so it survives reboot. USB serial remains a fallback while connected.

## Mac configurator/API

Project directory:

`host/synesthesia_controller` in this repository.

Start it against the hue controller:

```sh
cd host/synesthesia_controller
python3 api.py --serial <hue-controller-serial-port>
```

Open the browser configurator at [http://127.0.0.1:8765/](http://127.0.0.1:8765/).

Important API routes:

| Method | Route | Purpose |
|---|---|---|
| `GET` | `/api/status` | Current hue, OSC destination, device hostname, and Wi-Fi status |
| `GET` | `/api/config` | Current controller configuration |
| `PUT` | `/api/config` | Change OSC host, port, address, device hostname, or tuning values |
| `POST` | `/api/hue` | Set a hue value from the host API |
| `GET` | `/api/wifi` | Report SSID and whether a secret is set; never returns the secret |
| `PUT` | `/api/wifi` | Send shared Wi-Fi credentials to the ESP32 |

Hue examples:

```sh
# Raw Synesthesia hue range: 0.0–2.0
curl -X POST http://127.0.0.1:8765/api/hue \
  -H 'content-type: application/json' \
  -d '{"value":1.0,"mode":"raw"}'

# Normalized range: 0.0–1.0
curl -X POST http://127.0.0.1:8765/api/hue \
  -H 'content-type: application/json' \
  -d '{"value":0.5,"mode":"normalized"}'

# Inspect current state
curl http://127.0.0.1:8765/api/status
```

The host API sends normalized OSC values (`raw / 2.0`). Serial messages contain both raw and normalized representations. The API has a dry-run mode for development before sensors are connected:

```sh
python3 api.py --dry-run
```

The Wi-Fi secret is held only in the running API process and is not written to the project files or returned by the API.

## ESP32 firmware project

Project directory:

`firmware/hue-controller` in this repository.

The PlatformIO environment is `esp32-s3-test`, using the `esp32-s3-devkitc1-n4r2` board and Arduino framework. The firmware includes:

- rotary hue control and lock switch;
- GPIO48 hue feedback LED;
- Wi-Fi credentials stored in ESP32 Preferences/NVS;
- direct OSC UDP output;
- serial JSON configuration;
- HTTP `POST /api/osc` for live network reconfiguration;
- HTTP `GET /api/status`;
- Bonjour/mDNS hostname `huecontroller.local`;
- HTTP service advertisement on port 80.

The deployed image was built from `src/main.cpp` and flashed to the intended controller serial port. It was verified by:

- successful esptool write and hash verification;
- `huecontroller.local` resolving to the controller's deployment IP;
- successful ping;
- HTTP status reporting a connected Wi-Fi network, deployment IP, and the configured OSC address.

## Firmware flashing

Use the hue controller's explicit serial target; do not rely on an automatically selected port:

```sh
cd firmware/hue-controller
esptool --chip esp32s3 \
  --port <hue-controller-serial-port> --baud 460800 write-flash \
  0x0 .pio/build/esp32-s3-test/bootloader.bin \
  0x8000 .pio/build/esp32-s3-test/partitions.bin \
  0x10000 .pio/build/esp32-s3-test/firmware.bin
```

Do not flash unrelated firmware to another connected board. Confirm the serial target before flashing.

## Crowd sensor plan

The fresh crowd-controller board currently runs the preparation image in `firmware/crowd-controller/`. It advertises `crowdcontroller.local` after Wi-Fi is configured, uses the rotary encoder to select a 500–4000 mm threshold in 50 mm steps, and leaves VL53L1X/LD2410C inputs unassigned until wiring is confirmed.

Planned sensors:

- VL53L1X upward-facing time-of-flight distance sensor;
- LD2410C mmWave presence/distance sensor;
- rotary encoder with pushbutton for crowd-distance configuration.

The LD2410C can report target presence and distance gates/energies, but it does not provide lateral X/Y position of the nearest target. X/Y control would require a different sensor such as an LD2450/LD2461 or multiple sensors. The project currently accepts the no-X/Y design.

Planned crowd behavior:

1. Rotary encoder adjusts the shared detection distance/threshold for both sensors.
2. Pushbutton can lock the configured distance during a performance.
3. Sensor events are converted to a configurable OSC value.
4. The OSC address can be changed per scene without reflashing.
5. The future crowd device should advertise as `crowdcontroller.local`.

Not yet complete:

- confirm LD2410C UART pins and wiring;
- confirm VL53L1X I2C pins and wiring;
- implement crowd firmware and distance calibration;
- decide whether crowd detection should emit binary presence, distance, or a smoothed normalized value;
- test scene changes and long-running Wi-Fi behavior with sensors attached.

## Source files

- `synesthesia_controller/api.py` — local HTTP API, serial transport, OSC bridge, Wi-Fi/device configuration.
- `synesthesia_controller/protocol.py` — hue validation, serial JSON, and OSC packet encoding.
- `synesthesia_controller/configurator.html` — browser configurator and live slider.
- `synesthesia_controller/test_protocol.py` — protocol unit tests.
- `synesthesia_controller/README.md` — shorter implementation README.
- `esp32_s3_hue_test/src/main.cpp` — current hue-controller firmware.
- `esp32_s3_hue_test/platformio.ini` — board, port, and hostname configuration.
- `esp32_s3_hue_test/partitions.csv` — custom flash layout.

## Operational checklist

Before a performance:

1. Confirm the selected serial port belongs to the hue controller.
2. Start the API with `/dev/cu.usbmodem101`.
3. Confirm `curl http://127.0.0.1:8765/api/status` reports Wi-Fi connected and the expected deployment IP.
4. Confirm Synesthesia OSC input is enabled on port `6000`.
5. Set the scene-specific OSC address with `PUT /api/config`.
6. Turn the encoder and verify the Synesthesia control and onboard LED respond.
7. Press the encoder switch to lock the value before leaving the controller unattended.

## Known limitations

- The current hue firmware is a bring-up/controller firmware, not the final VL53L1X gesture firmware.
- The local API configuration is held in memory; the ESP32 persists its Wi-Fi and OSC settings in NVS.
- The ESP32 uses the Mac LAN IP for OSC rather than relying on `.local` hostname resolution.
- Crowd sensor wiring and UART/I2C pin assignments remain to be confirmed.


