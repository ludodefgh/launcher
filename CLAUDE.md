# CLAUDE.md

Context for any Claude Code session (new or resumed) working on this repo.

## What this is

`launcher`: a multi-program launcher/bootloader for ESP32 (S3, C3, classic
WROOM). Flashes 3-4 independent firmwares into fixed OTA partitions and lets
the user pick which one boots, via a TFT + rotary-encoder menu, without
reflashing. No SD card / dynamic loading in this iteration — see README.md
"Out of scope". Independent of the `ASCII-Aquarium` repo (a guest app that
consumes this launcher, not the other way around).

## Model usage convention for this project

- **Architecture/design work** (structural choices, HAL/Kconfig tradeoffs,
  anything with lasting maintainability impact) → Claude Opus.
- **Implementation** (writing code once design decisions are made) → Claude
  Sonnet, generally via dedicated implementation agents rather than the
  reasoning model.
- If an implementation task surfaces a design question the spec doesn't
  settle, raise it rather than deciding silently in Sonnet.

Full spec: the original spec document this repo was built from (in French)
covers the reasoning behind every section below in more detail; ask the
user if you need it and don't have it in context. Deviations actually taken
during implementation are recorded in README.md → "Design decisions that
deviate from the original spec" (worth reading before changing those areas
again).

## Constraints that must not be silently broken

- **`partitions.csv` (or `partitions_4mb.csv`) must stay byte-identical
  between the launcher and every guest app.** This is the #1 way to brick a
  slot. See README.md for why offsets are left blank for app partitions.
- **Any networking feature stays behind its own Kconfig define
  (`CONFIG_LAUNCHER_NET_*`) and must never block local boot** if
  disabled/unavailable — WiFi failures degrade to "network features
  inactive this cycle," never a hang or crash. See `main/Kconfig.projbuild`.
- **The launcher core (`boot_logic.c`, `nvs_state.c`, `nav_input.h` HAL,
  Kconfig) must stay chip-agnostic.** Only pin mapping (`main/display_st7789.c`,
  `main/nav_input_ec11.c` GPIO numbers, via Kconfig) and the partition CSV
  choice are allowed to be board-specific. CI builds `esp32s3`, `esp32c3`,
  `esp32` on every push specifically to catch accidental non-portability
  (e.g. a second-core-pinned FreeRTOS task, an implicit BLE dependency).
- **License: MIT.** Keep `LICENSE` and any new file headers consistent
  with that.

## Issue tracking

Deferred/"later" items (spec sections marked optional/v1.1, implementation
details discovered along the way, multi-chip limitations needing follow-up)
are tracked as GitHub issues in `ludodefgh/launcher`, not as inline TODOs.
Open a new issue as soon as something is identified as "not now" — don't
rely on conversation memory across sessions. Reference the relevant
README/spec section in the issue body when applicable.

## Useful commands

```
# Build (pick a target)
idf.py set-target esp32s3   # or esp32c3, esp32
idf.py build
idf.py menuconfig           # "Bootloader Launcher Configuration"
idf.py -p PORT flash monitor    # flash from the native host, not the devcontainer (see README)

# Host-based unit tests for the pure boot-decision logic (no ESP-IDF needed)
gcc -std=c11 -Wall -Wextra -I main main/boot_logic.c test/test_boot_logic.c -o /tmp/test_boot_logic && /tmp/test_boot_logic

# Verify a build against the pinned ESP-IDF image without installing the toolchain locally
docker run --rm -v "$PWD":/workspaces/launcher -w /workspaces/launcher espressif/idf:release-v5.5 \
  bash -c '. $IDF_PATH/export.sh && idf.py set-target esp32c3 && idf.py build'
```

Keep this section up to date as the project's tooling evolves — it's meant
to be trusted at face value by a fresh session, not re-derived from scratch
every time.
