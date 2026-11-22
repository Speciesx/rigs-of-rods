/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2016 Petr Ohlidal

    For more information, see http://www.rigsofrods.org/
    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.

    Rigs of Rods is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Rigs of Rods. If not, see <http://www.gnu.org/licenses/>.
*/

#include "HydraxWater.h"

#include "Actor.h"
#include "AppContext.h"
#include "CameraManager.h"
#include "GameContext.h"
#include "GfxScene.h"
#include "SkyManager.h"
#include "Terrain.h"

#ifdef USE_CAELUM
#include <Caelum.h>
#endif // USE_CAELUM

#include <cmath>

using namespace Ogre;
using namespace RoR;

// HydraxWater
HydraxWater::HydraxWater(float water_height, Ogre::String conf_file):
    waternoise(0)
    , mHydrax(0)
    , waterHeight(water_height)
    , waveHeight(water_height)
    , CurrentConfigFile(conf_file)
{
    App::GetCameraManager()->GetCamera()->setNearClipDistance(0.1f);

    InitHydrax();
}

HydraxWater::~HydraxWater()
{
    // ~Hydrax() calls remove() and also deletes the module (ProjectedGrid) and its noise (Perlin)
    delete mHydrax;
    mHydrax = nullptr;
    mModule = nullptr;
    waternoise = nullptr;
}

void HydraxWater::InitHydrax()
{
    mHydrax = new Hydrax::Hydrax(App::GetGfxScene()->GetSceneManager(), App::GetCameraManager()->GetCamera(), RoR::App::GetAppContext()->GetViewport());

    waternoise = new Hydrax::Noise::Perlin();
    mModule = new Hydrax::Module::ProjectedGrid(// Hydrax parent pointer
        mHydrax,
        // Noise module
        waternoise,
        // Base plane
        Ogre::Plane(Ogre::Vector3::UNIT_Y, 0),
        // Normal mode
        Hydrax::MaterialManager::NM_VERTEX,
        // Projected grid options
        Hydrax::Module::ProjectedGrid::Options());

    mHydrax->setModule(static_cast<Hydrax::Module::Module*>(mModule));

    mHydrax->loadCfg(CurrentConfigFile);

    // Waves must not rise over the beach: Hydrax flattens them where the water is shallow
    mHydrax->setTerrainHeightFunction([this](Ogre::Real x, Ogre::Real z) { return this->SampleTerrainHeight(x, z); });

    // Large 'PlanesError' values (maps use up to 10.5) put land above the waterline into the refraction map,
    // which shows as stretched streaks on slopes at the shore. Keep it small.
    const float MAX_PLANES_ERROR = 3.5f;
    if (mHydrax->getPlanesError() > MAX_PLANES_ERROR)
    {
        mHydrax->setPlanesError(MAX_PLANES_ERROR);
    }

    // Choose shader language based on renderer (HLSL=0, CG=1, GLSL=2)
    if (Root::getSingleton().getRenderSystem()->getName() == "Direct3D9 Rendering Subsystem" || Root::getSingleton().getRenderSystem()->getName() == "Direct3D11 Rendering Subsystem")
    {
        mHydrax->setShaderMode(static_cast<Hydrax::MaterialManager::ShaderMode>(0));
    }
    else
    {
        mHydrax->setShaderMode(static_cast<Hydrax::MaterialManager::ShaderMode>(2));
    }

    // Objects with other visibility flags (paged vegetation, particles, flares) are not rendered into the depth maps
    mHydrax->setDepthVisibilityMask(DEPTHMAP_ENABLED | HIDE_MIRROR);

    mHydrax->create();
    mHydrax->setPosition(Ogre::Vector3(0, waterHeight, 0));
}

void HydraxWater::UpdateWater()
{
#ifdef USE_CAELUM
    if (RoR::App::GetGameContext()->GetTerrain()->getSkyManager() != nullptr)
    {
        SkyManager* sky = RoR::App::GetGameContext()->GetTerrain()->getSkyManager();
        Ogre::Vector3 sunPosition = App::GetCameraManager()->GetCameraNode()->_getDerivedPosition();
        sunPosition -= sky->GetCaelumSys()->getSun()->getLightDirection() * 80000;
        mHydrax->setSunPosition(sunPosition);
        mHydrax->setSunColor(Ogre::Vector3(sky->GetCaelumSys()->getSun()->getBodyColour().r, sky->GetCaelumSys()->getSun()->getBodyColour().g, sky->GetCaelumSys()->getSun()->getBodyColour().b));
    }
#endif // USE_CAELUM
}

void HydraxWater::SetWaterVisible(bool value)
{
    if (mHydrax)
        mHydrax->setVisible(value);
}

void HydraxWater::WaterSetSunPosition(Ogre::Vector3 pos)
{
    if (mHydrax)
        mHydrax->setSunPosition(pos);
}

float HydraxWater::SampleTerrainHeight(float x, float z)
{
    const TerrainPtr& terrain = App::GetGameContext()->GetTerrain();
    if (!terrain)
    {
        return -1.0e9f; // no terrain: treat as deep water (full waves)
    }
    return terrain->getHeightAt(x, z);
}

bool HydraxWater::GetWaveHeightAt(float x, float z, float& out_height)
{
    if (mHydrax == nullptr || !mHydrax->isCreated())
    {
        return false;
    }
    out_height = mHydrax->getHeigth(Ogre::Vector2(x, z));
    return true;
}

float HydraxWater::GetMaxWaveAmplitude()
{
    if (mHydrax == nullptr || !mHydrax->isCreated() || mModule == nullptr)
    {
        return -1.f;
    }
    // ProjectedGrid::getHeigth() = Perlin noise * Strength.
    // The Perlin noise stays within +-2: base noise is 4*(rand - 0.5), each octave blends three frames with
    // weights summing to 1, and the octave weights are normalized to 1. Small reserve for rounding.
    return std::abs(mModule->getOptions().Strength) * 2.f + 0.1f;
}

void HydraxWater::FrameStepWater(float dt)
{
    const TerrainPtr& terrain = App::GetGameContext()->GetTerrain();
    if (terrain && terrain->getWater() != nullptr)
    {
        const float curWaterHeight = terrain->getWater()->GetStaticWaterHeight();
        if (waterHeight != curWaterHeight)
        {
            waterHeight = curWaterHeight;
            // Move the water plane too - before, a new water level (script, terrain editor) only changed the physics
            if (mHydrax)
            {
                mHydrax->setPosition(Ogre::Vector3(0, waterHeight, 0));
            }
        }
    }
    if (mHydrax)
    {
        mHydrax->update(dt);
    }
    this->UpdateWater();
}

