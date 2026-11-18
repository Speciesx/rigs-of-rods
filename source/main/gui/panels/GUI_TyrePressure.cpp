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

#include "GUI_TyrePressure.h"

#include "Application.h"
#include "GfxActor.h"
#include "GUIManager.h"
#include "SimBuffers.h"

#include <imgui.h>
#include <cmath>
#include <cstdio>

using namespace RoR;
using namespace GUI;

static const float PRESSURE_MAX = 100.f;
static const float GAUGE_START_DEG = 135.f; // pressure 0 = bottom-left, 50 = top, 100 = bottom-right
static const float GAUGE_SWEEP_DEG = 270.f;
static const float GAUGE_BASE_SIZE_PX = 128.f; // Native size of the old overlay texture (pressure.dds); multiplied by ui_scale_factor

static float PressureToRad(float p)
{
    return (GAUGE_START_DEG + (p / PRESSURE_MAX) * GAUGE_SWEEP_DEG) * (IM_PI / 180.f);
}

void TyrePressure::Draw(RoR::GfxActor* player)
{
    if (player == nullptr || !player->GetSimDataBuffer().simbuf_tyre_pressurizing)
        return;

    const float pressure = ImClamp(player->GetSimDataBuffer().simbuf_tyre_pressure, 0.f, PRESSURE_MAX);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float d = GAUGE_BASE_SIZE_PX * App::GetGuiManager()->GetUiScaleFactor();

    if (m_pos.x < 0.f) // Default: where the old overlay was (top right, 0.28 screen widths from the right edge)
        m_pos = ImVec2(screen.x - screen.x * 0.28f - d, 0.f);
    m_pos.x = ImClamp(m_pos.x, 0.f, ImMax(0.f, screen.x - d));
    m_pos.y = ImClamp(m_pos.y, 0.f, ImMax(0.f, screen.y - d));

    ImGui::SetNextWindowPos(m_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(d, d));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::Begin("TyrePressure", nullptr, flags);

    // Dragging: ImGui only moves a window by its background if we place an item over it
    ImGui::InvisibleButton("##drag", ImVec2(d, d));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
    {
        m_pos.x += ImGui::GetIO().MouseDelta.x;
        m_pos.y += ImGui::GetIO().MouseDelta.y;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(m_pos.x + d * 0.5f, m_pos.y + d * 0.5f);
    const float r = d * 0.5f;

    dl->AddCircleFilled(c, r, IM_COL32(21, 24, 28, 220), 64);
    dl->AddCircle(c, r - 1.f, IM_COL32(138, 144, 153, 255), 64, ImMax(1.f, d * 0.01f));

    // Low-pressure zone 0..20
    dl->PathArcTo(c, r * 0.95f, PressureToRad(0.f), PressureToRad(20.f), 24);
    dl->PathStroke(IM_COL32(239, 159, 39, 255), false, ImMax(2.f, d * 0.015f));

    // Ticks + labels
    ImFont* font = ImGui::GetFont();
    const float label_size = d * 0.09f;
    for (int i = 0; i <= 10; ++i)
    {
        const float a = PressureToRad(i * 10.f);
        const ImVec2 dir(cosf(a), sinf(a));
        dl->AddLine(ImVec2(c.x + dir.x * r * 0.76f, c.y + dir.y * r * 0.76f),
                    ImVec2(c.x + dir.x * r * 0.89f, c.y + dir.y * r * 0.89f),
                    IM_COL32(201, 205, 210, 255), ImMax(1.f, d * 0.006f));
        if (i % 2 == 0)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", i * 10);
            const ImVec2 ts = font->CalcTextSizeA(label_size, FLT_MAX, 0.f, buf);
            dl->AddText(font, label_size, ImVec2(c.x + dir.x * r * 0.60f - ts.x * 0.5f, c.y + dir.y * r * 0.60f - ts.y * 0.5f),
                        IM_COL32(201, 205, 210, 255), buf);
        }
    }

    // Needle
    const float na = PressureToRad(pressure);
    dl->AddLine(c, ImVec2(c.x + cosf(na) * r * 0.72f, c.y + sinf(na) * r * 0.72f),
                IM_COL32(226, 75, 74, 255), ImMax(2.f, d * 0.014f));
    dl->AddCircleFilled(c, ImMax(3.f, d * 0.025f), IM_COL32(201, 205, 210, 255), 16);

    // Value
    char val[8];
    snprintf(val, sizeof(val), "%.0f", pressure);
    const float vsize = d * 0.16f;
    const ImVec2 vs = font->CalcTextSizeA(vsize, FLT_MAX, 0.f, val);
    dl->AddText(font, vsize, ImVec2(c.x - vs.x * 0.5f, c.y + r * 0.38f), IM_COL32(255, 255, 255, 255), val);

    ImGui::End();
    ImGui::PopStyleVar(2);
}
