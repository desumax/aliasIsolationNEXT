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

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char key[64];
		float value;
		if (sscanf_s(line, " %63[A-Za-z] = %f", key, static_cast<unsigned>(sizeof(key)), &value) != 2) {
			continue;
		}

		if (strcmp(key, "sharpening") == 0) {
			settingsOut->sharpening = value;
		} else if (strcmp(key, "chromaticAberration") == 0) {
			settingsOut->chromaticAberration = value;
		} else if (strcmp(key, "showLogo") == 0) {
			settingsOut->showLogo = value != 0.0f;
		}
	}
	fclose(f);

	settingsOut->sharpeningEnabled = (settingsOut->sharpening > 0.0f);
	settingsOut->chromaticAberrationEnabled = (settingsOut->chromaticAberration > 0.0f);

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

	const float sharpening = settings.sharpeningEnabled ? settings.sharpening : 0.0f;
	const float chromaticAberration = settings.chromaticAberrationEnabled ? settings.chromaticAberration : 0.0f;
	const bool ok = fprintf_s(f, "sharpening = %f\n", sharpening) >= 0 &&
		fprintf_s(f, "chromaticAberration = %f\n", chromaticAberration) >= 0 &&
		fprintf_s(f, "showLogo = %d\n", settings.showLogo ? 1 : 0) >= 0;

	return fclose(f) == 0 && ok;
}
