/*
	This source file is part of Rigs of Rods
	Copyright 2005-2012 Pierre-Michel Ricordel
	Copyright 2007-2012 Thomas Fischer
	Copyright 2013-2014 Petr Ohlidal

	For more information, see http://www.rigsofrods.com/

	Rigs of Rods is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License version 3, as
	published by the Free Software Foundation.

	Rigs of Rods is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Rigs of Rods. If not, see <http://www.gnu.org/licenses/>.
*/

#include "SkyXManager.h"
#include <cstdlib>

#include "Actor.h"
#include "AppContext.h"
#include "CameraManager.h"
#include "Character.h"
#include "CharacterFactory.h"
#include "GameContext.h"
#include "GfxScene.h"
#include "HydraxWater.h"
#include "SkyXRainParticles.h"
#include "SkyXWeatherAudio.h"
#include "Terrain.h"
#include "TerrainGeometryManager.h"

#include <algorithm>

using namespace Ogre;
using namespace RoR;

SkyXManager::SkyXManager(Ogre::String configFile)
{
	InitLight();

	//Ogre::ResourceGroupManager::getSingleton().addResourceLocation("..\\resource\\SkyX\\","FileSystem", "SkyX",true); //Temp

	mBasicController = new SkyX::BasicController(/*deleteBySkyX=*/false); // deleted in ~SkyXManager()
	mSkyX = new SkyX::SkyX(App::GetGfxScene()->GetSceneManager(), mBasicController);

	mCfgFileManager = new SkyX::CfgFileManager(mSkyX, mBasicController, App::GetCameraManager()->GetCamera());
	mCfgFileManager->load(configFile);
	mBaseTimeMultiplier = mSkyX->getTimeMultiplier();

	mSkyX->create();

	RoR::App::GetAppContext()->GetOgreRoot()->addFrameListener(mSkyX);
	RoR::App::GetAppContext()->GetRenderWindow()->addListener(mSkyX);

	// Needed for precipitation (ported from Caelum) to know which viewports to create compositor instances for.
	mSkyX->attachViewport(RoR::App::GetAppContext()->GetViewport());

	// Lightning bolts should end on the ground (terrain or water), not in mid-air
	mSkyX->getVCloudsManager()->getVClouds()->getLightningManager()->setGroundHeightFunction(
		[](Ogre::Real x, Ogre::Real z) -> Ogre::Real
		{
			const TerrainPtr terrain = App::GetGameContext()->GetTerrain();
			if (terrain == nullptr)
				return 0.f;
			float h = terrain->getHeightAt(x, z);
			if (terrain->getWater() != nullptr)
				h = std::max(h, terrain->getWaterHeight());
			return h;
		});

	mWeatherAudio.reset(new SkyXWeatherAudio(mSkyX));
	mRainParticles.reset(new SkyXRainParticles(mSkyX, App::GetGfxScene()->GetSceneManager()));

	this->SaveMapDefaults();
}

SkyXManager::~SkyXManager()
{
    // Must happen before SkyX is removed (unregisters from the lightning manager)
    this->UpdateUnderwaterView(false);
    mWeatherAudio.reset();
    mRainParticles.reset();

    // Removes the precipitation compositor from the (persistent) viewport
    mSkyX->detachViewport(RoR::App::GetAppContext()->GetViewport());

    RoR::App::GetAppContext()->GetOgreRoot()->removeFrameListener(mSkyX);
    RoR::App::GetAppContext()->GetRenderWindow()->removeListener(mSkyX);
    mSkyX->remove();

    delete mCfgFileManager;
    mCfgFileManager = nullptr;

    delete mSkyX;
    mSkyX = nullptr;

    delete mBasicController;
    mBasicController = nullptr;
}

Vector3 SkyXManager::getMainLightDirection()
{
	if (mBasicController != nullptr)
		return mBasicController->getSunDirection();
	return Ogre::Vector3(0.0,0.0,0.0);
}

Light *SkyXManager::getMainLight()
{
	return mLight1;
}

bool SkyXManager::update(float dt, float audio_dt)
{
	UpdateSkyLight();

	// Freeze time of day, clouds, wind and lightning while the simulation is paused (also affects SkyX's own frame listener)
	if (audio_dt >= 0.f)
		mSkyX->setPaused(audio_dt <= 0.f);
	mSkyX->update(dt);

	// Precipitation (ported from Caelum): drops are tinted by the current ambient light
	this->DetectPlayerMovement(dt);
	mSkyX->getPrecipitationController()->update(dt, mLight1->getDiffuseColour());

	mWeatherAudio->Update(audio_dt < 0.f ? dt : audio_dt);

	// 3D rain (dt = 0 freezes it while paused)
	const Ogre::Vector3 camPos = App::GetCameraManager()->GetCameraNode()->_getDerivedPosition();
	const auto water = App::GetGameContext()->GetTerrain()->getWater();
	const bool camUnderwater = (water != nullptr) && water->IsUnderWater(camPos);
	mRainParticles->Update(audio_dt < 0.f ? dt : audio_dt, camPos, mLight1->getDiffuseColour(), camUnderwater);
	this->UpdateUnderwaterView(camUnderwater);
	if (!mUwActive)
		this->UpdateAltitudeFog(camPos);
	mSkyX->getPrecipitationController()->setScreenEffectShelter(mRainParticles->GetShelter());
	return true;
}

void SkyXManager::UpdateAltitudeFog(const Ogre::Vector3& camPos)
{
	Ogre::SceneManager* sm = App::GetGfxScene()->GetSceneManager();
	const Ogre::FogMode mode = sm->getFogMode();
	if (mode == Ogre::FOG_NONE)
		return;
	// Something else changed the fog (map load, weather, underwater): take it over as the new base
	if (!mAltFogValid || mode != mAltFogMode || sm->getFogStart() != mAltFogSetStart
		|| sm->getFogEnd() != mAltFogSetEnd || sm->getFogDensity() != mAltFogSetDensity)
	{
		mAltFogValid = true;
		mAltFogMode = mode;
		mAltFogStart = sm->getFogStart();
		mAltFogEnd = sm->getFogEnd();
		mAltFogDensity = sm->getFogDensity();
	}
	const float ground = App::GetGameContext()->GetTerrain()->getHeightAt(camPos.x, camPos.z);
	const float h = std::max(0.f, camPos.y - ground);
	const float f = 1.f + std::min(std::max(h - 300.f, 0.f) / 400.f, 40.f); // x1 up to 300 m, about x32 at 12 km
	mAltFogSetStart = mAltFogStart * f;
	mAltFogSetEnd = mAltFogEnd * f;
	mAltFogSetDensity = mAltFogDensity / f;
	sm->setFog(mode, sm->getFogColour(), mAltFogSetDensity, mAltFogSetStart, mAltFogSetEnd);
}

void SkyXManager::UpdateUnderwaterView(bool camUnderwater)
{
	Ogre::Camera* cam = App::GetCameraManager() ? App::GetCameraManager()->GetCamera() : nullptr;
	Ogre::Viewport* vp = cam ? cam->getViewport() : nullptr;
	Ogre::SceneManager* sm = App::GetGfxScene()->GetSceneManager();
	if (vp == nullptr || sm == nullptr)
		return;

	if (camUnderwater)
	{
		if (!mUwActive)
		{
			mUwSavedBackground = vp->getBackgroundColour();
			mUwSavedFogMode = sm->getFogMode();
			mUwSavedFogColour = sm->getFogColour();
			mUwSavedFogDensity = sm->getFogDensity();
			mUwSavedFogStart = sm->getFogStart();
			mUwSavedFogEnd = sm->getFogEnd();
			mUwActive = true;
		}
		const float point = (-mLight1->getDerivedDirection().y + 1.0f) / 2.0f; // same as in UpdateSkyLight()
		const Ogre::Vector3 c = mHasWaterColorOverride ? mWaterColorOverride : mWaterGradient.getColor(point);
		const Ogre::ColourValue water(std::min(c.x, 1.f), std::min(c.y, 1.f), std::min(c.z, 1.f));
		vp->setBackgroundColour(water);
		const Ogre::Real end = (mUwSavedFogMode == Ogre::FOG_LINEAR && mUwSavedFogEnd > 0.f) ? std::min<Ogre::Real>(mUwSavedFogEnd, 250.f) : 250.f;
		// Keep the fog MODE of the map: RTSS bakes the fog mode into every generated shader,
		// so changing it invalidates and regenerates all materials (lag on every dive/surface).
		// Only colour/density/range change here - those are shader constants.
		switch (mUwSavedFogMode)
		{
		case Ogre::FOG_EXP:  sm->setFog(Ogre::FOG_EXP,  water, 4.f / end, 0.f, end); break; // exp(-4)   ~ 2% at 'end'
		case Ogre::FOG_EXP2: sm->setFog(Ogre::FOG_EXP2, water, 2.f / end, 0.f, end); break; // exp(-2^2) ~ 2% at 'end'
		default:             sm->setFog(Ogre::FOG_LINEAR, water, 0.f, 0.f, end); break;
		}
	}
	else if (mUwActive)
	{
		vp->setBackgroundColour(mUwSavedBackground);
		if (mUwSavedFogMode == Ogre::FOG_NONE)
		{
			// Map without fog: stay in FOG_LINEAR (set while underwater) with a range that is never reached,
			// so the RTSS shaders don't get regenerated on every surface/dive switch.
			sm->setFog(Ogre::FOG_LINEAR, mUwSavedFogColour, 0.f, 1.0e7f, 2.0e7f);
		}
		else
		{
			sm->setFog(mUwSavedFogMode, mUwSavedFogColour, mUwSavedFogDensity, mUwSavedFogStart, mUwSavedFogEnd);
		}
		mUwActive = false;
	}
}

void SkyXManager::DetectPlayerMovement(float dt)
{
	// We only want precipitation to react to player movement, not camera orbiting/zooming.
	SkyX::PrecipitationController* prec = mSkyX->getPrecipitationController();
	const ActorPtr currentPlayerActor = App::GetGameContext()->GetPlayerActor();
	const CameraManager::CameraBehaviors behavior = App::GetCameraManager()->GetCurrentBehavior();

	if (currentPlayerActor == mLastPlayerActor
		&& behavior == mLastCameraBehavior
		&& behavior != CameraManager::CAMERA_BEHAVIOR_FREE
		&& behavior != CameraManager::CAMERA_BEHAVIOR_FIXED)
	{
		Character* character = App::GetGameContext()->GetCharacterFactory()->GetLocalCharacter();
		if (currentPlayerActor || character != nullptr)
		{
			const Ogre::Vector3 playerPos = (currentPlayerActor)
				? currentPlayerActor->getPosition()
				: character->getPosition();

			prec->setManualCameraSpeed(playerPos - mLastPlayerPosition);
			mLastPlayerPosition = playerPos;
		}
	}
	else
	{
		prec->setManualCameraSpeed(Ogre::Vector3::ZERO);
		mLastPlayerPosition = Ogre::Vector3::ZERO;
	}

	mLastPlayerActor = currentPlayerActor;
	mLastCameraBehavior = behavior;
}


bool SkyXManager::UpdateSkyLight()
{
	Ogre::Vector3 lightDir = -getMainLightDirection();
	const Ogre::Vector3 camPos = App::GetCameraManager()->GetCameraNode()->_getDerivedPosition();
	Ogre::Vector3 sunPos = camPos - lightDir*mSkyX->getMeshManager()->getSkydomeRadius(App::GetCameraManager()->GetCamera());

	// Calculate current color gradients point
	float point = (-lightDir.y + 1.0f) / 2.0f;

    if (App::GetGameContext()->GetTerrain()->getHydraxManager ()) 
    {
        App::GetGameContext()->GetTerrain()->getHydraxManager ()->GetHydrax ()->setWaterColor (mWaterGradient.getColor (point));
    }
		

	mLight0->getParentSceneNode()->setPosition(sunPos*0.02);
	mLight1->getParentSceneNode()->setDirection(lightDir);
    if (App::GetGameContext()->GetTerrain()->getWater())
    {
        App::GetGameContext()->GetTerrain()->getGfxWater()->WaterSetSunPosition(sunPos*0.1);
    }

	//setFadeColour was removed with https://github.com/RigsOfRods/rigs-of-rods/pull/1459
/*	Ogre::Vector3 sunCol = mSunGradient.getColor(point);
	mLight0->setSpecularColour(sunCol.x, sunCol.y, sunCol.z);
	if (App::GetGameContext()->GetTerrain()->getWater()) App::GetGameContext()->GetTerrain()->getWater()->setFadeColour(Ogre::ColourValue(sunCol.x, sunCol.y, sunCol.z));
	*/
	// Keep a water color picked in the 'Water settings' window (the gradient above would overwrite it every frame)
	if (mHasWaterColorOverride && App::GetGameContext()->GetTerrain()->getHydraxManager())
	{
		App::GetGameContext()->GetTerrain()->getHydraxManager()->GetHydrax()->setWaterColor(mWaterColorOverride);
	}

	Ogre::Vector3 ambientCol = mAmbientGradient.getColor(point);

	// Night brightness slider: only the night part of the day is affected (point 0.5 = horizon, below = night)
	float nightWeight = 1.f - std::min(std::max((point - 0.42f) / 0.08f, 0.f), 1.f);
	nightWeight = nightWeight * nightWeight * (3.f - 2.f * nightWeight); // smoothstep
	const float nightScale = std::max(0.f, 1.f + (mNightBrightness - 1.f) * nightWeight);

	// Water surface (reflection/refraction/sun glint are not lit by the scene): dim it at night
	if (App::GetGameContext()->GetTerrain()->getHydraxManager())
	{
		const float waterNight = std::min(1.f, std::max(0.06f, 0.15f * mNightBrightness));
		App::GetGameContext()->GetTerrain()->getHydraxManager()->GetHydrax()->setBrightness(1.f + (waterNight - 1.f) * nightWeight);
	}
	ambientCol *= nightScale;
	mLight1->setDiffuseColour(ambientCol.x, ambientCol.y, ambientCol.z);

	Ogre::SceneManager* sm = App::GetGfxScene()->GetSceneManager();
	if (!mBaseAmbientValid)
	{
		mBaseAmbient = sm->getAmbientLight();
		mBaseAmbientValid = true;
	}
	if (nightScale != 1.f || mAmbientTouched)
	{
		Ogre::ColourValue amb = mBaseAmbient * nightScale;
		amb.a = 1.f;
		amb.saturate();
		sm->setAmbientLight(amb);
		mAmbientTouched = (nightScale != 1.f); // back to the exact map value once the scale is neutral again
	}

	// Fog of a converted Caelum map: colour follows the sky (underwater view manages the fog by itself)
	if (mFogFollowsSky && !mUwActive)
	{
		// Haze tint: slightly blueish, darkens with the ambient light during dusk/night.
		Ogre::ColourValue fog_col(ambientCol.x * 0.75f, ambientCol.y * 0.8f, ambientCol.z * 0.9f);
		sm->setFog(sm->getFogMode(), fog_col, sm->getFogDensity(), sm->getFogStart(), sm->getFogEnd());
	}

	if (mBasicController->getTime().x > 12)
	{
		if (mBasicController->getTime().x > mBasicController->getTime().z)
			mLight0->setVisible(false);
		else
			mLight0->setVisible(true);
	} 
    else
    {
        if (mBasicController->getTime ().x < mBasicController->getTime ().z)
            mLight0->setVisible (false);
        else
            mLight0->setVisible (true);
    }
	
    if (round (mBasicController->getTime ().x) != mLastHour)
    {
        TerrainGeometryManager* gm = App::GetGameContext()->GetTerrain()->getGeometryManager ();
        if (gm)
            gm->updateLightMap ();

        mLastHour = round (mBasicController->getTime ().x);
    }

	return true;
}

bool SkyXManager::InitLight()
{
	// Water
	mWaterGradient = SkyX::ColorGradient();
	mWaterGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.058209,0.535822,0.779105)*0.4, 1));
	mWaterGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.058209,0.535822,0.729105)*0.3, 0.8));
	mWaterGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.058209,0.535822,0.679105)*0.25, 0.6));
	mWaterGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.058209,0.535822,0.679105)*0.2, 0.5));
	mWaterGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.058209,0.535822,0.679105)*0.1, 0.45));
	mWaterGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.058209,0.535822,0.679105)*0.025, 0));
	// Sun
	mSunGradient = SkyX::ColorGradient();
	mSunGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.8,0.75,0.55)*1.5, 1.0f));
	mSunGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.8,0.75,0.55)*1.4, 0.75f));
	mSunGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.8,0.75,0.55)*1.3, 0.5625f));
	mSunGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.6,0.5,0.2)*1.5, 0.5f));
	mSunGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.5,0.5,0.5)*0.25, 0.45f));
	mSunGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(0.5,0.5,0.5)*0.01, 0.0f));
	// Ambient
	mAmbientGradient = SkyX::ColorGradient();
	mAmbientGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(1,1,1)*1, 1.0f));
	mAmbientGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(1,1,1)*1, 0.6f));
	mAmbientGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(1,1,1)*0.6, 0.5f));
	mAmbientGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(1,1,1)*0.3, 0.45f));
	mAmbientGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(1,1,1)*0.1, 0.35f));
	mAmbientGradient.addCFrame(SkyX::ColorGradient::ColorFrame(Ogre::Vector3(1,1,1)*0.05, 0.0f));

	App::GetGfxScene()->GetSceneManager()->setAmbientLight(ColourValue(0.35,0.35,0.35)); //Not needed because terrn2 has ambientlight settings

	// Light
	mLight0 = App::GetGfxScene()->GetSceneManager()->createLight("Light0");
	mLight0->setDiffuseColour(1, 1, 1);
	mLight0->setCastShadows(false);

	mLight1 = App::GetGfxScene()->GetSceneManager()->createLight("Light1");
	mLight1->setType(Ogre::Light::LT_DIRECTIONAL);

        Ogre::SceneNode* mLight0_snode = RoR::App::GetGfxScene()->GetSceneManager()->getRootSceneNode()->createChildSceneNode();
        mLight0_snode->attachObject(mLight0);

        Ogre::SceneNode* mLight1_snode = RoR::App::GetGfxScene()->GetSceneManager()->getRootSceneNode()->createChildSceneNode();
        mLight1_snode->attachObject(mLight1);
        mLight1_snode->setPosition(100,100,100);

	return true;
}

void SkyXManager::SaveMapDefaults()
{
	if (mSkyX == nullptr || mBasicController == nullptr)
		return;

	MapDefaults& d = mMapDefaults;
	d.atmosphere = mSkyX->getAtmosphereManager()->getOptions();
	d.starfield = mSkyX->isStarfieldEnabled();
	d.moonPhase = mBasicController->getMoonPhase();
	d.sunrise = mBasicController->getTime().y;
	d.sunset = mBasicController->getTime().z;

	SkyX::VCloudsManager* vcm = mSkyX->getVCloudsManager();
	d.vclouds = (vcm != nullptr) && vcm->isCreated() && (vcm->getVClouds() != nullptr);
	if (d.vclouds)
	{
		SkyX::VClouds::VClouds* vc = vcm->getVClouds();
		d.vcloudsWeather = vc->getWheater();
		d.vcloudsHeight = vcm->getHeight();
		d.vcloudsWindSpeed = vcm->getWindSpeed();
		d.vcloudsWindDir = vc->getWindDirection();
		d.vcloudsOpacity = vc->getGlobalOpacity();
		d.vcloudsFieldScale = vc->getCloudFieldScale();
		d.vcloudsNoiseScale = vc->getNoiseScale();
		d.vcloudsTint = vcm->getColorTint();
		d.vcloudsLightResponse = vc->getLightResponse();
		d.vcloudsAmbientFactors = vc->getAmbientFactors();
		SkyX::VClouds::LightningManager* lm = vc->getLightningManager();
		d.lightning = lm->isEnabled();
		d.lightningAvgTime = lm->getAverageLightningApparitionTime();
		d.lightningSpeed = lm->getLightningTimeMultiplier();
		d.lightningColor = lm->getLightningColor();
	}
	SkyX::PrecipitationController* prec = mSkyX->getPrecipitationController();
	d.astronomical = mBasicController->isAstronomical();
	d.latitude = mBasicController->getLatitude();
	d.dayOfYear = mBasicController->getDayOfYear();
	d.precipIntensity = prec->getIntensity();
	d.precipTexture = prec->getTextureName();
	d.precipColour = prec->getColour();
	d.precipSpeed = prec->getSpeed();
	d.valid = true;
}

void SkyXManager::ResetToMapDefaults()
{
	const MapDefaults& d = mMapDefaults;
	if (!d.valid)
		return;

	mSkyX->getAtmosphereManager()->setOptions(d.atmosphere);
	mSkyX->setStarfieldEnabled(d.starfield);

	SkyX::PrecipitationController* prec = mSkyX->getPrecipitationController();
	prec->setTextureName(d.precipTexture);
	prec->setColour(d.precipColour);
	prec->setSpeed(d.precipSpeed);
	prec->setIntensity(d.precipIntensity);
	mBasicController->setLatitude(d.latitude);
	mBasicController->setDayOfYear(d.dayOfYear);
	mBasicController->setAstronomical(d.astronomical);
	mNightBrightness = 1.f;
	mRainParticles->SetEnabled(true);
	mRainParticles->SetSplashesEnabled(true);
	mRainParticles->SetDensity(1.f);
	mSkyX->getPrecipitationController()->setScreenEffectScale(0.5f);
	mBasicController->setMoonPhase(d.moonPhase);
	Ogre::Vector3 t = mBasicController->getTime();
	t.y = d.sunrise;
	t.z = d.sunset;
	mBasicController->setTime(t);

	SkyX::VCloudsManager* vcm = mSkyX->getVCloudsManager();
	if (vcm == nullptr)
		return;

	// Switch the volumetric clouds on/off as the map had them (rebuild also restores altitude and thickness)
	const bool vc_exists = vcm->isCreated() && (vcm->getVClouds() != nullptr);
	if (!d.vclouds)
	{
		if (vc_exists)
			vcm->remove();
		return;
	}
	if (vc_exists && vcm->getHeight() != d.vcloudsHeight)
	{
		vcm->remove();
	}
	if (!(vcm->isCreated() && vcm->getVClouds() != nullptr))
	{
		vcm->setHeight(d.vcloudsHeight);
		vcm->create();
	}

	SkyX::VClouds::VClouds* vc = vcm->getVClouds();
	if (vc == nullptr)
		return;
	vc->setWheater(d.vcloudsWeather.x, d.vcloudsWeather.y, false);
	vcm->setWindSpeed(d.vcloudsWindSpeed);
	vc->setWindDirection(d.vcloudsWindDir);
	vc->setGlobalOpacity(d.vcloudsOpacity);
	vc->setCloudFieldScale(d.vcloudsFieldScale);
	vc->setNoiseScale(d.vcloudsNoiseScale);
	vcm->setColorTint(d.vcloudsTint);
	vc->setLightResponse(d.vcloudsLightResponse);
	vc->setAmbientFactors(d.vcloudsAmbientFactors);

	SkyX::VClouds::LightningManager* lm = vc->getLightningManager();
	lm->setAverageLightningApparitionTime(d.lightningAvgTime);
	lm->setLightningTimeMultiplier(d.lightningSpeed);
	lm->setLightningColor(d.lightningColor);
	lm->setEnabled(d.lightning);
}

float SkyXManager::GetTimeOfDay()
{
	return (mBasicController) ? mBasicController->getTime().x : 0.f;
}

void SkyXManager::SetTimeOfDay(float hour)
{
	if (!mBasicController)
		return;
	hour = std::fmod(hour, 24.f);
	if (hour < 0.f)
		hour += 24.f;
	Ogre::Vector3 t = mBasicController->getTime();
	t.x = hour;
	mBasicController->setTime(t);
}

std::string SkyXManager::GetPrettyTime()
{
	const float hour = this->GetTimeOfDay();
	const int h = static_cast<int>(hour);
	const int m = static_cast<int>((hour - h) * 60.f);
	char buf[20];
	snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
	return buf;
}
