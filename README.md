# aliasIsolationNEXT-DLSS5
**The original Alias Isolation mod, plus bug fixes, plus DLSS 5.**

Alias Isolation is a mod for Alien: Isolation. It adds temporal anti-aliasing (TAA) to the shipped game and fixes a few small rendering issues. It injects itself into the game and hooks D3D11 calls, replacing a few shaders and adding some rendering of its own.

This fork picks up the [original mod](https://github.com/aliasIsolation/aliasIsolation), which is no longer updated. It fixes long-standing bugs, adds an installer, and sets up DLSS 5 through [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder). The files in the game folder keep their original names (`aliasIsolation.asi`, `mods\aliasIsolation`), so existing installs and guides still apply.

## Credits
* **Alias Isolation** was created by **aliasIsolation**, and version 1.2.0 was maintained by **Ryan J. Gray** ([RyanJGray](https://github.com/RyanJGray)), with contributions from Rick Runyon. It is public domain ([`LICENSE`](LICENSE)), as is this fork. All the TAA work is theirs. Thanks for making this mod a reality, aliasIsolation, whoever you are!
* **DLSS 5** comes from [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) by jlrouzies-fr (MIT). Its installer also sets up [ReShade](https://reshade.me) by crosire, LumeniteFX and NVIDIA's runtimes, each under its own licence.
* Bundled: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG (MIT), as `d3d11.dll`.
* Built with [MinHook](https://github.com/TsudaKageyu/minhook), [Dear ImGui](https://github.com/ocornut/imgui), [stb](https://github.com/nothings/stb), [GLM](https://github.com/g-truc/glm), [Tracy](https://github.com/wolfpld/tracy) and ReShade's add-on headers.

## The DLSS 5 pack is downloaded, not bundled
The DLSS 5 pack can't be shipped in this release: ReShade, LumeniteFX and NVIDIA's runtimes have their own licences and must come from their original sources. The release therefore contains only Alias Isolation and the ASI loader.

When you choose the DLSS 5 step, the installer downloads DLSS5-Feeder's own installer script from that project's latest official GitHub release. It shows you the script's address and SHA-256 hash, and runs it only after you agree. That script then downloads and installs the rest. You need an internet connection for this step. If you skip it, you get Alias Isolation without DLSS 5.

## What this fork fixes
Issue numbers refer to the [original repository's issues](https://github.com/aliasIsolation/aliasIsolation/issues). [`CHANGELOG.md`](CHANGELOG.md) has the details.

### Fixed (high confidence)
The cause was found in the code and fixed directly.
* The mod menu ignored the mouse: the game reads Raw Input and keeps recentring and clipping the cursor. (#62, #64, #66, #69)
* A damaged or partial `settings.txt` reset every setting to 0. A missing one is now created with the defaults. (#12, #62, #69, #71)
* `Ctrl+Delete` / `Ctrl+Insert` were unreliable and could switch the mod off mid-frame. (#70)
* The logo couldn't be turned off without deleting its file. It is now an option, and the texture isn't loaded while it is off. (#72)
* On CPUs without AVX2 the mod didn't load at all. Legacy AVX1 and SSE3 builds are included, and the installer picks the right one. (#64)
* Constant buffers were mapped with `MAP_WRITE_NO_OVERWRITE`, which is invalid without D3D11.1 and could show stale values on some drivers.
* TAA and chromatic aberration views were created every frame and leaked on resolution changes. (#32, #38, #65)

### Possibly fixed (lower confidence, untested)
These changes target the likely cause, but nobody has confirmed them on the affected setup yet. Reports are welcome.
* HDR highlights clipped by the chromatic aberration pass with Special K or Windows Auto HDR. It now keeps the swapchain's FP16 or 10-bit format, but it is untested on HDR hardware. (#56, #60)
* TAA silently switched off when Special K upgraded the scene buffer to FP16. FP16 is now accepted, but this is also untested on HDR hardware. (#56)
* Low performance on AMD GPUs. The leak fix above may help, but it hasn't been tried on AMD hardware. (#32, #38, #65)
* The Windows Auto HDR blur or "doubling" may have a second cause besides the clipping. (#60)
* A crash on some level loads with RTX HDR. The dumps point outside the mod, and the missing logo file involved is no longer required. (#67)
* The Epic Games Store crash "at `aliasIsolation_hookableOverlayRender`" is not fixed. The crash message now lists `module+offset` for each frame so it can be tracked down.
* DLSS 5's start-up stall is held back until the first loading screen, so it is hidden there. This is not yet tested in game.

## Installation instructions
**aliasIsolationNEXT-DLSS5:** close the game, extract `aliasIsolationNEXT-DLSS5-<version>.zip` anywhere and double-click `Install.bat`. The game is found through Steam, Epic or GOG; if it isn't, paste its folder or pick it in a window. Press Enter to install everything. That covers the mod, skipping the intro videos and the optional DLSS 5 + LumeniteFX pack through [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)'s own installer, which asks first. The menu also has each step on its own. To install by hand, copy the contents of the `files` folder (without `legacy`) next to `AI.exe`.

The original 1.2.0 archive layout:
Extract the following files from the `AliasIsolation-<version>.7z` archive, obtainable via the releases page, to the folder where _Alien: Isolation_ is installed (you should be in a folder that has a file called `AI.exe`):
1. Extract the `mods` folder to your _Alien: Isolation_ folder.
2. Extract `aliasIsolation.asi` and `d3d11.dll` to your _Alien: Isolation_ folder.

If the mod doesn't load at all on an older CPU (no menu, no logo), use the `AVX1` or `SSE3` build from the `Legacy` archive instead.

> [!NOTE]
> If you're using Linux and want to play the game via an older version of Proton (version < Proton 8), you _might_ need to perform an extra step to let Proton know that you want it to use the ASI loader:
> * Add this launch option to the game, in Steam: `WINEDLLOVERRIDES="d3d11=n,b"`

## Video settings
These options must be set to the correct values for the mod to work properly.

| Option | Value |
| --- | --- |
| Anti-aliasing | **SMAA T1x** |
| Chromatic Aberration | **Disabled** |
| Motion Blur | **Enabled** |

Why: TAA needs per-pixel motion vectors, and the game only renders them when Motion Blur is on. The mod hooks the SMAA T1x passes to jitter the camera and replaces SMAA's spatial filter with its sharpening pass. It also draws its own chromatic aberration (the `chromaticAberration` setting), so the game's own would be applied twice.

If Anti-Aliasing or Motion Blur is set wrong, the mod shows a reminder in the top-left corner of the screen.

## Runtime settings
To disable the mod at runtime, hit `Ctrl+Delete`. To re-enable it, hit `Ctrl+Insert`.
To show/hide the mod menu at runtime, hit `DELETE`. The menu takes the mouse while it is open.

Settings are saved to `mods\aliasIsolation\settings.txt`, which is created with the defaults on first launch. The file can be edited by hand (`key = value`, one per line); unknown or missing keys fall back to the defaults.

| Key | Default | Notes |
| --- | --- | --- |
| `sharpening` | `0.3` | 0 to 1 |
| `sharpeningEnabled` | `1` | Set to `0` to turn sharpening off and keep the amount |
| `chromaticAberration` | `0.5` | 0 to 1 |
| `chromaticAberrationEnabled` | `1` | Set to `0` to turn chromatic aberration off and keep the amount |
| `showLogo` | `1` | Set to `0` to hide the logo shown when a level loads |

## ReShade and intro videos
With ReShade 6 (add-on support), the installer adds **Alias Isolation** to ReShade's Home tab (`reshade-shaders\Shaders\AliasIsolation.fx`). Its checkbox switches the mod on and off, next to DLSS 5 Feed and LUMENITE. Its options are in the Variables list below. **Skip intro videos** is its own entry at the end of the Home list. Keep the *Alias Isolation* add-on enabled in the Add-ons tab: that is what connects the Home entry to the mod, and like every add-on it only loads at game start. Without `AliasIsolation.fx`, the mod menu (Delete) has a **ReShade** section with the same switches instead.

**Skip intro videos** moves the three logo videos from `DATA\UI\MOVIES` to `_intro-videos-backup` and takes effect from the next launch.

**HDR and the DLSS 5 pack:** Alien: Isolation always outputs SDR, and RTX HDR or Auto HDR convert it afterwards. The installer therefore reads each monitor's Windows HDR state and sets the feeder and OptiScaler to SDR input, unless Special K or RenoDX in the game folder makes the game's output HDR. Run option 9 again after changing that setup.

## Uninstalling
`Install.bat` > *Remove Alias Isolation* does this for you, or by hand:
Delete `aliasIsolation.asi`, `d3d11.dll` and the `mods\aliasIsolation` folder from the _Alien: Isolation_ folder. Keep `d3d11.dll` if another ASI mod still needs the loader.

## Known issues
* No support for the _MotherVR_ mod.
* RenderDoc, Windows Auto HDR, NVIDIA RTX HDR, MSI Afterburner, Rivatuner, Special K, or any other software which also uses API hooking may break the rendering. If the menu or logo is missing, try closing these first.
* HDR: RTX HDR and Special K HDR are expected to work; 1.3.0-fixes keeps HDR values through the mod's passes. Windows Auto HDR has been reported to cause a blur or "doubling" effect (#60).
* Some particle effects can appear thinner or exhibit ghosting. Sparks and embers are known to be eroded.
* Signs with text can noticeably "fade in" when the player's camera is stationary.
* While the motion tracker is in use, the _Depth of Field_ shader seems to cause the tracker's LED highlights to appear along the edges of the screen.
* The _Epic Games Store_ release of the game may crash with a fatal error at `aliasIsolation_hookableOverlayRender`. That name is probably just the closest exported symbol. The crash message now also lists `module+offset` per frame, so please include it in reports.

## Building from source
1. Clone this repo using Git (with `--recursive` for the submodules).
2. Install CMake, Visual Studio 2022 (Community edition is enough) and choose the C++ packages (including all the Clang support packages) in the installer.
3. Configure the project using CMake's GUI to use Visual Studio 2022, an x86 target (very important!) and `ClangCL` as the toolset parameter.
4. Set `TRACY_ENABLE` to `OFF` (`-DTRACY_ENABLE=OFF`) - your game will run out of memory from the performance profiler otherwise.
5. Optionally set `ALIASISOLATION_CPU` to `AVX1` or `SSE3` for the legacy builds (default `V3`, which needs AVX2).
6. Go to the output directory you chose using CMake, open the solution (`.sln`) in Visual Studio and build for `RelWithDebInfo`.
7. The compiled shaders (`.cso`) will be in the output directory, collect these files to create the below structure (have a look at a recent release to see how they're packaged if needs be).

* `mods`
  * `aliasIsolation`
      * `data`
          * `shaders`
              * `.cso` files
          * `textures`
              * `aliasIsolationLogo.png`
