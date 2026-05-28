# Nemo Firmware README Draft

Intended target path: `Nemo/README.md`

---

# Nemo Firmware

Nemo is the ESP32-C6 Matter firmware runtime for the Nemo & Anna platform.
It takes Anna JSON based action definitions and turns them into a running Matter device with endpoint setup, runtime validation, device state handling, and commissioning support.

This README is for developers working on the firmware repository itself.
Platform concepts and JSON semantics are documented in the top-level Nemo & Anna README and the Anna specification documents.

## What This Repository Contains

- ESP32-C6 firmware runtime for action-based Matter devices
- Anna JSON parsing and runtime apply logic
- Matter endpoint, cluster, and label mapping
- Runtime state storage and restore behavior
- Factory reset and power-cycle recovery
- Cloud config sync during Matter commissioning, including delayed `ConnectNetworkResponse` release until sync reaches a terminal result
- Runtime config rebuild without reboot, with endpoint ID reuse for topology-changing updates

## Current Scope

Current reference profile:

- Target chip: `esp32c6`
- Reference board: `ESP32-C6 DevKitC-1`
- Firmware stack: `ESP-IDF v5.4.1` + `anna-soft/esp-matter` branch `anna/cloud-sync-chip-pin` (derived from `esp-matter release/v1.4`)
- Input model: Anna JSON payloads with `meta` + `data`
- Action families: `Button`, `Switch`, `ConButton`, `ConSwitch`, `Modes`

Current implementation limits:

- Up to 5 `Button` / `ConButton` instances
- Up to 3 `Switch` / `ConSwitch` instances
- Up to 6 total action instances per device
- One `Modes` set per device

This repository is a development/reference firmware runtime.
Manufacturing credentials, production provisioning flows, and release certification asset management are intentionally out of scope for the public firmware tree.

## Release Status

The current validated firmware baseline uses `anna-soft/esp-matter` branch `anna/cloud-sync-chip-pin`.
That branch points its `connectedhomeip` submodule at `anna-soft/connectedhomeip` and pins the matching cloud sync commit required for the commissioning-time `ConnectNetworkResponse` delay and fail-open release path.
The current firmware configuration intentionally keeps development-oriented Matter settings such as example DAC credentials and CHIP shell enablement.

Cloud config sync is now functional during Matter commissioning sessions.
On the reference `esp32c6` profile, `ConnectNetworkResponse(Success)` is held until Anna cloud sync reaches a terminal result, with a fail-open release path if the wait cannot be sustained.
This behavior is controlled by `CONFIG_ANNA_CLOUD_SYNC_CONNECT_RESPONSE_WAIT` and is enabled in `sdkconfig.defaults.esp32c6`.

Runtime config rebuild now preserves existing endpoint IDs where possible during topology-changing Anna JSON updates.
When an old endpoint cannot be reused, Nemo tracks it briefly as a retired endpoint for metadata transition handling.
This is intended to reduce instability during live topology changes without preserving execution behavior for removed actions.

Official `esp-matter release/v1.4` remains the upstream lineage, but it is not the supported setup baseline for this repository.
Release hardening is still planned.
The remaining direction is to stay close to the `release/v1.4` lineage while replacing the current development credential/debug profile with a release-safe configuration.

## Repository Layout

- `main/`: application entrypoints, Matter integration, commissioning/data providers, cloud sync, runtime rebuild, factory reset
- `components/`: Anna config parsing, storage, runtime helpers, driver support modules

## Required Root Files

If this firmware is published as `Nemo/` inside a larger repository, the `Nemo/` directory still needs its ESP-IDF project root files.

Keep these files at the top level of `Nemo/`:

- `CMakeLists.txt`
- `idf_component.yml`
- `dependencies.lock`
- `partitions.csv`
- `sdkconfig.defaults`
- `sdkconfig.defaults.esp32c6`

`main/` and `components/` contain the firmware sources, but they are not enough on their own.
Without the root build files above, `idf.py build` cannot start.
`sdkconfig` itself is generated locally and does not need to be published as a tracked file.

## Environment Setup

The firmware expects a local `esp-matter` checkout referenced by `ESP_MATTER_PATH`; it does not use a registry-only dependency flow.
Clone the repositories and check out the validated baseline:

```bash
git clone -b v5.4.1 --recursive https://github.com/espressif/esp-idf.git

git clone --recursive https://github.com/anna-soft/esp-matter.git
cd esp-matter
git checkout anna/cloud-sync-chip-pin
git submodule update --init --recursive
```

This fork is derived from official `esp-matter release/v1.4` and already points its `connectedhomeip` submodule to `anna-soft/connectedhomeip` with the pinned cloud sync commit required by this firmware.

After cloning, set up the build environment in your shell:

- Run `./install.sh esp32c6` inside the `esp-idf` directory
- Source both `esp-idf/export.sh` and `esp-matter/export.sh`
- Export `ESP_MATTER_PATH` to point to your `esp-matter` checkout
- Export `IDF_TARGET=esp32c6`

`CMakeLists.txt` reads `ESP_MATTER_PATH` directly.
If `ESP_MATTER_DEVICE_PATH` is not exported explicitly, the project derives the device HAL path from `IDF_TARGET`.

## Configuration Model

Nemo consumes Anna JSON based configuration payloads.
At a high level:

- `meta` carries transport and integrity metadata
- `data.UnitInfo` carries per-device identity and cloud sync bootstrap fields such as `UniqueID` and `CloudSync.DeviceToken`
- `data.ProductInfo` carries product-level identity and user-visible properties
- action sections define runtime behavior, boundaries, and mode constraints

For commissioning-time cloud sync, Nemo reads bootstrap identity from the stored unit data before issuing device-scoped pull and ack requests.

For detailed JSON schema, field semantics, and runtime policy, see the two documents under `../Anna/`:

- `../Anna/Anna JSON Specification v1.2.docx`
- `../Anna/Anna JSON Operational Policy v1.0.docx`

## Related Documents

For platform-level context:

- `../README.md`: Nemo & Anna platform overview
- `../Anna/Anna JSON Specification v1.2.docx`: public action schema and field semantics
- `../Anna/Anna JSON Operational Policy v1.0.docx`: runtime behavior, provisioning, and deployment policy

## Summary

Anna defines the action model.
Nemo executes it as ESP32-C6 Matter firmware.

This README should stay focused on the firmware repository: what it contains, how to build it, what input it consumes, and where to read the higher-level platform documents.
