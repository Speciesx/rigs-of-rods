/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2019 Petr Ohlidal

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
/// @brief Round tyre pressure gauge (ImGui replacement of the old OGRE overlay)

#pragma once

#include "ForwardDeclarations.h"

#include <imgui.h>

namespace RoR {
namespace GUI {

class TyrePressure
{
public:
    void Draw(RoR::GfxActor* player); //!< Visible only while pressure is being adjusted; draggable.

private:
    ImVec2 m_pos = ImVec2(-1.f, -1.f); //!< Window position (session only); negative = not placed yet.
};

} // namespace GUI
} // namespace RoR
