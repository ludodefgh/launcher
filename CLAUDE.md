# CLAUDE.md

Context for any Claude Code session (new or resumed) working on this repo.

## What this is

`launcher`: a multi-program launcher/bootloader for ESP32 (S3, C3, classic
WROOM). Flashes 3-4 independent firmwares into fixed OTA partitions and lets
the user pick which one boots, via a SPI TFT (ST7789 or GC9A01) + a nav
input (EC11 rotary encoder or 3 push buttons) menu, without reflashing. No SD card / dynamic loading in this iteration — see README.md
"Out of scope". Independent of the `ASCII-Aquarium` repo (a guest app that
consumes this launcher, not the other way around). Optional network features
(OTA download, remote control over HTTP/BLE, version check) are implemented
and off by default — see README.md "Network features".

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
  Kconfig) must stay chip-agnostic.** Only the panel/nav driver
  implementations (`main/display_{st7789,gc9a01}.c`,
  `main/nav_input_{ec11,buttons}.c` — GPIO numbers / panel init, via
  Kconfig) and the partition CSV choice are allowed to be board-specific. CI builds `esp32s3`, `esp32c3`,
  `esp32` on every push specifically to catch accidental non-portability
  (e.g. a second-core-pinned FreeRTOS task, an implicit BLE dependency).
- **License: MIT.** Keep `LICENSE` and any new file headers consistent
  with that.

## ESP-IDF build-system gotchas hit while implementing this (don't re-discover them)

- **`REQUIRES`/`PRIV_REQUIRES` in `main/CMakeLists.txt` cannot reliably be
  made conditional on `CONFIG_*` Kconfig values** -- verified empirically
  (a `CONFIG_LAUNCHER_NET_WIFI_ENABLE`-gated `REQUIRES esp_wifi` silently
  had no effect while the equally-gated `SRCS` entry worked fine; ESP-IDF
  resolves requirements in an earlier pass, before Kconfig values are
  necessarily available). Fix in place: `main/CMakeLists.txt` declares
  every potentially-needed component unconditionally in `REQUIRES`, and
  keeps only `SRCS` conditional -- that's what actually controls binary
  size (unused code is dropped at link time via `-ffunction-sections`/
  `-fdata-sections`, and these components are already part of ESP-IDF's
  default build set for this target regardless, see any build's
  `-- Components: ...` log line). If you add a new net_*.c file, follow
  the same pattern.
- **Kconfig's `select`/`imply` has no effect on a `choice` block's member
  symbols** (Kconfig hard limitation, not a bug) -- e.g. can't force
  `BT_HOST`'s choice to `BT_NIMBLE_ENABLED` from `main/Kconfig.projbuild`.
  Where this matters (BLE remote transport), the Kconfig prompt's help text
  documents the manual `menuconfig` step instead of silently failing to
  work -- see README.md "Remote control" for the exact steps.
- **A stale `build/` directory silently keeps old component-requirements
  resolution even after `idf.py set-target` + editing `sdkconfig` +
  `idf.py build`, and even after `idf.py reconfigure`** -- always
  `rm -rf build` (not just `sdkconfig`) before testing a new Kconfig
  combination against the real IDF image, or you'll get misleading
  "header not found" errors that look like a real bug but aren't.
- **`idf.py build`'s partition-size overflow check is a warning, not a
  build failure** -- see the safety note at the top of `partitions.csv`.

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

# Same, but testing a specific network-feature Kconfig combination (always
# rm -rf build fresh first, see gotchas above)
docker run --rm -v "$PWD":/workspaces/launcher -w /workspaces/launcher espressif/idf:release-v5.5 bash -c '
  . $IDF_PATH/export.sh
  rm -rf build sdkconfig
  idf.py set-target esp32s3
  cat >> sdkconfig << "EOF"
CONFIG_LAUNCHER_NET_OTA_ENABLE=y
CONFIG_LAUNCHER_NET_OTA_URL_BASE="http://example.local/launcher"
EOF
  idf.py build
'
```

Keep this section up to date as the project's tooling evolves — it's meant
to be trusted at face value by a fresh session, not re-derived from scratch
every time.
