#pragma once

#include <string>
#include <vector>

// Lets the menu switch ReShade's effects (for example the DLSS5-Feeder and LumeniteFX techniques) when ReShade 6 with
// add-on support is loaded in the game. Without ReShade every function is a no-op and isAvailable() is false.
namespace ReShadeBridge
{
struct Technique
{
    std::string effect; // File name, e.g. "DLSS5_Feed.fx"
    std::string name;   // Technique name, e.g. "DLSS5_Feed"
    std::string label;  // The technique's ui_label, or its name
    bool enabled = false;
};

// Call from the render thread once per frame; registers with ReShade the first time it is found.
void update();

// Call from the game's Present before the per-frame pass flags are cleared. While DLSS 5 Feed is held back at
// launch, this watches for the first loading screen (the 3D scene pass stopping) to start it behind.
void onGameFrame(bool scenePass, bool shaderHooksEnabled);

// True while the hold waits for a loading screen. The game's Draw then stays hooked with the mod off, only to see
// whether the 3D scene was drawn.
bool isWatchingForLoadingScreen();

bool isAvailable();

// True when ReShade has loaded AliasIsolation.fx: the mod's switch and settings are then in ReShade's Home tab, and
// the bridge keeps them in sync with g_settings, the shader hooks and the intro videos.
bool hasHomePage();
bool getEffectsEnabled();
void setEffectsEnabled(bool enabled);

// Techniques loaded by ReShade, minus debug views and the DisplayDepth helper.
std::vector<Technique> getTechniques();
void setTechniqueEnabled(const Technique &technique, bool enabled);
} // namespace ReShadeBridge
