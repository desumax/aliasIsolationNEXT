#include "reshadeBridge.h"

// Dear ImGui must not be included before reshade.hpp here: with IMGUI_VERSION_NUM defined, registration also requires
// ReShade's ImGui function table, which this bridge doesn't use.
#include <Windows.h>
#include <reshade.hpp>

#include <algorithm>
#include <mutex>

#include "introVideos.h"
#include "settings.h"

extern Settings g_settings;
void requestShaderHooks(bool enable);
std::string getDataFilePath(std::string path, bool critical);

// Shown in ReShade's add-on list.
extern "C" __declspec(dllexport) const char *NAME = "Alias Isolation";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Shows Alias Isolation in the Home tab (AliasIsolation.fx). Keep this enabled; the mod itself is switched in Home.";

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

// --- The Home tab page (AliasIsolation.fx) ----------------------------------------------------------------------

constexpr const char *s_homeEffect = "AliasIsolation.fx";
constexpr const char *s_homeTechnique = "AliasIsolation";
constexpr const char *s_homeSkipIntrosTechnique = "AliasIsolation_SkipIntroVideos";

bool g_homePage = false;
// Cleared again after ReShade reloads its effects, which resets the uniforms to the values in the preset.
bool g_homePageSynced = false;
bool g_homeTechniqueEnabled = true;
bool g_homeSkipIntros = false;

bool getBool(reshade::api::effect_runtime *runtime, const char *name, bool fallback)
{
    const reshade::api::effect_uniform_variable variable = runtime->find_uniform_variable(s_homeEffect, name);
    if (variable == 0)
    {
        return fallback;
    }
    bool value = fallback;
    runtime->get_uniform_value_bool(variable, &value, 1);
    return value;
}

float getFloat(reshade::api::effect_runtime *runtime, const char *name, float fallback)
{
    const reshade::api::effect_uniform_variable variable = runtime->find_uniform_variable(s_homeEffect, name);
    if (variable == 0)
    {
        return fallback;
    }
    float value = fallback;
    runtime->get_uniform_value_float(variable, &value, 1);
    return value;
}

void setBool(reshade::api::effect_runtime *runtime, const char *name, bool value)
{
    const reshade::api::effect_uniform_variable variable = runtime->find_uniform_variable(s_homeEffect, name);
    if (variable != 0)
    {
        runtime->set_uniform_value_bool(variable, &value, 1);
    }
}

void setFloat(reshade::api::effect_runtime *runtime, const char *name, float value)
{
    const reshade::api::effect_uniform_variable variable = runtime->find_uniform_variable(s_homeEffect, name);
    if (variable != 0)
    {
        runtime->set_uniform_value_float(variable, &value, 1);
    }
}

bool sameSettings(const Settings &a, const Settings &b)
{
    return a.sharpening == b.sharpening && a.sharpeningEnabled == b.sharpeningEnabled &&
           a.chromaticAberration == b.chromaticAberration &&
           a.chromaticAberrationEnabled == b.chromaticAberrationEnabled && a.showLogo == b.showLogo;
}

void syncHomePage(reshade::api::effect_runtime *runtime)
{
    const reshade::api::effect_technique technique = runtime->find_technique(s_homeEffect, s_homeTechnique);
    g_homePage = technique != 0;
    if (!g_homePage)
    {
        return;
    }

    const bool techniqueEnabled = runtime->get_technique_state(technique);
    const reshade::api::effect_technique skipIntrosTechnique =
        runtime->find_technique(s_homeEffect, s_homeSkipIntrosTechnique);

    // settings.txt and the files on disk are the truth when the page first appears; after that the page is.
    if (!g_homePageSynced)
    {
        g_homePageSynced = true;
        setBool(runtime, "SharpeningEnabled", g_settings.sharpeningEnabled);
        setFloat(runtime, "Sharpening", g_settings.sharpening);
        setBool(runtime, "ChromaticAberrationEnabled", g_settings.chromaticAberrationEnabled);
        setFloat(runtime, "ChromaticAberration", g_settings.chromaticAberration);
        setBool(runtime, "ShowLogo", g_settings.showLogo);
        g_homeSkipIntros = IntroVideos::getState() == IntroVideos::State::Skipped;
        if (skipIntrosTechnique != 0 && runtime->get_technique_state(skipIntrosTechnique) != g_homeSkipIntros)
        {
            runtime->set_technique_state(skipIntrosTechnique, g_homeSkipIntros);
            runtime->save_current_preset();
        }
        g_homeTechniqueEnabled = techniqueEnabled;
        requestShaderHooks(techniqueEnabled);
        return;
    }

    if (techniqueEnabled != g_homeTechniqueEnabled)
    {
        g_homeTechniqueEnabled = techniqueEnabled;
        requestShaderHooks(techniqueEnabled);
    }

    Settings values = g_settings;
    values.sharpeningEnabled = getBool(runtime, "SharpeningEnabled", values.sharpeningEnabled);
    values.sharpening = getFloat(runtime, "Sharpening", values.sharpening);
    values.chromaticAberrationEnabled =
        getBool(runtime, "ChromaticAberrationEnabled", values.chromaticAberrationEnabled);
    values.chromaticAberration = getFloat(runtime, "ChromaticAberration", values.chromaticAberration);
    values.showLogo = getBool(runtime, "ShowLogo", values.showLogo);
    if (!sameSettings(values, g_settings))
    {
        g_settings = values;
        saveSettings(g_settings);
    }

    if (skipIntrosTechnique == 0)
    {
        return;
    }
    const bool skipIntros = runtime->get_technique_state(skipIntrosTechnique);
    if (skipIntros != g_homeSkipIntros)
    {
        IntroVideos::setSkipped(skipIntros);
        // Show what actually happened, e.g. when the files were missing or couldn't be moved.
        g_homeSkipIntros = IntroVideos::getState() == IntroVideos::State::Skipped;
        if (g_homeSkipIntros != skipIntros)
        {
            runtime->set_technique_state(skipIntrosTechnique, g_homeSkipIntros);
            runtime->save_current_preset();
        }
    }
}

// --- Holding DLSS 5 back until the first loading screen ---------------------------------------------------------
//
// DLSS5-Feeder starts its helper and NVIDIA's runtime when its technique first runs, and the first evaluations stall
// the game for a second or more. Starting it behind a loading screen hides that. A loading screen shows up here as
// the 3D scene pass stopping after it has run for a while. If none comes (the first load was missed, or the mod is
// off and there is no scene pass to watch) the feed starts anyway after a fallback time.

constexpr const char *s_feedEffect = "DLSS5_Feed.fx";
constexpr const char *s_feedTechnique = "DLSS5_Feed";
constexpr unsigned s_minSceneFrames = 120;         // 3D frames before a pause can count as a loading screen
constexpr unsigned s_loadingFrames = 30;           // Frames without the 3D scene that mean a loading screen
constexpr unsigned s_fallbackSceneFrames = 1800;   // 3D frames in a row after which the feed starts anyway
constexpr unsigned s_fallbackHooksOffFrames = 600; // Frames with the mod off after which the feed starts anyway

enum class FeedHold
{
    Waiting,  // The feed's technique hasn't been seen yet
    Holding,  // Switched off by the mod until a loading screen
    Released, // Left alone from now on
};
FeedHold g_feedHold = FeedHold::Waiting;
unsigned g_sceneFrames = 0;
unsigned g_noSceneFrames = 0;
unsigned g_hooksOffFrames = 0;
unsigned g_longestSceneRun = 0;
const char *g_releaseReason = nullptr; // Set from the game's Present, applied at ReShade's next present

void logInfo(const std::string &text)
{
    reshade::log::message(reshade::log::level::info, ("[Alias Isolation] " + text).c_str());
}

// ReShade may save the preset while the feed is held off (any change in its overlay does). This file says the
// switch is off because of the hold, so the next launch still treats DLSS 5 as wanted.
std::string feedHoldMarkerPath()
{
    return getDataFilePath("dlss5-held.txt", false);
}

bool feedHoldMarkerExists()
{
    return GetFileAttributesA(feedHoldMarkerPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

void setFeedHoldMarker(bool present)
{
    const std::string path = feedHoldMarkerPath();
    if (!present)
    {
        DeleteFileA(path.c_str());
        return;
    }
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file);
    }
}

void beginFeedHold(reshade::api::effect_runtime *runtime)
{
    if (g_feedHold != FeedHold::Waiting)
    {
        return;
    }
    const reshade::api::effect_technique technique = runtime->find_technique(s_feedEffect, s_feedTechnique);
    if (technique == 0)
    {
        return; // Not compiled yet, or DLSS5-Feeder isn't installed
    }

    const bool heldLastTime = feedHoldMarkerExists();
    const bool wanted = runtime->get_technique_state(technique) || heldLastTime;
    if (!wanted || !getBool(runtime, "StartDlss5AtLoading", true))
    {
        if (wanted && heldLastTime)
        {
            runtime->set_technique_state(technique, true);
            runtime->save_current_preset();
        }
        setFeedHoldMarker(false);
        g_feedHold = FeedHold::Released;
        return;
    }

    runtime->set_technique_state(technique, false);
    setFeedHoldMarker(true);
    g_feedHold = FeedHold::Holding;
    g_sceneFrames = g_noSceneFrames = g_hooksOffFrames = g_longestSceneRun = 0;
    g_releaseReason = nullptr;
    logInfo("DLSS 5 Feed is held off until the first loading screen.");
}

void updateFeedHold(reshade::api::effect_runtime *runtime)
{
    beginFeedHold(runtime);
    if (g_feedHold != FeedHold::Holding)
    {
        return;
    }

    const reshade::api::effect_technique technique = runtime->find_technique(s_feedEffect, s_feedTechnique);
    if (technique == 0)
    {
        g_feedHold = FeedHold::Released;
        setFeedHoldMarker(false);
        return;
    }
    if (runtime->get_technique_state(technique))
    {
        g_releaseReason = "switched on in ReShade";
    }
    else if (!getBool(runtime, "StartDlss5AtLoading", true))
    {
        g_releaseReason = "the option was turned off";
    }
    if (g_releaseReason == nullptr)
    {
        return;
    }

    runtime->set_technique_state(technique, true);
    runtime->save_current_preset();
    setFeedHoldMarker(false);
    g_feedHold = FeedHold::Released;
    logInfo(std::string("DLSS 5 Feed started: ") + g_releaseReason + ".");
}

bool isHiddenTechnique(reshade::api::effect_runtime *runtime, reshade::api::effect_technique technique,
                       const std::string &effect, const std::string &name)
{
    bool hidden = false;
    runtime->get_annotation_bool_from_technique(technique, "hidden", &hidden, 1);
    return hidden || containsNoCase(name, "debug") || containsNoCase(effect, "DisplayDepth") || effect == s_homeEffect;
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
    syncHomePage(runtime);
    updateFeedHold(runtime);

    // Also refresh now and then, to pick up changes made in ReShade's overlay or with its hotkeys.
    static unsigned framesSinceRefresh = 0;
    if (g_techniquesDirty || ++framesSinceRefresh >= 30)
    {
        framesSinceRefresh = 0;
        refreshTechniques(runtime);
    }
}

void onReShadeReloadedEffects(reshade::api::effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_techniquesDirty = true;
    g_homePageSynced = false;
    // As early as possible: DLSS5-Feeder starts on the first frame its technique runs.
    beginFeedHold(runtime);
}

void onDestroyEffectRuntime(reshade::api::effect_runtime *)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtimeSeen = false;
    g_techniques.clear();
    g_homePage = false;
    g_homePageSynced = false;
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

void onGameFrame(bool scenePass, bool shaderHooksEnabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_registration != Registration::Registered || g_feedHold != FeedHold::Holding || g_releaseReason)
    {
        return;
    }

    // With the mod off there is no scene pass to watch.
    g_hooksOffFrames = shaderHooksEnabled ? 0 : g_hooksOffFrames + 1;
    if (g_hooksOffFrames >= s_fallbackHooksOffFrames)
    {
        g_releaseReason = "Alias Isolation is off, so loading screens can't be detected";
        return;
    }

    if (scenePass)
    {
        if (g_sceneFrames == 0)
        {
            logInfo("3D scene started after " + std::to_string(g_noSceneFrames) + " frames without it.");
        }
        g_noSceneFrames = 0;
        if (++g_sceneFrames >= s_fallbackSceneFrames)
        {
            g_releaseReason = "no loading screen was seen in the first 30 seconds of 3D";
        }
        return;
    }

    if (g_sceneFrames > 0)
    {
        logInfo("3D scene stopped after " + std::to_string(g_sceneFrames) + " frames.");
        g_longestSceneRun = (std::max)(g_longestSceneRun, g_sceneFrames);
        g_sceneFrames = 0;
    }
    if (++g_noSceneFrames >= s_loadingFrames && g_longestSceneRun >= s_minSceneFrames)
    {
        g_releaseReason = "loading screen";
    }
}

bool isAvailable()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_registration == Registration::Registered && g_runtimeSeen;
}

bool hasHomePage()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_registration == Registration::Registered && g_runtimeSeen && g_homePage;
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
