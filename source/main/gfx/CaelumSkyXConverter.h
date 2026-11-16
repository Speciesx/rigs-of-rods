/*
    This source file is part of Rigs of Rods
    Copyright 2026 Rigs of Rods contributors

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

/// @file
/// @brief Caelum support was removed; terrains which ship a Caelum sky script (*.os, terrn2 `CaelumConfigFile`)
///        are rendered with SkyX instead. This module reads the Caelum script and maps it onto SkyX parameters.

#pragma once

#include "ForwardDeclarations.h"

#include <OgreColourValue.h>
#include <OgreVector.h>

#include <string>

namespace RoR {

/// @addtogroup Gfx
/// @{

/// The subset of a Caelum `caelum_sky_system` definition which can be expressed with SkyX.
struct CaelumSkyDef
{
    std::string name;                        //!< Name of the `caelum_sky_system` which was used

    bool   has_julian_day = false;
    double julian_day = 2451545.0;          //!< Caelum: UTC date+time as julian day
    bool   has_time_scale = false;
    float  time_scale = 1.f;                //!< Caelum: simulated seconds per real second

    bool   has_latitude = false;
    float  latitude_deg = 45.f;             //!< Observer latitude, north positive
    bool   has_longitude = false;
    float  longitude_deg = 0.f;             //!< Observer longitude, east positive

    bool          has_clouds = false;       //!< Script defines a `cloud_system`
    float         cloud_coverage = 0.f;     //!< Max coverage of all cloud layers [0..1]
    bool          has_cloud_speed = false;
    Ogre::Vector2 cloud_speed = Ogre::Vector2::ZERO; //!< Speed of the most covering cloud layer (UV units/s)

    bool              has_precipitation = false;       //!< Script defines a `precipitation` component
    float             precipitation_intensity = 0.f;   //!< [0..1]
    std::string       precipitation_texture;           //!< e.g. 'precipitation_rain.png' (same files in SkyX resources)
    bool              has_precipitation_colour = false;
    Ogre::ColourValue precipitation_colour = Ogre::ColourValue::White;
    bool              has_precipitation_speed = false;
    float             precipitation_speed = 0.f;       //!< Caelum `falling_speed`
    bool              has_precipitation_camera_speed_scale = false;
    Ogre::Vector3     precipitation_camera_speed_scale = Ogre::Vector3::UNIT_SCALE;
    bool              has_precipitation_auto_disable = false;
    float             precipitation_auto_disable = 0.f; //!< Caelum `auto_disable_threshold`/`auto_disable_intensity`

    bool  manage_scene_fog = false;
    float scene_fog_density_multiplier = 0.f;
};

/// Reads a Caelum sky script.
/// @param filename The Caelum script file (terrn2 `CaelumConfigFile`), looked up in all resource groups.
///        The `caelum_sky_system` with the same name (with or without extension) is used, otherwise the first non-abstract one.
/// @return False if the file doesn't exist or contains no usable sky system.
bool ParseCaelumSkyScript(std::string const& filename, CaelumSkyDef& out_def);

/// Applies a parsed Caelum sky definition onto an already created SkyX sky (which was set up from 'SkyXDefault.skx').
/// Uses the astronomical sun/moon and the precipitation of SkyX (both ported from Caelum) and stores the result as map defaults.
void ApplyCaelumSkyDefToSkyX(CaelumSkyDef const& def, SkyXManager* skyx_mgr);

/// Converts a Caelum julian day (UTC) to the day of year [1-365] and local solar hour [0-24) at the given longitude (east positive).
void CaelumJulianDayToDayOfYearAndHour(double julian_day, float longitude_deg, int& out_day_of_year, float& out_hour);

/// @} // addtogroup Gfx

} // namespace RoR
