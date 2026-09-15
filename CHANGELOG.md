# Save states (SummerCart64 fork)

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
