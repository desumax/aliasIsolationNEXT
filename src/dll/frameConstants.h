#pragma once

#include "glm/glm.hpp"
#include "common.h"


struct FrameConstants
{
	glm::mat4	currViewMatrix;
	glm::mat4	prevViewProjNoJitter;
	glm::mat4	currViewProjNoJitter;

	glm::dmat4	currInvViewProjNoJitter;

	uint		screenWidth = 0;
	uint		screenHeight = 0;

	uint		taaSampleIdx = 0;
	float		gameTime = 0;
	bool		taaRanThisFrame = false;
	// Set when the hooks are toggled back on: the TAA history no longer matches the scene and is re-seeded.
	bool		resetTaaHistory = false;

	// Which of the game passes the mod relies on ran since the last Present, to spot wrong video settings.
	bool		scenePassThisFrame = false;
	bool		motionPassThisFrame = false;
	bool		sharpenPassThisFrame = false;
};

// Video settings the mod needs that the game doesn't appear to be using; shown as a warning by the menu.
enum SettingsWarning : unsigned
{
	SettingsWarning_MotionBlur = 1,
	SettingsWarning_Smaa = 2,
};
extern unsigned g_settingsWarnings;

extern FrameConstants g_frameConstants;
