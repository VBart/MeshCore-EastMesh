# Build Locally With uv

This repo uses `uv` for Python tooling and runs PlatformIO through `uv run`.

## Setup

From the repo root:

```bash
uv sync
```

## Useful Commands

List build targets:

```bash
bash build.sh list
```

Plain PlatformIO build for a single target:

```bash
uv run pio run -e heltec_v4_repeater_mqtt
uv run pio run -e heltec_v4_companion_radio_wifi
```

Flash a target:

```bash
uv run pio run -e heltec_v4_repeater_mqtt -t upload --upload-port /dev/tty.usbmodemXXXX
```

Serial monitor:

```bash
uv run pio device monitor --port /dev/tty.usbmodemXXXX --baud 115200
```

## Release-Style Local Builds

If you want the same version metadata used by the release workflows, export the version variables first.

Companion WiFi:

```bash
export FIRMWARE_VERSION=v1.14.1
bash build.sh build-firmware heltec_v4_companion_radio_wifi
```

Repeater MQTT:

```bash
export FIRMWARE_VERSION=v1.14.1
export EASTMESH_VERSION=v1.0.1
bash build.sh build-firmware heltec_v4_repeater_mqtt
```

This produces versioned artifacts in `out/`.

## Supported `repeater_mqtt` Boards

```text
T_Beam_S3_Supreme_SX1262_repeater_mqtt
LilyGo_TLora_V2_1_1_6_repeater_mqtt
LilyGo_TBeam_1W_repeater_mqtt
Xiao_C3_repeater_mqtt
RAK_3112_repeater_mqtt
Tbeam_SX1262_repeater_mqtt
Heltec_E290_repeater_mqtt
Tbeam_SX1276_repeater_mqtt
Heltec_E213_repeater_mqtt
Heltec_Wireless_Tracker_repeater_mqtt
M5Stack_Unit_C6L_repeater_mqtt
LilyGo_TDeck_repeater_mqtt
Station_G2_repeater_mqtt
Station_G2_logging_repeater_mqtt
LilyGo_T3S3_sx1262_repeater_mqtt
Meshadventurer_sx1262_repeater_mqtt
Meshadventurer_sx1268_repeater_mqtt
Heltec_Wireless_Paper_repeater_mqtt
heltec_v4_repeater_mqtt
heltec_v4_tft_repeater_mqtt
heltec_tracker_v2_repeater_mqtt
Heltec_v3_repeater_mqtt
Heltec_WSL3_repeater_mqtt
LilyGo_T3S3_sx1276_repeater_mqtt
Heltec_v2_repeater_mqtt
nibble_screen_connect_repeater_mqtt
Tenstar_C3_sx1262_repeater_mqtt
Tenstar_C3_sx1268_repeater_mqtt
Heltec_ct62_repeater_mqtt
Xiao_S3_WIO_repeater_mqtt
Generic_E22_sx1262_repeater_mqtt
Generic_E22_sx1268_repeater_mqtt
```

Notes:

- some `*_repeater_mqtt_` experimental C6-style envs exist in the repo but are not listed here as primary EastMesh targets
- `Xiao_C3_repeater_mqtt` disables the web panel to stay within flash limits

## Supported `companion_radio_wifi` Boards

```text
Xiao_S3_WIO_companion_radio_wifi
Heltec_v3_companion_radio_wifi
Heltec_WSL3_companion_radio_wifi
LilyGo_TBeam_1W_companion_radio_wifi
RAK_3112_companion_radio_wifi
LilyGo_TLora_V2_1_1_6_companion_radio_wifi
ThinkNode_M5_companion_radio_wifi
Xiao_C3_companion_radio_wifi
heltec_tracker_v2_companion_radio_wifi
heltec_v4_companion_radio_wifi
heltec_v4_tft_companion_radio_wifi
Station_G2_companion_radio_wifi
ThinkNode_M2_companion_radio_wifi
nibble_screen_connect_companion_radio_wifi
Heltec_v2_companion_radio_wifi
T_Beam_S3_Supreme_SX1262_companion_radio_wifi
```

## Companion WiFi CLI

Current companion WiFi builds support persisted WiFi rescue commands:

- open a serial monitor at `115200` baud
- reboot the device
- long-press the user button within the first 8 seconds after boot to enter `CLI Rescue`
- wait for `========= CLI Rescue =========`
- then run the WiFi rescue commands below from the serial monitor

```text
get wifi.status
get wifi.ssid
get wifi.powersaving
set wifi.ssid <ssid>
set wifi.pwd <password>
set wifi.powersaving none|min|max
```
