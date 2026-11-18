/*
This source file is part of Rigs of Rods
Copyright 2005-2012 Pierre-Michel Ricordel
Copyright 2007-2012 Thomas Fischer

For more information, see http://www.rigsofrods.com/

Rigs of Rods is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 3, as
published by the Free Software Foundation.

Rigs of Rods is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Rigs of Rods.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "Application.h"
#include "CameraManager.h"

#include "gfx/skyx/SkyX.h"

#include <algorithm>
#include <memory>

namespace RoR {

class SkyXWeatherAudio;
class SkyXRainParticles;

/// @addtogroup Gfx
/// @{

class SkyXManager
{
public:
	SkyXManager(Ogre::String configFile);
	~SkyXManager();

	Ogre::Vector3 getMainLightDirection();

	Ogre::Light* getMainLight();

	/// @param audio_dt Time step of the simulation for sky, rain and weather sounds: 0 = paused (everything freezes, sounds are silent); negative = unknown, same as dt.
	bool update( float dt, float audio_dt = -1.f );

	SkyXWeatherAudio* GetWeatherAudio() { return mWeatherAudio.get(); }
	SkyXRainParticles* GetRainParticles() { return mRainParticles.get(); }

	bool InitLight();

	bool UpdateSkyLight();

	SkyX::SkyX* GetSkyX() { return mSkyX; }
	SkyX::BasicController* GetBasicController() { return mBasicController; }

	/// Time speed (sim hours per real second) used when no time key is pressed. Initially from the .skx file.
	float GetBaseTimeMultiplier() const { return mBaseTimeMultiplier; }
	void SetBaseTimeMultiplier(float m) { mBaseTimeMultiplier = m; }

	/// Time of day in hours [0-24]
	float GetTimeOfDay();
	void SetTimeOfDay(float hour);
	std::string GetPrettyTime(); //!< "HH:MM"

	/// Lets the scene fog colour follow the sky (fog itself must be enabled by the terrain).
	void SetFogFollowsSky(bool v) { mFogFollowsSky = v; }

	/// Stores the current sky state as the map defaults ("Reset to map defaults" in the sky settings).
	/// Call after the sky was configured from outside the .skx file (e.g. a converted Caelum script).
	void CommitMapDefaults() { this->SaveMapDefaults(); }

	/// The water color is recomputed from a time-of-day gradient every frame; while an override is set, it is used instead.
	void SetWaterColorOverride(const Ogre::Vector3& color) { mWaterColorOverride = color; mHasWaterColorOverride = true; }
	void ClearWaterColorOverride() { mHasWaterColorOverride = false; }
	bool HasWaterColorOverride() const { return mHasWaterColorOverride; }

	/// Brightness of the night: 1 = default, 0 = pitch black, >1 = brighter (moonlit) night.
	/// Scales the main light and the scene ambient light while the sun is below the horizon (smooth transition at dusk and dawn).
	void SetNightBrightness(float value) { mNightBrightness = std::max(0.f, value); }
	float GetNightBrightness() const { return mNightBrightness; }

	/// Restores the sky settings that the map (terrn2/SkyX config) created the sky with.
	void ResetToMapDefaults();

protected:
	Ogre::Light *mLight0 = nullptr;
	Ogre::Light *mLight1 = nullptr;
	
	SkyX::SkyX* mSkyX = nullptr;
	SkyX::BasicController* mBasicController = nullptr;

	SkyX::ColorGradient mWaterGradient, 
		                mSunGradient, 
						mAmbientGradient;

	SkyX::CfgFileManager* mCfgFileManager = nullptr;

	/// Rain/wind/thunder sounds
	std::unique_ptr<SkyXWeatherAudio> mWeatherAudio;

	/// 3D rain streaks around the camera
	std::unique_ptr<SkyXRainParticles> mRainParticles;

    int mLastHour = 0;

	void SaveMapDefaults();

	/// Precipitation drops react to movement of the player (not to camera orbiting/zooming)
	void DetectPlayerMovement(float dt);

	ActorPtr mLastPlayerActor;
	CameraManager::CameraBehaviors mLastCameraBehavior = CameraManager::CAMERA_BEHAVIOR_INVALID;
	Ogre::Vector3 mLastPlayerPosition = Ogre::Vector3::ZERO;

	/// Sky state right after the map was loaded
	struct MapDefaults
	{
		bool valid = false;
		SkyX::AtmosphereManager::Options atmosphere;
		bool starfield = true;
		Ogre::Real moonPhase = 0.f;
		Ogre::Real sunrise = 7.5f;
		Ogre::Real sunset = 20.5f;
		bool vclouds = false;
		Ogre::Vector2 vcloudsWeather = Ogre::Vector2::ZERO;
		Ogre::Vector2 vcloudsHeight = Ogre::Vector2::ZERO;
		Ogre::Real vcloudsWindSpeed = 0.f;
		Ogre::Radian vcloudsWindDir;
		Ogre::Real vcloudsOpacity = 1.f;
		Ogre::Real vcloudsFieldScale = 1.f;
		Ogre::Real vcloudsNoiseScale = 4.2f;
		Ogre::Vector3 vcloudsTint = Ogre::Vector3(1.f, 1.f, 1.f);
		Ogre::Vector4 vcloudsLightResponse = Ogre::Vector4::ZERO;
		Ogre::Vector4 vcloudsAmbientFactors = Ogre::Vector4::ZERO;
		bool lightning = false;
		Ogre::Real lightningAvgTime = 1.5f;
		Ogre::Real lightningSpeed = 2.f;
		Ogre::Vector3 lightningColor = Ogre::Vector3(1.f, 0.925f, 0.85f);
		bool astronomical = false;
		float latitude = 45.f;
		int dayOfYear = 150;
		float precipIntensity = 0.f;
		std::string precipTexture;
		Ogre::ColourValue precipColour = Ogre::ColourValue::White;
		float precipSpeed = 0.85f;
	} mMapDefaults;

	float mNightBrightness = 1.f;
	Ogre::ColourValue mBaseAmbient = Ogre::ColourValue::Black; //!< Scene ambient light as the map set it
	bool mBaseAmbientValid = false;
	bool mAmbientTouched = false;

	bool mHasWaterColorOverride = false;
	Ogre::Vector3 mWaterColorOverride = Ogre::Vector3::ZERO;

	/// Underwater the viewport background and fog take the water colour (map edge shows blue water instead of black).
	void UpdateUnderwaterView(bool camUnderwater);
	bool mUwActive = false;
	Ogre::ColourValue mUwSavedBackground;
	Ogre::FogMode mUwSavedFogMode = Ogre::FOG_NONE;
	Ogre::ColourValue mUwSavedFogColour;
	Ogre::Real mUwSavedFogDensity = 0.f, mUwSavedFogStart = 0.f, mUwSavedFogEnd = 0.f;
    float mBaseTimeMultiplier = 0.01f;
    bool mFogFollowsSky = false;

    /// High above the ground the fog range grows with the height, otherwise the whole terrain is swallowed by fog.
    void UpdateAltitudeFog(const Ogre::Vector3& camPos);
    bool mAltFogValid = false;
    Ogre::FogMode mAltFogMode = Ogre::FOG_NONE;
    Ogre::Real mAltFogDensity = 0.f, mAltFogStart = 0.f, mAltFogEnd = 0.f;      //!< fog as the map set it
    Ogre::Real mAltFogSetDensity = 0.f, mAltFogSetStart = 0.f, mAltFogSetEnd = 0.f; //!< what we applied last
};

/// @} // addtogroup Gfx

} // namespace RoR
