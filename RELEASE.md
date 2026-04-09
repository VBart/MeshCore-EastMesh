# Releasing Firmware

GitHub Actions is set up to automatically build and draft firmware releases.

## Required GitHub Variable

Set the repository Actions variable `OFFICIAL_MESHCORE_VERSION` to the current upstream MeshCore release version.

Example:

- `OFFICIAL_MESHCORE_VERSION=v1.2.3`

This value becomes the base `FIRMWARE_VERSION` used by the release workflows.

## EastMesh Release Tags

Push one or more of the following tag formats to trigger the matching firmware release workflow:

- `companion-eastmesh-v1.0.1`
- `repeater-eastmesh-v1.0.1`
- `room-server-eastmesh-v1.0.1`

Replace `v1.0.1` with the EastMesh release version you want to publish.

Each tag triggers a separate workflow:

- `companion-eastmesh-*` builds companion firmware
- `repeater-eastmesh-*` builds repeater firmware
- `room-server-eastmesh-*` builds room server firmware

You can push one, or more tags on the same commit, and they will all build separately.

## Resulting Firmware Version

During the GitHub Actions build:

- `FIRMWARE_VERSION` comes from `OFFICIAL_MESHCORE_VERSION`
- `EASTMESH_VERSION` comes from the tag

The final firmware/artifact version string becomes:

- `v1.2.3-eastmesh-v1.0.1-<commit>`

Example:

- tag: `room-server-eastmesh-v1.0.1`
- repo variable: `OFFICIAL_MESHCORE_VERSION=v1.2.3`
- resulting build version: `v1.2.3-eastmesh-v1.0.1-abcdef`

## Typical Release Flow

1. Update the `OFFICIAL_MESHCORE_VERSION` GitHub Actions variable if upstream has changed.
2. Create the release tags you want on the target commit.
3. Push the tags.
4. Wait for the workflows to finish building.
5. Review the draft GitHub Releases.
6. Update the release notes and publish them.

Example:

```bash
git tag companion-eastmesh-v1.0.1
git tag repeater-eastmesh-v1.0.1
git tag room-server-eastmesh-v1.0.1
git push origin companion-eastmesh-v1.0.1 repeater-eastmesh-v1.0.1 room-server-eastmesh-v1.0.1
```

## Legacy Tags

The workflows still accept older tag formats for compatibility:

- `companion-v1.2.3`
- `repeater-v1.2.3`
- `room-server-v1.2.3`
- `room-server-v1.2.3-eastmesh-v1.0.1`

However, the recommended EastMesh release format is:

- `companion-eastmesh-v1.0.1`
- `repeater-eastmesh-v1.0.1`
- `room-server-eastmesh-v1.0.1`
