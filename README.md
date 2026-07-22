# launcher

A multi-program launcher/bootloader for ESP32 (S3, C3, classic WROOM, ...).
Flash 3-4 independent firmware images into fixed partitions and pick which
one runs at boot, without reflashing — no SD card, no dynamic loading (see
"Out of scope" below). Built around an ESP32-S3 + 2.4" ST7789 TFT + EC11
rotary encoder, but the core (boot logic, HAL, Kconfig) is chip-agnostic.

## How it works

- A `factory` partition holds the launcher itself; it's always the first
  thing that boots.
- N `app` partitions (`app_slot1`, `app_slot2`, ...) hold independent guest
  firmwares, each with its own OTA subtype (`ota_0`, `ota_1`, ...).
- On boot, the launcher checks NVS for a remembered "last app". If one
  exists and the encoder button isn't held, it boots straight into it
  (`esp_ota_set_boot_partition` + `esp_restart`). Otherwise (first boot,
  forced menu, or button held) it shows a selection menu on the TFT.
- A guest app can hand control back to the menu by calling
  `launcher_request_menu_on_next_boot()` from `components/launcher_client`
  (e.g. on a long button press) and rebooting.

This is the same standard ESP-IDF OTA-partition mechanism used by 2-slot
OTA updates, just with N *different programs* instead of 2 versions of the
same program — see `main/boot_into.c` / `main/boot_logic.c`.

## Critical constraint: partitions.csv must match

**Every guest app must be built against the exact same `partitions.csv` (or
`partitions_4mb.csv`) as the launcher.** The launcher writes a binary to a
partition by label/offset; if a guest app's own build used a different
partition table, its assumptions about its own flash layout (and the
addresses of other partitions) won't match reality. Copy the launcher's
partition CSV into the guest app project rather than hand-editing a
separate copy.

Guest apps only need two more things from this repo:
1. Include `components/launcher_client` and call
   `launcher_request_menu_on_next_boot()` to return to the menu.
2. A "reasonably recent" ESP-IDF (5.5+ recommended) — no need for an exact
   version match, the app image format (`esp_app_desc_t` etc.) is stable
   across recent releases. Guest apps do **not** need to use this repo's
   devcontainer or even ESP-IDF itself (PlatformIO is fine).

## Repo layout

```
main/                   launcher firmware (see file headers for each module's role)
components/launcher_client/   tiny library for guest apps ("return to menu")
partitions.csv          8MB flash layout (S3 dev boards), 3 app slots
partitions_4mb.csv       4MB flash layout (C3 / classic WROOM), 2 app slots
test/test_boot_logic.c  host-based unit tests for the pure boot-decision logic
.devcontainer/          build-only ESP-IDF 5.5 container, see below
.github/workflows/      CI: build matrix (esp32/esp32s3/esp32c3) + host tests
```

## Building

```
idf.py set-target esp32s3   # or esp32c3, esp32
idf.py build
idf.py menuconfig           # under "Bootloader Launcher Configuration"
idf.py -p PORT flash monitor
```

`sdkconfig.defaults` targets the 8MB S3 board by default;
`sdkconfig.defaults.esp32c3` / `sdkconfig.defaults.esp32` automatically
layer on top of it (standard ESP-IDF per-target default mechanism) to
switch to `partitions_4mb.csv` and a 4MB flash size.

### Host-based unit tests

`main/boot_logic.c` (the menu-vs-direct-boot decision, slot-count check,
image-magic check) has zero ESP-IDF/FreeRTOS dependency on purpose, so it
builds and runs with a plain host compiler instead of needing hardware or
the ESP-IDF toolchain:

```
gcc -std=c11 -Wall -Wextra -I main main/boot_logic.c test/test_boot_logic.c -o /tmp/test_boot_logic
/tmp/test_boot_logic
```

## Dev container

`.devcontainer/` pins `espressif/idf:release-v5.5` for **building only**
(`idf.py build`, `menuconfig`, the host-based unit tests). It deliberately
does not flash: reliable USB passthrough into a container isn't guaranteed
in every hosting setup, and flashing needs direct serial/USB access, so
flashing is done from the native host environment instead. This is a dev
convenience for *this* repo only — see "Critical constraint" above, guest
apps have no obligation to use it.

## Multi-chip support

The launcher's core (boot logic, Kconfig, nav HAL) is chip-agnostic; CI
builds it for `esp32s3`, `esp32c3`, and `esp32` (classic WROOM) on every
push. What does vary per board/chip:
- **Flash size / partition table**: use `partitions.csv` (8MB, 3 slots) or
  `partitions_4mb.csv` (4MB, 2 slots) depending on your module.
- **Screen/encoder pin mapping**: set via `idf.py menuconfig` →
  "Bootloader Launcher Configuration" (`CONFIG_LAUNCHER_DISPLAY_GPIO_*`,
  `CONFIG_LAUNCHER_EC11_GPIO_*`) — always board-specific, not a portability
  issue in itself.
- **BLE remote control on ESP32-S2**: the S2 has no Bluetooth. The BLE
  transport choice for `LAUNCHER_NET_REMOTE_CONTROL_ENABLE` is gated behind
  `depends on SOC_BLE_SUPPORTED` in Kconfig, so it's simply unavailable
  there (use the HTTP transport instead).

## Reusing this launcher in a new project

1. Copy/reference this repo (submodule or vendor the `main/` + Kconfig).
2. Adjust `partitions.csv` (or `_4mb`) to your slot count/sizes.
3. `idf.py menuconfig`: set `LAUNCHER_APP_SLOT_COUNT` /
   `LAUNCHER_APP_SLOT_SIZE` to match, set display/EC11 GPIO pins.
4. Edit `main/app_registry.c` (`kApps[]`) with your own slot names —
   `boot_check_slot_count_consistency()` logs a warning at boot (not a
   crash) if this drifts from `partitions.csv`/Kconfig.
5. Pick a nav driver in menuconfig (EC11 or the console-based mock).

`main/boot_logic.c`, `main/nvs_state.c`, `main/ui_menu.c` shouldn't need to
change.

## Design decisions that deviate from the original spec

Kept here rather than only in commit history since they affect anyone
reusing/extending this repo:

- **`Kconfig.projbuild` lives in `main/`, not the repo root.** ESP-IDF's
  build system only auto-discovers `Kconfig.projbuild` inside a component
  directory (verified against the actual `espressif/idf:release-v5.5`
  Kconfig-discovery code) — a root-level file is silently ignored.
- **`partitions.csv` leaves app-partition offsets blank.** A first draft
  with hand-computed explicit offsets actually overlapped by 0x1000 bytes
  (otadata ending at `0x11000`, factory starting at `0x10000`) — the same
  mistake is easy to make by hand. Leaving app offsets blank lets
  `gen_esp32part` auto-align them to 64KB, which is what every table under
  ESP-IDF's own `components/partition_table/` does. This still satisfies
  "identical partitions.csv between launcher and guest app" since the
  computation is a deterministic function of the file contents.
- **`nav_input_driver_t` gained a fourth function pointer,
  `is_button_held()`**, beyond the spec's original 3-function sketch. The
  boot sequence needs an instantaneous "is the button held right now" level
  read *before* the menu (and its event callback) is even entered — that's
  a level query, not an edge/click/long-press event, so it didn't fit
  `nav_event_cb_t`. See the comment on the struct in `main/nav_input.h`.
- **Menu text renderer is a small custom 5x7 font, uppercase + digits +
  basic punctuation only, no accents.** Avoids depending on LVGL or another
  heavy graphics stack for a simple text menu (per spec). French display
  strings with accents (e.g. "Sélection") render uppercased/unaccented.
  Tracked as a follow-up if richer text is ever needed.
- **Screen driver uses ESP-IDF's built-in `esp_lcd` + `esp_lcd_panel_st7789`**
  (native IDF component, ST7789 support is built into ESP-IDF core) rather
  than `TFT_eSPI`, to avoid an Arduino-compatibility-layer dependency in a
  pure ESP-IDF project.
- Kconfig bool options only get a `#define` in `sdkconfig.h` when set to
  `y` (ESP-IDF/Kconfig behavior, not a bug) — code that reads a bool config
  as a plain C expression (not inside `#if`) needs an explicit
  `#ifndef ... #define ... 0 #endif` fallback. Done for
  `CONFIG_LAUNCHER_EC11_INVERT` in `main/nav_input_ec11.c`; keep this in
  mind if you add more bool-driven runtime behavior.

## Out of scope for this iteration

No SD card / dynamic binary loading — programs are flashed statically via
USB (or network OTA, see `CONFIG_LAUNCHER_NET_OTA_ENABLE`). May be revisited
in a future iteration. Network features
(`LAUNCHER_NET_OTA_ENABLE`/`LAUNCHER_NET_REMOTE_CONTROL_ENABLE`/
`LAUNCHER_NET_VERSION_CHECK_ENABLE`) have their Kconfig options wired up
already but no implementation yet — see open GitHub issues.

## License

MIT, see `LICENSE`.
