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
- The picture and touch are turned **180 degrees** (Tom, 2026-11-25):
  `panel.c` sends the canvas backwards, `touch_gt911.c` flips x / y. The
  canvas itself and all UI code stay upright.
- Firmware file prefix `CYD-MacSE_`; boards named by size, driver, touch.

## Architecture
- `components/minivmac/` Mini vMac core (vendored, GPL 2) + `cfg/`
  (hand-made) + `osglu_cyd.c` (the glue: disks, screen, events, sound,
  time). `components/macui/` the 1-bit canvas, font and touch UI (plain C).
  `main/` the board: RGB panel, GT911, I2S, SD, tasks. `host/` PC build.
- Display: one 800x480 **1-bit canvas in internal RAM** (48 KB); ESP-IDF's
  RGB driver with no frame buffer, our bounce-buffer refill expands it to
  RGB565. Nothing of the picture is in PSRAM (the Mac's RAM and the 68000
  core's 512 KB opcode table are: SmallGlobals 1, too big for DRAM; keeps
  the cache). Never use a DMA that reads PSRAM (cyd-classic-arcade
  v0.26.0's lesson).
- Cores: core 0 the panel interrupt, UI / touch (10 ms poll), sound;
  core 1 the Mac (no idle watchdog there).
- After a crash / watchdog / brown-out restart `main/crash_report.c` shows
  the core dump summary and least free stacks, and the saved disks stay
  out. Decode its addresses with the release's .elf (addr2line).
- Logs on the card (`main/sdlog.c`): mac/log.txt, log-old.txt, crash.txt.
  The PC can't see the card over USB: the USB port is the CH340 serial
  chip and the S3's own USB pins (19/20) are the touch I2C. Tom chose
  (2026-11-25) a **web page on the board** instead of pulling the card:
  `main/wifi_web.c` joins the Wi-Fi in mac/wifi.txt (ssid=, password=)
  and serves the three logs as plain text at http://<address>/ (shown in
  Settings and on the crash screen; hostname cyd-macse). Wi-Fi and its
  tasks on core 0. Not a GitHub upload (he declined it).
- Clock (Tom, 2026-11-25): once on Wi-Fi, NTP from **192.168.4.1** (his
  router; the code uses the DHCP gateway, or ntp= in wifi.txt). Time zone
  US Eastern unless tz=. Log lines and crash.txt carry date + time; the
  Mac's date follows the clock once set. The crash screen waits **60 s**.
- Crash hunt (0.1.0-0.1.8): internal heap / a task stack damaged, always
  ~1 s after a disk goes in (at start from macse.txt, or a tap on Disks),
  before the first disk transfer finishes. The same code with Tom's ROM +
  System 6.0.8 / 7.0 runs clean on the PC under ASan / TSan (host build,
  `make CC="gcc -fsanitize=address,undefined"`): board-only. 0.1.8: one
  recursive SD lock (`hal_fs_lock`) around every card user, comprehensive
  heap poisoning, the first damaged free block on the crash report.
- GT911 INT = the amp's LRCLK: touch is reset first, INT released, then I2S.
