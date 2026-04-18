# AGENTS.md

## Purpose

EastMesh layer on top of upstream MeshCore.

**Default:** preserve upstream behavior.  
Only modify code for clearly scoped EastMesh features:

- `*_repeater_mqtt`
- `*_companion_radio_wifi`
- MQTT uplink/broker
- repeater web panel
- docs, releases, automation

## Principles

- Minimal, targeted changes
- Prefer additive over modifying upstream code
- Avoid unrelated refactors
- Maintain parity with upstream behavior

## Guardrails

### Upstream

- Do not modify unrelated MeshCore logic
- Do not change CLI semantics unless explicitly required
- Do not introduce breaking changes to existing targets

### Docs (update in same PR when practical)

| Change                | File                 |
| --------------------- | -------------------- |
| CLI / allowlist       | `docs/custom-cli.md` |
| Web panel UI/behavior | `docs/web-panel.md`  |
| Releases              | `release-notes.yml`  |
| Flashing guidance     | `docs/releases.md`   |

### Web Panel Gate

If editing `examples/simple_repeater/MyMesh.cpp`, also update `docs/custom-cli.md`.

## Tooling

- Use `uv` + PlatformIO via `uv run`
- Do not assume global `pio`

### Build Policy

Builds are expensive. Avoid unless necessary.

Do NOT build for:

- docs / HTML / CSS only

Prefer:

- user-run local builds
- reasoning over execution

Build only if:

- high-risk change
- firmware behavior must be verified

### Commands

```bash
uv sync
uv run pio run -e <env>
uv run pio device monitor --port <port> --baud 115200
uv run --group docs zensical serve
uv run --group docs zensical build
```

Do not assume `pio` is installed globally.

## Common Commands

List build targets:

```bash
bash build.sh list
```

Build a single target:

```bash
uv run pio run -e heltec_v4_repeater_mqtt
uv run pio run -e T_Beam_S3_Supreme_SX1262_repeater_mqtt
uv run pio run -e heltec_v4_companion_radio_wifi
uv run pio run -e T_Beam_S3_Supreme_SX1262_companion_radio_wifi
```

Build with release-style metadata:

```bash
export FIRMWARE_VERSION=v1.14.1
export EASTMESH_VERSION=v1.0.1
bash build.sh build-firmware heltec_v4_repeater_mqtt
bash build.sh build-firmware T_Beam_S3_Supreme_SX1262_repeater_mqtt
```

Flash a target:

```bash
uv run pio run -e heltec_v4_repeater_mqtt -t upload --upload-port /dev/tty.usbmodemXXXX
uv run pio run -e T_Beam_S3_Supreme_SX1262_repeater_mqtt -t upload --upload-port /dev/tty.usbmodemXXXX
```

## Key Files

- `build.sh` — build wrapper
- `platformio.ini`
- `variants/eastmesh_mqtt/platformio.ini`
- `examples/simple_repeater/MyMesh.cpp`
- `src/helpers/mqtt/MQTTUplink.cpp`
- `docs/*.md`
- `RELEASE.md`
- `release-notes.yml`

## Docs Sync Requirements

If you change any of the following, update docs in the same PR when practical:

- Web-panel allowlisted commands:
  - update `docs/custom-cli.md`
- Web-panel user-facing behavior, sections, controls, or troubleshooting:
  - update `docs/web-panel.md`
- EastMesh CLI additions or changed semantics:
  - update `docs/custom-cli.md`
- Release/tag preparation:
  - update `release-notes.yml`
- Flashing/release asset guidance:
  - update `docs/releases.md`

## Repeater MQTT Notes

`*_repeater_mqtt` builds may include the local HTTPS web panel on supported ESP32 targets.

Operational guidance already reflected in docs:

- use for initial setup and troubleshooting
- prefer `set web off` afterward for maximum heap headroom

## Companion WiFi Notes

`*_companion_radio_wifi` targets support persisted Wi-Fi rescue commands via serial `CLI Rescue`.

Do not document companion rescue commands in repeater docs. Do not assume web-panel behavior applies.

Companion release/version rule:

- companion tags use the official upstream MeshCore release version only
- the current official MeshCore version is `v1.14.1`
- companion releases are only cut when `meshcore-dev/MeshCore` has made an official release
- do not invent separate EastMesh companion version numbers

## Release Workflow

Current tag formats:

```bash
git tag companion-wifi-v1.2.3
git tag repeater-mqtt-eastmesh-v1.0.1
```

Rules:

- `companion-wifi` tags use the upstream MeshCore version directly
- `repeater-mqtt` tags use the EastMesh release version in the tag
- GitHub Actions variable `OFFICIAL_MESHCORE_VERSION` supplies the upstream base version for repeater MQTT release builds
- if the upstream MeshCore release version changes, update `OFFICIAL_MESHCORE_VERSION` in GitHub before cutting release tags

Typical release flow:

1. Update `OFFICIAL_MESHCORE_VERSION` if upstream changed.
2. Update `release-notes.yml` on `develop`.
3. Merge the release PR from `develop` to `main`.
4. Create the desired release tag or tags on the target commit on `main`.
5. Push the tags.

## Upstream Sync Workflow

When asked to pull from upstream MeshCore:

- pull from `meshcore-dev/MeshCore:dev`
- start from local `develop`
- create a temporary integration branch off `develop`
- merge upstream `dev` into that temporary integration branch
- resolve conflicts in a way that preserves EastMesh-specific changes
- merge the finished integration branch back into `develop`

Do not merge upstream directly into `main`.

## Scope Boundaries

Do NOT (unless asked):

- rename tracks
- change tag formats
- expand allowlist without updating docs
- change upstream CLI semantics
- introduce new versioning schemes

## Commit Messages

Use concise, conventional prefixes:

- `feat:` new functionality
- `fix:` bug fixes
- `docs:` documentation changes
- `chore:` maintenance, tooling, non-functional
- `refactor:` code changes without behavior change

Keep messages short and scoped.

## Decision Rule

If a change is not clearly EastMesh-specific, do not modify the code.
When uncertain, prefer no change or request clarification.
