#pragma once

// The publisher and studio logo videos played at launch (AMD, Creative Assembly, Fox). Skipping them moves them from
// DATA\UI\MOVIES into a "_intro-videos-backup" folder next to AI.exe, which the installer script uses too. The game
// reads the folder at launch, so a change takes effect on the next start.
namespace IntroVideos
{
enum class State
{
    Playing,  // All of them are in DATA\UI\MOVIES
    Skipped,  // All of them are in the backup folder
    Unknown,  // Missing or split between the two; the menu doesn't offer the toggle
};

State getState();
bool setSkipped(bool skipped);
} // namespace IntroVideos
