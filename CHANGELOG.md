# Changelog

What changed in each release of Diablo III Recomp (Windows). The newest is at the top.

## v1.3.2 (2026-11-28)
- Controller: a skill on R1 / RB (such as Strafe) no longer jumps to L2 / LT for a moment in a fight. Holding A on
  something you can act on turned the shoulders over to the menus' layout until A was let go. (#23)
- Fixed: with the keyboard, walking could turn very slowly and late in fights (for example Zoltun Kulle's Soulstone
  Chamber): attacking made the game look like a menu was open, and the stick keys turned at menu speed. Controller
  buttons that moved around mid-fight (#23) had the same cause. (#24)
- Mods: **Bigger Stash**: the stash grows to 20 to 400 slots more than the 20 a new account starts with, without
  buying it. It never shrinks, so the space stays on your account. (#22)
- Mods: **Experience Multiplier** and **Gold Multiplier**: x2 up to x10,000 (gold you pick up; not what vendors pay).
  (#22)
- Mods: **Unlimited Challenge Rift Rewards**: every Challenge Rift you beat in time earns its reward cache, not only
  the first one each week (needs Challenge Rift Offline). (#22)
- Mods: **Keep Heroes Across Seasons**: seasonal heroes of an earlier season play on as seasonal heroes of the season
  you pick, with the seasonal stash, instead of being offered Rebirth. (#22)
- Keyboard: the stick keys now turn smoothly between directions, like yuzu's "Emulate analog with keyboard input"
  (in play too). In the Inventory and Skills menus they turn slower: hold S, add A or D, and let go of both keys when
  the slot you want is lit. PC Settings > Controls > Smooth keyboard stick turns it off. (#20)
- **After updating, the first start builds the game code again, once.**

## v1.3.1 (2026-11-28)
- Faster on older CPUs: the game's 3D models now stay on the graphics card between frames instead of being copied
  there again for every draw. Busy places such as towns with many characters used to slow down on 4-core CPUs even
  with the GPU half idle; in New Tristram the renderer now spends about 40% less CPU time, and its vertex work is
  about 10 times smaller. Thanks to the player who profiled it and found the cause.
- Also has everything in v1.3.0 below: keyboard key-cap button icons, and the smooth key stick in menus. (#20)

## v1.3.0 (2026-11-28)
- Keyboard: in the game's menus, hold one direction key and add a second one to turn the stick slowly toward it; let
  go of the second key to stop. Every equipment slot in the Inventory circle (and every skill choice) can now be
  picked from the keyboard. Walking works as before. (#20)
- **Button icons: Keyboard** (PC Settings > Controls): the game shows your keys as key caps instead of controller
  buttons. On Auto it shows the keys while you play with the keyboard and the controller's buttons once you use the
  controller again. (#20)

## v1.2.7 (2026-11-28)
- If the game stops with its own error (such as "Out of Memory"), the log now says the error, where it came from and
  how much memory the game was using, so a report with the log shows what happened.

## v1.2.6 (2026-11-27)
- Challenges are in the pause menu too, so you can check your progress without leaving the game. (#16)
- Mods: new cheat **No Rift Close Timer**: a rift closes as soon as you turn it in at Orek, without the 30-second
  "Nephalem Rift closes in" wait before the obelisk works again. (#15)
- Mods: new cheat **Unlock All Cosmetics**: every wing, pet, pennant and portrait frame is learned when you enter a
  game, and they stay on your account. (#15)
- Cosmetics the Switch version never had a picture for show their category's emblem instead of the pink
  "i need an icon" box, and their description says they are only included in the PC version.
- Settings that only take effect at a start (Mods, the game blur filter): closing the settings menu after changing
  one asks **Restart Now** or **Later**; Restart Now closes the game and starts it again.
- PC Settings > Quit has **Restart Game**: closes the game and starts it again.
- **After updating, the first start builds the game code again, once.**

## v1.2.5 (2026-11-27)
- Mods: All Difficulties now also opens Torment VII-XVI in Game Settings (they stayed "locked until level 70"). (#15)
- Mods: Auto Pickup now picks up gear and gems too, not only gold and materials. (#15)
- Season buffs: the Anniversary buffs show a name under Status Effects instead of "!!Missing!!". (#15)
- **After updating, the first start builds the game code again, once.**

## v1.2.4 (2026-11-26)
- Fixed a crash later in the game when the game's scripts hit an error (seen in Act 5). The v1.2.1 fix kept a spot for
  every place the scripts were called from and ran out of spots after a few minutes of play.

## v1.2.3 (2026-11-26)
- Text is readable again at Internal resolution 720p (letters were missing pixels and looked broken).

## v1.2.2 (2026-11-26)
- Fixed rare black rectangles in some areas (seen in the desert and swamp maps). The v1.2.0 dye fix read two other
  colour tables of the game wrong; all three are read right now.

## v1.2.1 (2026-11-25)
- Fixed a crash when the game's scripts hit an error (seen at the start of Act 4).

## v1.2.0 (2026-11-25)
- Starts on more graphics drivers: it no longer asks for Vulkan features the driver doesn't have (Proton on ARM /
  Adreno failed at launch). (#10)
- A crash no longer says "your game files are incomplete" (that check counted another game's files).

## v1.1.0 (2026-11-24, release v2026.11.24)
- **Mods** in PC Settings > Mods: offline seasons 14-40 with the Season Journey and seasonal heroes, season buffs and
  events, every cheat from d3hack moved to this game version (god mode, no cooldowns, instant crafting, movement and
  attack speed, always ancient / primal, auto pickup and more) and Challenge Rift offline. All off by default. Thanks
  to d3hack and Diablo III Mobile (credits in the README). (#4)
- **Open source under the GPL version 2** (it was "view only" before).
- **Voice packs:** your own voice pack NSPs in the `data` folder are unpacked once and the game can use that language.
- **Fixes:**
  - Nintendo button prompts now show the white A / B icons. (#6)
  - Controller layout set to Xbox now shows Xbox prompts with any controller.
  - The name keyboard can type Cyrillic, Greek and accented letters. (#5)
  - The Challenges entry (achievements) is back on the More page offline. (#3)
  - RB / LB switch tabs in menus again, at vendors and artisans too. (#2)
  - Dyes show their real colours (they read the wrong colours, with black blocks). (#2)
  - Xbox and other controllers without a touchpad: View opens the game menu (Options, Change Difficulty); PC
    Settings open with View + Menu together. (#2)
  - Ultrawide (21:9, 32:9) screens: the game stays 16:9 with black bars instead of stretching. (#8)
- **After updating, the first start builds the game code again, once** (a progress window, like the first setup).
  Your saves and settings stay.

## v1.0 (2026-11-18, release v2026.11.18)
- First public release: the game from the title screen on, couch co-op, resolutions up to 8K with FSR 1 and SMAA,
  60 / 120 fps, PlayStation and Xbox button prompts, PC Settings in the game's own style, offline play.
