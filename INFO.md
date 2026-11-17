# Project info

## What it is
The Switch release of Diablo III: Eternal Collection (version 2.7.7) is translated ahead of time from ARM64 machine code
into C, then compiled into a native program. The parts of the Switch it needs - its operating system services, the NVN
graphics API, audio output, controllers, saves - are reimplemented on the PC. Nothing is emulated at run time.

## Requirements
- Your own dump of the game: the base game and the 2.7.7 update (the DLC is optional)
- Windows 10/11 x64 and a Vulkan 1.3 graphics card
- A controller is recommended (DualSense, DualShock 4, Xbox and others); keyboard works for menus

## What works
- The game from the title screen on: hero creation, the campaign and saves (played through the start of Act I so far)
- Couch co-op: extra controllers join from the main menu
- Internal resolution Native, 720p up to 8K; FSR 1 upscaling with sharpening; SMAA
- 60 and 120 fps
- Borderless, exclusive full screen or windowed
- PlayStation or Xbox button prompts and controller pictures, chosen by the controller in use
- PC Settings: video, audio (Extra Volume, Extra Music Volume, Stereo / Mono), controls
- Offline play: Network, Season, Leaderboard and store entries and "Not Signed In" messages are gone

## Being worked on
- At 120 fps characters can now and then look doubled / see-through until the game pauses for a moment (fine at 60)
- A short pause (~1 s) when a level starts loading - also on the Switch
- Android build
- Local network play between PCs (later)

## How it works (short)
- The game's code is lifted function by function to C; its menus, text and a few behaviours are adjusted with small
  hooks (for example the PC Settings and Exit Game entries, text that mentions the console, button icons)
- Graphics: NVN calls are turned into Vulkan, and the game's Maxwell GPU shaders are translated to SPIR-V
- Audio: the game's 5.1 output is mixed to stereo (or mono); the music level is set on the game's own music channel
- Button prompts: the game's prompt textures are redrawn at load time with PlayStation or Xbox symbols

## Credits
- LAS Recomp (MIT) - the base this project is built from
- Ryujinx (MIT) - references for the audio renderer and the shader decoder
- Dear ImGui, SDL3, Vulkan SDK and other libraries under their own licenses
