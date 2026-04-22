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
- Cloud config sync during Matter commissioning
- Runtime config rebuild without reboot

## Current Scope

Current reference profile:

- Target chip: `esp32c6`
- Reference board: `ESP32-C6 DevKitC-1`
- Firmware stack: `ESP-IDF v5.4.1` + `esp-matter release/v1.4`
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

The current firmware baseline is aligned to the official `esp-matter release/v1.4` branch, but it is still not a production release profile yet.
The current firmware configuration intentionally keeps development-oriented Matter settings such as example DAC credentials and CHIP shell enablement.

Cloud config sync is now functional during Matter commissioning sessions.

Release hardening is still planned.
The remaining direction is to keep the `release/v1.4` baseline while replacing the current development credential/debug profile with a release-safe configuration.

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

git clone --recursive https://github.com/espressif/esp-matter.git
```

Inside the cloned esp-matter directory
```bash
git checkout release/v1.4
git submodule update --init --recursive
```

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
- `data.UnitInfo` carries per-device identity
- `data.ProductInfo` carries product-level identity and user-visible properties
- action sections define runtime behavior, boundaries, and mode constraints

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
