# MeshCore-EastMesh

EastMesh firmware builds for MeshCore, with MQTT repeaters, WiFi companions, and simple release downloads for supported boards.

This repository keeps the upstream MeshCore firmware intact and adds an EastMesh layer on top for packaging, release automation, WiFi-enabled companion builds, and MQTT-enabled repeater builds.

## What This Repo Adds

- `*_repeater_mqtt` firmware targets with:
  - native WiFi
  - MQTT over WSS with JWT auth
  - optional local HTTPS config panel on supported ESP32 targets
- `*_companion_radio_wifi` firmware targets for WiFi-connected companion devices
- EastMesh-specific release workflows and versioning on top of upstream MeshCore releases
- docs and release guidance for EastMesh users instead of the full upstream MeshCore docs set

## Releases

Prebuilt firmware is published on GitHub Releases:

- <https://github.com/xJARiD/MeshCore-EastMesh/releases>

For flashing guidance, including when to use `.bin` vs `-merged.bin`, see:

- [docs/releases.md](./docs/releases.md)

The custom firmware flasher site is:

- <https://flasher.meshcore.io/>

## Install uv

This repo uses `uv` for Python tooling and runs PlatformIO through `uv run`.

Official install docs:

- <https://docs.astral.sh/uv/getting-started/installation/>

Common install methods:

macOS and Linux:

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
```

Homebrew:

```bash
brew install uv
```

Windows PowerShell:

```powershell
powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"
```

## Common Repo Usage

Install tooling:

```bash
uv sync
```

List available build targets:

```bash
bash build.sh list
```

Build a single target:

```bash
uv run pio run -e heltec_v4_repeater_mqtt
uv run pio run -e heltec_v4_companion_radio_wifi
```

Build with release-style version metadata:

```bash
export FIRMWARE_VERSION=v1.14.1
export EASTMESH_VERSION=v1.0.1
bash build.sh build-firmware heltec_v4_repeater_mqtt
```

Flash a target:

```bash
uv run pio run -e heltec_v4_repeater_mqtt -t upload --upload-port /dev/tty.usbmodemXXXX
```

Open a serial monitor:

```bash
uv run pio device monitor --port /dev/tty.usbmodemXXXX --baud 115200
```

Build docs locally:

```bash
uv run --group docs mkdocs serve
uv run --group docs mkdocs build
```

## Key Files

- [`build.sh`](./build.sh)
  - main local build wrapper
  - injects `FIRMWARE_VERSION`, `CLIENT_VERSION`, and EastMesh release metadata
- [`pyproject.toml`](./pyproject.toml)
  - Python tooling and docs dependencies
- [`platformio.ini`](./platformio.ini)
  - root PlatformIO config and ESP32 helper scripts
- [`variants/eastmesh_mqtt/platformio.ini`](./variants/eastmesh_mqtt/platformio.ini)
  - shared EastMesh MQTT repeater env definitions
- [`examples/simple_repeater/MyMesh.cpp`](./examples/simple_repeater/MyMesh.cpp)
  - repeater CLI wiring, MQTT command surface, and web allowlist integration
- [`src/helpers/mqtt/MQTTUplink.cpp`](./src/helpers/mqtt/MQTTUplink.cpp)
  - MQTT uplink implementation, HTTPS web panel, WSS/JWT handling, and repeater WiFi control
- [`examples/companion_radio`](./examples/companion_radio)
  - companion firmware implementation
- [`RELEASE.md`](./RELEASE.md)
  - tag formats and release workflow behavior
- [`docs/`](./docs)
  - EastMesh-focused docs published to GitHub Pages

## Key EastMesh Features

### MQTT Repeater Additions

- hardcoded broker support for:
  - `eastmesh-au`
  - `letsmesh-eu`
  - `letsmesh-us`
- WSS transport at `/mqtt`
- JWT auth using the device identity
- CLI controls for:
  - WiFi credentials
  - WiFi powersaving
  - MQTT endpoint enablement
  - MQTT packet and raw publishing
  - owner public key and email
  - local web panel enablement

### Local Web Panel

On supported `*_repeater_mqtt` ESP32 targets, the repeater can expose a local HTTPS config panel over WiFi.

Features include:

- password-gated access using the existing repeater admin password
- allowlisted CLI execution
- grouped quick actions
- light and dark themes
- optional disable via `set web off`

Some constrained targets disable the panel to stay within flash limits. For example, `Xiao_C3_repeater_mqtt` builds with `WITH_WEB_PANEL=0`.

### Companion WiFi Additions

`*_companion_radio_wifi` targets now support persisted WiFi rescue commands:

- `get wifi.status`
- `get wifi.ssid`
- `get wifi.powersaving`
- `set wifi.ssid <ssid>`
- `set wifi.pwd <password>`
- `set wifi.powersaving none|min|max`

## Active GitHub Workflows

- `.github/workflows/build-companion-wifi-firmwares.yml`
- `.github/workflows/build-repeater-mqtt-firmwares.yml`
- `.github/workflows/pr-build-check.yml`
- `.github/workflows/push-build-check.yml`
- `.github/workflows/github-pages.yml`

The current release workflows intentionally focus only on:

- `companion-wifi`
- `repeater-mqtt`

## Release Tags

Current release tags are:

```bash
git tag companion-wifi-v1.14.1
git tag repeater-mqtt-eastmesh-v1.0.1
```

Companion WiFi uses the upstream MeshCore version in the tag.

Repeater MQTT uses:

- `OFFICIAL_MESHCORE_VERSION` from GitHub Actions variables as `FIRMWARE_VERSION`
- the EastMesh tag version as `EASTMESH_VERSION`

See [RELEASE.md](./RELEASE.md) for the full release flow.

## Documentation

Published docs site:

- <https://xjarid.github.io/MeshCore-EastMesh/>

Current docs pages:

- [Home](./docs/index.md)
- [Download and Flash Releases](./docs/releases.md)
- [Build Locally With uv](./docs/local-builds.md)
- [Custom CLI Commands](./docs/custom-cli.md)
