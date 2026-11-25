# CLAUDE.md - working rules for this repo

CYD Mac SE: Mini vMac as a Macintosh SE on the Freenove 5.0" ESP32-S3
board (FNK0115Q / Hosyond, 800x480 RGB, GT911). Started 2026-11-25. Sister
of tomtombombadil/cyd-classic-arcade (its 5" board facts are reused here).

## The developer
- Tom works on **Windows** with VS Code + PlatformIO. Never give Linux /
  bash commands in instructions for him; use the PlatformIO GUI or
  PowerShell. A Python tool on his side: always a venv and
  `python -m pip install`.
- He does not want to repeat instructions: decisions go in this file.
- He uses a stylus (Nintendo DS Lite), firm presses: taps, not drags; no
  confirmations; Title Case for buttons and menus; a handedness setting;
  sounds sparse.
- Tom gave full permission to manage, push, post and release in this repo
  (2026-11-25).

## Decisions (Tom, 2026-11-25)
- Emulate a **Mac SE** (not a Plus) so 1.44 MB images work as well as
  800K. Core: Mini vMac, model SE, 4 MB. Its Sony driver patch takes disk
  images of any size, so the plain SE ROM (checksum B2E362A8) reads
  1.44 MB images; SE FDHD / Classic ROMs are not supported by its SE build.
- **Hard disk images** from the microSD card too (Tom: "this could open up
  huge possibilities"): bare HFS volumes and Apple-partitioned images
  (first Apple_HFS partition; `osglu_cyd.c` FindHFSPartition).
- **Never commit a Mac ROM or a copyrighted disk image.** `.gitignore`
  blocks *.ROM / *.dsk / *.img / *.image / *.hda / *.hfv.
- GPL 2 for the whole repo (Mini vMac); keep its licence and attribution.
- SD layout: everything in `/mac/` (or the root): `MacSE.ROM`, images,
  `macse.txt` (settings + the disks inserted, put back at start).
- Screen: Mac 512x342 1:1 at x 144, y 0; keyboard (4 rows) below; side
  panels: tools (Disks, Drag, Double Click, status, Settings) and Mac keys
  (Command, Option, Control, Space, Enter, arrows); Left Handed swaps them.
- Firmware file prefix `CYD-MacSE_`; boards named by size, driver, touch.

## Architecture
- `components/minivmac/` Mini vMac core (vendored, GPL 2) + `cfg/`
  (hand-made) + `osglu_cyd.c` (the glue: disks, screen, events, sound,
  time). `components/macui/` the 1-bit canvas, font and touch UI (plain C).
  `main/` the board: RGB panel, GT911, I2S, SD, tasks. `host/` PC build.
- Display: one 800x480 **1-bit canvas in internal RAM** (48 KB); ESP-IDF's
  RGB driver with no frame buffer, our bounce-buffer refill expands it to
  RGB565. Nothing of the picture is in PSRAM (the Mac's RAM is, and keeps
  the cache). Never use a DMA that reads PSRAM (cyd-classic-arcade
  v0.26.0's lesson).
- Cores: core 0 the panel interrupt, UI / touch (10 ms poll), sound;
  core 1 the Mac (no idle watchdog there).
- GT911 INT = the amp's LRCLK: touch is reset first, INT released, then I2S.
