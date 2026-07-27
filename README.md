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
- Crash-loop failsafe: if the remembered app crashes (panic/watchdog reset)
  within `CONFIG_LAUNCHER_CRASH_LOOP_WINDOW_MS` (default 10s) of being
  booted, `CONFIG_LAUNCHER_CRASH_LOOP_THRESHOLD` (default 3) times in a
  row, the launcher shows the menu instead of retrying a direct boot into
  it — catching the case where an app crashes too fast for the user to
  react by holding the button themselves. See issue #23 and the "Design
  decisions" section below for exactly how the streak is tracked/reset.
- A guest app can hand control back to the menu by calling
  `launcher_request_menu_on_next_boot()` from `components/launcher_client`
  (e.g. on a long button press) and rebooting. This sets `force_menu` in NVS
  **and** points the boot partition back at `factory` before restarting —
  both are required: once otadata has ever been written (i.e. as soon as
  any guest app has booted at all), the 2nd-stage bootloader boots straight
  from whatever otadata points to and never falls back to `factory` on its
  own, so without the boot-partition switch the launcher's own `app_main()`
  (and therefore the code that reads `force_menu`) would simply never run
  again. Found on real hardware, see closed issue #12.

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

### Combining with a guest app: tools/merge_with_guest.py

Deliberately **two independent build systems, one flash step** — the guest
app is built entirely on its own (PlatformIO, Arduino, a second `idf.py`
project, whatever) and handed to this script as a plain `.bin`; the script
never touches the guest project. This is the intended way to consume this
repo without adopting its build system or a shared/vendored-dependency
model (see the design discussion closed in issue #11).

```
# from an ESP-IDF environment (. $IDF_PATH/export.sh, or inside .devcontainer)

# dry run: builds the launcher, prints the equivalent esptool command
tools/merge_with_guest.py --slot app_slot1 --guest-bin /path/to/guest.bin

# produce a single flashable image (esptool merge_bin under the hood)
tools/merge_with_guest.py --slot app_slot1 --guest-bin guest.bin -o combined.bin
python -m esptool --chip esp32s3 -p PORT write_flash 0x0 combined.bin

# or flash both directly to a connected board in one go
tools/merge_with_guest.py --slot app_slot1 --guest-bin guest.bin -p /dev/ttyUSB0
```

It resolves the target slot's real offset/size from the launcher's own
built partition table (never hand-computed, see "Design decisions" below
for why that matters) and refuses to proceed if the guest binary is larger
than the slot — `idf.py build`'s own overflow check is only a warning (see
the safety note atop `partitions.csv`), so this script enforces it as a
hard stop instead. Pass `--skip-build` to reuse an existing `build/`
instead of rebuilding the launcher first.

## Repo layout

```
main/                   launcher firmware (see file headers for each module's role)
components/launcher_client/   tiny library for guest apps ("return to menu")
partitions.csv          8MB flash layout (S3 dev boards), 3 app slots
partitions_4mb.csv       4MB flash layout (C3 / classic WROOM), 2 app slots
test/test_boot_logic.c  host-based unit tests for the pure boot-decision logic
tools/merge_with_guest.py   combine the launcher with a separately-built guest .bin, see below
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
- **Display orientation (`CONFIG_LAUNCHER_DISPLAY_SWAP_XY`/`_MIRROR_X`/
  `_MIRROR_Y`) is a Kconfig option, not hardcoded.** Found on real hardware
  (issue #13): a 2.4" ST7789 module is natively portrait, so
  `esp_lcd_panel_swap_xy()` is needed to get the landscape image this
  driver assumes, and mirroring depends on the specific panel's scan
  direction/mounting — not guessable without hardware in hand, same
  reasoning as `CONFIG_LAUNCHER_EC11_INVERT` for the encoder. Confirmed on
  this project's reference hardware: `SWAP_XY=y, MIRROR_X=y, MIRROR_Y=n`
  (the defaults) gives a correct, unmirrored landscape image; a different
  panel may need a different combination, adjustable via `menuconfig`
  without touching code.
- **Crash-loop failsafe (`CONFIG_LAUNCHER_CRASH_LOOP_THRESHOLD`/`_WINDOW_MS`,
  issue #23) diverges from that issue's own suggested direction in three
  ways, all deliberate:**
  - The crash-streak counter resets to 0 **only** when the user deliberately
    reselects an app from the menu — not merely by the menu being shown
    (e.g. a K0-long-press round trip). The issue's suggested text treated
    any "normal reset" as clearing it; this project chose the stricter
    reading so an incidental menu visit can't let a genuinely crash-looping
    app dodge the failsafe. Tradeoff: the streak is effectively a lifetime
    count since the app was last deliberately chosen, not a
    strictly-consecutive-in-time one — a few isolated crashes spread over
    months (each recovered from normally) will eventually trip it too.
  - `ESP_RST_BROWNOUT` is **not** counted as an abnormal reset, though the
    issue's suggested list included it. A brownout can come from external
    power flakiness (weak USB cable/supply) unrelated to a bug in the app
    itself; counting it risks forcing the menu for reasons the app can't
    fix.
  - Only counts as a "crash" if it happened within
    `CONFIG_LAUNCHER_CRASH_LOOP_WINDOW_MS` (default 10s) of the launcher
    handing control to the app, measured via `gettimeofday()` (backed by
    ESP-IDF's default RTC-clock time source, which keeps ticking through
    panic/watchdog resets and only resets on an actual power-on). Not in
    the issue at all — added because the actual failure mode this guards
    against is an app crashing before the user has any chance to react by
    holding the button; a crash after the app ran fine for a while is
    already recoverable that way and deliberately out of scope here.
- **App-slot menu labels never read a name out of the flashed image itself
  (issue #22).** A first attempt used `esp_app_desc_t.project_name` via
  `esp_ota_get_partition_description()`, but real-hardware testing showed
  it's meaningless for PlatformIO/Arduino-framework guests — it holds that
  framework's own internal build project name (e.g.
  `"arduino-lib-builder"`), never the guest sketch's actual name, since
  that field is populated by whatever build system produced the image, not
  something the launcher controls. Final approach: `app_registry.c`'s
  static `kApps[i].display_name` is always the label text; a slot's *empty
  vs. flashed* state is a separate boolean check
  (`app_registry_slot_is_flashed()`, still via
  `esp_ota_get_partition_description()`, just ignoring its `project_name`
  field), and for OTA-downloaded slots specifically, the real name comes
  from the OTA manifest entry's own `name` field, captured by `net_ota.c`
  into NVS (`nvs_state_set_slot_name()`) at the moment a download succeeds
  — the launcher already knows this name before it ever touches the
  binary, so it's guest-agnostic and build-system-agnostic. Tradeoff: if a
  slot is later reflashed by some means outside the launcher's own OTA
  flow, the recorded name can go stale (there's nothing in the image to
  cross-check it against, by design) until the next OTA download to that
  slot overwrites it again.
  A version number (` v<version>`) is shown next to the name wherever a
  version is available: the *locally installed* version
  (`esp_app_desc_t.version`, via `app_registry_get_version()` — this field
  doesn't have the same build-system-name problem `project_name` does, it's
  just a version string) on both the main menu and the OTA target-slot
  picker, and the *manifest's own candidate* version on the OTA
  program-selection picker (a different, remote concept, shown with the
  same formatting for visual consistency). The main menu and the
  target-slot picker are guaranteed to format the installed-version part
  identically because both call the same
  `app_registry_format_version_suffix()` rather than each formatting their
  own copy.

## Network features (optional, off by default)

All three are independently Kconfig-gated and require WiFi station mode
except the BLE remote transport (see below). None of them can block local
boot: a WiFi/network failure just leaves that feature inactive for the
current boot cycle, the local menu still works. **None of this has any
authentication beyond an optional shared PIN** — see "Security" below.

### WiFi credentials (`CONFIG_LAUNCHER_NET_WIFI_ENABLE`)

Read from NVS (namespace `CONFIG_LAUNCHER_NVS_NAMESPACE`, keys `wifi_ssid`/
`wifi_pass`) first; if absent, falls back to `CONFIG_LAUNCHER_NET_WIFI_SSID`/
`CONFIG_LAUNCHER_NET_WIFI_PASSWORD` (put these in a local, gitignored
`sdkconfig.local`, never in `sdkconfig.defaults`). There's no captive-portal
provisioning UI yet (spec explicitly allows this simpler fallback for now) —
see open issues. `net_wifi_save_credentials()` exists for a future
provisioning flow to call. Connection attempts are capped at ~10s.

### OTA download (`CONFIG_LAUNCHER_NET_OTA_ENABLE`)

Adds a "Telecharger un programme" entry to the bottom of the main menu:
connect WiFi → fetch the manifest → pick an app → pick a destination slot →
confirm (this overwrites that slot) → stream the `.bin` straight into the
chosen partition. Does **not** use the high-level `esp_https_ota` helper,
which always targets "the next OTA slot" — this project needs an
explicitly chosen slot, so it uses `esp_ota_begin`/`esp_ota_write`/
`esp_ota_end` manually against the partition the user picked
(`main/net_ota.c`). Never auto-boots the freshly written slot; select it
from the menu afterward like any other slot.

### Version check (`CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE`)

Before showing the menu, compares each slot's `esp_app_desc_t.version`
(read straight from flash, no boot needed) against the manifest and shows a
`(MAJ)` suffix next to outdated slots. Adds noticeable latency before the
menu appears (a WiFi connect + HTTP fetch, both bounded) — this is a
deliberate, honest tradeoff over blocking silently or caching stale results;
shown to the user as a "VERIF. MISES A JOUR..." status line.

### Manifest format

Both features above fetch `<CONFIG_LAUNCHER_NET_OTA_URL_BASE>/manifest.json`:

```json
{
  "apps": [
    {
      "name": "ASCII Aquarium",
      "slot": "app_slot1",
      "version": "1.2.0",
      "url": "https://example.com/bins/aquarium.bin",
      "size": 512000
    }
  ]
}
```

`slot` is used by version-check to match a manifest entry to a local
partition; `url` can point anywhere (doesn't have to be under
`URL_BASE`, e.g. a GitHub Releases asset — both the manifest fetch and the
`.bin` download follow up to 5 redirects via `net_http_util.c`, needed
since GitHub Releases download URLs always respond with a 302). `size` is
informational only (logged as a mismatch warning if it disagrees with what
was actually downloaded, never trusted for erase sizing —
`OTA_SIZE_UNKNOWN` is used instead, which erases the whole target
partition regardless).

### Remote control (`CONFIG_LAUNCHER_NET_REMOTE_CONTROL_ENABLE`)

Either transport calls the exact same `boot_into()` used by the local menu
— network is just another event source, per spec.

- **HTTP** (`main/net_remote_http.c`): `esp_http_server` on port 80. `GET /`
  lists slots with a boot button (and a PIN field if configured); `GET
  /boot?slot=<label>&pin=<pin>` triggers the boot. Requires WiFi.
  **Deliberately boot-only, no remote-triggered OTA download** — matches
  the original spec's scope for this feature ("list slots and trigger
  boot"), and the on-device "Telecharger un programme" menu entry already
  covers that case (see issue #18). Revisit if a concrete need comes up.
- **BLE** (`main/net_remote_ble.c`): a minimal NimBLE GATT peripheral, no
  pairing/bonding (PIN is checked at the application layer instead, same as
  HTTP, to keep both transports symmetric and avoid NimBLE's much larger
  security-manager surface for what's meant to be a trusted-home-network
  feature). One service, two characteristics: a READ one returning a
  comma-separated list of partition labels, and a WRITE one accepting
  `<slot_label>` or `<slot_label>:<pin>` as plain ASCII to trigger the boot.
  Independent of WiFi.

  **Manual menuconfig step required**: Kconfig's `select` cannot force a
  specific member of a `choice` block (a real Kconfig limitation — see
  "Design decisions" below), so picking this transport does *not*
  automatically switch the Bluetooth host stack to NimBLE. After enabling
  it, also go to `Component config > Bluetooth > Host` and pick "NimBLE -
  BLE only" yourself (Bluedroid, the default, is a different, incompatible
  API). On classic ESP32 also set `Component config > Bluetooth >
  Controller > Mode` to "BLE Only".

### Security

None of this is designed to survive a hostile network. `CONFIG_LAUNCHER_NET_REMOTE_PIN`
is a shared plaintext string checked equal — enough to stop someone on the
same WiFi/BLE range from accidentally or casually switching your running
program, not a real access control. Both transports are meant for a
trusted home network / BLE proximity only; don't expose the HTTP port to
the internet.

### Flash size tradeoff

Enabling these pulls in WiFi + mbedtls/TLS + `esp_http_client` + cJSON
(≈1MB combined) and, for BLE remote, NimBLE (another ≈250-300KB) — see the
sizing comments at the top of `partitions.csv` / `partitions_4mb.csv`.
`partitions.csv`'s `factory` partition is sized 1.5MB specifically to fit
all of these at once; `partitions_4mb.csv` was deliberately left smaller
(640KB) since a 4MB module doesn't have the budget to grow factory without
seriously cutting into app slot space — see that file's comment for
concrete measured sizes and guidance. **`idf.py build`'s partition-size
overflow check is a warning, not a build failure** — `esptool` has no
concept of partition boundaries and will happily flash straight into the
next partition, corrupting it silently. Never flash after seeing that
warning.

## Out of scope for this iteration

No SD card / dynamic binary loading — programs are flashed statically via
USB (or network OTA, see above). May be revisited in a future iteration.

## License

MIT, see `LICENSE`.
