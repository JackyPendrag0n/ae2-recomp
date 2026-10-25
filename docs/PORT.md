# Ape Escape 2 (PAL, SCES-50885) — port reference

This is a static recompilation of the PS2 game into C, plus a small runtime that stands in for the PS2 hardware and the IOP. It is not an emulator running the original MIPS code. Every function in `SCES_508.85` was translated to C ahead of time (`port/gen/code_*.c`, about 9,000 functions). The VU0/VU1 microprograms were translated the same way (`port/gen/vu1_progs.c`), and the busiest ones (the skinned-character program, and the stage geometry's strip kernel and backface pass) are also written by hand as plain C (`port/src/vu1_native.c`). The result is compiled into one native executable (Linux, Windows or macOS).

You need your own copy of the game: `port/gen` is generated from your disc's executable by `tools/setup_game.py`, and the program reads all of its assets from your disc files at run time.

## What works

- Boot, memory card check (saves go to a host folder), title screen with attract mode, START, the New Game / Continue / Options menu.
- New Game → the "Go ape and capture 3 monkeys!" intro → the first stage loads and plays with the HUD.
- Two GS (Graphics Synthesizer) renderers, switchable at any time from the debug menu:
  - **GPU (OpenGL 3.3), the default.** Renders at 1–8× the PS2's resolution (4× ≈ 4K). It picks a scale from your screen height: 1080p → 2×, 1440p → 3×, 2160p → 4×. Render-to-texture and framebuffer effects are kept on the GPU. On the stages tested, its output matches the software renderer to within about 0.5% of pixels at 1×. It does the GS fog stage, per-pixel blend enable (PABE) and additive blending with alpha above 1.0 like the software renderer; without fog, glows the game fades with it (the lamp globes, for example) drew as flat white shapes.
  - **Software.** Exact PS2 VRAM swizzling, CLUTs, bilinear filtering, mipmaps, alpha and depth tests, blending, fog. Multithreaded and SSE4.1. It is used automatically if OpenGL 3.3 is not available.
- Widescreen: the game's own 16:9 mode (Options > Screen ratio) with 16:9 output. `--widescreen` or the debug menu can force it.
- **Graphics options inside the game's own Options menu** (Options > Graphics): renderer, resolution, frame rate (50/60 Hz), field of view, brightness, frame skipping, FPS counter. They look and behave like the game's other options and are saved to `ae2-settings.ini`. See [Graphics options](#graphics-options).
- FMVs, with sound, when the `MOVIE/` folder is present.
- Mipmapping in both renderers. The game streams only the smaller texture levels for distant surfaces, so without it far-away textures show the wrong picture (PCSX2's wiki lists this as "broken textures at a distance"). The level follows the PS2's own LOD formula; a triangle whose corners want different levels is cut along the lines where the level changes (Q, like every vertex value, changes linearly across the screen), so the change is a straight line across the ground as on the PS2, not a patch shaped like the polygons.
- Debug menu (F1): level select, unlock all stages and gadgets, play as a monkey (any of the 7 kinds of pants), renderer and resolution, widescreen, FPS counter.
- Input: keyboard, or any SDL-compatible gamepad.

## What is missing or limited

- **Audio.** The game's own IOP sound driver (`SNDN2DRV.IRX`) is statically recompiled from the disc copy (`port/gen/iop_sndn.c`). It runs on an emulated SPU2:
  - 48 ADPCM voices with ADSR envelopes, and the PCM streaming input.
  - Sound effects, jingles and the in-game music that comes from the sound banks are all played this way.
  - Reverb is not emulated yet, so everything sounds "dry".
- **MOVIE / PCM / VOICE folders.** None of them is required, and any of them can be copied into the game folder later under the same names (for example `PCM/BGM/TITLE.SPS`); the port picks them up with no rebuild.
  - Without `MOVIE/`, every FMV is skipped automatically: the publisher logo, the opening, and the stage movies.
  - Without `PCM/` (the streamed music, such as the title theme), the game streams silence where that music would be.
  - Without `VOICE/`, there are no character voices.
- **FMVs** play when `MOVIE/` holds the real movie files. The PS2's MPEG decoder (IPU) is emulated in `port/src/ipu.c`, and the movie's colour conversion runs on the host instead of the PS2's vector unit, so movies run at full speed. START skips a movie, as on the PS2. `AE2_MOVIES=0` skips them anyway. The small picture-in-picture movie format (160x128) still uses the original, slower conversion path. Tested with test movies made in the game's format; the game's own movies were not available for testing.
- **Speed.** Tested on a 2-core 2.8 GHz cloud VM:
  - The title screen runs at about 18–20 fps.
  - The first stage runs at about 25–27 game frames per second. About half of those frames are drawn (see frame skipping below).
  - The game logic advances once per frame rather than by the clock, and the game appears to target 50 fps. Anything slower plays in slow motion. A 4+ core desktop with a modern CPU should get close to full speed, but this has not been measured.
  - The GS renderer scales with cores: one worker per core beyond the first two, up to 8.
- **Frame skipping.** This is on by default, in `auto` mode. When a game frame takes longer than one 50 Hz tick, the next frame is computed but not drawn. Two frames in a row are never skipped.
  - A fast machine never skips.
  - `AE2_FRAMESKIP=0` turns it off.
  - `AE2_FRAMESKIP=n` always draws one frame in every n+1.
  - If you see a stale or flickering image in a menu or effect, try `AE2_FRAMESKIP=0`.
- Tested: the opening, Liberty Island, Breezy Village, Port Calm and the Time Station (reached with the level select). Expect bugs further in.

## Memory

The PS2 has 32 MB of RAM and the game was written to fill it: its heap ran from the end of
the program to a fixed address near the top, and stages, characters and textures all load
there. The port gives the game **128 MB** by default, the amount the PS2 development kits
had, and replaces the game's memory set-up (0x1a4350) so the heap reaches the new top:
the free heap at the title screen goes from about 15 MB to 111 MB. `AE2_RAM_MB=n` sets
the size (32 gives back the console's exact layout; at most 256, since the game's pointers
are 32-bit and the PS2's hardware registers start at 0x10000000). The memory is reserved
once and only used as the game touches it.

What is still emulated, and what would take the port further from the PS2, is written up
in the project's "what is still PS2" document.

## Running

Point the executable at the folder that holds the game files. That folder must contain `SYSTEM.CNF`, `SCES_508.85`, `SARU2.MM`, `STAGE/`, `SOUND/` and the other disc folders.

```sh
./ae2 "/path/to/game"   (Windows: ae2.exe "C:\path\to\game")
```

Memory card data is written to `memcard/mc0/` in the current directory.

Command-line options:

| option       | meaning                                           |
|--------------|---------------------------------------------------|
| `-v` / `-vv` | more logging (`-v` prints per-second fps stats)  |
| `-q`         | quiet                                             |
| `--gpu` / `--sw` | GPU (OpenGL) or software renderer (default: GPU when OpenGL 3.3 works) |
| `--scale N`  | GPU internal resolution, 1–8 × the PS2's (2 ≈ 1080p, 3 ≈ 1440p, 4 ≈ 4K); default picked from your screen |
| `--widescreen` | turn on the game's 16:9 mode and 16:9 output |
| `--record [file.ae2rec]` | record your input for a bug report (see below) |
| `--replay file.ae2rec` | play a recording back |
| `--headless` | no window (for testing; see the environment variables) |

### Controls (keyboard)

| PS2            | key                |
|----------------|--------------------|
| Left stick     | W A S D            |
| Right stick    | I J K L            |
| D-pad          | arrow keys         |
| Cross ✕        | Space              |
| Circle ○       | E                  |
| Square □       | Q                  |
| Triangle △     | F                  |
| L1 / L2        | 1 / 2              |
| R1 / R2        | Left Shift or 3 / 4 |
| L3 / R3        | Z / X              |
| START / SELECT | Enter / Backspace  |

Other keys: F11 or Alt+Enter toggles fullscreen. Shift+Esc quits.

A connected gamepad uses the standard SDL mapping. Ape Escape 2 needs both analog sticks: the right stick swings the gadgets.

### Graphics options

In the game's **Options** screen, the row that was "Screen position" (which only moved the picture on a TV) is now **Graphics**. Choose it with ✕ and the same page switches to the graphics settings. It uses the game's cursor, highlights, sounds and its own style of lettering. ← / → change a setting, ✕ confirms on the last three rows, and △ (or □, or "Back") returns to the Options page.

| row | choices | what it does |
|---|---|---|
| Frame skipping | On / Off | On: when a frame takes longer than one vsync, the next one is computed but not drawn (see below) |
| FPS counter | On / Off | game and display frame rates in the corner |
| Renderer | GPU / Software | OpenGL renderer at a higher resolution, or the exact software renderer. GPU is not selectable if OpenGL 3.3 is unavailable |
| Brightness | bar | five segments lit (the middle) is the game's own picture; fewer is darker, more is brighter |
| Field of view | bar | 45° to 95° in 5° steps; three segments lit is the game's 60° |
| Frame rate | 50 Hz / 60 Hz | 60 Hz runs the game at 60 frames per second. The game logic is frame-based, so it also runs 20% faster, as it does on NTSC consoles. FMVs always play at 50 Hz, the rate they were made for |
| Resolution Nx | ✕ or ← / → | the GPU renderer's internal resolution, 1× to 8× the PS2's (2× ≈ 1080p, 4× ≈ 4K) |
| Reset settings | ✕ | back to the defaults (automatic renderer and resolution, 50 Hz, 60°, normal brightness) |
| Back | ✕ | returns to the Options page |

The settings are saved to `ae2-settings.ini` in the current folder (or the file named by `AE2_SETTINGS`), and loaded at start-up. Options given on the command line (`--gpu`, `--sw`, `--scale`) or in the environment win for that run.

There is no draw-distance setting. The game already draws each level to the horizon: raising the far plane, even six times over, changed no pixels in Liberty Island, Lookout Valley or Snowball Mountain, even with the free camera flown far outside the level.

The new words were rendered in the game's lettering style with the OFL font Fira Sans Black (pre-rendered into `port/src/menu_words.h`) and are drawn into the game's own text textures. They are English, like the rest of this version's menus.

### Debug menu

Press **F1** (or **SELECT + START** together on a gamepad). **F2** and **F3** toggle the free camera and no clip directly, **F4** switches who the controllers drive. Navigate with the arrow keys or d-pad. Enter, Space or ✕ chooses; Esc, Backspace or ○ goes back. Left/right changes a setting. The game is paused for input while the menu is open.

| item | what it does |
|---|---|
| Warp to stage... | level select: every stage, boss battle, the Time Station and the tutorial; stages with several areas let you pick the area |
| Warp with stage intro | Yes: go through the "capture N monkeys" intro; No: drop straight into the stage |
| Unlock all stages and gadgets | sets your progress so every stage is open and gives you all 12 gadgets, including the Magic Punch. It is written to your save the next time the game saves |
| Character | Hikaru or Kakeru (the game's own secret character, model `SYSTEM/HIKARU2.BIN`). It sets the character in your progress, which the game reads when a stage loads, so warp (or enter a stage) after changing it. It is saved with your game like the rest of your progress |
| Play as | Hikaru (or Kakeru), or a **monkey** in any of the seven kinds of pants: yellow, white, blue, black, red, light blue or green (see "Play as a monkey" below). Kept in `ae2-settings.ini` |
| Free camera (F2) | detaches the camera: left stick flies, right stick looks, R1 / L1 up / down, hold R2 for fast, L2 for slow. Hikaru stays where he is (START still pauses) |
| No clip (F3) | Hikaru flies through walls and floors: left stick moves him (relative to the camera), R1 / L1 up / down, R2 fast, L2 slow. The camera follows him as usual |
| Widescreen 16:9 | Game setting / On / Off |
| Renderer | GPU (OpenGL) or Software, switched on the next frame. If the GPU renderer could not start, this row says why (and a message appears for a few seconds at start-up) |
| Internal resolution | GPU scale, 1× to 8× |
| Controls (F4) | who each controller drives: *one player each* (keyboard and first pad: player 1, second pad: player 2), *all → player 1*, or *all → player 2* (handy for trying player 2 with one pad). A pad is only ever opened once, so one controller no longer drives both players |
| Split screen bottom view | *every frame*, or *every other frame (faster)*: the bottom half (player 2's view) is redrawn every second frame, which takes about 30% off the CPU time of split screen (`AE2_P2HALFRATE=1` sets it at start) |
| Show FPS | game and display frame rates in the corner, plus the renderer actually in use ("GPU 4x" or "software") |

On the keyboard the flying controls are: W A S D move, I J K L look, 3 (or Left Shift) up, 1 down, hold 4 for fast and 2 for slow.

Warping works once you are in a game (in a stage or the Time Station), not from the title screen.

### Play as a monkey

Debug menu > **Play as** (left/right cycles) turns player 1 into one of the game's own
monkeys, with the outfit of any of the seven kinds: yellow, white (glasses), blue
(goggles, scarf and sneakers), black (sunglasses), red (boxing gloves), light blue or
green (scope). It changes as you pick it, mid-stage too. The monkey runs, jumps, falls,
gets hurt and swings with the monkeys' own animations, each matched to the move Hikaru
is making, and holds the gadgets in its right hand. Catching, health and cookies are
Hikaru's.

Each kind has its own ability. Every monkey but yellow carries the net on △ (and the
button slot Hikaru's net starts on), and its ability on ○ and □:

| Pants | Ability | Controls |
|---|---|---|
| Yellow | none: the plain monkey | Hikaru's own gadgets and buttons |
| White | throws bombs | ○ straps on the monkeys' bomb belt, the right stick pulls back and throws real bombs |
| Black | shoots | ○ takes the monkeys' machine gun, holding the right stick fires it non-stop (a bullet every 4 frames) |
| Green | homing missiles | ○ shoulders the monkeys' missile launcher, the right stick fires three real missiles at once; they home in, nose first |
| Red | hits hard | ○ takes the Magic Punch, the right stick punches with the monkeys' own attack swing |
| Blue | runs fast | runs at double speed, all the time, with the blue monkey's own sprint |
| Light blue | shield | hold ○: raises the monkeys' pot-lid shield, stands still, and no hit gets through |

The weapons are temporary custom gadgets: the slingshot's slot is reused with a monkey
weapon model in place of the slingshot (one more part on the monkey's skeleton), the
weapon's own monkey animation, and the shot's model swapped for the monkeys' bomb,
machine-gun bullet or missile; the shot itself (aim, flight, homing, hits) is the
slingshot's. The models and animations all come with every stage (green's launcher with
the title screen's monkeys, loaded as for the outfits). Red's attack swing is the monkeys'
general one; the red monkeys' own three-hit combo is used in the stages that have them.

The gadget is lent even before you have earned it, and the ammo is endless (99); none
of it is saved: the save keeps Hikaru's own button slots, gadgets and ammo, and they
come back when you switch back to Hikaru. The shield turns away enemies' hits (falls,
water and other hazards still hurt).

A stage loads only the kinds of monkey it has, so the first time an outfit is missing the
title screen's monkey models (2.5 MB) are loaded once and kept. Player 2 stays Kakeru.
`play_as = monkey_red` in `ae2-settings.ini` or `AE2_MONKEY=r` (y w b k r c g, 0 = off)
sets it at start.

Not changed: the stage intro still shows Hikaru. Tested with the net and the stun club;
gadgets held in both hands or ridden may sit oddly.

### Play as Natalie or the Professor

The same Play as entry (debug menu, `play_as = natalie` / `play_as = professor`, or
`AE2_MONKEY=n` / `AE2_MONKEY=p`) also offers Natalie and the Professor. Their models
(`np_ntm_model`, `np_hks_model`) come from the Station's common archive (`STA_COM`,
2.4 MB), which is loaded once per stage when they are not there. They only have a
standing motion in the game, so Hikaru's animations are retargeted onto their skeletons
every frame: each of their joints that has a match in Hikaru's skeleton turns as his
does, the rest keep their rest pose, and bone lengths stay their own. Gadgets are held
in their right hand.

Known issues: outside the Station the Professor's head does not draw (only the dome on
top shows); Natalie is complete. Faces don't animate, and very stretched poses (ladders,
swinging) can look stiff. Player 2 stays Kakeru.

### Play as Specter

The same Play as entry (debug menu, `play_as = specter` in `ae2-settings.ini`, or
`AE2_MONKEY=s`) also offers Specter, taken from the final showdown: his model
(`bs_spector`) and his own boss animations (`bs_spt_*`) come with that stage's common
archive (`BOS_H_COM`, 1.9 MB), which is loaded once per stage when it is not there. His
motions are made for his skeleton, so no retargeting is needed: each of Hikaru's moves is
swapped for the Specter motion that fits it (standing, running, jumping and falling,
getting hurt, his attack swing, his shooting and throwing poses, his laugh and taunt when
idle). Gadgets sit in his right hand, turned the way they sit in Hikaru's (measured once
from both standing poses).

| Button | Move |
|---|---|
| △ (and Hikaru's net slot) | the net, swung with his attack swing |
| ○ | his energy shot: homing shots from his bare hand (the slingshot's aim and flight, endless) in his shooting pose |
| □ | his strike: the Magic Punch swung with his attack swing (its glove still flies out) |
| L1 | warp dash: a short burst at four times the speed in his dodge pose, and nothing hurts him while it lasts |
| R3 (press the right stick) while standing | he laughs (L2 stays the game's look-around view) |

As with the monkeys, the gadgets are lent and the ammo is endless; the save keeps
Hikaru's own button slots, gadgets and ammo. Health, cookies and catching are Hikaru's.

Known issues: the stage intro and cut-scenes still show Hikaru. In the final showdown
there are two Specters (the boss is still there). He has no jumping animation of his
own, so jumps use his dodge; swimming uses his run. Player 2 stays Kakeru.
`AE2_SPTTEST=n` (testing) cycles through all his `bs_spt` motions, n frames each.

### Two players (experimental)

The title menu has a fourth item, **2 Players: Off/On**. Switch it on, then pick New game
or Continue as usual: in every stage a second player joins beside player 1 on the second
controller, and the screen is split, top half player 1, bottom half player 2. Player 2 is
the other brother: Kakeru, or Hikaru when player 1 plays as Kakeru (new game+). With only
one controller plugged in, the keyboard is player 1 and the controller is player 2 (in
stages; menus still accept both).

2-player mode has a memory card of its own (`memcard/mc0-2p/` next to the usual
`memcard/mc0/`), so its saves never touch your 1-player game. New game in 2-player mode
starts a fresh adventure; Continue loads a 2-player save.

* Both players have the full set of gadgets, each with their own gadget state: player 2's
  club, net and the rest hit, stun and catch monkeys the same as player 1's. Each player
  also puts his own gadgets on the buttons (Choose Gadget in his own pause menu), and
  player 2 keeps his choice from stage to stage.
* Boss fights: bosses have twice their health (each hit counts half), and either player
  can hurt them. Boss fights are in split screen too, each half with its own camera
  (`AE2_P2BOSSSPLIT=0`: full screen with one camera, as before). A boss goes after the
  brother nearer to it, and turns to the other one when he is clearly nearer (by a fifth),
  so it does not switch back and forth when both are about as close. (`AE2_P2BOSS=0`: as
  before, where the boss followed whoever stood nearer a fixed spot of its arena.)
* Enemies and hazards hurt whichever brother they hit (Kakeru's own model has no hit
  volumes, so player 2 takes player 1's body shape for them).
* Each brother has his own health (cookies); they share the lives. Each half has its own
  HUD (gadgets, health, monkeys left), anchored to the edges of that half.
* Falling out of a stage in two-player mode does not fade out and restart from the
  checkpoint: the brother who fell is back at once where he last stood on solid ground,
  one cookie down. With one cookie or less left, player 1's fall goes on as in one-player
  mode, and player 2 rejoins next to player 1. One-player mode is unchanged.
  `AE2_P2FALL=0`: the game's own falls.
* When player 2 loses all his health he does not end the run for both: he rejoins next
  to player 1 with player 1's health.
* Cut-scenes (stage and boss intros, the camera shots when player 1 is held still), the
  pause menu, hint phone text and the travel station are shown full screen.
* Either player can pause. Player 2's Start opens the pause menu for player 2 (a "Player 2"
  banner shows at the top): it is driven by the second pad and its Choose Gadget page
  sets player 2's own gadgets.
* Player 2 has their own voice and player sounds (Kakeru's, or Hikaru's when player 1 is
  Kakeru). The PS2's 2 MB of sound memory is full, so the emulated sound chip has 4 MB and
  the other brother's sound bank goes above the first 2 MB. The bank is checked each time
  player 2 joins a stage and loaded again if the game released it. `AE2_P2VOICE=0` turns
  this off.
* In the "Go ape and capture" tunnel before each stage both brothers float side by side.
  `AE2_P2INTRO=x,y,z` moves player 2 there (default `9,0,0`; player 1 moves the opposite way).
* Split screen runs each game frame twice (the second time from player 2's camera, with
  its game-state changes thrown away), so it needs more CPU time than one player. Only
  the memory pages the second pass writes are put back (a few hundred of the 8192), and
  that pass never waits on the game's other threads, so it cannot freeze the game. The
  textures that pass uploads are put back the same way, page by page. Each
  half shows the middle band of a full view, so nothing is squashed; each player has the
  game's own chase camera (pad 2's right stick turns player 2's). Each player's left stick moves
  them relative to their own half's camera, and player 2's camera starts fresh in each stage.
* The same can be had without the menu, from environment variables: `AE2_P2=1` (player 2),
  `AE2_P2SPLIT=1` (split screen), `AE2_P2KAKERU=1` (player 2 is the other brother). Each
  one given overrides the menu's choice, for example `AE2_P2SPLIT=0` for one shared screen
  whose camera looks at the point between the players and backs away as they separate
  (`AE2_P2CAM=0`: follow player 1 only).

* Monkeys notice either brother: a monkey's senses list the nearer one, so it spots Kakeru,
  goes "!" and runs from him the same as from Hikaru (`AE2_P2SENSE=0`: player 1 only).
* A gotcha (the camera shot when a monkey is caught) shows only in the half of the player
  who caught it; the other player keeps playing in their own view.
* Stage objects react to whichever brother is nearer. Every object (items, switches,
  hint phones, enemies) finds "the player" at one fixed place in memory, so each frame an
  object nearer to player 2 runs its step with player 2 in that place, the same way player
  2's own steps run. An object keeps the brother it saw last while he is within touching
  distance of it (a switch or platform he stands on, an item he is next to) or until the
  other one is clearly nearer, so the other brother walking up does not take it over, and
  an object between the two does not swap players every frame. Monkeys keep their own
  handling (above). Not during cut-scenes or player 2's pause menu. `AE2_P2OBJECTS=0`: objects see player 1 only, as before.
  A fixed camera such an object sets up for player 2 (a pole, a conveyor) goes to player
  2's own half only; with the shared camera it is ignored, and the camera stays player 1's.
  (`AE2_P2BODYTAG=1` is the older attempt: it retags Kakeru's model in the draw list, which
  turned out to be read only by the drawing code, so it swaps the drawn models and does
  nothing for objects. Leave it off.)
* Vehicles (the robot in Vita-Z Factory): either brother gets in with R1 and drives it on
  his own, and the other one keeps playing. The game keeps the rider as "the player", so
  before, player 2 getting in pulled player 1 into the seat. Now each vehicle remembers
  which brother is inside, its step and the seat run with him, and each brother has his
  own ride state (`0x5e03f0`). Tested with the robot in the second area (player 2 in,
  player 1 in, getting out); both brothers in two vehicles at once is untested.
* Player 2's character (1.4 MB) stays loaded in every stage, the Time Station included,
  now that the game has 128 MB (see "Memory" below). With `AE2_RAM_MB=32` it is put
  aside while a stage loads and comes back when player 2 joins, if there is room, as in
  earlier builds; `AE2_P2KEEP=0/1` forces either behaviour.

Limits for now: cut-scenes and the camera follow player 1, and an object sees one brother
at a time (the one touching it, or the nearer one).

### Reporting a bug with a recording

`./ae2 --record "/path/to/Ape Escape 2"` runs the game in deterministic mode and writes every pad input to `ae2-<date>-<time>.ae2rec` (plus a copy of your memory card). Play until the bug happens, quit, and send the `.ae2rec` file and its `.memcard` folder. `--replay file.ae2rec` plays it back identically on any machine.

### Useful environment variables

| variable | effect |
|---|---|
| `AE2_SETTINGS=file` | where the Graphics options are saved (default `ae2-settings.ini`) |
| `AE2_FOV=x` | field of view multiplier for this run (1 = the game's) |
| `AE2_MOVIES=0` | skip the FMVs even when `MOVIE/` is present |
| `AE2_MVHLE=0` | FMV colour conversion through the game's own VU1 program (slow; for comparison) |
| `AE2_GS_THREADS=n` | number of rasteriser worker threads (0 = render on the emulation thread) |
| `AE2_MTGS=0/1` | run the GS front-end on its own thread (default: on when there are 2 or more cores) |
| `AE2_MTVU=0/1` | with `AE2_MTGS`, run VIF1 and the VU1 microprograms on a thread of their own, so the game logic, the geometry work and the GS front end overlap (default: on) |
| `AE2_MTSTAT=1` | every 250 frames, print how busy the VU1 and GS threads were and how long the game waited for them |
| `AE2_FRAMESKIP=auto/0/n` | frame skipping (default `auto`); `AE2_FRAMESKIP_MAX=n` sets the most frames skipped in a row in auto mode (default 1) |
| `AE2_VUINTERP=1` | use the VU interpreter instead of the recompiled microprograms |
| `AE2_VUVERIFY=1` | run every VU program in both the interpreter and the compiled code and report mismatches |
| `AE2_VUNATIVE=0` / `verify` | turn off the hand-written VU1 programs (`src/vu1_native.c`), or run them next to the compiled ones and report any difference in output |
| `AE2_P2RAMBLOCK=n` | split screen: pages (a power of 2, default 4) unlocked per write fault when the second view's RAM changes are undone |
| `AE2_VUCAPTURE=dir` | save any VU microprogram that has no compiled version (see below) |
| `AE2_NOSIMD=1` | disable the SSE4.1 span renderer |
| `AE2_NOAUDIO=1` | don't open an audio device (sound is still mixed for timing) |
| `AE2_WAVOUT=file.wav` | also write the audio output to a WAV file |
| `AE2_INPUT="..."` | scripted input for testing, e.g. `m1+350:start;m1+356:-start;m3+700:stick=0,-127;m3+800:rstick=127,0` (frame or mode+frame : buttons / sticks) |
| `AE2_DUMP_EVERY=n`, `AE2_DUMP_DIR=dir` | write every n-th frame as a PPM file |
| `AE2_FPU_NEAREST=1` | use the PC's round-to-nearest for game maths instead of the PS2's round-toward-zero |
| `AE2_RENDERER=gpu/sw`, `AE2_SCALE=n` | same as `--gpu` / `--sw` and `--scale` |
| `AE2_NOGL=1` | don't use OpenGL at all (software renderer, SDL_Renderer output) |
| `AE2_RAM_MB=n` | EE RAM the game gets, 32 to 256 MB (default 128; see "Memory") |
| `AE2_MONKEY=y/w/b/k/r/c/g/n/p/s`, `0` | play as a monkey in yellow/white/blue/black/red/light blue/green pants, Natalie, the Professor or Specter, or not (overrides `play_as` in `ae2-settings.ini`) |
| `AE2_P2=1`, `AE2_P2SPLIT=0/1`, `AE2_P2KAKERU=0/1`, `AE2_P2CAM=0` | second player, split screen, player 2 as the other brother, shared-camera off (see "Two players"; each overrides the title menu's 2-player mode) |

Debugging aids (headless testing):

| variable | effect |
|---|---|
| `AE2_DETERMINISTIC=1` | virtual clock driven by the game's own work: a scripted run plays out identically every time |
| `AE2_TURBO=1` | skip idle time instead of sleeping |
| `AE2_DUMP_POLLS=1`, `AE2_DUMP_RANGE=a-b` | name/limit frame dumps by game frame instead of display frame |
| `AE2_FTRACE=a-b[/step]:prefix` | count which game functions run in each window of game frames |
| `AE2_MEMDUMP=frame,...:prefix` | write EE RAM at those game frames |
| `AE2_SPUSTAT=1` | print the number of active sound voices once a second |
| `AE2_DEBUG_ACTION="poll:warp=scene[/intro],poll:unlock,poll:kakeru"` | run debug-menu actions at given game frames (`kakeru`/`hikaru` set the character) |
| `AE2_INPUT="...;2@m3+N:btn"` | a `2@` prefix sends that scripted input to the second pad |
| `AE2_TEST_TELE=addr,who,from[,dist,until,axis]` | from game frame `from`, keep player `who` (1 or 2) within reach (`dist`, default 15) of the object at `addr` (position at +16), e.g. a boss; `AE2_P2LOG=1` logs boss hits |
| `AE2_TEST_DROP=frame,who` | at game frame `frame`, drop player `who` (1 or 2) 3000 units, to test falls |
| `AE2_P2HURT=frame` | at game frame `frame`, player 2 asks for a restart (as a fall or loss of health does), to test that he rejoins |
| `AE2_NODRAW=1`, `AE2_EXIT_AT=frame` | benchmarks: run the game without drawing anything, and quit at that game frame (printing the `AE2_PROF` report) |
| `AE2_TEST_HIT=frame` | at that game frame, send player 1 an enemy's hit (testing the light blue monkey's shield) |
| `AE2_GLOG=1` | log player 1's button slots, gadget in hand, state, and position every 20 frames (testing the monkeys' abilities); also each monkey shot (kind, start, aim, model) |
| `AE2_MODELS=1` | list the loaded models and the monkey (`ps_`) animations about 1 and 10 seconds into each stage |
| `AE2_DMGLOG=1` | log every hit a player takes (which player, health, and with `make CHECKS=1` the game functions that sent it) |
| `AE2_FALLLOG=from,to` | log both players' state, height and health at every player phase between those game frames (falls and deaths) |
| `AE2_TEST_VISIT_P1=1` | `AE2_TEST_VISIT` moves player 1 instead of player 2 |
| `AE2_SPLITLOG=1`, `AE2_SPLITFX=0` | split screen: log screen effects that read the picture back (the afterimage when a brother falls or is knocked out blends the last frame back in), which are moved with each view so the trail lines up with its own half; `AE2_SPLITFX=0` leaves them as the game drew them |
| `AE2_ACTORLIST=frame` | at game frame `frame`, list the stage's objects (address, handler, position) |
| `AE2_HEXDUMP=frame,addr,len;...` | print guest memory at those game frames |
| `AE2_TEST_PULL=obj,who,from[,dz]` | for 300 frames from `from`, put the monkey object at `obj` (position at +0x30) just in front of player `who` (testing captures) |
| `AE2_TEST_TELE2=...` | a second `AE2_TEST_TELE` (same fields; `until` stops it after that frame, `axis` 1 offsets along z) |
| `AE2_P2SENSELOG=addr` | log one monkey sensor's distances to both players and its alert flags each frame |
| `AE2_P2BOSSLOG=1` | every 25 frames in a boss fight, log each boss's position, both players' and which one the boss runs with |
| `AE2_P2CAMLOG=1` | log the camera manager's choice of camera at each camera step |
| `AE2_TEST_VISIT=from[,step]` | from game frame `from`, put player 2 on each stage actor in turn for `step` frames (default 150) and log the actor's state, to see which objects react to player 2 (`AE2_P2OBJLOG=1` logs each actor that runs with player 2) |
| `AE2_RWATCH=addr,len[,stride,off]` | `make CHECKS=1` builds: log which game functions read that range (`*addr` = the range starts at the address stored there) |
| `AE2_P2SPARE=n` | heap room (bytes) left free beyond player 2's character before it is loaded in a stage (default 0x40000) |
| `AE2_P2CAMSTICK=0` | player 2's stick follows player 1's camera again (the old behaviour) |
| `AE2_SPULOG=1` | log sound memory uploads, voice start addresses and the reverb areas |
| `AE2_P2RAMCHECK=1` | 2-player split screen: also keep a full copy of RAM and warn when the second pass leaves a change behind (slow) |
| `AE2_GIFREC=file`, `AE2_GIFREC_START=n`, `AE2_GIFREC_FRAMES=n` | record the GS command stream (renderer comparisons) |

## How it is put together

```
recomp/        EE recompiler (Python): elfinfo.py finds functions and jump tables,
               recomp.py emits C; overrides.txt lists functions replaced by HLE code;
               iop_recomp.py translates IOP (R3000) modules such as SNDN2DRV.IRX
recomp/vu_programs.recipe  where each VU0/VU1 microprogram lies in SCES_508.85 (tools/vu_programs.py)
port/include/  CPU semantics: 128-bit GPRs, MMI, COP1 FPU, COP2/VU0 macro ops
port/src/      runtime:
  rt_kernel.c    EE kernel HLE (threads on host pthreads, semaphores, alarms, interrupts)
  rt_sif.c       SIF / SIF-RPC protocol
  iop_*.c        IOP modules re-implemented at the RPC level: cdvd (disc reads + streaming),
                 loadfile, heap, memory card (host folder)
  iop_module.c   host side of recompiled IOP modules (imports: libsd, SIF RPC, threads)
  spu2.c         SPU2 sound processor (voices, envelopes, PCM input, SDL audio output)
  rt_hw.c        timers, INTC, DMAC, FIFOs
  vif.c, vu.c    VIF unpacking, VU interpreter, compiled-program dispatch
  vu1_native.c   VU1 programs rewritten by hand as plain C (skinned models, stage strips, backface culling)
  gs.c, gs_raster.h, gs_span.h, gs_simd.h   the GS (software renderer)
  gs_hw.h        the GPU renderer (OpenGL 3.3)
  host_sdl.c, host_gl.c   window, input, OpenGL presentation and menu overlay
  debug.c, debug_menu.c   input recording/replay, the debug menu
  monkey.c       play as a monkey (model, outfits, motions, gadget hand, each kind's ability)
  hle_overrides.c  game-specific replacements (pad, movies, camera projection, debug tty)
  gfx_menu.c       the Graphics page in the game's Options menu, and ae2-settings.ini
  menu_words.h     its words, pre-rendered from Fira Sans Black
  ipu.c            the PS2 MPEG decoder (IPU), for FMVs
port/gen/      generated C (EE code and VU microprograms)
  platform.c     the Linux / macOS / Windows differences (memory, page protection, coroutines)
port/compat/   headers that fill POSIX gaps on Windows, and SSE-on-ARM (sse2neon) for Apple Silicon
tools/         setup_game.py, iso_extract.py, vu_programs.py, vu_recomp.py
platform/      build scripts for Linux, Windows (MSYS2 or cross from Linux) and macOS
```

### Regenerating the code

`tools/setup_game.py` does this from your own disc (see the main README). Later stages may upload VU microprograms that are not in `recomp/vu_programs.recipe` yet; those run in the VU interpreter, which is correct but slower. To add them: run once with `AE2_VUCAPTURE=captures`, then `python3 tools/vu_programs.py make captures game/SCES_508.85 new.recipe` turns the captures into recipe lines (ELF offsets only, no game code) to merge into `recomp/vu_programs.recipe`.
