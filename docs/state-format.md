# Save-state file format

Every slot of a game is one file, `sd:/savestates/<checkcode>.st<slot>`, where
`<checkcode>` is the 16 hex digits of the ROM header's check code (the two CRC
words at ROM offsets 0x10 and 0x14) and `<slot>` counts from 0. The file is a byte
copy of the start of the state slot in cartridge memory, so the two share one
layout. Everything is big-endian, as the N64 stores it.

This is format version 2. The rules a reader should follow are at the end; a
writer of a later version keeps to them so that files made today stay loadable.

## Layout

| Offset | Size | Contents |
| --- | --- | --- |
| 0x0000 | 4 KiB | header (below) |
| 0x1000 | 9600 | thumbnail, 80x60 pixels, RGBA5551 |
| 0x3580 | | spare |
| 0x3E00 | 512 | a zeroed sector (see "Writing") |
| 0x4000 | `image_len` | the RAM image, from address 0x80000000 |

A state written by this build (0.3.4-ss1.3, hook version 12; version 11 states have the same shape) has `image_len`
= 0x800000: all 8 MiB of RAM (the 128 KiB the routine borrows come from its stash,
so the image holds the game's bytes), followed at 0x804000 by the RSP's memories
(region kind 1, below), so the file's used length is 0x806000 bytes, which is also
its allocated size. States written by the 0.3.3-ss1.0 build have `image_len` =
0x7C0000 (7.75 MiB, everything below the hook it kept resident) and no region, in
8 MiB files; a reader takes both from the header.

## Header

| Offset | Field | Meaning |
| --- | --- | --- |
| 0x00 | `magic` | `ST64` (0x53543634) |
| 0x04 | `version` | 2 |
| 0x08 | `flags` | bit 0 valid, bit 1 thumbnail present |
| 0x0C | `image_len` | bytes of RAM in the image |
| 0x10 | `image_base` | 0x80000000 |
| 0x14 | `rom_crc1`, `rom_crc2` | the ROM's check code; a state is bound to its ROM |
| 0x1C | `stamp` | cart RTC date word (BCD: century, year, month, day) |
| 0x20 | `cause`, `badvaddr` | CP0 at the moment of capture |
| 0x28 | `compare_delta` | CP0 Compare minus Count at capture |
| 0x2C | `mi_mask` | MI interrupt mask |
| 0x30 | `reraise_sp` | the interrupt the state was taken on: 0 VI, 1 RSP, 2 RDP. Hook version 12 and up: 0x10 set means a VI moment the routine held while the RSP's queue ran dry (libdragon games), with bit 0 (RSP) and bit 1 (RDP) the interrupts that arrived during the hold, raised again by a load; 0x20 set means the RSP was halted in its wait for the CPU and runs on from there after a load |
| 0x34 | `memsize` | the game's `osMemSize` |
| 0x38 | `dma_ticks`, `wait_frames` | how long the save took (diagnostic) |
| 0x40 | `hook_version` | the hook that wrote it |
| 0x44 | `stamp_time` | cart RTC time word (BCD: weekday, hour, minute, second) |
| 0x48 | reserved[6] | PI_DRAM_ADDR, PI_CART_ADDR, SI_DRAM_ADDR, DPC_STATUS, SP_STATUS, CP0 Count |
| 0x60 | `hdr_len` | 0x1000 |
| 0x64 | `image_off` | 0x4000 |
| 0x68 | `thumb_off`, `thumb_size` | 0x1000, 9600 |
| 0x70 | `checksum` | over the whole header with this word 0; 0 means none |
| 0x74 | `slot_len` | the slot stride the writer used (informational) |
| 0x78 | `regions_n` | entries used in the region table (1 in this build: the RSP's memories; 0 in ss1.0 files) |
| 0x80 | `vi[16]` | the VI registers at capture |
| 0xC0 | CPU context | 32 GPRs, LO, HI, 32 FPRs (64-bit each), Status, EPC, FCR31, EntryHi, then 32 TLB entries of 4 words |
| 0x4E0 | regions[8] | {kind, offset, length, arg} per entry. Kind 1 is the RSP's memories: 4 KiB DMEM then 4 KiB IMEM at `offset` (0x804000), `length` 0x2000, `arg` the RSP's program counter at capture. Other kinds may come in later versions |
| 0x560 | rcp[8] | hook version 9 and up: DPC_START, DPC_END, DPC_CURRENT and SP_PC at capture, the rest zero (earlier writers left zeros; a loader with CURRENT == END and the RDP idle puts the RDP back there) |
| 0x580 | rsp_gpr[32] | hook version 11 and up: word 0 is the marker `RSPG` (0x52535047) when words 1..31 hold the RSP's scalar registers 1..31 at capture, zero otherwise. Written for games built with libdragon, whose RSP command queue sleeps with its place in a register; a loader puts them back after the RSP's memories and program counter |

The checksum is a rotate-left-by-one and exclusive-or over the 1024 header words,
seeded with the magic, with the checksum word taken as zero; a result of zero is
stored as 1.

Everything a reader needs to decide whether a file holds a usable state (magic,
version, the ROM binding, the image length and offset) sits in the first 512 bytes,
so one sector read is enough to list a card.

## Writing

The cart slot is written image first, then thumbnail, then header, so an
interrupted save never leaves a valid header over a partial image. The copy to the
SD card is written in this order: the zeroed sector over the file's header, the
image, the rest of the head, the header last. A power cut during the copy therefore
leaves a file that reads as empty rather than as a state whose image does not
match its header. The file's first sector is never written unless it already holds
this ROM's state or the menu's fresh-file marker (`STFR`, then the two check-code
words, then the slot number), so a wrong sector table cannot damage anything else
on the card.

## Rules for readers

- Accept any `version` of 2 or more; refuse older files.
- Verify the checksum when it is non-zero.
- Take `image_off`, `image_len` and `thumb_off` from the header, never from this
  document.
- Refuse a state whose ROM check code differs from the running ROM's.
- Refuse an image longer than the reader can restore (a build that keeps its hook
  resident restores at most 7.75 MiB, because it lives above that; this build
  restores 8 MiB).
- Ignore region kinds you do not know.
