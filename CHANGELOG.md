# Save states (SummerCart64 fork)

## 0.3.4-ss1.8 (2026-09-22)

- The list of save-state slots grows. It lives on the card, one file per slot with no
  fixed count, and the cartridge's own slots (by ROM size, as before) hold the states
  in use: a save goes to the cartridge and to the card right after, a load of a state
  not in the cartridge reads it from the card first, about a second, and the state
  used longest ago makes room. Every launch keeps six empty slots beyond the last one
  used (Empty slots to keep in the Save States options: 6, 12, 24 or 48), gives back
  empty ones past that, and re-makes any file that is missing. The panel scrolls,
  eight rows at a time (C-up and C-down page), fetches the thumbnail of a state on the
  card, asks before the last empty slot is used and says when none is left, and the
  Game page has Delete slot. Today's files are the first slots, untouched; nothing
  changes until a game saves past the old count. Asked for by Acurrz.
- A state saved with Slow motion on is refused with a message when it is asked for with
  the option off, and the panel marks such states SLOW. Loaded that way, a state saved
  while the game was busy froze it; with the option on it loads as before. Found while
  testing the slots on Banjo-Kazooie.
- Set ROM to autoload saves the game whose page is open. It saved the file browser's
  folder and highlighted entry, a different file when the page was opened from the
  History or Favorites tab, and the next start stopped at "Couldn't open ROM file".
  1.4 to 1.7 stored the game's own folder and name instead, but for a game in the
  card's root folder opened from History or Favorites that left the prefix alone,
  and the next boot looked for `sd:/sd:/<name>`. Every case now.
- After a game was set to autoload, other games' pages opened in the same session
  showed and booted the autoload game, and after a failed autoload the next page
  opened would have crashed: the page kept the earlier game's path, or none. Fixed
  the way rmmh's upstream pull request does.
- Holding Start while the menu starts, to cancel autoload, then setting a game in the
  same session freed a string that was never allocated. No harm seen; fixed.
- A box confirms Set ROM to autoload, with the file name and how to get the menu
  back (Start held while the console is switched on). Nothing showed before.

## 0.3.4-ss1.7 (2026-09-19)

- A hotkey can be set for every game at once, the screenshot button included. On a
  game's Hotkeys and Screenshot page, after the hold, A keeps the buttons for this game
  and Z gives them to every game: the ones given their own earlier included, and games
  added later. A game's own set afterwards still wins for it. The menu's Save State
  Hotkeys page sets the same every-game buttons and has the screenshot button now. The
  frame step button may share buttons with the hotkeys, as before, but not with the
  screenshot button, which fires at Step speed too. Asked for by DEFAULTDNB.
- A game's options menu stays open after a setting, back at its top row, and comes back
  after the Hotkeys and Screenshot and Manage saves pages, for the next change.
- The Virtual Controller Paks view no longer crashes on a real pak holding a note whose
  name or extension has a byte outside the pak's character set. The menu showed its
  assertion screen ("cannot convert invalid N64CP char") the moment the view opened:
  the view handed the raw bytes to libdragon's path formatter, which refuses such a
  byte, where libdragon itself replaces them when it reads a pak. The set has codes for
  space, digits, capitals, a few marks and katakana; a game or a tool had written
  something else into that note. The byte shows as `?` now and the note copies as it
  is. Reported by Tom Scro, with the dump that named it.
- A controller whose Controller Pak cannot come out, such as a Nintendo Switch Online
  N64 controller through a BlueRetro adapter, presents a pak the launch refuses in the
  virtual pak's port. The notes now say its Rumble Pak mode is the way around, which is
  how Tom got past it.
- A virtual pak file of the wrong size is left alone. A file copied into
  `sd:/savestates/paks` that is not a plain 32 KiB image (a multi-bank image, a DexDrive
  dump with its header) was replaced by a fresh pak at the next launch; now the launch
  stops and says what it found, and only an empty file, from a power cut while it was
  made, is remade.
- The notes say when a game's pak writes reach the card (about a second and a half
  after the last one, the LED blinking) and that the console must stay on for that.

## 0.3.4-ss1.6 (2026-09-19)

- The vertical jitter with the virtual pak on is fixed: Tony Hawk's Pro Skater 3 at its
  boot and in its menus, Perfect Dark at its logos, in their high-resolution modes.
  With Slow motion off the routine runs from the cartridge, slowly, and three things
  it did around a pak transaction held the console's interrupts too long there, so the
  game's video interrupt came up to 3 ms late, past the vertical blank of a high-
  resolution mode: a walk over the memory after the game's controller block, looking
  for buttons that were not there; the checksum of every pak read, computed a byte at
  a time; and a wait for the game's own cartridge transfer to end before the read could
  be answered. The walk is gone, the checksums come from a table the menu prepares
  when it loads the pak, and the wait is a hand-off: the transfer's end brings the
  read back. A pak presence check, which some games make a hundred times a second, is
  answered without the rest of the machinery. Reads are cheap now, so the logos and the
  menus hold still. A write to the pak still costs the routine about a millisecond from
  the cartridge, so a game that writes to its pak as it starts keeps a mild jitter for a
  second or two there and nowhere else: THPS3 tests the pak with fifty writes at its
  legal screens, Perfect Dark touches its save at its logos. With the virtual pak off
  for the game that goes too. THPS3 adds one late frame of its own at its pak check, by
  switching interrupts off there.
- Tony Hawk's Pro Skater 3 refuses Slow motion, as the other games that use all of the
  Expansion Pak do: its high-resolution mode did not boot with it on.

## 0.3.4-ss1.5 (2026-09-19)

- The freeze after a load is fixed. Since 1.2 a state could be saved while the RDP was
  still finishing the frame: the test that picks the save moment had stopped counting
  the RDP's pipe flag, a change made for libdragon homebrew, whose frames never clear
  it, but applied to every game. In such a state the frame's final interrupt was still
  owed to the game, and a load never delivered it, so the game waited forever: Wave
  Race 64 on about one load in three, Banjo-Kazooie on one in five. The flag counts
  again for games that clear it, so the moment comes once the frame is done, as in
  1.1; libdragon games are unchanged. States saved by 1.2 to 1.4 at such a moment load
  as well: the load runs the frame's closing command itself and the interrupt arrives.
  The files do not change; hook version 13 marks the states written with the new rule.
- The Menu information screen (Start, then Menu information) opens with the save
  states release, this repository and the base menu's version, so a report can quote
  the line; the build timestamp below it pins the build.

## 0.3.4-ss1.4 (2026-09-18)

- ROM autoload is back. In a ROM's information screen, R, then "Set ROM to autoload",
  and the console boots straight into that game from then on, with the routine armed
  as on any launch. Hold Start while switching the console on to get the menu back;
  that switches autoload off as well. Upstream dropped it in 0.3.0 for Fast Reboot and
  has brought it back on its develop branch, and this build follows: the Fast Reboot
  setting is gone with it, as there.
- Banjo-Kazooie booted with a black sky and a thin strip of ground in 1.2 and 1.3,
  with Save States on and Slow motion off. Since 1.2 the routine kept a counter on a
  word in low memory that the game's copy protection checks against its boot code,
  and a wrong word there makes the game sabotage its own perspective setup. The
  counter lives elsewhere now. With Slow motion on the routine is placed differently
  and the game was fine, which is how it went unnoticed here. Reported by Acurrz.
- The video reset before a game boots switches the VI off as well, the state it is in
  at power-on, for every ROM that gets the reset. libdragon homebrew built on the
  stable branch before it shipped its own boot code carries Nintendo's, so 1.3 gave
  it the full reset and its display setup waited forever for a vblank. With the VI
  off it does not wait.
- Known, not fixed here: a load can freeze the game when its state was saved while
  the RDP was still finishing a frame, which a load cannot reproduce. Wave Race 64
  hits it most, about one save in three in its attract demo in testing, Banjo-Kazooie
  once in five, Super Mario 64 never. The cause is understood; the fix, a better
  choice of the save moment, needs the full survey behind it and comes next.

## 0.3.4-ss1.3 (2026-09-18)

- Built on N64FlashcartMenu 0.3.4. Its one change resets the video registers
  before a game boots, which fixed crashes in Ocarina of Time upstream. A ROM
  built with libdragon skips that reset: libdragon's display setup waits for a
  vblank when it finds the VI switched on (the stable branch since August 2023,
  the preview branch until April 2025), and with every timing zeroed that vblank
  never comes, so FlappyBird stayed on a blank screen. Newer preview builds cope.
  A ROM with Nintendo's boot code still got the full reset, so a stable-branch
  build from before October 2024 stayed blank; 1.4 covers those.
- Saves move between a game's virtual Controller Pak and a real one from the
  menu: single notes either way, or whole paks, from any port. Start in the
  browser, Virtual Controller Paks, or a game's options, Virtual Controller Pak,
  Manage saves. The real pak's contents are backed up to `sd:/cpak_saves` before
  it is first changed. Asked for by Tom Scro on Discord.
- The Virtual Controller Pak's port list shows what is plugged into each port,
  live, and refuses a port holding a real Controller Pak; a launch with one in the
  virtual pak's port is refused as well, since the game's writes would land in the
  real pak too. With 1.2 that launch went ahead: a pak game played with the
  virtual pak on port 1 and a real Controller Pak in port 1 could leave its writes
  in the real pak. A Rumble Pak there is noted and allowed: the panel can pull the
  virtual pak out when the game wants the rumble.
- The screenshot button could kill the game's sound, or the game, when its tap fell
  on the instant a cartridge read of the game's own had just finished: the copy of
  the picture swallowed that completion, and whatever waited for it waited for
  good. A few quick taps in a row found that instant before long. A screenshot is
  taken at the same kind of moment as a save now.
- AeroGauge, BattleTanx, BattleTanx: Global Assault and Cruis'n World work; the
  compatibility list said otherwise. 280 of 298 now.

## 0.3.3-ss1.2 (2026-09-17)

- The slot panel opens with R + Z + Start now. L + R + Start never worked on an
  original controller, which takes it as the stick reset and drops the Start.
  Reported, with the fix, by drumstix576. The hotkeys page refuses that combo.
- Hotkeys can be changed: in the menu settings for every game, or per game in the
  ROM's options under "Hotkeys and Screenshot". One button or more each. The game
  doesn't see the buttons while a hotkey is held.
- Screenshot button, set per game. A tap writes the screen to
  `sd:/screenshots/<game>/` as a PNG named with the cartridge clock's time. The
  game never sees the button. Hundreds fit per launch; they wait in one file that
  the menu sorts out on its next start.
- The virtual Controller Pak's port is a setting: Port 1 to 4, or Off. On the
  panel's Game page the pak can be pulled out and put back in any port while the
  game runs, and the game is told about it the way the console reports a real
  swap. Beetle Adventure Racing can use a Controller Pak to save and a Rumble Pak
  to race; Perfect Dark takes a Rumble Pak in port 1 with the pak in port 2.
- Save states for homebrew built with libdragon. Tested on about forty ROMs,
  including nineteen of the N64brew Game Jam 2025 entries and most of the SDK
  examples. A ROM with libdragon's pre-2023 entry code, or a boot code the menu
  doesn't know, runs without the routine.
- Frame step: one press lets exactly one frame through, holding the button steps
  ten times a second. L by default. A 30 fps game used to need two presses.
- Slow motion is refused for Donkey Kong 64, Perfect Dark, Indiana Jones and Rush
  2049. They use all of the Expansion Pak, so they never booted with it on. They
  always launch on the cartridge engine now, whatever their ini says.
- Left and right switch the panel's pages, same as L and R.
- Image viewer: L, R, C-Left, C-Right and the D-pad step through the folder, the
  file browser previews the highlighted image, and a background image is scaled
  to fill the screen.
- Fixed: a tap of a hotkey's own button showed the game L and R released and
  pressed again. Mario 64 took it for a camera press.
- Fixed: in 480-line modes the panel and the on-screen text drew a line past the
  end of the frame buffer. A 640x480 game crashed on leaving the panel under slow
  motion.
- Fixed: games that mask an interrupt and poll it instead (libdragon leaves the
  cartridge's pending) blocked every save, load and panel request. Clean moments
  now count only the interrupts the game has enabled, and a request that finds
  none times out instead of holding up later hotkeys.
- Fixed: the RDP's pipe-busy flag blocked saves in games that never send a full
  sync. The routine waits for the command and DMA units only.
- Fixed: after a save, load or panel visit the game resumes on its vblank. The
  first frame after a resume used to show a field late.
- Fixed: with Slow motion on, a state saved with it off could not be read back from
  the card after a power cycle: the panel showed the slot empty and a suspended game
  did not resume. It loads now, as it always did while still in cartridge memory.
- The controller is also read every frame, for games whose controller transfers
  don't raise an exception the routine sees.
- libdragon details: a state carries the RSP's scalar registers (header version
  11); a game whose RSP is busy at every frame is held at the frame boundary until
  its queue runs dry (header version 12); after a load the RDP is put back where
  the saved list left it (BotBoy!64 crashed seconds after every load); debug
  builds that log over USB get their card copies written while the game is held;
  a ROM without a checksum in its header gets one computed from its contents, for
  the state and pak file names.

## 0.3.3-ss1.1 (2026-09-14)

- **Nothing resident any more.** The routine now lives on the cartridge: a gate of a
  few dozen instructions in the exception-vector page and a 16 KiB monitor in the
  cartridge memory, run in place. A save or a load borrows the top 128 KiB of RAM
  for its length and puts the game's bytes back, so games that use every byte of the
  Expansion Pak work (Donkey Kong 64, Perfect Dark, Indiana Jones and the Infernal
  Machine, Rush 2049). The "Hook Placement" option of the interim builds is gone;
  the resident routine of the first release comes back only through the Slow motion
  option below.
- The **virtual Controller Pak** is answered by the monitor from the cartridge memory.
  It is on by default for the games the menu's database marks as Controller Pak users
  and off for the rest (a Rumble Pak in port 1 then works as usual); the per-ROM option
  switches it either way. It answers as a Controller Pak does (the Rumble Pak's
  detection area reads as zeros), and a transaction is served once, at the interrupt
  that brings the PIF's answer, not twice; the four games that ran only with the pak
  off in the sweep (Wetrix, Razor Freestyle Scooter, NASCAR 99, WCW Nitro) run with it
  on now.
- States hold all 8 MiB of RAM plus the RSP's memories and program counter (hook
  v10; files are 16 KiB longer, and files from ss1.0 are made afresh at the game's
  first boot). The game's clock is carried across a freeze; the displayed frame is
  restored last on a load. The card mirror of a state includes the RSP's memories, so
  a state loaded after a power cycle gets them from the card.
- USB: the menu's file push (`send-file`) answers `push ok <bytes>` once the file is
  closed on the card, and reads the whole payload into RAM before writing it (a slow
  card write between two pieces of a payload made the cartridge drop the rest and the
  menu wait for it for ever). A tool that pushes a file must wait for that line before
  it powers the console off: the directory entry is written last, and a cut before it
  leaves a 0-byte file, which the cart's bootloader "loads" without complaint and then
  boots whatever the cartridge memory holds.
- Boot fixes: games that unpack their code at boot (Mario Tennis, Excitebike 64)
  get the watchpoint patch applied after the unpacking; the boot-time RDP kick no
  longer leaves the RDP busy (Wave Race 64, Rayman 2 loads); hi-res titles get a
  shorter panel instead of none.
- **Slow motion** is a per-game option now (off by default): on, the routine stays
  resident in the top 128 KiB of RAM as in the first release, and the panel's Game page
  offers slow motion and frame step. Both placements share one slot layout, so the
  option switches without losing states (a state saved with it on holds 7.75 MiB and
  loads either way); games that use all of the Expansion Pak do not boot with it on.
- GameShark codes keep the engine at its classic place at the top of RAM (as in the
  stock menu); states and the pak ride along with it there.
- The Slow motion option is remembered when switched on, and the Virtual Controller Pak
  when switched off for a game the database marks as a pak user (both used to fall
  back to their defaults at the next visit: the settings writer dropped the key).
- Suspend's resume is armed like a held combo, so the routine is entered on a frame
  with the RCP idle, and a try that finds no such frame is repeated every three
  seconds for half a minute. It used to start on whatever frame the timer fell on and
  give up after one try, which on a busy boot (Super Mario 64's logo) meant no resume
  at all; the development build's own timing hid it.
- A built-in list of titles the menu treats specially, by game code, with nothing to
  set. Since 12 September the engine's watchpoint on the exception vector has covered
  reads as well as writes (Indiana Jones copies the vector's words and chains to the
  copy); Rare's boot does not survive it (the engine's handling of libultra's vector
  write goes wrong there, in a way not yet understood), so Banjo-Kazooie and GoldenEye
  007 boot with the watchpoint on writes only, as in the first release (`watch_reads=0`
  in a ROM's ini does the same for any other title). GoldenEye 007 also gets the
  cartridge routine with its ten scratch words moved from 0x010 to 0x2A8 in the
  exception page: the game keeps its own TLB refill handler in the first 128 bytes of
  that page, where the routine parks registers on every entry, and died at boot with
  them over it. The virtual pak's stub would land there too, so it gets no virtual pak
  (it has no Controller Pak use).
- Known: Ocarina of Time probes port 1 for a Rumble Pak every few frames when any pak
  answers and slows to a crawl with the virtual pak on; it is off for it by default (the
  game has no Controller Pak use). Games that unpack their code at boot need a per-title
  entry (three known without one).

Tested on hardware from the PC: 298 games launched with save states and the virtual pak on, a save, a load and a look at the picture and the frame rate; 276 work. Not working: AeroGauge, All-Star Baseball 2000, All-Star Baseball 2001, All-Star Baseball 99, BattleTanx, BattleTanx: Global Assault, Cruis'n World, Jeremy McGrath Supercross 2000, Madden NFL 2002, Ms. Pac-Man: Maze Madness, NBA Jam 2000, NBA Jam 99, NFL QB Club 2001, NFL Quarterback Club 2000, NFL Quarterback Club 2001, NFL Quarterback Club 98, NFL Quarterback Club 99, NHL Breakaway 98, Power Rangers: Lightspeed Rescue, South Park: Chef's Luv Shack, WCW Backstage Assault, WCW Mayhem. Working only with the virtual pak switched off: Indiana Jones and the Infernal Machine.

## 0.3.3-ss1.0 (2026-09-06)

Based on N64FlashcartMenu V0.3.3.

- Save states on N64 hardware, no PC: L + R + D-pad Up saves, L + R + D-pad Down
  loads, L + R + Start opens a slot panel with time stamps and thumbnails. See
  `docs/savestates.md`.
- State files are format v2: header first, checksummed, the image length stated
  in the header, written in a torn-safe order to the card (`docs/state-format.md`).
  Later builds will keep reading them.
- Slow motion and frame step from the panel's Game page (L or R): 1/2, 1/4, 1/8 or
  Step (Z advances one frame), with the sound slowed to match (pitch down) or left
  to stutter.
- **Exit to menu** and **Suspend** on the panel's Game page: back to the SC64 menu
  without the reset button, with everything pending written to the card first;
  Suspend saves the slot with a resume mark and the next launch of the game loads
  it by itself.
- A per-ROM **Virtual Controller Pak**: the game sees a Controller Pak in port 1,
  answered from a 32 KiB image and saved to `sd:/savestates/paks/`. The answers are
  given from inside the game's own interrupt handler (four instructions of
  libultra's SI acknowledge are patched at boot), which is what makes every
  transfer land; games with another handler get a best-effort answer at the
  interrupt.
- A per-ROM **Save States** option in the ROM's options menu, stored as
  `savestates_enabled` in the ROM's `.ini`, independent of **Use Cheats**.
- States live in the cartridge memory above the ROM (7 slots for ROMs up to 8 MiB,
  6 up to 16 MiB, 4 for 32 MiB, 3 for 40 MiB) and are mirrored to
  `sd:/savestates/` so they survive power cycles.
- Boot integration for the resident hook: engine placement in the exception-vector
  page with a reinstall stub for games that clear RAM at boot, a boot patch for
  games that disarm the engine's watchpoint (libultra 2.0K+), `osMemSize` set for
  Expansion Pak games, and the cheat engine kept working alongside.
- Fixes found on the way: the 6101 IPL3 patch offset (Star Fox 64 booted without
  the engine), the `I_J` macro's unparenthesised argument, `.datel` code files
  loaded at boot without a visit to the code editor, and "Clear RDRAM on boot"
  honoured with cheats installed.

Tested: Super Mario 64, Mario Kart 64, Star Fox 64, F-Zero X, Ocarina of Time,
Super Smash Bros., GoldenEye 007, Banjo-Kazooie, Banjo-Tooie, Diddy Kong Racing,
Kirby 64, Yoshi's Story, Mario Party, Wave Race 64, Paper Mario, Pokémon Stadium,
Turok 2. Not working: Donkey Kong 64, Perfect Dark (they use all of the Expansion
Pak).

---
# Release Notes

- For the SummerCart64, use the `sc64menu.n64` file in the root of your SD card.
- For the 64Drive, use the `menu.bin` file in the root of your SD card.
- For the ares emulator, use the `N64FlashcartMenu.n64` file.

## Release Notes 202x-vNext

- **New Features**
	- ~~Browser now allows hiding files and folders with hidden attributes set (thanks [Xeroxxx](https://github.com/Xeroxxx)).~~ Awaiting performance enhancement.

- **Bug Fixes**

- **Documentation**

- **Refactor**

- **Other**

### Breaking changes
- None.

### Notes
- None.

### Current known Issues
- Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is fully implemented.
- Some users have reported crashes in Zelda OOT (anti-piracy checks). Menu V0.2.0 works as expected.
- PixelFX HDMI mods may need to be updated to latest FW to support display.

### Deprecation notices
- None.

## Release Notes 2026-09-12 - Tagged 0.3.4

- **New Features**
	- ~~Browser now allows hiding files and folders with hidden attributes set (thanks [Xeroxxx](https://github.com/Xeroxxx)).~~ Awaiting performance enhancement.

- **Bug Fixes**
	- Fix potential crashes in Zelda OOT, The boot function now resets the VI (mainly H-Sync) registers to fix the issue.

- **Documentation**

- **Refactor**

- **Other**

### Breaking changes
- None.

### Notes
- None.

### Current known Issues
- Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is fully implemented.
- PixelFX HDMI mods may need to be updated to latest FW to support display.

### Deprecation notices
- None.

## Release Notes 2026-08-26 - Tagged 0.3.3

- **New Features**
	- Adds Background music.
	- Adds ability to use infinite scroll within the file browser.
	- ROM override submenu now remembers current custom settings.
	- Added extra info to the flashcart information screen to show Button state and voltage on supported carts.
	- Experimental DD swap support.
	- Added console region to the system information screen.
	- Audio (MP3) player now supports FLAC and ID3 metadata (thanks [zelifcam](https://github.com/zelifcam)).
	- Add Emulator ROM override INI and extra emulator extension file type matches.
	- Add progress of save file creation on ROM load.
	- Add ability to override emulator configurations through INI file.
	- Add option to clear RDRAM on a per ROM basis (required for RTYI demo).
	- Add ROM additional metadata (Maximum number of simultaneous players that the game supports).
	- Add warning before loading a ROM requiring an Expansion Pak that isn't present for supported ROMs.
	
- **Bug Fixes**
	- CPak manager: Large Controller Paks with more than 123 pages are now shown properly.
	- CPak notes backup and restore now works with invalid FAT characters.
	- Move CA and KR ROM tv types to NTSC.

- **Documentation**
	- Improved DD section with latest information.
	- Improved MP3 player information.
	- Improved Emulator information with ROM overrides.
	- Minor fixes.

- **Refactor**
	- Optimized PNG handler to work on memory-constrained consoles (without an Expansion Pak).
	- Improved memory usage and OOB fixes across the whole codebase.

- **Other**
	- Added ED64 pseudo state for later consumption.
	

### Breaking changes
- None.

### Notes
- Progress has been made towards disk swapping, but it is still WiP.
- RTYI Demo requires the new "Clear RDRAM on boot" ROM override to work.

### Current known Issues
- Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is fully implemented.
- Some users have reported crashes in Zelda OOT (anti-piracy checks). Menu V0.2.0 works as expected.
- PixelFX HDMI mods may need to be updated to latest FW to support display.

### Deprecation notices
- None.


## Release Notes 2026-05-23 - Tagged 0.3.2

- **New Features**
	- Adds settings to hide cheat and save file types in the browser.
	- Adds ability to display embedded homebrew ROM metadata in ROM info.
	- Adds ability to display Commercial game metadata using ROM DB.
	- Menu settings now know what setting is currently applied.

- **Bug Fixes**
	- Neon64 1Mbit SRAM.
	- Potential buffer overflows.
	- Fixed an issue where large ROMs failed to load in certain circumstances.
	- Fixed a lockup when selecting a game in history when the ROM no longer exists.

- **Documentation**
	- Minor fixes.

- **Refactor**
	- PAL60 (using new libdragon support).
	- ROM view, Age ratings and other metadata now align and support homebrew metadata standard.
	- Menu credits.
	- Disk Drive, disk info view.
	- CPak manager, Added menu option to for notes restore.
	- Replace mini.c INI lib with custom implementation.
	- Browser highlight colour for better display on CRT.

- **Other**
	- Updated libDragon {preview} SDK.
	- Updated miniz lib.
	- Updated minimp3.
	- Add docfx devcontainer.
	- Remove rolling prerelease (all releases to main should be tagged).
	- Added AI instructions to repo.
	- Added an extra build option (run-debug-reboot) that aids debugging remotely without the need for uploading files to the SD card.
	

### Breaking changes
- (as of 2026-03-15) libdragon SDK (and this menu) now requires MI repeat mode support, (supported by latest Ares and Gopher64, A3D also works though needs the latest FW). 

### Notes
- (as of 2026-03-01) libdragon {preview} SDK now compiles ROMs that use EEPROM to conform with OG wait timings by default.
- Progress has been made towards disk swapping, but it is still WiP.

### Current known Issues
- Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
- Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is fully implemented.
- Some users have reported crashes in Zelda OOT (anti-piracy checks). Menu V0.2.0 works as expected.
- A user has reported that the menu fails to load RTYI demo 2. Workaround by not setting a background image.
- PixelFX HDMI mods may need to be updated to latest FW to support display.


### Deprecation notices
- None.


## Release Notes 2025-12-04 - Tagged 0.3.1

- **New Features**
	- Settings contexts now preset to the saved option.
	- Added latest Viewpoint64 final proto ROM to database.
	- Added Rumble PAK and Transfer PAK features to ROM info screen.

- **Bug Fixes**
	- Fixed MP3 Player crashes menu if the MP3 file's sample rate is less than 44100 hz and menu SFX are enabled.
	- Fixed game_code_path size that caused crash when loading homebrew boxart.
	- Fixed boot process which could lead to blank screens or crashes.
	- Fixed a potential issue that could happen when a RTC was not detected.


- **Documentation**
	- Moved ED64 documentation to [98_flashcart_wip.md](./docs/98_flashcart_wip.md)
	- Other minor fixes.

- **Refactor**
	- Output 4MB files as MB, rather than kB.
	- Improved icons for direction.
	- Controller Pak now selects notes using up/down rather than left/right.

- **Other**
	- Updated libDragon SDK.
	- Updated docker container to Trixy

### Breaking changes
- None.


### Current known Issues
- Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
- Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is implemented.
- Some users have reported crashes in Zelda OOT (anti piracy checks). Menu V0.2.0 works as expected.
- A user has reported that the menu crashes with a CPU exception. Menu V0.2.0 works as expected.


### Deprecation notices
- None.


## Release Notes 2025-11-15 - Tagged 0.3.0

- **New Features**
	- Added ability to hide save folders (on by default).
	- Added ability to reset the menu setting to default from the menu UI.
	- Updated the UI font to Firple-Bold which supports more characters.
	- Shows info message within the loading progress bar.
	- Add the ability to display ESRB age ratings (see [documentation](./docs/65_experimental.md)).
	- Add Beta Datel code GUI (see [documentation](./docs/13_datel_cheats.md)).
	- Add ability to load boxart from ROMs that use the homebrew header (see [documentation](./docs/19_gamepak_boxart.md)).
	- Add ability to extract files from ZIP archives (thanks [VicesOfTheMind](https://github.com/VicesOfTheMind)).
	- Add Alpha FEATURE_PATCHER_GUI_ENABLED (build flag to enable it).
	- Add Controller Pak manager (thanks [LuEnCam](https://github.com/LuEnCam))
	- Add Game art image switching (thanks [dpranker](https://github.com/dpranker))

- **Bug Fixes**
	- Fix ability to set the RTC via menu (Hotfixed in last release).
	- Fix Game ID (used by PixelFX HDMI mods) sent over Joybus is not working (Hotfixed in last release).
	- Fix GB / GBC emulator not saving in certain circumstances (Hotfixed in last release).
	- Fix issue with emulation of cold boot, as otherwise the FPU might start in an unexpected state.
	- Fix missing enum case for 1 Mbit SRAM saves (Hotfixed in last release).

- **Documentation**
	- Improved Emulator information for known working NES emulator version.
	- Updated experimental features to reflect feature change.
	- Added sounds documentation.
	- Updated autoload to reflect feature change.

- **Refactor**
	- Improve tab navigation by using any left/right control input and add cursor SFX.
	- Add ability for font style to be used in ui_components_main_text_draw and ui_components_actions_bar_text_draw.

- **Other**
	- Updated libDragon SDK.
	- Updated miniz library.
	- Updated Github templates.

### Breaking changes
* Deprecated "Autoload ROM" function was removed from menu (use `FEATURE_AUTOLOAD_ROM_ENABLED` as a build flag to re-enable it).
* Deprecated Boxart image handler was removed (see [documentation](./docs/19_gamepak_boxart.md) for new boxart link).
* ROM's that used custom CIC, TV and/or Save type set from the menu will need to re-set them, now uses "custom_boot" header within the ini file.


### Current known Issues
* Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
* Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is implemented.
* MP3 Player crashes menu if the MP3 file's sample rate is less than 44100 hz and menu SFX are enabled.
- Some users have reported crashes in Zelda OOT (anti piracy checks). Menu V0.2.0 works as expected.
- A user has reported that the menu crashes with a CPU exception. Menu V0.2.0 works as expected.


### Deprecation notices
* Boxart directory has changed to metadata directory.


## Release Notes 2025-03-31 - Tagged 0.2.0

- **New Features**
	- Introduced tabs in main menu for ROM favorites and recently played ROM history.
	- Introduced first run check to ensure users are aware of latest changes.
	- Introduced ability to turn off GUI loading bar.
	- BETA_FEATURE: Introduces ROM descriptions from files.
	- BETA_FEATURE: Enabled setting for fast ROM reboots on the SC64.
	- Add macOS metadata to hidden files.
	- Added settings schema version for future change versioning.
	- Added setting for PAL60 compatibility mode (see breaking changes).
	- BETA_FEATURE: Added setting for line doublers that need progressive output, enable using "force_progressive_scan" setting in `config.ini`.


- **Bug Fixes**
	- Menu sound FX issues (hissing, popping and white noise).
	- RTC not showing or setting correct date parameters in certain circumstances.
	- ~~GB / GBC emulator not saving in certain circumstances.~~


- **Documentation**
	- Re-orginised and improved user documentation.
	- Added a lot of doxygen compatible code comments.
	- Added project license.


- **Refactor**
	- RTC subsystem (align with libDragon improvements).
	- Boxart images (Deprecates old boxart image folder layout).
	- Settings (PAL60 compatibility, schema version, fast reboot, first run, progress bar).

- **Other**
	- Updated libDragon SDK.
	- Updated miniz library.

### Breaking changes
* ~~GB /GBC emulator changed save type to SRAM (from FRAM) to improve compatibility with Summercart64 (which only uses H/W compatible FRAM), this may break your ability to load existing saves.~~
* For similar PAL60 functionality, you may need to also enable the new "pal60_compatibility_mode" setting in `config.ini`.


### Current known Issues
* The RTC UI requires improvement (awaiting UI developer).
* Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
* Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is implemented.
* MP3 Player crashes menu if the MP3 file's sample rate is less than 44100 hz.


### Deprecation notices
* Autoload ROM's will be deprecated in favor of Fast Reboot in a future menu version.
* Old boxart images using filenames for game ID is deprecated and the compatibility mode will be removed in a future release.


## Release Notes 2025-01-10

- **Bug Fixes**
	- Fixed menu display (PAL60) by reverted libdragon to a known working point and re-applying old hacks.

### Current known Issues
* The RTC UI requires improvement (awaiting UI developer).
* Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
[Pre-release menu]:
* BETA_SETTING: PAL60 when using HDMI mods has regressed (awaiting libdragon fix).
* ALPHA_FEATURE: ED64 X Series detection does not occur properly (however this is not a problem as not tag released asset).
* ALPHA_FEATURE: ED64 V Series only supports loading ROMs (however this is not a problem as not tag released asset).


## Release Notes 2024-12-30

- **New Features**
	- Introduced menu sound effects for enhanced user experience (the default is off).
	- Added N64 ROM autoload functionality, allowing users to set a specific ROM to load automatically.
	- Added menu boot hotkey (hold `start` to return to menu when autoload is enabled).
	- Added context menu and settings management options GUI for managing various settings in `config.ini`.
	- Added functionality for editing the real-time clock (RTC) within the RTC menu view.
	- Improved flashcart info view for showing supported flashcart features and version.
	- Enhanced UI components with new drawing functions and improved organization.
	- Added emulator support for `SMS`, `GG`, and `CHF` ROMs.
	- Enhanced joypad input handling for menu actions, improving responsiveness.
	- Optimized boxart image loading from filesystem.
	- Improved various text to make the functionality more clear.

- **Bug Fixes**
	- Improved error handling in multiple areas, particularly in save loading and ROM management.
	- Enhanced memory management to prevent potential leaks during error conditions.
	- Fixed text flickering in certain circumstances.

- **Documentation**
	- Updated README and various documentation files to reflect new features and usage instructions.
	- Added detailed setup instructions for SD cards and menu customization.
	- Enhanced clarity in documentation for RTC settings and menu customization.
	- Improved organization and clarity of SD card setup instructions for various flashcarts.

- **Refactor**
	- Standardized naming conventions across UI components for better organization.
	- Restructured sound management and input handling for improved responsiveness.
	- Streamlined the loading state management for ROMs and disks within the menu system.
	- Improved clarity and usability of the developer guide and other documentation files.

### Current known Issues
* BETA_SETTING: PAL60 when using HDMI mods has regressed (awaiting libdragon fix).
* The RTC UI requires improvement (awaiting UI developer).
* Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
* ALPHA_FEATURE: ED64 X Series detection does not occur properly (however this is not a problem as not tag released asset).
* ALPHA_FEATURE: ED64 V Series only supports loading ROMs (however this is not a problem as not tag released asset).

### Breaking changes
* Disk drive expansion ROMs are now loaded with `Z|L` instead of `R` to align with ROM info context menu (and future functionality).
