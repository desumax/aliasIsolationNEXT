#include "reshadeBridge.h"

// Dear ImGui must not be included before reshade.hpp here: with IMGUI_VERSION_NUM defined, registration also requires
// ReShade's ImGui function table, which this bridge doesn't use.
#include <Windows.h>
#include <reshade.hpp>

#include <algorithm>
#include <mutex>

// Shown in ReShade's add-on list.
extern "C" __declspec(dllexport) const char *NAME = "Alias Isolation";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Lets the Alias Isolation menu switch ReShade effects.";

namespace
{
enum class Registration
{
    NotTried,
    Registered,
    Unavailable,
};

struct PendingTechnique
{
    std::string effect;
    std::string name;
    bool enabled;
};

std::mutex g_mutex;
Registration g_registration = Registration::NotTried;
unsigned g_framesUntilRetry = 0;

// Written by the ReShade callbacks, read by the menu; both run on the render thread, the mutex is just a safeguard.
bool g_runtimeSeen = false;
bool g_effectsEnabled = true;
std::vector<ReShadeBridge::Technique> g_techniques;
bool g_techniquesDirty = true;

// Requests from the menu, applied at ReShade's next present.
int g_pendingEffectsState = -1;
std::vector<PendingTechnique> g_pendingTechniques;

bool containsNoCase(std::string haystack, std::string needle)
{
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
    return haystack.find(needle) != std::string::npos;
}

bool isHiddenTechnique(reshade::api::effect_runtime *runtime, reshade::api::effect_technique technique,
                       const std::string &effect, const std::string &name)
{
    bool hidden = false;
    runtime->get_annotation_bool_from_technique(technique, "hidden", &hidden, 1);
    return hidden || containsNoCase(name, "debug") || containsNoCase(effect, "DisplayDepth");
}

void refreshTechniques(reshade::api::effect_runtime *runtime)
{
    std::vector<ReShadeBridge::Technique> techniques;
    runtime->enumerate_techniques(
        nullptr,
        [](reshade::api::effect_runtime *runtime, reshade::api::effect_technique technique, void *user_data) {
            char buffer[256] = "";
            size_t size = sizeof(buffer);
            runtime->get_technique_effect_name(technique, buffer, &size);
            const std::string effect = buffer;

            size = sizeof(buffer);
            runtime->get_technique_name(technique, buffer, &size);
            const std::string name = buffer;

            if (isHiddenTechnique(runtime, technique, effect, name))
            {
                return;
            }

            size = sizeof(buffer);
            std::string label = name;
            if (runtime->get_annotation_string_from_technique(technique, "ui_label", buffer, &size) && buffer[0])
            {
                label = buffer;
            }

            static_cast<std::vector<ReShadeBridge::Technique> *>(user_data)->push_back(
                {effect, name, label, runtime->get_technique_state(technique)});
        },
        &techniques);

    g_techniques = std::move(techniques);
    g_techniquesDirty = false;
}

void onReShadePresent(reshade::api::effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtimeSeen = true;

    if (g_pendingEffectsState >= 0)
    {
        runtime->set_effects_state(g_pendingEffectsState == 1);
        g_pendingEffectsState = -1;
    }

    if (!g_pendingTechniques.empty())
    {
        for (const PendingTechnique &pending : g_pendingTechniques)
        {
            const reshade::api::effect_technique technique =
                runtime->find_technique(pending.effect.c_str(), pending.name.c_str());
            if (technique != 0)
            {
                runtime->set_technique_state(technique, pending.enabled);
            }
        }
        g_pendingTechniques.clear();
        // Keep the change across launches, the same as toggling it in ReShade's own overlay.
        runtime->save_current_preset();
        g_techniquesDirty = true;
    }

    g_effectsEnabled = runtime->get_effects_state();

    // Also refresh now and then, to pick up changes made in ReShade's overlay or with its hotkeys.
    static unsigned framesSinceRefresh = 0;
    if (g_techniquesDirty || ++framesSinceRefresh >= 30)
    {
        framesSinceRefresh = 0;
        refreshTechniques(runtime);
    }
}

void onReShadeReloadedEffects(reshade::api::effect_runtime *)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_techniquesDirty = true;
}

void onDestroyEffectRuntime(reshade::api::effect_runtime *)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtimeSeen = false;
    g_techniques.clear();
}
} // namespace

namespace ReShadeBridge
{
void update()
{
    if (g_registration != Registration::NotTried)
    {
        return;
    }

    // ReShade can finish loading after the first frames, so look for it for a while (about 10 seconds at 60 fps).
    if (g_framesUntilRetry > 0)
    {
        --g_framesUntilRetry;
        return;
    }

    static unsigned attempts = 0;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&update), &self);

    if (self && reshade::register_addon(self))
    {
        reshade::register_event<reshade::addon_event::reshade_present>(&onReShadePresent);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&onReShadeReloadedEffects);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&onDestroyEffectRuntime);
        g_registration = Registration::Registered;
        return;
    }

    if (++attempts >= 10)
    {
        g_registration = Registration::Unavailable;
    }
    g_framesUntilRetry = 60;
}

bool isAvailable()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_registration == Registration::Registered && g_runtimeSeen;
}

bool getEffectsEnabled()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pendingEffectsState >= 0 ? g_pendingEffectsState == 1 : g_effectsEnabled;
}

void setEffectsEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pendingEffectsState = enabled ? 1 : 0;
}

std::vector<Technique> getTechniques()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Technique> techniques = g_techniques;
    // Show pending changes straight away instead of flickering back for a frame.
    for (Technique &technique : techniques)
    {
        for (const PendingTechnique &pending : g_pendingTechniques)
        {
            if (pending.effect == technique.effect && pending.name == technique.name)
            {
                technique.enabled = pending.enabled;
            }
        }
    }
    return techniques;
}

void setTechniqueEnabled(const Technique &technique, bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pendingTechniques.push_back({technique.effect, technique.name, enabled});
}
} // namespace ReShadeBridge
