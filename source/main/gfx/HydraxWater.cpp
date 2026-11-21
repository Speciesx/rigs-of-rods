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
#include "Application.h"
#include "CameraManager.h"
#include "GameContext.h"
#include "GfxActor.h"
#include "GfxScene.h"
#include "SkyXManager.h"
#include "Terrain.h"
#include "ThreadPool.h"

#include "Perlin.h"

#include <algorithm>
#include <chrono>
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

    // Bow and stern waves: drawn on top of the Hydrax waves, the physics does not see them
    mWake.SetTerrainFunction([this](float x, float z) { return this->SampleTerrainHeight(x, z); });
    mWake.SetParallel([](std::vector<std::function<void()>>& tasks) { App::GetThreadPool()->Parallelize(tasks); },
        (App::GetThreadPool() != nullptr) ? std::min(std::max(App::app_num_workers->getInt(), 1), 4) : 1);
    mModule->setVisualHeightFunction([this](Ogre::Real x, Ogre::Real z) { return mWake.GetHeight(x, z); });

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

float HydraxWater::GetMaxWaveAmplitude()
{
    if (mHydrax == nullptr || !mHydrax->isCreated() || mModule == nullptr)
    {
        return -1.f;
    }
    // ProjectedGrid::_getWaveHeight() = Perlin noise * Strength (only ever flattened towards the shore).
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
        this->UpdateWakes(dt);
        mHydrax->update(dt);
    }
    this->UpdateWater();
}

void HydraxWater::UpdateWakes(float dt)
{
    if (mModule == nullptr || !mHydrax->isCreated())
        return;
    const auto t0 = std::chrono::steady_clock::now();

    const Ogre::Vector3 cam = App::GetCameraManager()->GetCamera()->getDerivedPosition();
    mWake.BeginFrame(cam.x, cam.z, waterHeight);

    // Nodes below this level are under water for sure, nodes above it are dry for sure
    const float amp = std::abs(mModule->getOptions().Strength) * 2.f;
    const float band_top = waterHeight + amp + 0.3f;
    const float band_bottom = waterHeight - amp - 0.3f;
    const float reach = HydraxWake::N * HydraxWake::DX * 0.5f + 200.f;

    for (GfxActor* gfx_actor: App::GetGfxScene()->GetGfxActors())
    {
        const ActorSB& sb = gfx_actor->GetSimDataBuffer();
        const Ogre::AxisAlignedBox& box = sb.simbuf_aabb;
        if (sb.simbuf_nodes.empty() || box.isNull() || box.isInfinite())
            continue;
        if (box.getMinimum().y > band_top)
            continue; // Not in the water
        if (box.getMaximum().x < cam.x - reach || box.getMinimum().x > cam.x + reach ||
            box.getMaximum().z < cam.z - reach || box.getMinimum().z > cam.z + reach)
            continue; // Outside the wake area

        // Slow hulls make small waves (and the waves would pile up at the wave speed of ~3 m/s)
        const float hspeed = Ogre::Vector2(sb.simbuf_node0_velo.x, sb.simbuf_node0_velo.z).length();
        const float speed_factor = Ogre::Math::Clamp(hspeed / 8.f, 0.f, 1.f);

        // The footprint of the hull is the outline of all submerged nodes; the draft makes big ships push more water
        mWake.BeginActor(gfx_actor->GetActorId(), sb.simbuf_pos.x, sb.simbuf_pos.z);
        float draft = 0.f;
        for (const NodeSB& node: sb.simbuf_nodes)
        {
            const Ogre::Vector3& p = node.AbsPosition;
            if (p.y > band_top)
                continue;
            float depth = waterHeight - p.y;
            if (p.y > band_bottom)
            {
                depth = mHydrax->getHeigth(Ogre::Vector2(p.x, p.z)) - p.y; // near the surface: the actual (wavy) surface
                if (depth < 0.05f)
                    continue;
            }
            draft = std::max(draft, depth);
            mWake.AddNode(p.x, p.z);
        }
        const float size_factor = Ogre::Math::Clamp(std::sqrt(draft / 1.5f), 0.7f, 2.5f);
        mWake.EndActor(speed_factor * size_factor);
    }
    mWake.EndFrame(dt);
    mModule->setVisualHeightActive(mWake.IsActive());

    // Perf log, every 10 s while there are wakes
    mWakeTimeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mWakeFrames++;
    mWakeActiveFrames += mWake.IsActive() ? 1 : 0;
    mWakeMaxNodes = std::max(mWakeMaxNodes, mWake.GetNumNodes());
    mWakeLogTimer += dt;
    if (mWakeLogTimer >= 10.f)
    {
        if (mWakeActiveFrames > 0)
        {
            char buf[200];
            snprintf(buf, sizeof(buf), "[RoR|Hydrax] Boat wakes: %.3f ms/frame avg over %d frames (active in %d), up to %d submerged nodes",
                mWakeTimeMs / std::max(mWakeFrames, 1), mWakeFrames, mWakeActiveFrames, mWakeMaxNodes);
            Ogre::LogManager::getSingleton().logMessage(buf);
        }
        mWakeTimeMs = 0.0;
        mWakeFrames = mWakeActiveFrames = mWakeMaxNodes = 0;
        mWakeLogTimer = 0.f;
    }
}

