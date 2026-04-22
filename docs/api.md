# Repeater Web API

This page documents the local HTTPS API exposed by EastMesh `*_repeater_mqtt` builds that support the web panel.

It is intended for:

- lightweight automation on your local network
- dashboards or scripts that need current repeater status
- remote CLI access through the same authenticated path used by the web panel

This is not a cloud API and not a separate backend service. The repeater firmware serves it directly.

## Scope And Availability

The API is available only when:

- you are running a supported `*_repeater_mqtt` firmware build
- the repeater web panel is enabled and running
- you can reach the repeater over the local network
- you have authenticated with the repeater admin password

The API is intended for trusted local-network administration. Do not expose it directly to the public internet.

## Base URL

Use the repeater's local HTTPS address:

```text
https://<repeater-ip>/
```

Example:

```text
https://192.168.1.123/
```

## Authentication

The API uses the same admin password as the repeater CLI and web panel.

1. `POST` the password to `/login`
2. store the returned session token
3. send that token in the `X-Auth-Token` header on later requests

Example:

```bash
TOKEN=$(curl -sk -X POST https://<repeater-ip>/login --data '<admin-password>')
```

Use the token:

```bash
curl -sk https://<repeater-ip>/api/stats -H "X-Auth-Token: $TOKEN"
```

Notes:

- the repeater uses a self-signed certificate, so most tools will need `-k` or equivalent
- if the session expires or is locked, requests return `401 Unauthorized`
- logging in again gives you a fresh token

## Performance Guidance

The API runs on the repeater itself, so polling frequency matters.

If the repeater is also running two MQTT connections, avoid frequent API polling. The current EastMesh usage pattern is:

- `60` second polling for stats
- on-demand requests for everything else

That is the recommended baseline if you want to avoid overloading the board. Keep the request rate low, avoid bursty polling, and prefer manual refresh or event-triggered reads for heavier operations.

Recommended practice:

- poll `/api/stats` no more than once per minute
- avoid scraping multiple endpoints in parallel
- use on-demand calls for configuration reads and CLI actions
- close out your session when you are finished and stop polling when not actively using the data

## Endpoints

### `POST /login`

Authenticate with the repeater admin password.

Request body:

```text
<admin-password>
```

Response:

- plain-text session token on success
- `401` on bad password

Example:

```bash
curl -sk -X POST https://<repeater-ip>/login --data '<admin-password>'
```

### `POST /api/command`

Run a repeater CLI command remotely.

Headers:

```text
X-Auth-Token: <token>
```

Request body:

```text
get wifi.status
```

Response:

- plain-text CLI output
- `OK` if a command succeeds without returning text

Example:

```bash
curl -sk https://<repeater-ip>/api/command \
  -H "X-Auth-Token: $TOKEN" \
  --data 'get wifi.status'
```

### `GET /api/stats`

Fetch the current summary payload used by the dedicated `/stats` page.

Headers:

```text
X-Auth-Token: <token>
```

Example:

```bash
curl -sk https://<repeater-ip>/api/stats \
  -H "X-Auth-Token: $TOKEN"
```

Notes:

- this summary view is also what the repo's web panel requests first before loading trend series
- if `web.stats` is disabled, the endpoint returns `503 Service Unavailable`
- supported boards may also include an optional `sensors` object in the summary payload for current GPS and environmental telemetry
- the `core` object includes raw `battery_mv`, board-reported `battery_pct` when available, a UI-ready `battery_display_pct`, and board-specific `battery_min_mv` / `battery_max_mv` range hints used by `/stats` when the board does not expose its own battery percentage

### `GET /api/stats?series=<name>`

Fetch one trend series.

Supported series:

- `battery`
- `memory`
- `signal`
- `noise_floor`
- `packets`
- `voltage`
- `sensor_temp`
- `humidity`
- `pressure`
- `pressure_altitude`
- `mcu_temp`
- `gps_altitude`
- `gps_satellites`

Example:

```bash
curl -sk "https://<repeater-ip>/api/stats?series=memory" \
  -H "X-Auth-Token: $TOKEN"
```

Notes:

- use `?series=battery`, not just `?series`
- the built-in web panel loads these series sequentially rather than all at once to keep board memory pressure lower
- environment series are included only when the board reports those readings; if a series has no captured points yet, it returns an empty `points` array and `current:null`

### `GET /api/stats?view=legacy`

Fetch the older bundle-style stats payload.

Example:

```bash
curl -sk "https://<repeater-ip>/api/stats?view=legacy" \
  -H "X-Auth-Token: $TOKEN"
```

This exists for compatibility and troubleshooting. For new integrations, prefer the summary endpoint plus specific `series` requests.

## Common Use Cases

### 1. Remote CLI Access

`/api/command` is the most flexible endpoint. It lets you run the same CLI commands accepted by the repeater.

Examples:

- `get wifi.status`
- `get mqtt.status`
- `get web.status`
- `get web.stats.status`
- `get repeat`
- `get radio`

This is useful for:

- remote diagnostics from a laptop or phone
- simple scripts that collect operational state
- admin tools that want to reuse existing CLI behavior instead of adding new firmware endpoints

Example:

```bash
curl -sk https://<repeater-ip>/api/command \
  -H "X-Auth-Token: $TOKEN" \
  --data 'get mqtt.status'
```

### 2. Build A Lightweight Status Dashboard

Use `/api/stats` for summary information and one `series` call at a time for trend lines.

Recommended pattern:

1. fetch `/api/stats`
2. render current service state and summary fields
3. fetch one trend series only when needed
4. refresh at `60` second intervals, or slower if the repeater is busy

This is the same basic pattern used by the built-in `/stats` page.

### 3. Reuse The API For Quick Health Checks

Because `/api/command` returns CLI output directly, it works well for small operational checks in scripts or home-lab monitoring.

Examples:

- confirm the repeater still has Wi-Fi
- check MQTT broker connection state
- verify that the web panel is enabled before attempting stats reads
- confirm current radio settings before applying changes

Example:

```bash
curl -sk https://<repeater-ip>/api/command \
  -H "X-Auth-Token: $TOKEN" \
  --data 'get web.status'
```

### 4. Remote Admin Actions

The web panel also uses `/api/command` for operator actions, not just read-only queries.

Examples:

- `advert`
- `reboot`
- `start ota`
- `time <epoch>`
- `time.force <epoch>`

These are powerful commands. Treat them the same way you would treat direct serial CLI access.

Example:

```bash
curl -sk https://<repeater-ip>/api/command \
  -H "X-Auth-Token: $TOKEN" \
  --data 'advert'
```

### 5. Remote Configuration Helpers

The web panel saves settings by generating CLI commands and sending them through `/api/command`.

That means your own tools can do the same for EastMesh-specific settings such as:

- repeater identity fields
- owner info
- MQTT broker toggles
- EastMesh MQTT owner metadata
- radio settings supported by the repeater CLI

This is a practical way to automate setup while preserving existing CLI semantics.

## Example Script

This shell example logs in, fetches summary stats, fetches one trend series, and runs a CLI command:

```bash
#!/usr/bin/env bash
set -euo pipefail

BASE_URL="https://192.168.1.123"
PASSWORD="your-admin-password"

TOKEN=$(curl -sk -X POST "$BASE_URL/login" --data "$PASSWORD")

echo "Summary:"
curl -sk "$BASE_URL/api/stats" \
  -H "X-Auth-Token: $TOKEN"

echo
echo "Memory trend:"
curl -sk "$BASE_URL/api/stats?series=memory" \
  -H "X-Auth-Token: $TOKEN"

echo
echo "MQTT status:"
curl -sk "$BASE_URL/api/command" \
  -H "X-Auth-Token: $TOKEN" \
  --data 'get mqtt.status'
```

## Error Cases

Common responses:

- `401 Unauthorized`: missing or expired token
- `503 Service Unavailable`: stats are disabled
- `404 No stats data`: requested stats payload could not be built
- `400 Bad request`: malformed login or command request body

If stats requests fail:

1. confirm the web panel is enabled
2. confirm `web.stats` is enabled
3. confirm the session token is still valid
4. reduce polling frequency if the board is under memory pressure

## Practical Recommendations

- prefer `/api/command` when you need exact CLI parity
- prefer `/api/stats` for dashboards and trend views
- keep polling conservative, especially on repeaters with two active MQTT connections
- if you are finished with troubleshooting, consider disabling the web panel with `set web off` to maximize heap headroom on constrained boards
