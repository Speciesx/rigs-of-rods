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

#include "Application.h"
#include "ForwardDeclarations.h"

#include "gfx/skyx/SkyX.h"
#include "gfx/skyx/VClouds/LightningManager.h"

#include <OgreVector3.h>
#include <vector>

namespace RoR {

/// @addtogroup Gfx
/// @{

/// Ambient weather sound for SkyX: rain loop (from precipitation), wind loop (from VClouds wind) and thunder (from lightning).
/// Sound files: resources/sounds/skyx_rain.wav, skyx_wind.wav, skyx_thunder1..3.wav (replace them with your own samples if you like).
/// All functions do nothing if the game was built/started without audio.
class SkyXWeatherAudio : public SkyX::VClouds::LightningManager::Listener
{
public:
    SkyXWeatherAudio(SkyX::SkyX* skyx);
    ~SkyXWeatherAudio();

    /// @param dt Seconds since last frame; pass 0 while the simulation is paused.
    void Update(float dt);

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    // LightningManager::Listener
    virtual void lightningAdded(SkyX::VClouds::Lightning* l) override;

private:
    struct PendingThunder
    {
        float delay;   //!< Seconds until the sound arrives
        float gain;
        int variant;   //!< 0..2
        Ogre::Vector3 direction; //!< From listener towards the bolt
    };

    void RegisterListener();
    void UnregisterListener();
    void StopAll();

    SkyX::SkyX* m_skyx;
    bool m_enabled = true;
    float m_rain_gain = 0.f;
    float m_wind_gain = 0.f;
    std::vector<PendingThunder> m_pending;
};

/// @}

} // namespace RoR
