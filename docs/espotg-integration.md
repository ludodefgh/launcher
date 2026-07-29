# EspOTG integration guide — launcher remote control (BLE + HTTP)

What EspOTG needs to implement to talk to a running launcher over BLE
and/or HTTP: boot a slot, provision WiFi credentials, flash a slot
directly, or trigger a manifest-driven OTA update — all without a serial
cable. Self-contained; doesn't assume familiarity with the launcher's own
source.

Everything here requires `CONFIG_LAUNCHER_NET_REMOTE_CONTROL_ENABLE` on the
device. Some pieces additionally require `CONFIG_LAUNCHER_NET_OTA_ENABLE`
or `CONFIG_LAUNCHER_NET_WIFI_ENABLE` — noted per-feature below, since these
are independent Kconfig options and not every device will have all of them
on. **Discover BLE characteristics via GATT rather than assuming a fixed
set are present** — a device with remote control on but OTA off simply
won't expose the OTA-update characteristic, for example.

## BLE

Peripheral name: `launcher`. No pairing/bonding — PIN (if configured on the
device) is checked at the application layer inside each characteristic's
payload, not via BLE security manager.

### Service

```
01000100-0100-0100-0100-48434e55414c
```

### Characteristic: slots list (READ)

```
02000100-0100-0100-0100-48434e55414c
```

Always present. Returns one record per slot, `\n`-separated, each record
tab-separated:

```
label\tname\tversion\tflashed\n
```

- `label` — the raw partition label (`app_slot1`, ...) — this is the value
  to send to the **select** characteristic below to boot this slot, and to
  the **OTA-update** characteristic to update it. Not meant for display.
- `name` — friendly display name (falls back to `label` if the slot was
  never OTA-downloaded through the launcher and has no recorded name).
- `version` — installed version string, or empty if unknown/unflashed.
- `flashed` — `"1"` if the slot has a valid flashed image, `"0"` if empty.

**Compatibility note**: older launcher firmware (pre-2026-07-28) returns a
bare comma-separated list of labels instead (`app_slot1,app_slot2,...`),
no version/flashed info. Detect the format by checking for `\n` in the
response — its presence means the new tab/newline format; its absence
means the legacy comma-separated one. Fall back to using the raw label as
the display name and treating every slot as flashed if you hit the legacy
format.

This is built once when BLE starts and cached — it does **not** live-update
if a slot changes (e.g. after an OTA-update trigger completes). Reconnect
or otherwise force the device to rebuild it to see fresh data, or just
treat "flashed"/"version" here as approximate and confirm success some
other way after triggering a change (there's no live-refresh mechanism
today).

### Characteristic: select / boot (WRITE)

```
03000100-0100-0100-0100-48434e55414c
```

Always present. Write either:

```
<label>
```

or, if a PIN is configured on the device:

```
<label>:<pin>
```

Boots that slot immediately (`esp_restart()` on success — the BLE
connection will drop as the device reboots, that's expected, not an
error). Max practical payload ~63 bytes (label is capped at 31 chars on
the device side).

**No ATT-level error response for a bad label or wrong PIN** — the device
just logs a warning and does nothing. There's no way to distinguish
"wrong PIN" from "unknown slot" from "malformed payload" from the BLE
side; if nothing happens within a couple seconds (no disconnect from the
device rebooting), treat it as a failed request.

### Characteristic: OTA-update trigger (WRITE) — requires `CONFIG_LAUNCHER_NET_OTA_ENABLE`

```
04000100-0100-0100-0100-48434e55414c
```

Only present if the device has OTA enabled. Same payload shape as select:

```
<label>
```
or
```
<label>:<pin>
```

Looks up the manifest entry whose `slot` field matches `<label>` and
downloads it into that slot — the launcher does its own WiFi-based fetch
exactly as if the user had used the local menu's "download a program"
flow, just without any local UI involved. **No binary is transferred over
BLE** — this characteristic only carries the trigger + slot, the actual
download happens over the device's own WiFi connection.

Important limitations, by design (not bugs):
- **Fire-and-forget**: writing this characteristic does not itself confirm
  anything. There's no progress or result reported back over BLE. Re-read
  the slots characteristic some time later (the download can take several
  seconds to tens of seconds) and compare `version`/`flashed` to what they
  were before, to infer whether it worked. A slot that still shows the old
  version after a generous wait likely failed (check `unknown slot` /
  `no manifest entry for this slot` / network issues on the device's own
  logs if you have serial access).
- **Always installs the newest release** for a manifest entry that uses
  `github_repo` (see [`docs/launcher-manifest.md`](launcher-manifest.md)).
  There's no remote version-picker round trip yet — installing an older
  version (e.g. to rollback) still requires the local menu's interactive
  "CHOOSE VERSION" step.
- Fails silently (same as select above — a log line on the device, nothing
  over BLE) if: the slot label is unknown, no manifest entry declares that
  slot, the manifest/GitHub fetch fails, or (for a `github_repo` entry) the
  release has no `launcher.manifest.json` asset resolving a binary for the
  device's own chip target.

### Characteristic: WiFi credentials (WRITE) — requires `CONFIG_LAUNCHER_NET_WIFI_ENABLE`

```
05000100-0100-0100-0100-48434e55414c
```

Only present if the device has WiFi support compiled in. Payload, always
tab-separated with exactly two tabs present even if a field is empty:

```
<ssid>\t<pass>\t<pin>
```

- `<ssid>` required, non-empty (max 32 chars).
- `<pass>` may be empty (open network), max 64 chars.
- `<pin>` the device's configured remote-control PIN if one is set, empty
  string if not (still needs the trailing tab before it either way).

This is the one that can **bootstrap a device that has never had working
WiFi credentials at all** — unlike the HTTP endpoint below, this
characteristic has no dependency on WiFi already being connected (BLE
starts independently). Persists to NVS only; does not attempt a live
reconnect (takes effect on the device's next WiFi connect attempt or
reboot). No confirmation over BLE that the credentials were valid/that a
connection succeeded — reconnect later and check via some other channel
(e.g. try the HTTP endpoints, or the OTA-update characteristic, which both
require a live WiFi connection to do anything).

## HTTP

Base URL: `http://<device-ip>/` (find the IP from the device's local menu
footer, or serial log, or by having previously connected over BLE — there's
no separate BLE characteristic exposing the IP today).

All endpoints below require `CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_HTTP`. All
require WiFi already connected (the HTTP server itself won't be running
otherwise). If a PIN is configured, pass it as `?pin=<pin>` on every
request below (query string, even for POST endpoints).

### `GET /`

HTML page listing slots with boot buttons. Not meant for programmatic use,
but harmless to fetch — mainly useful as a manual fallback / sanity check.

### `GET /boot?slot=<label>&pin=<pin>`

Boots `<label>` immediately. `400` if `slot` is missing or unknown. `401`
if the PIN is wrong/missing when one is configured. On success the
connection just drops as the device reboots (`esp_restart()` never
returns a response).

### `POST /wifi?pin=<pin>`

Body (`application/x-www-form-urlencoded`): `ssid=<ssid>&pass=<pass>`.
Persists credentials for the next reconnect/boot. `400` for a missing/empty
`ssid`, `401` for a bad PIN. **Update-only**, same reasoning as the BLE
asymmetry above but the other way round: this endpoint requires WiFi
already working (the whole HTTP server does), so it can never bootstrap a
device with none configured — only the BLE characteristic can do that.

### `POST /upload?slot=<label>&pin=<pin>`

Body: the raw `.bin` file, `Content-Length` set accurately (checked against
the target partition's size before any write starts — `400` if it doesn't
fit). Writes straight into the slot via `esp_ota_begin`/`write`/`end`. No
manifest/repo/URL involved at all — this is a direct push of a file EspOTG
already has locally, the HTTP equivalent of a serial flash but wireless and
scoped to one app slot (not the launcher's own bootloader/partition-table,
which still needs a cable — see
[`docs/launcher-manifest.md`](launcher-manifest.md)'s `flash_images`
section for that side of things).

`200 OK` (plain text body `"OK"`) on success. `400`/`401`/`500` with a short
plain-text error otherwise (bad slot, bad PIN, missing body, or a flash
write/verify failure — `esp_ota_end` failing means the image was
incomplete/invalid, the slot is left in whatever partial state the write
reached, treat it as failed and retry the whole upload).

**No equivalent over BLE yet** — GATT writes are payload-size-bounded, a
multi-hundred-KB binary needs an actual chunking protocol that hasn't been
designed. HTTP is the only way to push a raw binary wirelessly today.

## Practical flows

**Provision WiFi on a brand-new device** (BLE only, since HTTP needs WiFi
already working): connect BLE → write the WiFi characteristic
(`ssid\tpass\tpin`) → device picks up the new credentials on its next
connect attempt.

**Boot a slot**: read the slots characteristic (or `GET /`) to get labels →
write the label to the select characteristic / `GET /boot?slot=...`.

**Update a slot from its manifest entry, no file transfer**: write the
label to the OTA-update characteristic → wait → re-read the slots
characteristic, compare `version`/`flashed` to before.

**Push a binary you already have on the phone**: `POST /upload?slot=...`
with the file as the body (HTTP only).
