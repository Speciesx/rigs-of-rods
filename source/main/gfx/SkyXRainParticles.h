/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2024 Petr Ohlidal

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

#pragma once

#include "ForwardDeclarations.h"

#include "gfx/skyx/SkyX.h"

#include <OgreColourValue.h>
#include <OgreVector3.h>

#include <OgreAxisAlignedBox.h>

#include <vector>

namespace Ogre { class Billboard; class BillboardSet; class SceneNode; class SceneManager; }

namespace RoR {

/// @addtogroup Gfx
/// @{

/// 3D rain: streaks falling in a box around the camera, with impact splashes.
/// - Each drop falls until it hits the highest surface below it (terrain, water, buildings and other collision meshes/boxes,
///   see Collisions::getSurfaceHeightBelow()), so there is no rain under roofs, bridges or in tunnels.
/// - Vehicles (their bounding boxes) shield the rain as well, e.g. nothing falls into a cab.
/// - The streaks are aligned with the drops' motion relative to the camera, so the rain slants backwards when you drive.
/// - Drops that hit a surface near the camera create a short ripple/splash.
/// - Amount and type follow the intensity and texture of SkyX::PrecipitationController, slant follows the VClouds wind,
///   colour follows the current light (dark at night).
/// It complements the flat screen-space precipitation of the controller. Textures and materials are created in code,
/// so no extra resource files are needed.
class SkyXRainParticles
{
public:
    SkyXRainParticles(SkyX::SkyX* skyx, Ogre::SceneManager* sm);
    ~SkyXRainParticles();

    /// @param dt Seconds since last frame; 0 while the simulation is paused (freezes the drops).
    /// @param light Current light colour, tints the drops (dark at night).
    void Update(float dt, const Ogre::Vector3& cam_pos, const Ogre::ColourValue& light, bool camera_underwater);

    /// 0..1: how sheltered the camera is from the rain (inside a vehicle, under a roof or underwater)
    float GetShelter() const { return m_shelter; }

    /// False if the rain could not be created (see RoR.log)
    bool IsAvailable() const { return m_rain_set != nullptr; }

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    void SetSplashesEnabled(bool enabled) { m_splashes_enabled = enabled; }
    bool AreSplashesEnabled() const { return m_splashes_enabled; }

    /// 1 = default amount of drops
    void SetDensity(float density) { m_density = density; }
    float GetDensity() const { return m_density; }

private:
    struct Drop
    {
        Ogre::Vector3 pos = Ogre::Vector3::ZERO; //!< World position of the drop's centre
        float kill_y = 0.f;                      //!< Height of the surface the drop will hit
        float speed_mul = 1.f;
        bool alive = false;
        Ogre::Billboard* bb = nullptr;
    };

    struct Splash
    {
        Ogre::Vector3 pos = Ogre::Vector3::ZERO;
        float age = 0.f;
        float life = 0.f;
        float max_size = 0.f;
        bool active = false;
        Ogre::Billboard* bb = nullptr;
    };

    void SetActive(bool active);
    bool SpawnDrop(Drop& d, bool scatter);
    void AddSplash(const Drop& d);
    float SurfaceBelow(float x, float z, float top) const;
    bool InsideOccluder(float x, float y, float z) const;
    /// Returns the roof height if the drop's lower end entered a vehicle, otherwise a very low value
    float VehicleRoofHit(const Drop& d, float half_len) const;

    SkyX::SkyX* m_skyx;
    Ogre::SceneManager* m_scene_mgr;
    Ogre::SceneNode* m_node = nullptr;
    Ogre::BillboardSet* m_rain_set = nullptr;
    Ogre::BillboardSet* m_splash_set = nullptr;

    bool m_enabled = true;
    bool m_splashes_enabled = true;
    bool m_active = false;
    float m_density = 1.f;

    std::vector<Drop> m_drops;
    std::vector<Splash> m_splashes;
    std::vector<Ogre::AxisAlignedBox> m_occluders; //!< Bounding boxes of the vehicles near the camera
    size_t m_next_splash = 0;
    int m_splash_budget = 0;

    // per-frame state
    Ogre::Vector3 m_cam_pos = Ogre::Vector3::ZERO;
    Ogre::Vector3 m_last_cam_pos = Ogre::Vector3::ZERO;
    Ogre::Vector3 m_cam_vel = Ogre::Vector3::ZERO;
    Ogre::Vector3 m_box_center = Ogre::Vector3::ZERO;
    Ogre::Vector3 m_velocity = Ogre::Vector3(0.f, -22.f, 0.f);
    Ogre::Vector3 m_dir = Ogre::Vector3(0.f, -1.f, 0.f);      //!< Direction of the drops' motion in the world
    Ogre::Vector3 m_view_dir = Ogre::Vector3(0.f, -1.f, 0.f); //!< Direction relative to the camera (orientation of the streaks)
    float m_streak_scale = 1.f;
    float m_shelter = 0.f;
    Ogre::Vector3 m_drift = Ogre::Vector3::ZERO;
    float m_water_height = 0.f;
    bool m_has_water = false;
    Ogre::ColourValue m_drop_colour = Ogre::ColourValue::Black;
    Ogre::ColourValue m_splash_colour = Ogre::ColourValue::Black;
};

/// @}

} // namespace RoR
