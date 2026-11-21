# ReBruised 0.5.0: first public release

The first public release of ReBruised, a native PC recompilation of **Black & Bruised** (PS2, PAL).

**The game is playable.** Menus, IP Fight and the movies work, and a few of the Boxer's Life fights have been played
through with their cutscenes. It's still early, though, so **expect some bugs**. When you hit one, the launcher's
**REPORT A PROBLEM** page collects the logs we need; please attach them to an issue.

**No game files are included.** You need your own **Black & Bruised (Europe)** disc image (`.iso`, SLES-51620).
On first launch, ReBruised builds the game from your disc on your PC (a few minutes, once; up to 10 on slower PCs).

## Getting started

1. Extract the whole zip to a folder you can write to, such as `Documents\ReBruised` (not Program Files).
2. Run `ReBruised.exe` and select your disc image.
3. Wait for **BUILDING THE GAME** to finish, then press **PLAY**.

Requirements: Windows 10 1903+ or 11 (64-bit), a CPU with SSE4.1, OpenGL 3.3 graphics with a current driver, and about
1 GB of free disk space.

## What's in this version

- **The game running natively:** menus, IP Fight, Boxer's Life with its cutscenes, the intro and the movies.
- **60 fps** gameplay using the game's own 60 Hz timing (or the original PAL 50 fps).
- **Higher resolutions** up to 4K, **FXAA/SSAA**, anisotropic filtering and a crisp HD HUD option.
- **Widescreen 16:9** in fights and, optionally, menus (off by default; see known issues).
- **Sound** through the game's own sound driver: music, voices and movie audio, with low latency.
- **XInput controllers** with matching button prompts in the game, plus keyboard controls.
- **Launcher:** options, controls, disc setup and problem reports.

## Known issues

- **Widescreen:** on the character select, some boxers' arms are cut off near the screen edges (the game skips
  drawing what it thinks is off its 4:3 screen). Widescreen is off by default.
- **Not tested much yet:** most of Boxer's Life, memory card saves, two-player matches, long play sessions, and AMD
  and Intel graphics.
- **Antivirus:** the game is compiled on your PC, so a freshly built `BlackAndBruised.exe` has no reputation yet.
  Some antivirus programs may scan it slowly or flag it. Allow it if that happens.
- The very first movie (publisher logos) plays slightly below full speed.

## Checksums

`ReBruised-0.5.0.zip` SHA-256: `f64feee2b89a201999dd3b9fe1965c570cf087f7ffadff2397a049c411ef9b5b`
