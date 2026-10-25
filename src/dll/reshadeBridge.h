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

bool isAvailable();
bool getEffectsEnabled();
void setEffectsEnabled(bool enabled);

// Techniques loaded by ReShade, minus debug views and the DisplayDepth helper.
std::vector<Technique> getTechniques();
void setTechniqueEnabled(const Technique &technique, bool enabled);
} // namespace ReShadeBridge
