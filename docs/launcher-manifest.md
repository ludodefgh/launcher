# `launcher.manifest.json` — per-release manifest spec

This document is self-contained. If you're an AI assistant working in a
*different* repository (a guest app meant to run under
[ludodefgh/launcher](https://github.com/ludodefgh/launcher)) and the user
points you at this file, you have everything you need to generate a
compliant `launcher.manifest.json` and wire it into that repo's release
process — no need to read the launcher's own source.

## What this solves

The launcher's OTA download flow lets a user pick a program to install from
a manifest the launcher operator hosts (`manifest.json`, see the launcher's
own README). One entry in that manifest can point at a GitHub repo
(`"github_repo": "owner/repo"`) instead of a single fixed binary URL — the
launcher then lists that repo's actual GitHub releases and lets the user
pick any of them, including older ones (e.g. to intentionally roll back a
regression), without the manifest operator having to hand-maintain a
separate entry per version.

`launcher.manifest.json` is how a specific release tells the launcher (and
optionally other tooling) which of *that release's own assets* is the right
one to use, per chip target — attached to the release like any other build
artifact.

## Where it lives

Attach it as a release asset, on every release you want to be automatically
resolvable, with this **exact filename**: `launcher.manifest.json`.

A release with no such asset still works — the launcher falls back to
showing the user a raw list of that release's assets and letting them pick
manually — but automatic per-target resolution requires this file.

## Schema

```json
{
  "targets": {
    "esp32s3": "ascii-aquarium-esp32s3.bin",
    "esp32c3": "ascii-aquarium-esp32c3.bin",
    "esp32": "ascii-aquarium-esp32.bin"
  },
  "flash_images": {
    "esp32s3": [
      {"file": "launcher-bootloader.bin", "offset": "0x0"},
      {"file": "launcher-partition-table.bin", "offset": "0x8000"},
      {"file": "launcher-ota_data_initial.bin", "offset": "0xf000"},
      {"file": "launcher-app.bin", "offset": "0x20000"},
      {"file": "ascii-aquarium-esp32s3.bin", "offset": "0x1a0000"}
    ]
  }
}
```

### `targets` (what the launcher itself reads)

An object keyed by chip target. Each value is the **filename** (must match
an asset's name in this same release exactly — not a URL, not a path) of
the binary to install into whatever app slot the user picked in the
launcher's UI. Valid keys are ESP-IDF's own target strings — currently
`esp32`, `esp32s3`, `esp32c3` (matches `CONFIG_IDF_TARGET`; if the launcher
ever supports more chips, the same strings apply). Omit a target entirely
if this release doesn't have a build for it — the launcher will just not
offer automatic resolution for that chip (falls back to manual asset pick).

This is the only section this launcher's own firmware code
(`main/net_github.c`) reads. Everything below is for other tooling.

### `flash_images` (optional — for full-device flashing tools, not the launcher itself)

An object keyed by chip target, each value a list of `{file, offset}`
objects describing a **complete from-scratch flash** of that chip: the
launcher's own bootloader/partition-table/otadata/app, plus optionally this
guest app pre-placed into one of the launcher's app slots — useful for a
phone- or USB-based flashing tool bringing up a brand new device in one
pass, without a separate "flash the launcher, then OTA the guest" step.

- `offset` is a hex string, exactly as `esptool.py write_flash` expects.
- These offsets are **specific to the launcher repo this guest targets**
  and its `partitions.csv` — they are not universal constants and differ by
  chip target (e.g. the bootloader offset is `0x0` on esp32s3/esp32c3 but
  `0x1000` on classic esp32, since the ROM bootloader reserves the first
  4KB there). Get the real values from that launcher repo's own build
  output (`idf.py build` prints the exact `esptool.py ... write_flash`
  command with every offset) or its `partitions.csv` — don't guess or reuse
  values from an unrelated project.
- The guest binary's own offset must match whichever app slot this guest
  app is meant to occupy in that launcher's partition table — the same
  slot this repo's own OTA-manifest entry declares as `"slot"` (see the
  launcher's README "Manifest format").
- Every file listed here must also be attached as a release asset (the
  launcher binaries are typically produced by the *launcher's own* release
  process, not this guest's — if bundling them here, this guest's release
  workflow needs to fetch/embed them from there).
- This launcher's own firmware never parses this section. It exists purely
  as a documented convention for other tools (e.g. a phone-based flashing
  app) to consume.

## Full worked example

A release of a guest app repo, publishing standalone OTA binaries for two
chip targets, plus a combined from-scratch flash set for one of them:

```json
{
  "targets": {
    "esp32s3": "myapp-esp32s3.bin",
    "esp32c3": "myapp-esp32c3.bin"
  },
  "flash_images": {
    "esp32s3": [
      {"file": "launcher-bootloader.bin", "offset": "0x0"},
      {"file": "launcher-partition-table.bin", "offset": "0x8000"},
      {"file": "launcher-ota_data_initial.bin", "offset": "0xf000"},
      {"file": "launcher-app.bin", "offset": "0x20000"},
      {"file": "myapp-esp32s3.bin", "offset": "0x1a0000"}
    ]
  }
}
```

The release's assets, in this example, would be: `myapp-esp32s3.bin`,
`myapp-esp32c3.bin`, `launcher-bootloader.bin`,
`launcher-partition-table.bin`, `launcher-ota_data_initial.bin`,
`launcher-app.bin`, and `launcher.manifest.json` itself.

## Generating this automatically

The simplest approach is a small step in the repo's own release workflow
(e.g. a GitHub Actions job) that writes this file from known build outputs
and uploads it alongside the other binaries, e.g.:

```yaml
- name: Write launcher.manifest.json
  run: |
    cat > launcher.manifest.json << 'EOF'
    {
      "targets": {
        "esp32s3": "myapp-esp32s3.bin",
        "esp32c3": "myapp-esp32c3.bin"
      }
    }
    EOF
- name: Upload release assets
  uses: softprops/action-gh-release@v2
  with:
    files: |
      myapp-esp32s3.bin
      myapp-esp32c3.bin
      launcher.manifest.json
```

Adjust to whatever this repo's actual build/release tooling is — the only
hard requirement is that the final filenames in `targets` (and
`flash_images`, if used) exactly match the names of the assets actually
attached to the release.
