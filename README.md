# Diablo III: Eternal Collection - Switch static recompilation (PC)

A native PC port of the Nintendo Switch version of **Diablo III: Eternal Collection** (2.7.7). The game's ARM64 code is
translated ahead of time into C and compiled into a native program; the parts of the Switch it needs - its system
services, the NVN graphics API, audio output, controllers, saves - are reimplemented on the PC. Graphics run on
**Vulkan**; input, audio and windows on **SDL3**. Nothing is emulated at run time.

**No game files are or will be included.** The game is built from the player's own legally dumped copy on their own
machine.

## Requirements
- **Your own copy of the game, as NSP files:** the Diablo III: Eternal Collection **base game** and the **2.7.7 update**
  (v1441792). Other versions of the game are not supported - the recompiled code is tied to that exact build, and the
  setup checks it.
- **Your own `prod.keys`**, dumped from your own Switch (firmware 22.5.0 or newer).
- **DLC (optional):** your own Diablo III DLC NSPs. The game then sees them as installed and unlocks their items.
- **Voice packs (optional):** your own Diablo III voice pack NSPs (free add-ons on the Switch). Put them in the `data`
  folder like the DLC: the next start unpacks their voices once, and the game can then use that voice language.
- Windows 10/11 x64, a graphics card with Vulkan 1.3, a CPU with AVX2, and about 25 GB of free space for the setup.
- A controller is recommended (DualSense, DualShock 4, Xbox and others); keyboard works for menus.

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
- PlayStation or Xbox button prompts and controller pictures, chosen by the controller in use
- PC Settings in the game's own style, from the main menu or the Share / PS button: video, audio (Extra Volume,
  Extra Music Volume, Stereo / Mono), controls - with the game's own menu sounds and highlight, made from your copy
- Offline play: online-only menus, Season / Leaderboard / store entries and "Not Signed In" messages are gone;
  "Exit Game" on the main menu

## Known issues
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
The source code of this port is in the `source` folder, for reading. **It is source-available, not open source:**
you may read it and use the released program for yourself, but you may not copy, modify, redistribute or republish it
(or make another port from it) without permission. See [LICENSE](LICENSE). The parts that come from other projects keep
their own licenses - most importantly **LAS Recomp**, the base of this port, which is MIT (see `source/LICENSE`).

The source contains no game code, game files, keys or game assets: the game's code is generated on each player's PC
from their own copy.

## Credits
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
