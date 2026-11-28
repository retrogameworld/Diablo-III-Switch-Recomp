# Diablo III: Eternal Collection - Switch static recompilation (PC)

A native PC port of the Nintendo Switch version of **Diablo III: Eternal Collection** (2.7.7). The game's ARM64 code is
translated ahead of time into C and compiled into a native program; the parts of the Switch it needs - its system
services, the NVN graphics API, audio output, controllers, saves - are reimplemented on the PC. Graphics run on
**Vulkan**; input, audio and windows on **SDL3**. Nothing is emulated at run time.

**No game files are or will be included.** The game is built from the player's own legally dumped copy on their own
machine.

## What's new in v1.3.2 (2026-11-28)
- Controller: a skill on R1 / RB (such as Strafe) no longer jumps to L2 / LT for a moment in a fight. (#23)
- Keyboard: walking no longer turns very slowly and late in fights. (#24)
- Mods: Bigger Stash (up to 400 more slots), Experience and Gold Multipliers (up to x10,000), Unlimited Challenge Rift
  Rewards, Keep Heroes Across Seasons. (#22)
- Keyboard: the stick keys turn smoothly between directions, like yuzu's "Emulate analog with keyboard input". (#20)
- The first start after updating builds the game code again, once.
- (v1.3.1) Everything below.

### v1.3.1 (2026-11-28)
- Faster on older CPUs: the game's 3D models stay on the graphics card instead of being copied there for every draw,
  so busy towns with many characters no longer slow down on 4-core CPUs.
- (v1.3.0) Keyboard: key-cap button icons (PC Settings > Controls > Button Icons), and in menus a second direction key
  turns the stick slowly, so every Inventory equipment slot can be picked from the keyboard. (#20)
- (v1.2.7) If the game stops with its own error (such as "Out of Memory"), the log now says where it came from.
- (v1.2.6) Everything below.

### v1.2.6 (2026-11-27)
- Challenges are in the pause menu too: check your progress without leaving the game. (#16)
- Mods: new cheat No Rift Close Timer: a rift closes as soon as you turn it in at Orek, no 30-second wait. (#15)
- Mods: new cheat Unlock All Cosmetics: every wing, pet, pennant and portrait frame. (#15)
- Settings that need a restart now offer Restart Now when you close the settings menu, and Quit has Restart Game.
- The first start after updating builds the game code again, once.
- (v1.2.5) Mods: All Difficulties opens Torment VII-XVI, Auto Pickup takes gear and gems, Anniversary buffs have names. (#15)
- (v1.2.4) Fixed a crash later in the game when the game's scripts hit an error (seen in Act 5). The v1.2.1 fix could
  stop working after a few minutes of play.
- (v1.2.3) Text is readable again at Internal resolution 720p (letters looked broken).
- (v1.2.2) Fixed rare black rectangles in some areas (seen in the desert and swamp maps).
- (v1.2.1) Fixed a crash when the game's scripts hit an error (seen at the start of Act 4).

Earlier releases: see [CHANGELOG.md](CHANGELOG.md).

## Requirements
- **Your own copy of the game, as NSP files:** the Diablo III: Eternal Collection **base game** and the **2.7.7 update**
  (v1441792). Other versions of the game are not supported - the recompiled code is tied to that exact build, and the
  setup checks it.
- **Your own `prod.keys`**, dumped from your own Switch (firmware 22.5.0 or newer).
- **DLC (optional):** your own Diablo III DLC NSPs. The game then sees them as installed and unlocks their items.
- **Voice packs (optional):** your own Diablo III voice pack NSPs (free add-ons on the Switch). Put them in the `data`
  folder like the DLC: the next start unpacks their voices once, and the game can then use that voice language.
- Windows 10/11 x64, a graphics card with Vulkan 1.3, a CPU with AVX2, and about 25 GB of free space for the setup.
- A controller is recommended (DualSense, DualShock 4, Xbox and others). The keyboard works too (WASD left stick,
  K / J / L / I for A / B / X / Y; change them in PC Settings > Controls), with key icons in the game if you choose
  Button icons: Keyboard.

## Setup
1. Put your base game NSP, your 2.7.7 update NSP, your `prod.keys` and (optionally) your DLC NSPs in the `data` folder,
   next to `Diablo3Recomp.exe`. File names don't matter.
2. Start `Diablo3Recomp.exe`. The first start unpacks the game and builds its code for your PC, with a progress
   window (about 15-45 minutes, once). Then the game starts.
3. After that your NSPs and `prod.keys` are moved to a folder named "Your game files (no longer needed)" next to the
   program, with a note: the game doesn't use them any more, so you can delete that folder or keep it as a backup.
   Your DLC stays installed (`data\dlc`).

Later starts go straight into the game.

**The PC Settings menu's lettering:** free look-alike fonts are built in. If you also have Diablo III on PC, the setup
uses Diablo's own fonts from that install (found by itself in the usual places, or pick its folder in PC Settings >
Video > Menu fonts) - copied on your PC only.

## What works
- The game from the title screen on: hero creation, the campaign and saves
- Couch co-op: extra controllers join from the main menu
- Internal resolution from native and 720p up to 8K; FSR 1 upscaling with sharpening; SMAA
- 60 fps (recommended) and 120 fps (experimental, see below)
- Borderless, exclusive full screen or windowed
- PlayStation, Xbox or Nintendo button prompts and controller pictures, chosen by the controller in use or in PC
  Settings > Controls
- PC Settings in the game's own style, from the main menu, the Share / PS button, or View + Menu on Xbox: video, audio (Extra Volume,
  Extra Music Volume, Stereo / Mono), controls - with the game's own menu sounds and highlight, made from your copy
- Offline play: online-only menus, Season / Leaderboard / store entries and "Not Signed In" messages are gone;
  "Exit Game" on the main menu

## Mods (PC Settings > Mods)
All off by default; changes apply after a restart. Back up your saves before trying them.
- **Offline seasons 14-40:** make seasonal heroes offline, with the Season Journey.
- **Season buffs and events:** the chosen season's own theme, or any of 20 buffs / events at once (Kanai's Powers,
  Ethereal Items, Double Treasure Goblins, Soul Shards, ...).
- **Cheats:** all difficulties, free Kanai's Cube, instant town portal, no cooldowns, instant crafting (identify,
  craft, enchant, cube), gem upgrades always work / faster / straight to level 150, infinite resource, extra greater
  rift orbs, the same legendary twice, easy kill damage, red damage numbers, drop any item, guaranteed legendaries
  from Kadala and the cube, any gem in any socket, auto pickup, god mode, the developers' debug flags, any item in any
  slot, no rift close timer (a rift closes as soon as you turn it in at Orek), unlock all cosmetics (every wing,
  pet, pennant and portrait frame; they stay on your account), unlimited Challenge Rift rewards (every Challenge Rift
  beaten in time earns its cache, not only the first each week), keep heroes across seasons (heroes of an earlier
  season play on as seasonal heroes of the season you pick, instead of being offered Rebirth).
- **Movement and attack speed:** 100% to 300%.
- **Experience and gold multipliers:** x2 to x10,000 (gold you pick up).
- **Bigger Stash:** 20 to 400 more slots than a new account's 20, without buying them. The stash never shrinks: the
  space stays on your account when this is set back to Normal. (The bag can't grow: the game has its 60 slots built
  in as a fixed 10 x 6 grid.)
- **Legendary drops:** always ancient, always primal, no primal or no ancient.
- **Challenge Rift offline:** the weekly Challenge Rift without servers (Game Settings > Mode > Challenge Rifts; the
  game unlocks it after a solo greater rift). It needs the rift files from d3hack, which are not included here: copy
  the files from d3hack's `examples/config/d3hack-nx/rift_data` folder (`challengerift_config.dat` and
  `challengerift_00.dat` ...) into a folder named `challenge_rifts` inside the `data` folder. A new rift every week.

## Known issues
- Mods: extra season buffs and events show as empty green frames in the buff bar. The buffs work; the game only has
  icon art for each season's own theme buff.
- **120 fps is experimental.** The game steps its world 60 times a second; at 120 fps moving characters can now and
  then show ghosting (a double image). The port detects it and corrects itself within about a second, but if it
  bothers you, use 60 fps (the default).
- A short stutter the first time a new effect appears (its shaders are prepared then and kept for next time), and
  short pauses while a new area loads.

## Planned
- Android (ARM64) build
- Local network play between PCs

## How it works (short)
- The game's code is lifted function by function to C. Its menus, text and a few behaviours are adjusted with small
  hooks (for example the PC Settings and Exit Game entries, text that mentions the console, button icons).
- Graphics: NVN calls become Vulkan, and the game's Maxwell GPU shaders are translated to SPIR-V.
- Audio: the game's 5.1 output is mixed to stereo (or mono); the music level is set on the game's own music channel.
- Button prompts: the game's prompt textures are redrawn at load time with PlayStation or Xbox symbols.
- 120 fps: the time handed to each frame is kept on the game's half-step grid, and the hero's on-screen motion is
  watched for the double-image pattern and corrected.

## Source code and license
The source code of this port is in the `source` folder. It is **open source under the GNU GPL version 2** (see
[LICENSE](LICENSE)): you may use, study, share and change it under the GPL's terms. The Mods (offline seasons, season
buffs, cheats) are derived from [d3hack](https://github.com/god-jester/d3hack) by god-jester through its 2.7.7 port in
[Diablo III Mobile](https://github.com/Desm0nt/diablo3-port), both GPL-2.0 - thanks! The parts from other projects keep
their own licenses - most importantly **LAS Recomp**, the base of this port, which is MIT (see `source/LICENSE`).

The source contains no game code, game files, keys or game assets: the game's code is generated on each player's PC
from their own copy.

## Credits
- **d3hack** by god-jester (GPL-2.0) and **Diablo III Mobile** by Desm0nt (its 2.7.7 port; GPL-2.0) - the Mods
- **LAS Recomp**, a static recompilation of The Legend of Zelda: Link's Awakening for Switch (MIT) - the base this
  project is built from
- **Ryujinx** (MIT) - references for the audio renderer and the shader decoder
- **hactool** by SciresM (ISC) - unpacks your NSPs (with a small change to read them in place)
- **zig** (MIT) - the compiler the setup builds the game code with on your PC
- **Marcellus SC** and **EB Garamond** (SIL Open Font License) - the built-in PC Settings fonts
- Dear ImGui, SDL3, FFmpeg (from MSYS2; some parts under the GPL), the Vulkan SDK and other libraries under their own
  licenses

Diablo is a trademark of Blizzard Entertainment. Nintendo Switch is a trademark of Nintendo. This project is not
affiliated with or endorsed by Blizzard Entertainment or Nintendo.
