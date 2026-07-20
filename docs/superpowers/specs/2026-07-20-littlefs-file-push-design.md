# Per-File LittleFS Push + File Browser — Design

**Date:** 2026-07-20
**Status:** Approved design, pre-implementation
**Project:** `Arduino/can_receiver_display` (RP2040 CAN display) + `Arduino/xiao_ota_bridge`

## Problem / Goal

Firmware OTA already works (XIAO SoftAP → UART → RP2040 self-flash). But **fonts and
splash frames live in LittleFS**, which OTA does not touch — updating a tweaked `.vlw`
font or swapping animation frames still requires the wired `arduino-littlefs-upload`
step.

Add **wireless per-file management of the RP2040's LittleFS** through the same XIAO page:
browse the filesystem, upload/replace individual files into the current folder, and
delete files. This covers "swap a font", "replace some splash frames", and "clean up
stale frames" without a cable and without a firmware reflash.

## Why per-file (not whole-FS OTA)

arduino-pico's `Update.begin(size, U_FS)` can OTA a whole filesystem image, but for
changing one font it would ship and rewrite the entire ~2 MB partition and reboot, and it
stages *into* LittleFS while overwriting LittleFS. Per-file push reuses our proven
UART protocol (framing + CRC32 + block-ACK flow control), updates one file at a time,
needs **no reboot**, and never touches the risky app-flash/bootloader path.

## Decisions (locked)

- **Operations:** list + upload + delete. (No rename/mkdir — YAGNI for fonts/frames.)
- **Upload target:** the file lands in the **currently-browsed folder** under its own
  name → the browser has directory navigation.
- **Safety:** uploads are written to a temp file, CRC-verified, then **atomically
  renamed** to the destination — a corrupt/interrupted push never clobbers an existing
  file. No reboot for any file op.
- **UI:** dark instrument-cluster theme, **two-column CSS grid** (firmware | files)
  collapsing to one column under ~640 px. MAZDASPEED orange (`#ff6a00`) accent on
  near-black. Self-contained (inline CSS/JS, no external assets).

## Non-Goals

- Whole-filesystem image OTA (`U_FS`).
- Rename, mkdir, or moving files.
- Multi-client / concurrent access (single user assumed).
- Editing file contents in the browser (upload replaces whole files only).

## Architecture

Three layers, same split as the firmware OTA path; the XIAO stays a dumb HTTP⇄UART
translator and the RP2040 owns the filesystem.

```
[phone] --WiFi/WPA2--> [XIAO ESP32-S3] --UART frames--> [RP2040] LittleFS
          HTTP+JSON     /api/list /upload /delete       list / write-temp+rename / remove
```

### Protocol extension (`ota_protocol`)

New `MsgType`s (framing, CRC32, block-ACK data phase all unchanged):

| Msg | Direction | Payload |
|-----|-----------|---------|
| `LIST_REQ`    | XIAO→RP2040 | directory path (`/`, `/splash`) |
| `LIST_ENTRY`  | RP2040→XIAO | `flags`(1, bit0=isDir) + `size`(u32 LE) + name — one frame per item |
| `LIST_END`    | RP2040→XIAO | `total`(u32 LE) + `used`(u32 LE) bytes |
| `WRITE_FILE`  | XIAO→RP2040 | `len`(u32) + `crc32`(u32) + dest path; then block-ACK data phase; then `RESULT` |
| `DELETE_FILE` | XIAO→RP2040 | path → `RESULT` |

- Entry/name and path strings fit the existing `MAX_PAYLOAD` (64) — filenames here are
  short; the encoders reject over-long names/paths.
- New small (de)serializers: `list_entry_encode/decode`, `writehdr_encode/decode`.
- `ResultCode` gains `NOT_FOUND` and `WRITE_ERROR` for file ops.

### RP2040 (`ota_update.cpp`)

- Factor the existing block-ACK receive loop into a reusable
  `receive_blocks(len, crc, sink, ctx)` where `sink` writes either to `Update` (firmware,
  unchanged behavior) or to an open `File` (new).
- `service()` dispatch gains three cases:
  - `LIST_REQ` → walk the dir (`LittleFS.openDir`/`openNextFile`), emit a `LIST_ENTRY`
    per item, then `LIST_END` with `LittleFS.totalBytes()`/`usedBytes()`.
  - `WRITE_FILE` → validate path, open `/.upload.tmp`, ACK, `receive_blocks` into it,
    verify CRC, `LittleFS.rename` to dest (replaces existing), `RESULT`.
  - `DELETE_FILE` → validate path, `LittleFS.remove`, `RESULT` (`NOT_FOUND` if absent).
- **File ops do not take over the TFT** (sub-second; the car may be showing speed). Only
  firmware flash keeps its "Updating…" screen. `ota::begin/service` signatures unchanged,
  so the sketch's boot integration is untouched.
- **Path validation:** must start with `/`, reject `..`, cap length. Temp path is fixed
  (`/.upload.tmp`) and hidden from listings.

### XIAO (`xiao_ota_bridge.ino`)

New HTTP endpoints beside the existing `/update` (firmware) and `/ping`:

- `GET /api/list?path=/splash` → `LIST_REQ`, collect entries, return **JSON**
  `{path, total, used, entries:[{name,size,dir}]}`.
- `POST /api/upload?path=/splash` → buffer file in PSRAM (existing mechanism),
  `WRITE_FILE` to `/splash/<uploadFilename>`, return the RP2040 result text.
- `POST /api/delete` (path in body) → `DELETE_FILE`, return result.

### Web page (`upload_page.h`)

Self-contained, dark two-column grid (→ one column < ~640 px):

- **Header:** ● connection dot + "MAZDASPEED DISPLAY" + running version (`/ping`).
- **Left card FIRMWARE:** existing pick-`.bin` + Flash + progress (restyled).
- **Right card FILES:** breadcrumb + free-space readout, listing (subdirs first with ▸ to
  enter, then files with size + 🗑), and "⬆ Upload here" targeting the current folder.
- **JS (vanilla):** `/ping` for version; `/api/list` to populate; folder click / ".." to
  navigate (current path = upload target); upload via XHR with a progress bar; delete via
  an **inline** confirm (small ✓/✕ swap, not blocking `window.confirm`) then re-list. Each
  action shows a status line (uploading… / OK / FAIL + the RP2040's reason). If the RP2040
  doesn't answer, the files card shows "display not responding" rather than hanging.

## Edge Cases

| Case | Behavior |
|------|----------|
| Corrupt/interrupted upload | temp file discarded, `RESULT=CRC_MISMATCH`, **original intact** |
| Upload over an existing file | temp-then-rename replaces it atomically |
| Delete a font/frame currently loaded in RAM | safe — sprite/frame data lives in RAM until next boot |
| Delete a high-numbered splash frame | splash plays the shorter run (player stops at first gap) |
| Upload into a non-existent dir | only the browsed (existing) dir is a target; root always exists |
| Bad path (`..`, no leading `/`) | rejected with an error result |
| RP2040 not responding | XIAO returns an error; page shows it, no hang |
| Large listing (~30 frames) | one small frame per entry; instant at 921600 |

## Testing Strategy

### Tier 1 — host unit tests (`ota_protocol`, g++)
- `LIST_ENTRY` encode/decode: flags/size/name round-trip; max-length name.
- `WRITE_FILE` header encode/decode: len + crc + path round-trip.
- New `MsgType`s survive frame encode→parse (extends the existing frame suite).

### Tier 2 — on-hardware bench
1. **List root** — fonts + `/splash` shown, sizes correct, free space matches.
2. **Navigate** — enter `/splash`, see frames; breadcrumb back to `/`.
3. **Upload a font** — push a tweaked `.vlw` to `/`; size updates; reboot → new font renders.
4. **Upload a frame** — push a `.jpg` into `/splash`; it lands.
5. **Delete** — remove a high frame; reboot → splash plays the shorter sequence.
6. **Corrupt-push safety** — interrupt/corrupt an upload → CRC_MISMATCH, original intact.
7. **Firmware still works** — a normal `.bin` OTA still flashes + reboots (refactor regression).

### Tier 3 — regression
CAN display + boot splash unaffected; the block-receiver refactor doesn't change
firmware-flash behavior.

**Non-negotiable gates:** corrupt-push leaves the original intact; firmware OTA still
works after the block-receiver refactor.

## Resolved Implementation Details (as-built target)

- **Protocol:** `LIST_REQ=0x08, LIST_ENTRY=0x09, LIST_END=0x0A, WRITE_FILE=0x0B,
  DELETE_FILE=0x0C`; `ResultCode` adds `NOT_FOUND`, `WRITE_ERROR`.
- **Temp file:** `/.upload.tmp`, hidden from listings, renamed on CRC success.
- **Endpoints:** `GET /api/list`, `POST /api/upload`, `POST /api/delete`; existing
  `/update`, `/ping`, `/` retained.
- **Reuse:** same 4096-byte block-ACK data phase and CRC32 as firmware OTA.
- **No reboot** for any file operation.
