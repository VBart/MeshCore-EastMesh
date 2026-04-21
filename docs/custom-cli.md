# Custom CLI Commands

This page covers the EastMesh-specific CLI commands added in this repository.

It does not try to repeat the full upstream MeshCore CLI surface.

## Repeater MQTT Commands

These commands are available on `*_repeater_mqtt` firmware targets.

### MQTT Status And Routing

- `get mqtt.status`: shows WiFi, NTP, IATA, endpoint status, status publishing state, and TX state.
- `get mqtt.statuscfg`: shows whether periodic status messages are enabled as a simple `on` or `off` value. Most users can just use `get mqtt.status`.
- `get mqtt.iata`: shows the IATA/location code used in MQTT topics.
- `set mqtt.iata <code>`: sets the IATA/location code, for example `MEL`.
- `set mqtt.iata UNSET`: marks MQTT IATA as not configured yet. While it is `UNSET`, enabled MQTT brokers stay disconnected until a real code is saved.

### MQTT Identity

- `get mqtt.owner`: shows the configured owner public key.
- `set mqtt.owner <64-hex-char-public-key>`: sets the owner public key used in JWT metadata.
- `mqtt.owner <64-hex-char-public-key>`: shorthand for setting the owner public key.
- `get mqtt.email`: shows the configured owner email.
- `set mqtt.email <email>`: sets the owner email used in JWT metadata.
- `mqtt.email <email>`: shorthand for setting the owner email.

### MQTT Message Controls

- `get mqtt.packets`: shows whether packet messages are published.
- `set mqtt.packets on|off`: enables or disables packet publishing.
- `get mqtt.raw`: shows whether raw packet payloads are published.
- `set mqtt.raw on|off`: enables or disables the separate `raw` MQTT topic.
- `set mqtt.status on|off`: enables or disables periodic MQTT status publishing.
- `get mqtt.tx`: shows whether TX packets are included.
- `set mqtt.tx on|off`: enables or disables TX packet publishing.

### MQTT Endpoints

- `get mqtt.eastmesh-au`
- `set mqtt.eastmesh-au on|off`
- `get mqtt.letsmesh-eu`
- `set mqtt.letsmesh-eu on|off`
- `get mqtt.letsmesh-us`
- `set mqtt.letsmesh-us on|off`

Notes:

- new repeater MQTT installs default `mqtt.iata` to `UNSET`
- `letsmesh-eu` and `letsmesh-us` remain off by default unless already configured in saved prefs
- if `mqtt.iata` is `UNSET`, `eastmesh-au`, `letsmesh-eu`, and `letsmesh-us` will not connect even if they are toggled on

Legacy dotted aliases are also accepted:

- `mqtt.eastmesh.au`
- `mqtt.letsmesh.eu`
- `mqtt.letsmesh.us`

### WiFi Settings For MQTT Repeaters

- `get wifi.status`: shows SSID, connection state, raw WiFi status code, and IP when connected.
- `get wifi.ssid`: shows the configured WiFi SSID.
- `set wifi.ssid <ssid>`: sets the WiFi SSID.
- `set wifi.pwd <password>`: sets the WiFi password.
- `get wifi.powersaving`: shows the current WiFi power save mode.
- `set wifi.powersaving none|min|max`: sets WiFi power saving mode.

### Web Panel Controls

- `get web`
- `get web.status`: shows whether the local HTTPS panel is available. After `start ota`, this reports `web:suspended ota` until the repeater reboots.
- `get web.stats.status`: shows whether the dedicated stats page and history subsystem are enabled, whether recent history is active, whether PSRAM-backed history is available, and whether the SD-backed archive is mounted. When enabled, the history capture now covers supported environment telemetry too, not just the original battery/radio series.
- `set web on|off`
- `set.web on|off`: enables or disables the local HTTPS panel.
- `set web.stats on|off`
- `set.web.stats on|off`: enables or disables the dedicated `/stats` page and historical stats collection.

### Runtime Diagnostics

- `memory`: shows current heap and PSRAM usage.
- `stats-core`: shows battery, uptime, sticky error count, and outbound queue depth.
- `stats-radio`: shows radio noise floor, last RSSI, last SNR, and TX/RX airtime.
- `stats-packets`: shows packet receive/send totals, flood/direct breakdown, and receive errors.

> If `noise_floor` reports `0`, check `get agc.reset.interval`; if it is not `0`, try `set agc.reset.interval 0` and test again.

### Board Battery Reporting

- `get battery.reporting`: shows whether board battery reporting is enabled. Support is board-dependent.
- `set battery.reporting on|off`: enables or disables battery voltage reporting on supported boards. This is currently useful for Heltec V3 boards where USB-only power can produce misleading battery readings. If your board needs this too, open an issue and support can be added board-by-board.

### T-Beam 1W Fan Control

These commands are only available on `LilyGo_TBeam_1W_*` repeater builds.

- `get fan`: shows the current fan mode, current fan state, and the last NTC-based board temperature when available.
- `set fan auto`: returns the fan to automatic control and persists that mode across reboot.
- `set fan on`: forces the fan on and persists that mode across reboot.
- `set fan off`: forces the fan off and persists that mode across reboot.
- `set fan timeout <Ns>`: changes the automatic post-TX hold window in seconds and persists it across reboot, for example `set fan timeout 45s`.

Auto mode behavior:

- forces the fan on during TX and keeps it on for the configured timeout afterward
- otherwise turns the fan on at `48C`
- turns it back off at `42C`
- keeps the fan on if the NTC reading is unavailable

Notes:

- default repeater fan mode is `auto`
- default post-TX timeout is `30s`
- fan mode and timeout are stored in repeater prefs and survive reboot
- only `LilyGo_TBeam_1W_*` repeater builds use these persisted fan settings
- accepted range is `0s` to `600s`

## Web Panel CLI Access

When the repeater web panel is enabled and you are authenticated, the browser CLI panel can run the same CLI commands accepted by the repeater.

Notes:

- the panel still uses the repeater admin password for access
- commands run with the same care as if you typed them into the repeater CLI directly
- this is intended for local admin use on a trusted network
- `start ota` suspends the local repeater web panel until reboot so the OTA HTTP listener can take over port `80`

## Companion WiFi Rescue Commands

These commands are available in the serial rescue CLI for `*_companion_radio_wifi` builds.

To enter `CLI Rescue`:

- open a serial monitor at `115200` baud
- reboot the device
- long-press the user button within the first 8 seconds after boot
- wait for `========= CLI Rescue =========`

- `get wifi.status`: shows configured SSID, connection status, raw WiFi status code, and IP when connected.
- `get wifi.ssid`: shows the configured WiFi SSID.
- `get wifi.powersaving`: shows the current WiFi power saving mode.
- `set wifi.ssid <ssid>`: saves a WiFi SSID and immediately retries connection.
- `set wifi.pwd <password>`: saves a WiFi password and immediately retries connection.
- `set wifi.powersaving none|min|max`: changes the WiFi power save mode.

Companion WiFi builds also still support the existing rescue commands such as:

- `set pin <6-digit-pin>`
- `rebuild`
- `erase`
- `ls ...`
- `cat ...`
- `rm ...`
- `reboot`
