# Diablo III: Eternal Collection - Switch static recompilation (PC)

A native PC port of the Nintendo Switch version of **Diablo III: Eternal Collection** (2.7.7). The game's ARM64 code is
translated ahead of time into C and compiled into a native program; the parts of the Switch it needs - its system
services, the NVN graphics API, audio output, controllers, saves - are reimplemented on the PC. Graphics run on
**Vulkan**; input, audio and windows on **SDL3**. Nothing is emulated at run time.

**No game files are or will be included.** The game is built from the player's own legally dumped copy on their own
machine.

## Requirements
- Your own dump of the game: the base game and the 2.7.7 update (the DLC is optional)
- Windows 10/11 x64 and a Vulkan 1.3 graphics card
- A controller is recommended (DualSense, DualShock 4, Xbox and others); keyboard works for menus

## What works
- The game from the title screen on: hero creation, the campaign and saves
- Couch co-op: extra controllers join from the main menu
- Internal resolution from native and 720p up to 8K; FSR 1 upscaling with sharpening; SMAA
- 60 fps (recommended) and 120 fps (experimental, see below)
- Borderless, exclusive full screen or windowed
- PlayStation or Xbox button prompts and controller pictures, chosen by the controller in use
- PC Settings in the game's own style, from the main menu or the Share / PS button: video, audio (Extra Volume,
  Extra Music Volume, Stereo / Mono), controls
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

## Credits
- **LAS Recomp**, a static recompilation of The Legend of Zelda: Link's Awakening for Switch (MIT) - the base this
  project is built from
- **Ryujinx** (MIT) - references for the audio renderer and the shader decoder
- Dear ImGui, SDL3, the Vulkan SDK and other libraries under their own licenses

Diablo is a trademark of Blizzard Entertainment. Nintendo Switch is a trademark of Nintendo. This project is not
affiliated with or endorsed by Blizzard Entertainment or Nintendo.
