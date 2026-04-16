# Repeater Web Panel

This page is for end users running an EastMesh `*_repeater_mqtt` build with the local web panel enabled.

It covers how to reach the panel, what each section does, and what to expect when using it on desktop or mobile.

## What It Is

The repeater web panel is a local HTTPS configuration page served directly by the repeater over WiFi.

It gives you:

- a password-gated local admin page at `/app`
- a dedicated stats and trends page at `/stats`
- quick `get` commands for common repeater and MQTT checks
- a terminal-style CLI panel for allowlisted commands
- editable repeater settings
- editable MQTT settings
- a historical stats view with trends, neighbours, and recent events

Operational guidance:

- use it for initial setup, occasional configuration changes, and troubleshooting
- when you are finished, prefer `set web off` on MQTT repeaters that need maximum headroom
- this leaves more internal heap available for MQTT/WSS activity, especially on dual-broker setups

## Screenshot Overview

The screenshots below show the current split between the lighter `/app` admin page and the dedicated `/stats` status page.

### `/app`

![Repeater web panel `/app` overview](./_assets/repeater_web_panel_app_light.png#only-light)
![Repeater web panel `/app` overview](./_assets/repeater_web_panel_app_dark.png#only-dark)

### `/stats`

![Repeater web panel `/stats` overview](./_assets/repeater_web_panel_stats_light.png#only-light)
![Repeater web panel `/stats` overview](./_assets/repeater_web_panel_stats_dark.png#only-dark)

## Requirements

You need:

- a supported `*_repeater_mqtt` firmware build
- WiFi configured on the repeater
- the repeater connected to your local network
- the repeater admin password

Some constrained targets disable the web panel to stay within flash limits. If your board does not support it, `get web.status` will not show it as available.

## How To Open It

1. Connect the repeater to WiFi.
2. Find its IP address.
3. Open `https://<repeater-ip>/` in a browser.
4. Accept the browser warning for the self-signed certificate.
5. Enter the repeater admin password.

Useful CLI commands:

- `get wifi.status`: shows WiFi state and IP address when connected.
- `get web.status`: shows whether the web panel is up and which URL to use.

Example:

- `https://10.33.135.208/`

## Login And Security

- the panel uses the same admin password as the repeater CLI
- the connection is HTTPS, but the certificate is self-signed
- browsers will warn the first time you connect
- the panel only exposes an allowlisted subset of CLI commands

This is intended for local admin use on a trusted network, not for open internet exposure.

## Performance Notes

The panel is designed to load more gently than earlier versions. On login it now fetches sections in sequence instead of requesting one large bootstrap payload up front.

Even with that change, the panel still uses HTTPS and internal heap. On boards running one or two WSS MQTT brokers, opening the panel reduces MQTT headroom while the session is active.

Recommended practice for repeater deployments:

- enable the panel for initial configuration
- use it again for occasional checks or troubleshooting
- disable it with `set web off` when finished so MQTT has the most headroom available

## Navigation And Actions

The web console now has two main pages:

- `/app`: lighter-weight control and configuration view
- `/stats`: current status, trends, neighbours, and recent events

Both pages share the same top navigation and utility actions.

### `/app`

The `/app` page is the main admin and configuration surface.

It includes:

- navigation to `App` and `Stats`
- `Advert`
- `Start OTA`
- `Reboot`
- theme toggle
- `Logout`

Use `Start OTA` only when you intend to update firmware.

### `/stats`

The `/stats` page is the home for current status and historical visibility.

It includes:

- navigation to `App` and `Stats`
- `Refresh`
- `Reboot`
- theme toggle
- `Logout`

## Quick "get" Commands

This section runs common read-only commands for:

- Wi-Fi
- MQTT

These are useful for quick checks without typing into the CLI field.

## Run CLI Command

This is a small terminal for allowlisted commands.

- press `Enter` to run the command
- command history is shown in the terminal box below
- save buttons elsewhere in the page also show the generated command and the reply here
- `clock` is available here if you want to check the repeater's current board time

This makes it easy to see exactly what the panel sent to the repeater.

## Repeater Settings

This section includes:

- Device Name
- Latitude
- Longitude
- Guest Password
- Private Key
- Advert Interval
- Flood Interval
- Flood Max
- Owner Info

Notes:

- `Latitude` and `Longitude` default to `0.0` as placeholders
- changing the private key requires a reboot to apply
- the refresh buttons load the current value from the repeater
- the save buttons send the matching CLI command immediately

## MQTT Settings

This section includes:

- `mqtt.iata`: selected from a curated east-coast/south-east list.
- `mqtt.owner`: owner public key.
- `mqtt.email`: owner contact email.
- MQTT server toggles: `eastmesh-au`, `letsmesh-eu`, and `letsmesh-us`.

`MEL` is used as the default dropdown option until the repeater's saved value is loaded.

Notes:

- the current MQTT server states are loaded when the page opens
- you can toggle each MQTT server on or off from this panel
- if all three servers are enabled at once, the panel shows a warning recommending two at most

## `/stats` Overview

The stats page is loaded separately from `/app` and is intended to keep the main admin page lighter.

The `/stats` page currently shows:

- `Services`: MQTT, web, archive, card, neighbour count, and archive capacity
- `Trends`: battery, heap free, packet activity, and signal
- `Neighbours`: current neighbour table
- `Events`: current boot/session events

The trend graphs load sequentially rather than as one large payload:

1. summary/status
2. battery
3. memory
4. packet activity
5. signal

This keeps browser-side and device-side memory use lower than the previous in-page stats view.

If `web.stats` is enabled and an SD archive is mounted, trends can restore archived summary points after reboot. Recent live points are still added from in-memory history.

### Stats History Capacity

Stats samples are collected once per minute.

Current in-memory history caps are:

| Board class                    | Sample cap | Event cap | Approx. sampled history  |
| ------------------------------ | ---------: | --------: | ------------------------ |
| No PSRAM                       |       `24` |       `8` | Live-only recent history |
| Less than `4MB` PSRAM          |      `240` |      `96` | About `4` hours          |
| `4MB` to less than `8MB` PSRAM |      `480` |     `192` | About `8` hours          |
| `8MB` PSRAM or more            |      `720` |     `288` | About `12` hours         |

Archive-backed restore requires `web.stats` enabled plus a mounted SD card on boards that support the EastMesh archive path.

The main purpose of the SD card is to let the repeater retain and restore stats history for `/stats`. As a secondary option, the archive files can also be removed and inspected on a computer for deeper manual review.

On no-PSRAM boards, `/stats` can still show recent graphs while the stats view is active, but the history is smaller and does not provide the same archive-backed behaviour as PSRAM-capable boards.

Useful CLI commands:

- `set web.stats on`
- `set web.stats off`
- `get web.stats.status`

## Mobile Use

The page is responsive and should work cleanly on a phone.

On mobile:

- quick command buttons collapse into a two-column layout
- top navigation and action groups stay compact and touch-friendly
- input rows stay usable for touch interaction
- trend cards reorganize into single-column sections where needed

## Common Tasks

### Check WiFi And MQTT

1. Open the panel.
2. Press `wifi.status` in Quick `get` Commands.
3. Press `mqtt.status` in Quick `get` Commands.
4. Open `/stats` from the top navigation for the historical stats view.

### Change Device Name

1. Edit `Device Name`.
2. Press `Save`.
3. Confirm the generated command and reply in the CLI terminal box.

### Update MQTT Owner Or Email

1. Go to `MQTT Settings`.
2. Enter the new value.
3. Press `Save`.
4. Use the refresh button if you want to re-read the stored value from the repeater.

### Start OTA

1. Press `Start OTA`.
2. Confirm the action.
3. Continue with your normal OTA workflow.

### Use Historical Stats

1. Enable stats if needed with `set web.stats on`.
2. Open `/stats` from the top navigation.
3. Review `Services` for archive and runtime state.
4. Review `Trends` for recent graph history.
5. Use `Refresh` to reload the stats page.

## Troubleshooting

### The browser warns about the certificate

That is expected. The panel uses a self-signed certificate generated for local use.

### I cannot reach the page

Check:

- the repeater is on WiFi
- the IP address from `get wifi.status`
- `get web.status` reports the panel as up
- your board/firmware target supports the web panel

### The panel opens but login fails

Use the repeater admin password, not the guest password.

### MQTT becomes unstable when I log in

The web panel now loads settings section-by-section to reduce startup pressure, but HTTPS still consumes internal heap.

Check:

- whether one or two MQTT brokers are enabled
- `memory` before and after login
- whether stability improves after `set web off`

For fixed installations where MQTT uptime matters more than browser access, use the panel briefly and then disable it again.

### A command says it is not allowlisted

The panel intentionally limits what can be run from the browser. Use the serial CLI for commands outside the web allowlist. `clock` is included, but most maintenance and debug commands are still serial-only.

### Stats or settings do not refresh

Try:

- refreshing the browser tab
- using `Refresh` on `/stats`
- logging out and back in
- checking WiFi stability with `get wifi.status`

### `/stats` is unavailable

Check:

- `get web.status`
- `get web.stats.status`
- whether `set web.stats on` has been applied

If `web.stats` is off, `/stats` will stay disabled and the historical graph requests will not run.

## Related Docs

- [Custom CLI Commands](./custom-cli.md)
- [Download and Flash Releases](./releases.md)
- [Build Locally With uv](./local-builds.md)
