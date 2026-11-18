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
#include "SkyXManager.h"
#include "Terrain.h"

#include "Perlin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

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
    mCellMaxCacheSize = 1u << 16;
    mCellMaxCache.reset(new std::atomic<uint64_t>[mCellMaxCacheSize]);
    for (size_t i = 0; i < mCellMaxCacheSize; ++i)
        mCellMaxCache[i].store(0, std::memory_order_relaxed);
    mHeightCacheSize = 1u << 17;
    mHeightCache.reset(new std::atomic<uint64_t>[mHeightCacheSize]);
    for (size_t i = 0; i < mHeightCacheSize; ++i)
        mHeightCache[i].store(0, std::memory_order_relaxed);
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

    this->SaveMapDefaults();
}

void HydraxWater::SaveMapDefaults()
{
    MapDefaults& d = mMapDefaults;
    d.waterLevel = waterHeight;
    d.components = mHydrax->getComponents();
    d.depthMask = mHydrax->getDepthVisibilityMask();
    d.globalTransparency = mHydrax->getGlobalTransparency();
    d.normalDistortion = mHydrax->getNormalDistortion();
    d.planesError = mHydrax->getPlanesError();
    d.waterColor = mHydrax->getWaterColor();
    d.foamScale = mHydrax->getFoamScale();
    d.foamStart = mHydrax->getFoamStart();
    d.foamTransparency = mHydrax->getFoamTransparency();
    d.foamMaxDistance = mHydrax->getFoamMaxDistance();
    d.depthLimit = mHydrax->getDepthLimit();
    d.distLimit = mHydrax->getDistLimit();
    d.smoothPower = mHydrax->getSmoothPower();
    d.causticsScale = mHydrax->getCausticsScale();
    d.causticsPower = mHydrax->getCausticsPower();
    d.causticsEnd = mHydrax->getCausticsEnd();
    d.sunStrength = mHydrax->getSunStrength();
    d.sunArea = mHydrax->getSunArea();
    d.sunColor = mHydrax->getSunColor();
    d.godRaysExposure = mHydrax->getGodRaysExposure();
    d.godRaysIntensity = mHydrax->getGodRaysIntensity();
    d.has_grid = (mModule != nullptr);
    if (d.has_grid)
    {
        d.grid = mModule->getOptions();
    }
    d.has_noise = (waternoise != nullptr);
    if (d.has_noise)
    {
        d.noise = waternoise->getOptions();
    }
    d.valid = true;
}

void HydraxWater::ApplyComponents(Hydrax::HydraxComponent components)
{
    if (mHydrax == nullptr || !mHydrax->isCreated())
        return;

    // Same path as at map load: remove everything, set the components while nothing exists, create again.
    mHydrax->remove();
    mHydrax->setComponents(components);
    mHydrax->create();
    mHydrax->setPosition(Ogre::Vector3(0, waterHeight, 0));

    char buf[160];
    snprintf(buf, sizeof(buf), "[RoR|Hydrax] Components applied: requested=0x%02X, active=0x%02X (sun=1 foam=2 depth=4 smooth=8 caustics=16 underwater=32 uw-refl=64 godrays=128)",
        (unsigned)components, (unsigned)mHydrax->getComponents());
    Ogre::LogManager::getSingleton().logMessage(buf);
}

void HydraxWater::ResetToMapDefaults()
{
    const MapDefaults& d = mMapDefaults;
    if (!d.valid || mHydrax == nullptr || !mHydrax->isCreated())
        return;

    // The sky overrides the water color every frame while the 'Water color' picker is in use
    SkyXManager* skyx = (App::GetGameContext()->GetTerrain()) ? App::GetGameContext()->GetTerrain()->getSkyXManager() : nullptr;
    if (skyx != nullptr)
    {
        skyx->ClearWaterColorOverride();
    }

    // Water level (FrameStepWater() moves the Hydrax plane when the level changes)
    if (App::GetGameContext()->GetTerrain() && App::GetGameContext()->GetTerrain()->getWater())
    {
        App::GetGameContext()->GetTerrain()->getWater()->SetStaticWaterHeight(d.waterLevel);
    }

    // Components first (rebuilds shaders), the component-specific values after
    if (mHydrax->getComponents() != d.components)
    {
        this->ApplyComponents(d.components);
    }
    mHydrax->setDepthVisibilityMask(d.depthMask);
    mHydrax->setGlobalTransparency(d.globalTransparency);
    mHydrax->setNormalDistortion(d.normalDistortion);
    mHydrax->setPlanesError(d.planesError);
    mHydrax->setWaterColor(d.waterColor);

    if (mHydrax->isComponent(Hydrax::HYDRAX_COMPONENT_FOAM))
    {
        mHydrax->setFoamScale(d.foamScale);
        mHydrax->setFoamStart(d.foamStart);
        mHydrax->setFoamTransparency(d.foamTransparency);
        mHydrax->setFoamMaxDistance(d.foamMaxDistance);
    }
    if (mHydrax->isComponent(Hydrax::HYDRAX_COMPONENT_DEPTH))
    {
        mHydrax->setDepthLimit(d.depthLimit);
        mHydrax->setDistLimit(d.distLimit);
    }
    if (mHydrax->isComponent(Hydrax::HYDRAX_COMPONENT_SMOOTH))
    {
        mHydrax->setSmoothPower(d.smoothPower);
    }
    if (mHydrax->isComponent(Hydrax::HYDRAX_COMPONENT_CAUSTICS))
    {
        mHydrax->setCausticsScale(d.causticsScale);
        mHydrax->setCausticsPower(d.causticsPower);
        mHydrax->setCausticsEnd(d.causticsEnd);
    }
    if (mHydrax->isComponent(Hydrax::HYDRAX_COMPONENT_SUN))
    {
        mHydrax->setSunStrength(d.sunStrength);
        mHydrax->setSunArea(d.sunArea);
        mHydrax->setSunColor(d.sunColor);
    }
    if (mHydrax->isComponent(Hydrax::HYDRAX_COMPONENT_UNDERWATER_GODRAYS))
    {
        mHydrax->setGodRaysExposure(d.godRaysExposure);
        mHydrax->setGodRaysIntensity(d.godRaysIntensity);
    }

    if (d.has_grid && mModule != nullptr)
    {
        mModule->setOptions(d.grid);
    }
    if (d.has_noise && waternoise != nullptr)
    {
        waternoise->setOptions(d.noise);
    }
}

void HydraxWater::UpdateWater()
{
    // Sun position/colour is updated by SkyXManager::UpdateSkyLight() (Caelum support was removed).
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

float HydraxWater::CachedTerrainHeight(Terrain* terrain, int ix, int iz)
{
    const float cell = 2.f;
    const int LIMIT = 16384; // 15 bit per coordinate
    if (ix < -LIMIT || ix >= LIMIT || iz < -LIMIT || iz >= LIMIT)
    {
        return terrain->getHeightAt(ix * cell, iz * cell);
    }
    const size_t mask = mHeightCacheSize - 1;
    const size_t idx = (static_cast<size_t>(static_cast<unsigned>(ix) * 73856093u) ^ static_cast<size_t>(static_cast<unsigned>(iz) * 19349663u)) & mask;
    const uint32_t key = (1u << 30) | (static_cast<uint32_t>(ix + LIMIT) << 15) | static_cast<uint32_t>(iz + LIMIT);
    const uint64_t cur = mHeightCache[idx].load(std::memory_order_relaxed);
    if (static_cast<uint32_t>(cur & 0xFFFFFFFFu) == key)
    {
        const uint32_t bits = static_cast<uint32_t>(cur >> 32);
        float h;
        std::memcpy(&h, &bits, sizeof(h));
        return h;
    }
    const float h = terrain->getHeightAt(ix * cell, iz * cell);
    uint32_t bits;
    std::memcpy(&bits, &h, sizeof(bits));
    mHeightCache[idx].store((static_cast<uint64_t>(bits) << 32) | key, std::memory_order_relaxed);
    return h;
}

float HydraxWater::CellMaxHeight(Terrain* terrain, int cx, int cz)
{
    const int LIMIT = 16384;
    const size_t mask = mCellMaxCacheSize - 1;
    const size_t idx = (static_cast<size_t>(static_cast<unsigned>(cx) * 73856093u) ^ static_cast<size_t>(static_cast<unsigned>(cz) * 19349663u)) & mask;
    const bool cacheable = (cx >= -LIMIT && cx < LIMIT && cz >= -LIMIT && cz < LIMIT);
    const uint32_t key = (1u << 30) | (static_cast<uint32_t>(cx + LIMIT) << 15) | static_cast<uint32_t>(cz + LIMIT);
    if (cacheable)
    {
        const uint64_t cur = mCellMaxCache[idx].load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(cur & 0xFFFFFFFFu) == key)
        {
            const uint32_t bits = static_cast<uint32_t>(cur >> 32);
            float h;
            std::memcpy(&h, &bits, sizeof(h));
            return h;
        }
    }
    // An 8 m cell covers 2 m grid points 4*cx .. 4*cx+4 (the bilinear lookup of SampleTerrainHeight never leaves their range)
    float max_h = -1.0e9f;
    for (int iz = 4 * cz; iz <= 4 * cz + 4; ++iz)
    {
        for (int ix = 4 * cx; ix <= 4 * cx + 4; ++ix)
        {
            max_h = std::max(max_h, this->CachedTerrainHeight(terrain, ix, iz));
        }
    }
    if (cacheable)
    {
        uint32_t bits;
        std::memcpy(&bits, &max_h, sizeof(bits));
        mCellMaxCache[idx].store((static_cast<uint64_t>(bits) << 32) | key, std::memory_order_relaxed);
    }
    return max_h;
}

float HydraxWater::SampleTerrainHeight(float x, float z)
{
    const TerrainPtr& terrain = App::GetGameContext()->GetTerrain();
    if (!terrain || mHeightCacheSize == 0)
    {
        return -1.0e9f; // no terrain: treat as deep water (full waves)
    }
    Terrain* const terrain_ptr = terrain.GetRef();
    // Open water: if the terrain is far below the surface, the waves are not flattened and any upper bound of its height gives
    // the same result as the exact height, so skip the bilinear lookup
    {
        const float cell_max = this->CellMaxHeight(terrain_ptr, static_cast<int>(std::floor(x / 8.f)), static_cast<int>(std::floor(z / 8.f)));
        if (cell_max <= waterHeight - mDeepMargin)
        {
            return cell_max;
        }
    }
    // Bilinear lookup in a 2 m grid of cached terrain heights (the wave grid asks for tens of thousands of points per frame)
    const float cell = 2.f;
    const float fx = x / cell, fz = z / cell;
    const int ix = static_cast<int>(std::floor(fx)), iz = static_cast<int>(std::floor(fz));
    const float tx = fx - ix, tz = fz - iz;
    Terrain* t = terrain.GetRef();
    const float h00 = CachedTerrainHeight(t, ix, iz),     h10 = CachedTerrainHeight(t, ix + 1, iz);
    const float h01 = CachedTerrainHeight(t, ix, iz + 1), h11 = CachedTerrainHeight(t, ix + 1, iz + 1);
    return (h00 * (1 - tx) + h10 * tx) * (1 - tz) + (h01 * (1 - tx) + h11 * tx) * tz;
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

void HydraxWater::FrameStepWater(float dt)
{
    const TerrainPtr& terrain = App::GetGameContext()->GetTerrain();
    if (terrain && terrain->getWater() != nullptr)
    {
        const float curWaterHeight = terrain->getWater()->GetStaticWaterHeight();
        if (waterHeight != curWaterHeight)
        {
            waterHeight = curWaterHeight;
            if (mHydrax)
            {
                mHydrax->setPosition(Ogre::Vector3(0, waterHeight, 0));
            }
        }
    }
    if (mHydrax)
    {
        if (mModule != nullptr)
        {
            // Same fade distance as ProjectedGrid::_getWaveHeight(), plus a little reserve
            mDeepMargin = std::max(std::abs(mModule->getOptions().Strength) * 0.5f, 0.5f) + 0.5f;
        }
        mHydrax->update(dt);
    }
    this->UpdateWater();
}

