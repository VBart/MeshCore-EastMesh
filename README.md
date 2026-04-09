# MeshCore-EastMesh

MeshCore-EastMesh is MeshCore with an EastMesh layer on top, focused on providing an out-of-the-box experience for EastMesh users.

The firmware remains MeshCore. This repository adds EastMesh-specific packaging, release automation, versioning, and integrations such as native WiFi and MQTT support, while staying aligned with upstream MeshCore releases.

## Coming Soon

Documentation for flashing, configuration, supported hardware, and EastMesh-specific features is still being prepared.

For now, this repository is primarily used for:

- EastMesh firmware builds and release automation
- EastMesh versioning on top of upstream MeshCore releases
- ongoing integration of native WiFi and MQTT support

## Development

Python tooling in this repo is managed with [`uv`](https://docs.astral.sh/uv/).

```bash
uv sync
```

See [RELEASE.md](./RELEASE.md) for the current release process and tag format.
