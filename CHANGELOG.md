# Changelog

## 1.3.0-fixes (aliasIsolationNEXT fork)

### Fixed
- The mod menu can now be used with the mouse. The game reads the mouse through Raw Input and keeps recentring and clipping the cursor, so ImGui never saw clicks. While the menu is open the mod now reads the cursor itself, blocks the game's cursor recentring and clipping, and draws its own cursor. (#62, #64, #66, #69)
- `settings.txt` is created with the defaults when it is missing, and is read as `key = value` lines in any order. A damaged or partial file no longer makes every setting zero. (#12, #62, #69)
- `Ctrl+Delete` / `Ctrl+Insert` work reliably. The hotkeys were read with `GetKeyState` from a thread with no message queue, and the shader hooks were switched from that thread mid-frame. The toggle is now applied by the render thread at the next Present, and the TAA history is reset on re-enable, so there is no smeared first frame. (#70)
- Constant buffers are updated with `MAP_WRITE_DISCARD`. `MAP_WRITE_NO_OVERWRITE` on a constant buffer is invalid without D3D11.1 support and could show stale constants on some drivers.
- TAA and chromatic aberration views are created once instead of every frame, and their textures are released on resolution change, which fixes a leak. This may help the low performance reported on AMD GPUs, but it has not been verified on AMD hardware. (#32, #38, #65)

- With an HDR swapchain (Special K, Windows Auto HDR) the chromatic aberration pass copied the frame through an 8-bit texture, clipping highlights. It now uses the swapchain's FP16 or 10-bit format. (#56, #60)
- TAA silently turned off when an HDR tool upgraded the game's R11G11B10 scene buffer to FP16 (Special K "HDR Remaster 11 bits"). FP16 is now accepted, and the history textures follow the scene buffer's format. (#56)

### Added
- An installer script (`Install.bat` / `Install-AliasIsolationNEXT.ps1`). Pressing Enter installs everything: the mod (the legacy build on CPUs without AVX2), skipped intro videos, and the DLSS 5 + LumeniteFX pack, which it gets by running DLSS5-Feeder's official installer after asking. It finds the game through Steam, the Epic Games Launcher or GOG; otherwise you paste the folder or pick it in a window. It keeps the ASI loader as `d3d11.dll` and moves ReShade to `dxgi.dll` if needed.
- Alias Isolation in ReShade's **Home** tab (`AliasIsolation.fx`). Its checkbox switches the mod, with sharpening, chromatic aberration and logo options in the Variables list. Skipping the intro videos is its own Home entry. They stay in sync with `settings.txt`. The installer also hides DLSS5-Feeder's debug view and DisplayDepth from Home, and starts the Add-ons tab's entries collapsed.
- The installer sets the DLSS 5 pack's HDR options (`hdr`, `hdr_bridge` in `dlss5-feed.cfg`; `HDR`, `ForceHDR`, `UseHDR10` in `host64\OptiScaler.ini`) from the monitors' Windows HDR state. The game always presents SDR, so with RTX HDR or Auto HDR they are set to SDR. "Auto" could take the SDR frame for HDR there, and the neural pass then looked like it did nothing. They are set to HDR only when Special K or RenoDX in the game folder makes the output HDR. Menu option 9 redoes this.
- DLSS 5 Feed is held off at launch and switched on at the first loading screen, so its start-up stall (NVIDIA's runtime starting in the feeder's helper) is hidden there. A loading screen is the 3D scene pass stopping after it has run; without one the feed starts after 30 seconds in 3D. It can be turned off in Home > Alias Isolation, and what it sees is written to ReShade.log. Untested in game so far.
- The crash message lists `module+offset` for each stack frame. Without a PDB every address in the mod was shown as the nearest export, which is likely why Epic crashes all read `aliasIsolation_hookableOverlayRender`.
- A ReShade section in the menu when ReShade 6 with add-on support is loaded, used when `AliasIsolation.fx` is not installed. It has switches for ReShade's effects as a whole and for each technique, without the debug views and DisplayDepth. Changes are saved in the ReShade preset.
- A "Skip intro videos" switch in the menu, which takes effect from the next launch.
- `tools/Package-Release.ps1` builds the release archive, including the legacy AVX1/SSE3 builds.
- An overlay in the top-left corner when the game isn't using SMAA T1x or Motion Blur, which the mod needs. It goes away once the setting is fixed or the mod is turned off.
- "Show logo when a level loads" option (`showLogo` in `settings.txt`). When it is off, the logo texture is not loaded. (#72)
- `ALIASISOLATION_CPU` build option (`V3`, `AVX1`, `SSE3`) for legacy CPU builds. (#64)
- README sections on uninstalling and on settings, and a note on conflicts with RTX HDR, RTSS and Special K. (#62, #67, #74)

### Changed
- Default sharpening is 0.3 and default chromatic aberration is 0.5. Previously both were 0 when no settings file existed. (#71)

### Not addressed
- The HDR fixes above are untested on HDR hardware, and the Windows Auto HDR "doubling" in #60 may have a second cause.
- RTX HDR crash on some level loads (#67): the crash dumps point outside the mod's code, and the reporter's missing logo file may have been involved, which 1.3.0-fixes no longer touches. Needs retesting.
- Barrel distortion (#73) and motion blur rework (#57): feature requests.
- Known issues from the README (thin or ghosting particles, signs fading in, DoF showing the motion tracker's LEDs at the screen edges) are in the TAA compute shader, whose HLSL source was never published; only the compiled `taa_cs` is in the repository. Fixing them needs a rewritten TAA shader.
- MotherVR support.
