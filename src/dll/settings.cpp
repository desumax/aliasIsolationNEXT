#include "settings.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "windows.h"

namespace {
	std::string g_settingsFilePath;
}

void setSettingsFilePath(const char* path)
{
	g_settingsFilePath = path;
}

// Reads "key = value" lines in any order. Unknown keys and malformed lines are skipped, and missing keys keep
// their defaults, so a partial or hand-edited file never discards the rest of the settings.
bool loadSettings(Settings* settingsOut)
{
	FILE* f = nullptr;
	if (fopen_s(&f, g_settingsFilePath.c_str(), "r") != 0 || !f) {
		return false;
	}

	bool sharpeningSwitchRead = false;
	bool chromaticAberrationSwitchRead = false;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char key[64];
		float value;
		if (sscanf_s(line, " %63[A-Za-z] = %f", key, static_cast<unsigned>(sizeof(key)), &value) != 2) {
			continue;
		}

		if (strcmp(key, "sharpening") == 0) {
			settingsOut->sharpening = value;
		} else if (strcmp(key, "sharpeningEnabled") == 0) {
			settingsOut->sharpeningEnabled = value != 0.0f;
			sharpeningSwitchRead = true;
		} else if (strcmp(key, "chromaticAberration") == 0) {
			settingsOut->chromaticAberration = value;
		} else if (strcmp(key, "chromaticAberrationEnabled") == 0) {
			settingsOut->chromaticAberrationEnabled = value != 0.0f;
			chromaticAberrationSwitchRead = true;
		} else if (strcmp(key, "showLogo") == 0) {
			settingsOut->showLogo = value != 0.0f;
		}
	}
	fclose(f);

	// Older files have no switches, and an amount of 0 meant off. Keep the default amount then, so switching the
	// effect back on doesn't give an amount of 0.
	const Settings defaults;
	if (!sharpeningSwitchRead) {
		settingsOut->sharpeningEnabled = (settingsOut->sharpening > 0.0f);
		if (!settingsOut->sharpeningEnabled) {
			settingsOut->sharpening = defaults.sharpening;
		}
	}
	if (!chromaticAberrationSwitchRead) {
		settingsOut->chromaticAberrationEnabled = (settingsOut->chromaticAberration > 0.0f);
		if (!settingsOut->chromaticAberrationEnabled) {
			settingsOut->chromaticAberration = defaults.chromaticAberration;
		}
	}

	return true;
}

// Loads the settings file, or writes one with the defaults if it is missing, so players can always edit
// settings.txt by hand even when the in-game menu is unavailable.
void loadOrCreateSettings(Settings* settingsOut)
{
	if (!loadSettings(settingsOut)) {
		saveSettings(*settingsOut);
	}
}

bool saveSettings(const Settings& settings)
{
	FILE* f = nullptr;
	if (fopen_s(&f, g_settingsFilePath.c_str(), "w") != 0 || !f) {
		return false;
	}

	// The switches are saved apart from the amounts, so turning an effect off keeps its amount for later.
	const bool ok = fprintf_s(f, "sharpening = %f\n", settings.sharpening) >= 0 &&
		fprintf_s(f, "sharpeningEnabled = %d\n", settings.sharpeningEnabled ? 1 : 0) >= 0 &&
		fprintf_s(f, "chromaticAberration = %f\n", settings.chromaticAberration) >= 0 &&
		fprintf_s(f, "chromaticAberrationEnabled = %d\n", settings.chromaticAberrationEnabled ? 1 : 0) >= 0 &&
		fprintf_s(f, "showLogo = %d\n", settings.showLogo ? 1 : 0) >= 0;

	return fclose(f) == 0 && ok;
}
