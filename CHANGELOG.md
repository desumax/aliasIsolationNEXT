# Changelog

## 1.3.0 (community fork)

### Fixed
- The mod menu can now be used with the mouse. The game reads the mouse through Raw Input and keeps recentring and clipping the cursor, so ImGui never saw clicks. While the menu is open the mod now reads the cursor itself, blocks the game's cursor recentring and clipping, and draws its own cursor. (#62, #64, #66, #69)
- `settings.txt` is created with the defaults when it is missing, and is read as `key = value` lines in any order. A damaged or partial file no longer makes every setting zero. (#12, #62, #69)
- `Ctrl+Delete` / `Ctrl+Insert` work reliably. The hotkeys were read with `GetKeyState` from a thread with no message queue, and the shader hooks were switched from that thread mid-frame. The toggle is now applied by the render thread at the next Present, and the TAA history is reset on re-enable, so there is no smeared first frame. (#70)
- Constant buffers are updated with `MAP_WRITE_DISCARD`. `MAP_WRITE_NO_OVERWRITE` on a constant buffer is invalid without D3D11.1 support and could show stale constants on some drivers.
- TAA and chromatic aberration views are created once instead of every frame, and their textures are released on resolution change, which fixes a leak. This may help the low performance reported on AMD GPUs, but it has not been verified on AMD hardware. (#32, #38, #65)

### Added
- "Show logo when a level loads" option (`showLogo` in `settings.txt`). When it is off, the logo texture is not loaded. (#72)
- `ALIASISOLATION_CPU` build option (`V3`, `AVX1`, `SSE3`) for legacy CPU builds. (#64)
- README sections on uninstalling and on settings, and a note on conflicts with RTX HDR, RTSS and Special K. (#62, #67, #74)

### Changed
- Default sharpening is 0.3 and default chromatic aberration is 0.5. Previously both were 0 when no settings file existed. (#71)

### Not addressed
- HDR / deep-colour output (#41, #56, #60): needs testing on HDR hardware.
- Barrel distortion (#73) and motion blur rework (#57): feature requests.
