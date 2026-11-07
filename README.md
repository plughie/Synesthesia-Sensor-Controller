# Synesthesia Sensor Controller

ESP32-S3 controller firmware and a small Python host bridge for live OSC control of Synesthesia Pro.

## What works now

- Rotary encoder hue control over the 0.0–2.0 Synesthesia hue range.
- Pushbutton lock/unlock.
- Onboard RGB LED feedback tied to hue.
- Browser configurator with live slider updates.
- Runtime OSC host, port, and address changes.
- Wi-Fi configuration without committing credentials.
- Bonjour/mDNS HTTP device name support.
- Serial and direct Wi-Fi OSC paths.

## Repository layout

- host/synesthesia_controller/ — Python API, configurator, OSC encoder, and tests.
- firmware/hue-controller/ — PlatformIO ESP32-S3 firmware.
- firmware/crowd-controller/ — sensor-ready preparation firmware with no sensor pins assigned yet.
- docs/project.md — architecture, wiring, API, flashing, and crowd-sensor plan.
- TODO.md — implementation roadmap.

## Quick start

~~~sh
cd host/synesthesia_controller
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements.txt
python3 -m unittest -v
python3 api.py --dry-run
~~~

Open http://127.0.0.1:8765/ for the configurator.

For a connected board, replace dry-run with a platform-specific serial port:

~~~sh
python3 api.py --serial <hue-controller-serial-port>
~~~

## Configure OSC

The host API and ESP32 have separate OSC destinations. The Mac API can send to Synesthesia on `127.0.0.1`, while the ESP32 must send to the Mac's LAN IP. Replace the example values for the local installation:

~~~sh
curl -X PUT http://127.0.0.1:8765/api/config \
  -H 'content-type: application/json' \
  -d '{"device_host":"huecontroller.local","osc_host":"127.0.0.1","device_osc_host":"192.168.1.100","osc_port":6000,"osc_address":"/controls/meta/hue"}'
~~~

Scene-specific addresses can be changed without reflashing:

~~~sh
curl -X PUT http://127.0.0.1:8765/api/config \
  -H 'content-type: application/json' \
  -d '{"osc_address":"/controls/scene/zoomfactor"}'
~~~

## Firmware

~~~sh
cd firmware/hue-controller
pio run
pio run -t upload --upload-port <serial-port>
~~~

Review the explicit target before flashing. Do not flash unrelated device firmware.

## Public repository hygiene

Do not commit Wi-Fi secrets, API tokens, private keys, hardware MAC addresses, or personal filesystem paths. Deployment-specific values belong in runtime configuration or local ignored files.

The project is licensed under the MIT License. See LICENSE.

