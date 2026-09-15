# Save states on the SummerCart64

This build of N64FlashcartMenu can save and load the complete state of a running
game on a real Nintendo 64, from the controller, with nothing attached to the
console. A state is the whole machine: every byte of RAM, the CPU and coprocessor
registers, the pending interrupts, the video timing, the RSP's memories and the
game's own clock. Load one and the game continues from that exact frame.

Nothing like this existed on the N64 before. The FXPak Pro and EverDrive N8 Pro do
it on the SNES and NES with a resident routine that makes the console copy its own
memory to the cartridge; this is the same idea on a machine that never had it, with
one twist: nothing stays resident in the console's memory. The routine lives on the
cartridge and borrows a corner of RAM only for the length of each save or load, so
games that use every byte of the Expansion Pak (Donkey Kong 64, Perfect Dark,
Indiana Jones) work too.

It also gives games that use one a **virtual Controller Pak**, so games that save to
a pak save to the SD card whether or not a pak is plugged in, and it keeps the first
release's slow motion and frame step as a per-game option.

Tested on 298 games so far; 276 work (see
[Compatibility](#compatibility)).

## What you need

- A SummerCart64.
- An Expansion Pak. The top 128 KiB of it is the working room the routine borrows
  during a save or a load (and gives back, byte for byte, before the game runs
  again), so states are not possible on a 4 MiB console.
- ROMs up to 48 MiB. The states are kept in the cartridge memory above the ROM;
  the bigger the ROM, the fewer slots fit, and a 64 MiB ROM leaves no room at all
  (the option refuses to switch on for those, and such a game gets no virtual pak
  either).
- Free space on the SD card: about 8 MiB per slot per game, allocated the first time
  a game boots with save states on (48 MiB for a game with six slots), and 32 KiB
  per game for its virtual pak.

## Install

Copy `sc64menu.n64` from the release to the root of the SD card. Keep a copy of your
current one somewhere; putting it back is the whole uninstall. Everything else about
the menu is unchanged.

## Switch it on for a game

Select the ROM, open its options (the menu you get with the ROM highlighted, the
same place as "Use Cheats"), choose **Save States**, then **Enabled**. The setting
is stored in the ROM's `.ini` file next to it (`savestates_enabled=1`), so it stays
on for that game. It is off until you switch it on, because the first boot with it
on creates the game's slot files on the card.

**Virtual Controller Pak** is in the same menu. It is **on by default for the games the
menu's database marks as Controller Pak users** (Mario Kart 64, Perfect Dark, Turok,
the wrestling games...) and off for the rest, since a game with no pak use gains nothing
from one and a Rumble Pak in port 1 keeps working. Switch it on for a game the database
misses, or off for a game where you want a real pak (`vpak_enabled=1` or `0` in the
ROM's `.ini`).

**Slow motion** is in the same menu, off by default. On, the routine stays resident in
the top 128 KiB of RAM instead of living on the cartridge, which is what lets it hold
the game between frames: the panel's Game page then offers slow motion and frame step.
Everything else is the same either way, and the two placements share one slot layout,
so you can switch it on and off for a game without losing its states (a state saved
with it on holds the RAM below the routine, 7.75 MiB, and loads either way). The one
cost: games that use every byte of the Expansion Pak (Donkey Kong 64, Perfect Dark,
Indiana Jones and the Infernal Machine, San Francisco Rush 2049) do not boot with it
on; switch it back off for those. Stored as `hook_borrowed=0` in the ROM's `.ini`.

Boot the game normally. Save states and cheats can be on at the same time; with
GameShark codes loaded the engine sits at its classic place at the top of RAM, as in
the stock menu, and the states and the pak ride along with it.

## In the game

| Buttons (hold about a fifth of a second) | What happens |
| --- | --- |
| L + R + D-pad Up | Save the current slot |
| L + R + D-pad Down | Load the current slot |
| L + R + Start | Open the slot panel |

The game freezes for about two seconds on a save and about a second and a half on a
load, then carries on. "STATE SAVED" or "STATE LOADED" shows for a moment in the
bottom-left corner. After a save the cartridge LED blinks for a couple of seconds
while the state is copied to the SD card; you can keep playing meanwhile.

The panel lists the slots with the date and time of each state and a thumbnail of
the selected one. Up and down (D-pad or stick) pick a slot, which also becomes the
slot the quick combos use. **A** loads it, **Z** saves into it (an occupied slot
asks for confirmation), **B** or **Start** closes the panel. States that only exist
on the SD card (after a power cycle) are read back in when the panel opens or when
a load asks for them.

**L** or **R** switches to the panel's second page, **Game**:

- **Exit to menu**: back to the SC64 menu without touching the reset button.
  Anything still on its way to the card is written first (a state being mirrored,
  the virtual pak's last writes, the game's own save), then the cartridge's
  bootloader is switched back in and started, the path a reset takes. **A** asks
  for confirmation.
- **Suspend to slot N**: saves the game into the selected slot with a resume mark,
  then exits as above. The next launch of that game loads the slot by itself about
  a second after the game is up, then clears the mark; the state stays in the slot
  like any other. **A** asks for confirmation (an occupied slot is overwritten).
- **Speed** and **Sound**: slow motion at 1/2, 1/4 or 1/8, or frame step (Z advances
  one frame), with the sound slowed to match (pitch down) or left to stutter. These
  need the **Slow motion** option switched on for the game (see above); with it off
  the two rows say so and do nothing.

How many slots a game gets depends on its ROM size:

| ROM size | Slots |
| --- | --- |
| up to 4 MiB | 7 |
| 8 and 12 MiB | 6 |
| 16 MiB | 5 |
| 24 MiB | 4 |
| 32 MiB | 3 |
| 40 MiB | 2 |
| 48 MiB | 1 |
| 64 MiB | none |

States survive power cycles: each slot is mirrored to
`sd:/savestates/<checkcode>.st<slot>` on the card. The files are plain copies of
the state (a header and the thumbnail, then all 8 MiB of RAM and the RSP's memories;
see [state-format.md](state-format.md)), so they can be backed up or moved to another
card. A state is bound to the exact ROM it was made with and is refused for any
other. The format is versioned and later builds will keep reading these files.
(State files made by the earlier 0.3.3-ss1.0 build are 16 KiB shorter; this build
makes them afresh the first time the game boots, so states from that build are not
carried over.)

## Virtual Controller Pak

With **Virtual Controller Pak** on (the default for games that use one), the game sees
a Controller Pak in port 1 whether or not one is plugged in: the game's pak reads and writes are
answered from a 32 KiB image in the cartridge memory and the changes go to
`sd:/savestates/paks/<checkcode>.pak` on the card a moment after the game writes them.
The file is a plain pak image (one bank), so the menu's Controller Pak tools can
open it, and a fresh one is formatted the first time a game boots with the option
on. One pak per game.

The answers are given from inside the game's own interrupt handler. At boot the
routine finds the place in libultra's exception handler where a finished controller
transfer is acknowledged and patches four instructions there to call the cartridge
first. That is the only way to catch every transfer: the game's handler services
several finished transfers in one pass, and a pak command answered any later is
read by the game as "no pak" (the symptoms are a save that fails now and then, or a
pak reported as changed or damaged). A game with a different handler falls back to
being answered at the interrupt itself, which works most of the time but not
always; the menu shows nothing about which case a game is, so if a pak misbehaves
in a game not listed below, that is the likely reason.

While it is on, a real Controller Pak or Rumble Pak in port 1 is not seen by the
game, since the cartridge answers first. Turn it off for games where you want
rumble. The virtual pak answers as a Controller Pak does (reads in the Rumble Pak's
detection area come back as zeros), so a game that supports both takes it for a
Controller Pak and does not try to rumble it.

## Compatibility

Every game below was launched from the PC with save states and the virtual pak on, then
given a save, a load, and a look at the picture and the frame rate before and after;
the ones marked as tried by hand also had the combos, the panel, and loads across level
and scene changes. The virtual pak was on for every game in this pass; in the release it
is on by default only for the games the database marks as Controller Pak users (a game
that probes port 1 for a Rumble Pak every few frames, Ocarina of Time for one, slows down
badly when any pak answers, and gains nothing from one). Two games
are on a built-in list the menu applies by itself: Banjo-Kazooie and GoldenEye 007 boot
with the engine's watchpoint on writes only (Rare's boot does not survive the watch on
reads that other games need; `watch_reads=0` in a ROM's ini does the same for any other
title), and GoldenEye 007 gets the cartridge routine with its ten scratch words moved
(the game keeps its own TLB handler at the start of the exception page, where the
routine usually parks registers) and no virtual pak (the pak's stub would land in that
handler too; the game has no Controller Pak use).

| Game | Result | Notes |
| --- | --- | --- |
| 007: The World Is Not Enough | works |  |
| A Bug's Life | works |  |
| Aerofighter's Assault | works |  |
| AeroGauge | does not work | a save or a load hangs the game |
| Aidyn Chronicles: The First Mage | works |  |
| All-Star Baseball 2000 | does not work | black screen at boot with the engine in place |
| All-Star Baseball 2001 | does not work | black screen at boot with the engine in place |
| All-Star Baseball 99 | does not work | a save or a load hangs the game |
| Armorines: Project S.W.A.R.M. | works |  |
| Army Men: Air Combat | works |  |
| Army Men: Sarge's Heroes | works |  |
| Army Men: Sarge's Heroes 2 | works |  |
| Automobili Lamborghini | works |  |
| Banjo-Kazooie | works |  |
| Banjo-Tooie | works |  |
| Bassmasters 2000 | works |  |
| Batman Beyond: Return of the Joker | works |  |
| BattleTanx | does not work | the game unpacks its code at boot and the engine never arms; needs a per-title entry |
| BattleTanx: Global Assault | does not work | the game unpacks its code at boot and the engine never arms; needs a per-title entry |
| Battlezone: Rise of the Black Dogs | works | the loaded screen needed a Start press to move |
| Beast Wars Transmetal | works |  |
| Beetle Adventure Racing! | works | the loaded screen needed a Start press to move |
| Big Mountain 2000 | works |  |
| Bio F.R.E.A.K.S. | works |  |
| Blast Corps | works |  |
| Blues Brothers 2000 | works |  |
| Body Harvest | works |  |
| Bomberman 64: The Second Attack! | works |  |
| Bomberman Hero | works |  |
| Bottom of the 9th | works |  |
| Brunswick Circuit Pro Bowling | works |  |
| Buck Bumble | works |  |
| Bust-A-Move '99 | works |  |
| Bust-A-Move 2: Arcade Edition | works |  |
| California Speed | works |  |
| Carmageddon 64 | works |  |
| Castlevania | works |  |
| Castlevania: Legacy of Darkness | works |  |
| Chameleon Twist | works | the loaded screen needed a Start press to move |
| Chameleon Twist 2 | works |  |
| Charlie Blast's Territory | works | the loaded screen needed a Start press to move |
| Chopper Attack | works |  |
| Clay Fighter 63 1-3 | works |  |
| Clay Fighter: Sculptor's Cut | works |  |
| Command & Conquer | works |  |
| Command and Conquer 3D | works |  |
| Conker's Bad Fur Day | works | 64 MiB ROM: boots as in the stock menu, no states, no pak |
| Cruis'n Exotica | works |  |
| Cruis'n USA | works |  |
| Cruis'n World | does not work | a save or a load hangs the game |
| CyberTiger | works | the picture could not be checked from the PC (its screen mode); save and load ran |
| Dark Rift | works |  |
| Deadly Arts | works |  |
| Destruction Derby 64 | works |  |
| Diddy Kong Racing | works |  |
| Disney's Donald Duck: Goin' Quackers | works |  |
| Disney's Tarzan | works |  |
| Donald Duck: Goin' Quackers | works |  |
| Donkey Kong 64 | works |  |
| Doom 64 | works |  |
| Dr. Mario 64 | works |  |
| Dual Heroes | works |  |
| Duck Dodgers Starring Daffy Duck | works |  |
| Duke Nukem 64 | works |  |
| Duke Nukem: Zero Hour | works |  |
| Earthworm Jim 3D | works |  |
| ECW Hardcore Revolution | works |  |
| Elmo's Letter Adventure | works |  |
| Excitebike 64 | works | the loaded screen needed a Start press to move |
| Extreme-G | works |  |
| F-1 World Grand Prix | works |  |
| F-Zero X | works |  |
| F1 Pole Position 64 | works | the loaded screen needed a Start press to move |
| FIFA 99 | works | the loaded screen needed a Start press to move |
| FIFA Soccer 64 | works |  |
| FIFA: Road to World Cup 98 | works |  |
| Fighter's Destiny 2 | works |  |
| Fighters Destiny | works |  |
| Fighting Force 64 | works |  |
| Flying Dragon | works |  |
| Forsaken 64 | works |  |
| Fox Sports College Hoops '99 | works |  |
| Frogger 2 | works |  |
| Gauntlet Legends | works |  |
| Gex 3: Deep Cover Gecko | works |  |
| Gex 64: Enter the Gecko | works |  |
| Glover | works |  |
| Glover 2 | works |  |
| Goemon's Great Adventure | works |  |
| Golden Nugget 64 | works |  |
| GoldenEye 007 | works | the menu gives it the cartridge routine with its scratch words moved (the game keeps its own TLB handler where they usually go) and no virtual pak; the game has no Controller Pak use |
| GT64: Championship Edition | works |  |
| Harvest Moon 64 | works |  |
| Hercules: The Legendary Journeys | works | the loaded screen needed a Start press to move |
| Hexen | works |  |
| Hey You, Pikachu! | works | the loaded screen needed a Start press to move |
| Hot Wheels Turbo Racing | works |  |
| Hybrid Heaven | works |  |
| Hydro Thunder | works |  |
| Iggy's Reckin' Balls | works |  |
| In-Fisherman: Bass Hunter 64 | works |  |
| Indiana Jones and the Infernal Machine | works | switch the Virtual Controller Pak off for this game (it fails with it on); the picture could not be checked from the PC (its screen mode); save and load ran |
| Indy Racing 2000 | works |  |
| International Superstar Soccer '98 | works |  |
| International Superstar Soccer 2000 | works |  |
| International Superstar Soccer 64 | works |  |
| International Track and Field 2000 | works |  |
| Jeopardy! | works |  |
| Jeremy McGrath Supercross 2000 | does not work | black screen at boot with the engine in place |
| Jet Force Gemini | works |  |
| John Romero's Daikatana | works | the loaded screen needed a Start press to move |
| Ken Griffey Jr.'s Slugfest | works |  |
| Killer Instinct Gold | works | the picture could not be checked from the PC (its screen mode); save and load ran |
| Kirby 64: The Crystal Shards | works |  |
| Knife Edge: Nose Gunner | works |  |
| Knockout Kings 2000 | works |  |
| Kobe Bryant's NBA Courtside | works |  |
| Legend of Zelda, The: Ocarina of Time | works | the virtual pak is off for it by default (the game has no Controller Pak use); with it on the game probes port 1 for a Rumble Pak every few frames and slows to a crawl |
| LEGO Racers | works |  |
| Lode Runner 3-D | works |  |
| Mace: The Dark Age | works |  |
| Madden Football 64 | works |  |
| Madden NFL 2000 | works |  |
| Madden NFL 2001 | works | the loaded screen needed a Start press to move |
| Madden NFL 2002 | does not work | the game unpacks its code at boot and the engine never arms; needs a per-title entry |
| Madden NFL 99 | works |  |
| Magical Tetris Challenge | works |  |
| Major League Baseball featuring Ken Griffey Jr. | works |  |
| Mario Golf | works |  |
| Mario Kart 64 | works |  |
| Mario Party | works |  |
| Mario Party 2 | works |  |
| Mario Party 3 | works |  |
| Mario Tennis | works |  |
| Mega Man 64 | works |  |
| Mia Hamm Soccer 64 | works | the picture could not be checked from the PC (its screen mode); save and load ran |
| Mickey's Speedway USA | works |  |
| Micro Machines 64 Turbo | works |  |
| Midway's Greatest Arcade Hits Volume 1 | works |  |
| Mike Piazza's StrikeZone | works |  |
| Milo's Astro Lanes | works |  |
| Mini Racers | works |  |
| Mischief Makers | works |  |
| Mission Impossible | works |  |
| Monaco Grand Prix | works |  |
| Monopoly | works |  |
| Monster Truck Madness 64 | works |  |
| Mortal Kombat 4 | works |  |
| Mortal Kombat Mythologies: Sub-Zero | works |  |
| Mortal Kombat Trilogy | works | the loaded screen needed a Start press to move |
| MRC: Multi Racing Championship | works |  |
| Ms. Pac-Man: Maze Madness | does not work | the game unpacks its code at boot and the engine never arms; needs a per-title entry |
| Mystical Ninja Starring Goemon | works |  |
| Nagano Olympic Hockey '98 | works |  |
| Nagano Winter Olympics '98 | works |  |
| Namco Museum 64 | works |  |
| NASCAR 2000 | works |  |
| NASCAR 99 | works | the loaded screen needed a Start press to move |
| NBA Courtside 2 featuring Kobe Bryant | works |  |
| NBA Hangtime | works |  |
| NBA in the Zone '98 | works |  |
| NBA in the Zone '99 | works |  |
| NBA in the Zone 2000 | works |  |
| NBA Jam 2000 | does not work | black screen at boot with the engine in place |
| NBA Jam 99 | does not work | the engine never runs |
| NBA Live 2000 | works |  |
| NBA Live 99 | works |  |
| NBA Showtime: NBA on NBC | works |  |
| NFL Blitz | works |  |
| NFL Blitz 2000 | works |  |
| NFL Blitz 2001 | works |  |
| NFL Blitz: Special Edition | works |  |
| NFL QB Club 2001 | does not work | black screen at boot with the engine in place |
| NFL Quarterback Club 2000 | does not work | a save or a load hangs the game |
| NFL Quarterback Club 2001 | does not work | black screen at boot with the engine in place |
| NFL Quarterback Club 98 | does not work | black screen at boot with the engine in place |
| NFL Quarterback Club 99 | does not work | black screen at boot with the engine in place |
| NHL 99 | works |  |
| NHL Blades of Steel '99 | works |  |
| NHL Breakaway 98 | does not work | black screen at boot with the engine in place |
| NHL Breakaway 99 | works |  |
| Nightmare Creatures | works |  |
| Nuclear Strike 64 | works |  |
| O.D.T. | works |  |
| Off Road Challenge | works |  |
| Ogre Battle 64: Person of Lordly Caliber | works |  |
| Olympic Hockey 98 | works |  |
| Olympic Hockey Nagano '98 | works |  |
| Paper Mario | works |  |
| Paperboy | works |  |
| Penny Racers | works |  |
| Perfect Dark | works |  |
| PGA European Tour | works |  |
| Pilotwings 64 | works |  |
| Pokemon Puzzle League | works |  |
| Pokemon Snap | works |  |
| Pokemon Snap Station | works |  |
| Pokemon Stadium | works |  |
| Polaris SnoCross | works |  |
| Power Rangers: Lightspeed Rescue | does not work | the game unpacks its code at boot and the engine never arms; needs a per-title entry |
| Powerpuff Girls, The: Chemical X-Traction | works |  |
| Quake 64 | works |  |
| Quake II | works |  |
| Quest 64 | works |  |
| Racing Simulation | works | the loaded screen needed a Start press to move |
| RAINBOW SIX | works |  |
| RALLY CHALLENGE | works |  |
| Rally Challenge 2000 | works |  |
| Rampage 2: Universal Tour | works | the loaded screen needed a Start press to move |
| Rampage: World Tour | works | the loaded screen needed a Start press to move |
| Rat Attack! | works |  |
| Rayman 2: The Great Escape | works | the loaded screen needed a Start press to move |
| Razor Freestyle Scooter | works |  |
| Re-Volt | works | the loaded screen needed a Start press to move |
| Ready 2 Rumble Boxing | works |  |
| Resident Evil 2 | works | 64 MiB ROM: boots as in the stock menu, no states, no pak |
| Road Rash 64 | works |  |
| Roadsters | works |  |
| Robotech: Crystal Dreams | works |  |
| Robotron 64 | works |  |
| Rocket: Robot on Wheels | works |  |
| RR64: Ridge Racer 64 | works |  |
| Rugrats in Paris: The Movie | works | the loaded screen needed a Start press to move |
| Rugrats: Scavenger Hunt | works |  |
| Rush 2: Extreme Racing USA | works |  |
| S.C.A.R.S. | works |  |
| San Francisco Rush 2049 | works |  |
| Scooby-Doo!: Classic Creep Capers | works |  |
| Shadow Man | works |  |
| Shadowgate 64: Trials of the Four Towers | works |  |
| Sin and Punishment: Successor of the Earth | works |  |
| Smash Brothers | works |  |
| Snowboard Kids | works |  |
| Snowboard Kids 2 | works |  |
| South Park | works |  |
| South Park Rally | works |  |
| South Park: Chef's Luv Shack | does not work | black screen at boot with the engine in place |
| Space Invaders | works | the loaded screen needed a Start press to move |
| Spacestation Silicon Valley | works |  |
| Spider-Man | works |  |
| Star Fox 64 | works |  |
| Star Soldier Vanishing Earth | works |  |
| Star Wars Episode I: Racer | works |  |
| Star Wars: Episode 1: Racer | works |  |
| Star Wars: Rogue Squadron | works |  |
| Star Wars: Shadows of the Empire | works |  |
| StarCraft 64 | works |  |
| Starshot: Space Circus Fever | works |  |
| Stunt Racer 64 | works |  |
| Super Bowling 64 | works |  |
| Super Mario 64 | works |  |
| Super Smash Bros. | works |  |
| Supercross 2000 | works |  |
| Superman | works | the loaded screen needed a Start press to move |
| Tetrisphere | works |  |
| The New Tetris | works |  |
| Tigger's Honey Hunt | works |  |
| Tom and Jerry in Fists of Furry | works |  |
| Tom Clancy's Rainbow Six | works |  |
| Tonic Trouble | works |  |
| Tony Hawk's Pro Skater | works |  |
| Tony Hawk's Pro Skater 2 | works |  |
| Tony Hawk's Pro Skater 3 | works |  |
| Top Gear Hyper-Bike | works |  |
| Top Gear Overdrive | works |  |
| Top Gear Rally 2 | works |  |
| Toy Story 2 | works |  |
| Transformers: Beast Wars Transmetal | works |  |
| Triple Play 2000 | works | the loaded screen needed a Start press to move |
| Turok 2: Seeds of Evil | works |  |
| Turok: Dinosaur Hunter | works |  |
| Turok: Rage Wars | works |  |
| Twisted Edge Extreme Snowboarding | works |  |
| V-Rally Edition 99 | works |  |
| Vigilante 8 | works |  |
| Vigilante 8: 2nd Offense | works | the picture could not be checked from the PC (its screen mode); save and load ran |
| Virtual Chess 64 | works |  |
| Virtual Pool 64 | works |  |
| VNES64 + Test Cart | works | the loaded screen needed a Start press to move |
| Waialae Country Club: True Golf Classics | works |  |
| War Gods | works |  |
| Wave Race 64 | works |  |
| Wayne Gretzky's 3D Hockey | works |  |
| Wayne Gretzky's 3D Hockey 98 | works |  |
| WCW Backstage Assault | does not work | black screen at boot with the engine in place |
| WCW Mayhem | does not work | black screen at boot with the engine in place |
| WCW Nitro | works |  |
| WCW vs. nWo: World Tour | works |  |
| WCW-nWo Revenge | works |  |
| Wetrix | works |  |
| Wheel of Fortune | works |  |
| WinBack: Covert Operations | works |  |
| Wipeout 64 | works |  |
| World Cup 98 | works |  |
| World Driver Championship | works |  |
| World is Not Enough, The | works |  |
| WWF No Mercy | works |  |
| Yoshi's Story | works |  |

Games not on the list have simply not been tried yet.

## Known limitations

- **A state does not include the game's own save.** EEPROM, SRAM and FlashRAM
  saves, and anything in a Controller Pak (virtual or real), keep going forward.
  Loading an old state does not rewind the game's save file, and a game that saved
  after the state was made will find that newer save when the state is loaded.
- **Slow motion and frame step need the Slow motion option** switched on for the
  game (see the Game page above); with it on, games that use every byte of the
  Expansion Pak do not boot.
- **Hi-res modes.** In 480-line modes the panel is drawn shorter, and a state saved
  from the panel may show the panel for one frame when it is loaded. The quick
  combos are unaffected.
- **The panel reads the controller in port 1** and needs a standard controller
  there.
- **Freeze length.** Saving copies 8 MiB through the cartridge port at about
  4 MB/s; the two seconds are what the hardware allows. A load is the same copy in
  the other direction.
- **A copy to the card interrupted by a power cut empties that slot**, including
  the state it held before. The alternative, a half-written state that loads,
  is worse. The other slots are untouched.
- **The virtual pak is port 1 only**, replaces whatever is in that port's accessory
  slot while it is on, and its file goes to the card a second and a half after the
  game's last write; a power cut inside that window loses those last writes.
- **A resume shows the game's boot first.** Suspend's resume waits for the game to
  start polling its controller (a second in at least), then for a frame on which the
  RCP is idle; a try that finds none is repeated every three seconds for half a
  minute. So the boot logo, and sometimes a few seconds more, appear before the
  suspended moment does. Exit to menu relies on the SC64's
  own bootloader switch; a cart that refuses it gets "NO EXIT HERE" on the panel,
  with the game untouched.
- **A few games notice.** Some titles check the memory the boot code leaves behind
  or the top of RAM and misbehave with the routine's traces there; the ones found
  so far are listed above.
- 64DD disk images and the emulators the menu can launch are not supported.

## How it works, briefly

The menu already patches the boot code so that the Datel cheat engine runs on
every exception of the game; that is how GameShark codes work on the SummerCart64.
This build hangs a second routine off the same path, and keeps it off the console:

- In the exception-vector page (the first kilobyte of RAM, which every game leaves
  to the CPU's vectors) sits a **gate** of a few dozen instructions. On every
  interrupt it checks whether the video or controller hardware has something to say
  and, if so, jumps to the cartridge.
- On the cartridge, in the memory above the ROM, sits a 16 KiB **monitor**, run in
  place by the CPU. Its tick, a few hundred instructions, reads the controller
  buttons from the game's own controller buffer in RAM (never from the PIF, which
  hangs the joybus if polled at the wrong moment), watches for the combos, answers
  the virtual pak, and otherwise hands the interrupt straight back to the game.
- When a save or a load is due, the monitor waits for a clean moment (a frame
  boundary with the RSP halted, the RDP idle and no cartridge or controller
  transfer in flight), copies the top 128 KiB of RAM to a stash on the cartridge,
  copies the 110 KB **hook** in its place, and runs it. The hook captures the CPU
  context (all registers, the FPU, the TLB, Status and EPC), the pending interrupt
  mask, the RDP and RSP status bits, the DMA address registers, the video registers
  and the CPU cycle counter, copies RAM to the cartridge with the PI DMA engine (its
  own corner from the stash, so the state holds the game's bytes, not the hook's),
  then puts the game's 128 KiB back and returns through the monitor as if nothing
  had happened. While the game is frozen the picture is kept steady by re-aiming
  the video interface every field, which interlaced games need, and the game's clock
  is handed back so it does not see the freeze as elapsed time.
- A load is the reverse: RAM back (the displayed frame last, so the screen changes
  once), the RSP's memories and program counter back, the interrupt the state was
  taken on re-raised, the registers restored, then an ERET straight into the saved
  context.

The header of a state goes to the cartridge last, so an interrupted save never
looks valid, and the copy to the SD card clears the file's header first for the same
reason. The SD mirror runs in the background in 128 KiB pieces paced to the game's
own cartridge traffic.

Games that clear all of RAM at boot (Ocarina of Time) wipe the gate; a tiny stub
fetches it again from the cartridge on the next exception. Games whose boot code
disarms the engine's watchpoint (libultra 2.0K and later) get that one instruction
patched out by the menu, and games that keep their code compressed in the ROM and
unpack it at boot (Mario Tennis, Excitebike 64) get the same patch applied by a
stub that runs after the unpacking.

Everything the routine does with the cartridge goes through the PI bus, which is
also how the game reads its ROM. It therefore saves and restores the DMA address
registers on every entry, never touches the bus while a game transfer is running,
and only enables writes to the ROM area for as long as a copy needs them.

The virtual Controller Pak is answered by the monitor from the image on the
cartridge. At the first video interrupt after boot the hook follows the game's
original exception vector to libultra's `__osException`, finds the block that
handles a finished controller transfer (the pending-bit test, the acknowledge, the
call that wakes the waiting thread) and replaces the four instructions ending with
that call by a jump to the monitor's server, which rewrites the transfer's reply
block in RAM for the pak commands and then does what the four instructions did.
Games that run their code through the TLB (Turok 2's handler sits at 0x0029xxxx)
are patched through the physical page. The patch is lifted while a state is copied
and put back after a load, so a state never carries it. Changes to the pak image
reach the card in 2 KiB pieces, a second and a half after the game's last write.

Exit to menu is the reset button done in software. Once the pending card writes
are through (including the SC64's own writeback of the game's save, which it does
a moment after the game's last write), the hook quiets the RSP, the RDP, the audio
and the video the way the menu does before it boots a ROM, tells the SC64 to map
its bootloader at 0x10000000 again, and runs that image's IPL3 from SP DMEM with
the menu's own boot routine in SP IMEM and the registers the PIF sets at power-on.
The bootloader then loads sc64menu.n64 from the card. Suspend is a save with a mark
in the header; at the next launch the menu spots the mark in the slot's file and
hands the slot to the hook, which loads it as a combo would once the game polls its
controller, then clears the mark on the cartridge and in the file. One thing a state
cannot carry is the PIF's command block, which libultra only re-sends when its last
command changes; after every load the hook puts the standard controller-poll block
there, or a world resumed early in a boot would keep polling through whatever the
game was doing at that moment (Mario 64 reading its EEPROM) and never see a button.

The source is in `src/boot/hook/` (the hook, the gate in `lowpage.S`, the monitor in
`monitor.S`) and `src/boot/cheats.c` (the boot integration). `src/boot/hook/build.sh`
rebuilds the blobs that the menu embeds.

## Credits

- [N64FlashcartMenu](https://github.com/Polprzewodnikowy/N64FlashcartMenu) and the
  [SummerCart64](https://github.com/Polprzewodnikowy/SummerCart64) by
  Polprzewodnikowy and contributors; the menu's Datel cheat-engine boot patch is the
  foundation this stands on.
- [libdragon](https://github.com/DragonMinded/libdragon), which builds the menu and
  the hook.
- Save states by John Steinmeyer.

Licensed like the menu itself, under the GNU AGPL v3.
