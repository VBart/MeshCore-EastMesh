# Custom CLI Commands

This page covers the EastMesh-specific CLI commands added in this repository.

It does not try to repeat the full upstream MeshCore CLI surface.

## Repeater MQTT Commands

These commands are available on `*_repeater_mqtt` firmware targets.

### MQTT Status And Routing

- `get mqtt.status`
  - shows WiFi, NTP, IATA, endpoint status, status publishing state, and TX state
- `get mqtt.statuscfg`
  - shows whether periodic status messages are enabled
- `get mqtt.iata`
  - shows the IATA/location code used in MQTT topics
- `set mqtt.iata <code>`
  - sets the IATA/location code, for example `MEL`

### MQTT Identity

- `get mqtt.owner`
  - shows the configured owner public key
- `set mqtt.owner <64-hex-char-public-key>`
  - sets the owner public key used in JWT metadata
- `mqtt.owner <64-hex-char-public-key>`
  - shorthand for setting the owner public key
- `get mqtt.email`
  - shows the configured owner email
- `set mqtt.email <email>`
  - sets the owner email used in JWT metadata
- `mqtt.email <email>`
  - shorthand for setting the owner email

### MQTT Message Controls

- `get mqtt.packets`
  - shows whether packet messages are published
- `set mqtt.packets on|off`
  - enables or disables packet publishing
- `get mqtt.raw`
  - shows whether raw packet payloads are published
- `set mqtt.raw on|off`
  - enables or disables the separate `raw` MQTT topic
- `set mqtt.status on|off`
  - enables or disables periodic MQTT status publishing
- `get mqtt.tx`
  - shows whether TX packets are included
- `set mqtt.tx on|off`
  - enables or disables TX packet publishing

### MQTT Endpoints

- `get mqtt.eastmesh-au`
- `set mqtt.eastmesh-au on|off`
- `get mqtt.letsmesh-eu`
- `set mqtt.letsmesh-eu on|off`
- `get mqtt.letsmesh-us`
- `set mqtt.letsmesh-us on|off`

Legacy dotted aliases are also accepted:

- `mqtt.eastmesh.au`
- `mqtt.letsmesh.eu`
- `mqtt.letsmesh.us`

### WiFi Settings For MQTT Repeaters

- `get wifi.status`
  - shows SSID, connection state, raw WiFi status code, and IP when connected
- `get wifi.ssid`
  - shows the configured WiFi SSID
- `set wifi.ssid <ssid>`
  - sets the WiFi SSID
- `set wifi.pwd <password>`
  - sets the WiFi password
- `get wifi.powersaving`
  - shows the current WiFi power save mode
- `set wifi.powersaving none|min|max`
  - sets WiFi power saving mode

### Web Panel Controls

- `get web`
- `get web.status`
  - shows whether the local HTTPS panel is available
- `set web on|off`
- `set.web on|off`
  - enables or disables the local HTTPS panel

### Runtime Diagnostics

- `memory`
  - shows current heap and PSRAM usage
- `stats-core`
  - shows battery, uptime, sticky error count, and outbound queue depth
- `stats-radio`
  - shows radio noise floor, last RSSI, last SNR, and TX/RX airtime
- `stats-packets`
  - shows packet receive/send totals, flood/direct breakdown, and receive errors

## Web Panel Allowlisted Commands

When the repeater web panel is enabled, it only allows a limited command set.

That allowlist currently includes:

- `clock`
- `get mqtt.status`
- `get web`
- `get web.status`
- `advert`
- `reboot`
- `start ota`
- `memory`
- `stats-core`
- `stats-radio`
- `stats-packets`
- `get wifi.status`
- `get wifi.powersaving`
- `get mqtt.iata`
- `set mqtt.iata <code>`
- `get mqtt.owner`
- `set mqtt.owner <64-hex-char-public-key>`
- `get mqtt.email`
- `set mqtt.email <email>`
- `get mqtt.packets`
- `set mqtt.packets on|off`
- `get mqtt.raw`
- `set mqtt.raw on|off`
- `get mqtt.statuscfg`
- `set mqtt.status on|off`
- `get mqtt.tx`
- `set mqtt.tx on|off`
- `get mqtt.eastmesh-au`
- `set mqtt.eastmesh-au on|off`
- `get mqtt.letsmesh-eu`
- `set mqtt.letsmesh-eu on|off`
- `get mqtt.letsmesh-us`
- `set mqtt.letsmesh-us on|off`
- `set web on|off`
- `get name`
- `set name <device-name>`
- `get lat`
- `set lat <latitude>`
- `get lon`
- `set lon <longitude>`
- `get guest.password`
- `set guest.password <password>`
- `set prv.key <64-hex-char-private-key>`
- `get advert.interval`
- `set advert.interval <minutes>`
- `get flood.advert.interval`
- `set flood.advert.interval <hours>`
- `get flood.max`
- `set flood.max <count>`
- `get owner.info`
- `set owner.info <text>`

## Companion WiFi Rescue Commands

These commands are available in the serial rescue CLI for `*_companion_radio_wifi` builds.

To enter `CLI Rescue`:

- open a serial monitor at `115200` baud
- reboot the device
- long-press the user button within the first 8 seconds after boot
- wait for `========= CLI Rescue =========`

- `get wifi.status`
  - shows configured SSID, connection status, raw WiFi status code, and IP when connected
- `get wifi.ssid`
  - shows the configured WiFi SSID
- `get wifi.powersaving`
  - shows the current WiFi power saving mode
- `set wifi.ssid <ssid>`
  - saves a WiFi SSID and immediately retries connection
- `set wifi.pwd <password>`
  - saves a WiFi password and immediately retries connection
- `set wifi.powersaving none|min|max`
  - changes the WiFi power save mode

Companion WiFi builds also still support the existing rescue commands such as:

- `set pin <6-digit-pin>`
- `rebuild`
- `erase`
- `ls ...`
- `cat ...`
- `rm ...`
- `reboot`
