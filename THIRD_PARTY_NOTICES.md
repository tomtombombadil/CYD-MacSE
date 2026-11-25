# Third-party code

- **Mini vMac** - `components/minivmac/src/` and `cfg/` (the config files
  hand-made from its setup tool's output for `-m SE`). Copyright Paul C.
  Pratt and contributors, GPL version 2 (`components/minivmac/COPYING.txt`).
  From https://github.com/minivmac/minivmac at the commit in
  `components/minivmac/UPSTREAM_COMMIT.txt`. Changed: `COMOSGLU.h`
  `ScreenClearChanges()` empties the changed area (upstream marked the
  whole screen changed every tick).
- `components/minivmac/osglu_cyd.c` is new, based on Mini vMac's
  `OSGLUNDS.c` / `OSGLUSDL.c`: GPL version 2.
- **DejaVu Sans Bold** - the key labels' font, turned into bitmaps by
  `tools/mkfont.py` (`components/macui/font.h`). DejaVu fonts licence
  (Bitstream Vera derived, free to use and change).
- Board facts (pins, panel timing, the bounce-buffer panel setup) from
  tomtombombadil/cyd-classic-arcade (same owner).

No Apple ROM or system software is included. Users supply their own.
