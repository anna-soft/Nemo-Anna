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

## Current Scope

Current reference profile:

- Target chip: `esp32c6`
- Reference board: `ESP32-C6 DevKitC-1`
- Firmware stack: `ESP-IDF v5.4.1` + pinned `esp-matter` commit `78bc8e21c932a5014a08ecdd8bfa5535a771c107`
- `connectedhomeip` snapshot under that tree: `326cabf99c0cd2d8e9099ea0a5cc849c8e28dda7`
- Input model: Anna JSON payloads with `meta` + `data`
- Action families: `Button`, `Switch`, `ConButton`, `ConSwitch`, `Modes`

Current implementation limits:

- Up to 5 `Button` / `ConButton` instances
- Up to 3 `Switch` / `ConSwitch` instances
- Up to 6 total action instances per device
- One `Modes` set per device

This repository is a development/reference firmware runtime.
Release certification assets, manufacturing credentials, and production provisioning flows are intentionally out of scope for the public firmware tree.

## Release Status

The current public snapshot is not a production release profile yet.
It is pinned to a validated development-time `esp-matter` commit rather than an official release branch, and the current firmware configuration still includes development-oriented Matter settings such as example DAC credentials and CHIP shell enablement.

Release alignment is planned.
The intended direction is to move this firmware onto an official release baseline and replace the current development credential/debug profile with a release-safe configuration.

## Repository Layout

- `main/`: application entrypoints, Matter integration, commissioning/data providers
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

Use a Linux shell and run the commands below in order.
The firmware expects a local `esp-matter` checkout referenced by `ESP_MATTER_PATH`; it does not use a registry-only dependency flow.
For reproducible results, use `ESP-IDF v5.4.1` and the validated `esp-matter` commit `78bc8e21c932a5014a08ecdd8bfa5535a771c107`.

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.4.1 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32c6

cd ~/esp
git clone --recursive https://github.com/espressif/esp-matter.git
cd ~/esp/esp-matter
git checkout 78bc8e21c932a5014a08ecdd8bfa5535a771c107
git submodule update --init --recursive

export IDF_PATH=~/esp/esp-idf
. "$IDF_PATH/export.sh"

export ESP_MATTER_PATH=~/esp/esp-matter
. "$ESP_MATTER_PATH/export.sh"

export IDF_TARGET=esp32c6
```

After the commands above complete, the local shell is ready for Nemo firmware work.
`CMakeLists.txt` reads `ESP_MATTER_PATH` directly and derives the device HAL path from `IDF_TARGET`.

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
