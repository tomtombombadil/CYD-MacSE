# Makes a stand-in "ROM" for the PC test build (not Apple's code): 256 KB
# whose checksum is the Mac SE ROM's, so the emulator accepts it, with a
# few 68000 instructions that fill the screen with stripes and stop.
# It checks the CPU, the memory map, the screen copy and the timing -
# it can't boot a Mac. python tools/make_test_rom.py <out file>
import struct
import sys

rom = bytearray(0x40000)
code = bytes.fromhex(
    "41F9003FA700"   # lea     $3FA700,a0   (the SE screen with 4 MB)
    "303C155F"       # move.w  #5471,d0     (21888 bytes / 4 - 1)
    "20FCAAAA5555"   # move.l  #$AAAA5555,(a0)+
    "51C8FFF8"       # dbra    d0,*-6
    "60FE")          # bra     *
rom[0x1000:0x1000 + len(code)] = code
struct.pack_into(">I", rom, 4, 0x00401000)   # reset PC: ROM + 0x1000


def wordsum():
    return sum(struct.unpack(">%dH" % ((len(rom) - 4) // 2), rom[4:])) & 0xFFFFFFFF


target = 0xB2E362A8   # the Mac SE ROM's checksum (first long word)
i = 0x20000
n = (target - wordsum()) // 0xFFFF
rom[i:i + 2 * n] = b"\xff\xff" * n
i += 2 * n
struct.pack_into(">H", rom, i, target - wordsum())
struct.pack_into(">I", rom, 0, target)
assert wordsum() == target
open(sys.argv[1], "wb").write(rom)
