#pragma once

struct Settings
{
	float sharpening = 0.3f;
	bool sharpeningEnabled = true;
	float chromaticAberration = 0.5f;
	bool chromaticAberrationEnabled = true;
	bool showLogo = true;
};

void setSettingsFilePath(const char*);
bool loadSettings(Settings*);
void loadOrCreateSettings(Settings*);
bool saveSettings(const Settings&);
