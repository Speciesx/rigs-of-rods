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

#pragma once

#include "Hydrax.h"
#include "IGfxWater.h"
#include "Perlin.h"
#include "ProjectedGrid.h"

#include <Ogre.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace RoR {

/// @addtogroup Gfx
/// @{

class HydraxWater : public IGfxWater
{
public:

    HydraxWater(float waterHeight, Ogre::String configFile = "HydraxDefault.hdx");
    ~HydraxWater();

    void SetWaterVisible(bool value) override;
    void WaterSetSunPosition(Ogre::Vector3) override;
    void FrameStepWater(float dt) override;
    void UpdateWater() override;
    bool GetWaveHeightAt(float x, float z, float& out_height) override;

    Hydrax::Hydrax* GetHydrax() { return mHydrax; }

    /// Restores everything the 'Water settings' window can change to the values the map started with.
    void ResetToMapDefaults();

    /// Changes the Hydrax components. Rebuilds the whole water (the live switch of Hydrax::setComponents() crashes).
    void ApplyComponents(Hydrax::HydraxComponent components);

protected:

    void InitHydrax();

    /// Terrain height with a small cache; used by Hydrax to flatten the waves at the shore.
    float SampleTerrainHeight(float x, float z);
    float CachedTerrainHeight(Terrain* terrain, int ix, int iz);
    /// Lock-free cache (one 64 bit word per entry: key + height), safe to use from the physics threads and the wave worker threads
    std::unique_ptr<std::atomic<uint64_t>[]> mHeightCache;
    size_t mHeightCacheSize = 0;

    /// Upper bound of the terrain height in 8 m cells (lazily filled, lock-free): where it is far below the water, the exact height is not needed
    float CellMaxHeight(Terrain* terrain, int cx, int cz);
    std::unique_ptr<std::atomic<uint64_t>[]> mCellMaxCache;
    size_t mCellMaxCacheSize = 0;
    float mDeepMargin = 6.f; //!< Terrain this far below the water does not influence the waves (the fade distance of the wave height)
    void SaveMapDefaults();

    /// Water state right after the map was loaded
    struct MapDefaults
    {
        bool valid = false;
        float waterLevel = 0.f;
        Hydrax::HydraxComponent components = Hydrax::HYDRAX_COMPONENTS_NONE;
        Ogre::uint32 depthMask = 0xFFFFFFFF;
        float globalTransparency = 0.f;
        float normalDistortion = 0.075f;
        float planesError = 10.5f;
        Ogre::Vector3 waterColor = Ogre::Vector3::ZERO;
        float foamScale = 0.1f, foamStart = 0.f, foamTransparency = 1.f, foamMaxDistance = 7.5e6f;
        float depthLimit = 70.f, distLimit = 90.f, smoothPower = 5.f;
        float causticsScale = 8.5f, causticsPower = 5.5f, causticsEnd = 0.7f;
        float sunStrength = 1.75f, sunArea = 150.f;
        Ogre::Vector3 sunColor = Ogre::Vector3(1.f, 0.9f, 0.6f);
        Ogre::Vector3 godRaysExposure = Ogre::Vector3(0.76f, 2.46f, 2.29f);
        float godRaysIntensity = 0.015f;
        Hydrax::Module::ProjectedGrid::Options grid;
        Hydrax::Noise::Perlin::Options noise;
        bool has_grid = false;
        bool has_noise = false;
    } mMapDefaults;
    Hydrax::Hydrax* mHydrax;
    float waveHeight;
    float waterHeight;
    Hydrax::Noise::Perlin* waternoise;
    Hydrax::Module::ProjectedGrid* mModule;
    Ogre::String CurrentConfigFile;
};

/// @} // addtogroup Gfx

} // namespace RoR
