# Diablo III: Eternal Collection - Switch static recompilation (PC)

A native PC port of the Nintendo Switch version of **Diablo III: Eternal Collection** (2.7.7), made by statically
recompiling the game's ARM64 code to C and running it on a reimplementation of the Switch's system services, with the
graphics on **Vulkan** and input, audio and windows on **SDL3**. Windows x64 first; an Android (ARM64) build is planned.

**No game files are or will be included.** The game is built from the player's own legally dumped copy on their own
machine. See [INFO.md](INFO.md) for features, status and how it works.

## Highlights
- Native Windows build, up to 8K internal resolution, 60 or 120 fps
- Couch co-op with several controllers; PlayStation and Xbox button prompts and controller layouts
- PC Settings menu in the game's own style, opened from the main menu or the Share / PS button
- Offline: online-only menus and messages removed; "Exit Game" on the main menu

## Credits
- Based on **LAS Recomp**, a static recompilation of The Legend of Zelda: Link's Awakening for Switch (MIT license).
- Audio renderer and shader decoder references from **Ryujinx** (MIT).

Diablo is a trademark of Blizzard Entertainment. Nintendo Switch is a trademark of Nintendo. This project is not
affiliated with or endorsed by Blizzard Entertainment or Nintendo.
