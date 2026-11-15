/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2020 Petr Ohlidal

    For more information, see http://www.rigsofrods.org/

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

/// @file
/// @brief Live settings for the active sky (SkyX or Caelum), Tools menu.

#pragma once

#include "Application.h"

#include <OgreVector2.h>

namespace RoR {
namespace GUI {

/// Controls that exist for several backends share one widget, controls that the active backend lacks are greyed out.
class SkyControlsPanel
{
public:
    void SetVisible(bool visible)
    {
        m_is_visible = visible;
        if (visible)
        {
            // Re-read the edit fields of the 'Apply' buttons from the live objects
            m_cloud_height_valid = false;
        }
    }
    bool IsVisible() const { return m_is_visible; }
    bool IsHovered() const { return IsVisible() && m_is_hovered; }

    void Draw();

private:
    bool m_is_visible = false;
    bool m_is_hovered = false;

    Ogre::Vector2 m_cloud_height = Ogre::Vector2(825.f, 3500.f); //!< x = altitude, y = thickness (applied by button)
    bool m_cloud_height_valid = false;
};

} // namespace GUI
} // namespace RoR
