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

#include <string>
#include "GUI_WaterSettings.h"

#include "Application.h"
#include "GameContext.h"
#include "GUIManager.h"
#include "GUIUtils.h"
#include "HydraxWater.h"
#include "Language.h"
#include "SkyXManager.h"
#include "Terrain.h"

#include "Hydrax.h"
#include "Perlin.h"

namespace HX = Hydrax;

using namespace RoR;
using namespace GUI;

namespace {

/// Disables and dims all widgets between BeginGreyed() and EndGreyed() (call both with the same value).
void BeginGreyed(bool greyed)
{
    if (greyed)
    {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }
}

void EndGreyed(bool greyed)
{
    if (greyed)
    {
        ImGui::PopStyleVar();
        ImGui::PopItemFlag();
    }
}

} // anonymous namespace

void WaterControlsPanel::Draw()
{
    ImGui::SetNextWindowPosCenter(ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.f, ImGui::GetFontSize() * 30.f), ImGuiCond_FirstUseEver);
    bool keep_open = true;
    ImGui::Begin(_LC("WaterSettings", "Water settings"), &keep_open, ImGuiWindowFlags_NoCollapse);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 12.5f);

    // --- Which water is active? ---
    const TerrainPtr& terrain = App::GetGameContext()->GetTerrain();
    Wavefield* wavefield = (terrain) ? terrain->getWater() : nullptr;
    HydraxWater* hx_water = (terrain) ? terrain->getHydraxManager() : nullptr;
    HX::Hydrax* hx = (hx_water != nullptr) ? hx_water->GetHydrax() : nullptr;
    const bool hydrax_active = (hx != nullptr) && hx->isCreated();
    const bool standard_active = (terrain) && !hydrax_active && (terrain->getGfxWater() != nullptr);

    std::string active_name = _LC("WaterSettings", "None");
    if (!terrain)
    {
        active_name = _LC("WaterSettings", "none (no map loaded)");
    }
    else if (hydrax_active)
    {
        active_name = "Hydrax";
    }
    else if (standard_active)
    {
        active_name = ToLocalizedString(App::gfx_water_mode->getEnum<GfxWaterMode>());
    }
    ImGui::Text("%s", _LC("WaterSettings", "Active water:"));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "%s", active_name.c_str());
    if (hydrax_active)
    {
        ImGui::SameLine();
        if (ImGui::Button(_LC("WaterSettings", "Reset Hydrax to map values")))
        {
            hx_water->ResetToMapDefaults();
            m_components_valid = false;
            m_grid_complexity_valid = false;
        }
    }
    ImGui::Separator();

    HX::Module::ProjectedGrid* grid = hydrax_active ? dynamic_cast<HX::Module::ProjectedGrid*>(hx->getModule()) : nullptr;
    HX::Noise::Perlin* perlin = (grid != nullptr) ? dynamic_cast<HX::Noise::Perlin*>(grid->getNoise()) : nullptr;

    // --- Shared controls (Hydrax and standard water) ---
    const bool can_level = (wavefield != nullptr) && (hydrax_active || standard_active);
    if (can_level)
    {
        float level = wavefield->GetStaticWaterHeight();
        if (ImGui::DragFloat(_LC("WaterSettings", "Water level (m):"), &level, 0.1f, -500.f, 5000.f, "%.2f"))
        {
            wavefield->SetStaticWaterHeight(level);
        }
    }

    // Wave height: Hydrax = visual wave strength, standard water = wave field height.
    const bool std_waves = standard_active && (wavefield != nullptr) && App::gfx_water_waves->getBool()
        && (App::mp_state->getEnum<MpState>() != MpState::CONNECTED);
    const bool hx_waves = (grid != nullptr);

    if (standard_active)
    {
        DrawGCheckbox(App::gfx_water_waves, _LC("WaterSettings", "Waves enabled"));
    }

    if (std_waves || hx_waves)
    {
        float wave_height = 0.f;
        if (hx_waves)
        {
            wave_height = grid->getOptions().Strength;
        }
        else if (std_waves)
        {
            wave_height = wavefield->GetWavesHeight();
        }
        const float wave_max = hx_waves ? 10.f : 4.f;
        if (ImGui::SliderFloat(_LC("WaterSettings", "Wave height:"), &wave_height, 0.f, wave_max, "%.2f"))
        {
            if (hx_waves)
            {
                HX::Module::ProjectedGrid::Options go = grid->getOptions();
                go.Strength = wave_height;
                grid->setOptions(go);
            }
            else if (std_waves)
            {
                wavefield->SetWavesHeight(wave_height);
            }
        }
    }

    if (!can_level && !standard_active && !hydrax_active)
    {
        ImGui::TextDisabled("%s", _LC("WaterSettings", "The active water has no adjustable settings."));
    }

    // --- Hydrax: appearance ---
    if (hydrax_active && ImGui::CollapsingHeader(_LC("WaterSettings", "Appearance"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        {
            float transparency = hydrax_active ? hx->getGlobalTransparency() : 0.f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Global transparency:"), &transparency, 0.f, 1.f) && hydrax_active)
            {
                hx->setGlobalTransparency(transparency);
            }

            float distortion = hydrax_active ? hx->getNormalDistortion() : 0.075f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Normal distortion:"), &distortion, 0.f, 0.3f, "%.3f") && hydrax_active)
            {
                hx->setNormalDistortion(distortion);
            }

            const Ogre::Vector3 wc = hydrax_active ? hx->getWaterColor() : Ogre::Vector3(0.089f, 0.204f, 0.239f);
            float color[3] = { wc.x, wc.y, wc.z };
            if (ImGui::ColorEdit3(_LC("WaterSettings", "Water color:"), color) && hydrax_active)
            {
                hx->setWaterColor(Ogre::Vector3(color[0], color[1], color[2]));
                // SkyX recomputes the water color from the time of day every frame, unless told to keep this one
                SkyXManager* skyx_mgr = terrain->getSkyXManager();
                if (skyx_mgr != nullptr)
                {
                    skyx_mgr->SetWaterColorOverride(Ogre::Vector3(color[0], color[1], color[2]));
                }
            }

            float planes_error = hydrax_active ? hx->getPlanesError() : 10.5f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Planes error:"), &planes_error, 0.f, 12.f, "%.1f") && hydrax_active)
            {
                hx->setPlanesError(planes_error);
            }

            bool wireframe = hydrax_active && (hx->getPolygonMode() == Ogre::PM_WIREFRAME);
            if (ImGui::Checkbox(_LC("WaterSettings", "Wireframe"), &wireframe) && hydrax_active)
            {
                hx->setPolygonMode(wireframe ? Ogre::PM_WIREFRAME : Ogre::PM_SOLID);
            }

            // Same setting as in Settings > Graphics
            DrawGCheckbox(App::gfx_water_multithread, _LC("WaterSettings", "Hydrax multithreading"));
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", _LC("WaterSettings", "Computes the wave heights on several CPU cores. Turn off if it slows down scenes with many vehicles."));
            }
        }
    }

    // --- Hydrax: foam ---
    const bool foam_on = hydrax_active && hx->isComponent(HX::HYDRAX_COMPONENT_FOAM);
    if (hydrax_active && ImGui::CollapsingHeader(_LC("WaterSettings", "Foam")))
    {
        if (!foam_on)
        {
            ImGui::TextDisabled("%s", _LC("WaterSettings", "(component disabled in config)"));
        }
        BeginGreyed(!foam_on);
        {
            float scale = foam_on ? hx->getFoamScale() : 0.1f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Foam scale:"), &scale, 0.f, 1.f, "%.3f") && foam_on)
            {
                hx->setFoamScale(scale);
            }

            float start = foam_on ? hx->getFoamStart() : 0.f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Foam start:"), &start, 0.f, 1.f, "%.3f") && foam_on)
            {
                hx->setFoamStart(start);
            }

            float transp = foam_on ? hx->getFoamTransparency() : 1.f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Foam transparency:"), &transp, 0.f, 1.f, "%.3f") && foam_on)
            {
                hx->setFoamTransparency(transp);
            }

            float max_dist = foam_on ? hx->getFoamMaxDistance() : 7.5e6f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Foam max distance:"), &max_dist, 0.f, 2.0e7f, "%.0f", 4.f) && foam_on)
            {
                hx->setFoamMaxDistance(max_dist);
            }
        }
        EndGreyed(!foam_on);
    }

    // --- Hydrax: depth, shore, caustics ---
    const bool depth_on = hydrax_active && hx->isComponent(HX::HYDRAX_COMPONENT_DEPTH);
    const bool smooth_on = hydrax_active && hx->isComponent(HX::HYDRAX_COMPONENT_SMOOTH);
    const bool caustics_on = hydrax_active && hx->isComponent(HX::HYDRAX_COMPONENT_CAUSTICS);
    if (hydrax_active && ImGui::CollapsingHeader(_LC("WaterSettings", "Depth, shore and caustics")))
    {

        // Paged vegetation (grass, bushes, trees) is kept out of the depth map by default, it only distorts the water depth there
        {
            const Ogre::uint32 veg_bit = static_cast<Ogre::uint32>(DEPTHMAP_VEGETATION);
            const Ogre::uint32 depth_mask = hydrax_active ? hx->getDepthVisibilityMask() : 0u;
            bool veg_in_depth = (depth_mask & veg_bit) != 0;
            if (ImGui::Checkbox(_LC("WaterSettings", "Vegetation in depth map"), &veg_in_depth) && hydrax_active)
            {
                hx->setDepthVisibilityMask(veg_in_depth ? (depth_mask | veg_bit) : (depth_mask & ~veg_bit));
            }
        }

        BeginGreyed(!depth_on);
        {
            float depth_limit = depth_on ? hx->getDepthLimit() : 70.f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Depth limit:"), &depth_limit, 1.f, 200.f, "%.1f") && depth_on)
            {
                hx->setDepthLimit(depth_limit);
            }

            float dist_limit = depth_on ? hx->getDistLimit() : 90.f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Distance limit:"), &dist_limit, 1.f, 300.f, "%.1f") && depth_on)
            {
                hx->setDistLimit(dist_limit);
            }
        }
        EndGreyed(!depth_on);

        BeginGreyed(!smooth_on);
        {
            float power = smooth_on ? hx->getSmoothPower() : 5.f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Smooth power:"), &power, 0.f, 20.f, "%.2f") && smooth_on)
            {
                hx->setSmoothPower(power);
            }
        }
        EndGreyed(!smooth_on);

        BeginGreyed(!caustics_on);
        {
            float scale = caustics_on ? hx->getCausticsScale() : 8.5f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Caustics scale:"), &scale, 0.f, 20.f, "%.2f") && caustics_on)
            {
                hx->setCausticsScale(scale);
            }

            float power = caustics_on ? hx->getCausticsPower() : 5.5f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Caustics power:"), &power, 0.f, 20.f, "%.2f") && caustics_on)
            {
                hx->setCausticsPower(power);
            }

            float end = caustics_on ? hx->getCausticsEnd() : 0.7f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Caustics end:"), &end, 0.f, 1.f, "%.2f") && caustics_on)
            {
                hx->setCausticsEnd(end);
            }
        }
        EndGreyed(!caustics_on);
    }

    // --- Hydrax: components. Sun reflection and god rays are usually switched off in the map's Hydrax config ---
    if (hydrax_active && ImGui::CollapsingHeader(_LC("WaterSettings", "Components")))
    {
        if (hydrax_active && !m_components_valid)
        {
            m_components = static_cast<int>(hx->getComponents());
            m_components_valid = true;
        }
        {
            struct { const char* name; HX::HydraxComponent bit; } const comps[] = {
                { "Sun reflection", HX::HYDRAX_COMPONENT_SUN },
                { "Foam", HX::HYDRAX_COMPONENT_FOAM },
                { "Depth", HX::HYDRAX_COMPONENT_DEPTH },
                { "Smooth shore", HX::HYDRAX_COMPONENT_SMOOTH },
                { "Caustics", HX::HYDRAX_COMPONENT_CAUSTICS },
                { "Underwater", HX::HYDRAX_COMPONENT_UNDERWATER },
                { "Underwater reflections", HX::HYDRAX_COMPONENT_UNDERWATER_REFLECTIONS },
                { "God rays", HX::HYDRAX_COMPONENT_UNDERWATER_GODRAYS },
            };
            for (const auto& c : comps)
            {
                bool on = (m_components & c.bit) != 0;
                // '##comp' keeps the ID unique: the 'Foam' header above uses the same label and would swallow the clicks
                const std::string label = std::string(c.name) + "##comp";
                if (ImGui::Checkbox(label.c_str(), &on))
                {
                    m_components = on ? (m_components | c.bit) : (m_components & ~c.bit);
                }
            }
            ImGui::TextDisabled("%s", _LC("WaterSettings", "Smooth/Caustics need Depth, God rays need Depth, Underwater and Sun."));
            if (ImGui::Button(_LC("WaterSettings", "Apply components (rebuilds the water shaders)")) && hydrax_active)
            {
                hx_water->ApplyComponents(static_cast<HX::HydraxComponent>(m_components));
                m_components = static_cast<int>(hx->getComponents()); // dependencies may have switched some off
            }
        }
    }

    // --- Hydrax: sun reflection and god rays (components off by default, broken with OpenGL) ---
    const bool sun_on = hydrax_active && hx->isComponent(HX::HYDRAX_COMPONENT_SUN);
    const bool rays_on = hydrax_active && hx->isComponent(HX::HYDRAX_COMPONENT_UNDERWATER_GODRAYS);
    if (hydrax_active && ImGui::CollapsingHeader(_LC("WaterSettings", "Sun reflection and god rays")))
    {
        BeginGreyed(!sun_on);
        {
            float strength = sun_on ? hx->getSunStrength() : 1.75f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Sun strength:"), &strength, 0.f, 10.f, "%.2f") && sun_on)
            {
                hx->setSunStrength(strength);
            }

            float area = sun_on ? hx->getSunArea() : 150.f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "Sun area:"), &area, 1.f, 1000.f, "%.0f", 3.f) && sun_on)
            {
                hx->setSunArea(area);
            }

            const Ogre::Vector3 sc = sun_on ? hx->getSunColor() : Ogre::Vector3(1.f, 0.9f, 0.6f);
            float sun_color[3] = { sc.x, sc.y, sc.z };
            if (ImGui::ColorEdit3(_LC("WaterSettings", "Sun color:"), sun_color) && sun_on)
            {
                hx->setSunColor(Ogre::Vector3(sun_color[0], sun_color[1], sun_color[2]));
            }
        }
        EndGreyed(!sun_on);

        BeginGreyed(!rays_on);
        {
            const Ogre::Vector3 ev = rays_on ? hx->getGodRaysExposure() : Ogre::Vector3(0.76f, 2.46f, 2.29f);
            float exposure[3] = { ev.x, ev.y, ev.z };
            if (ImGui::SliderFloat3(_LC("WaterSettings", "God rays exposure:"), exposure, 0.f, 5.f, "%.2f") && rays_on)
            {
                hx->setGodRaysExposure(Ogre::Vector3(exposure[0], exposure[1], exposure[2]));
            }

            float intensity = rays_on ? hx->getGodRaysIntensity() : 0.015f;
            if (ImGui::SliderFloat(_LC("WaterSettings", "God rays intensity:"), &intensity, 0.f, 0.1f, "%.4f") && rays_on)
            {
                hx->setGodRaysIntensity(intensity);
            }
        }
        EndGreyed(!rays_on);
    }

    // --- Hydrax: wave shape (projected grid + Perlin noise, as created by RoR) ---
    if (hydrax_active && ImGui::CollapsingHeader(_LC("WaterSettings", "Wave shape")))
    {
        if (grid == nullptr)
        {
            ImGui::TextDisabled("%s", _LC("WaterSettings", "(unsupported water module)"));
        }
        BeginGreyed(grid == nullptr);
        {
            HX::Module::ProjectedGrid::Options go;
            if (grid != nullptr)
            {
                go = grid->getOptions();
            }
            bool grid_dirty = false;
            grid_dirty |= ImGui::Checkbox(_LC("WaterSettings", "Choppy waves"), &go.ChoppyWaves);
            grid_dirty |= ImGui::SliderFloat(_LC("WaterSettings", "Choppy strength:"), &go.ChoppyStrength, 0.f, 2.f, "%.3f");
            grid_dirty |= ImGui::Checkbox(_LC("WaterSettings", "Smooth geometry"), &go.Smooth);
            if (grid_dirty && grid != nullptr)
            {
                grid->setOptions(go);
            }

            // Complexity rebuilds the grid -> edit field + button
            if (grid != nullptr && !m_grid_complexity_valid)
            {
                m_grid_complexity = grid->getOptions().Complexity;
                m_grid_complexity_valid = true;
            }
            ImGui::SliderInt(_LC("WaterSettings", "Grid complexity:"), &m_grid_complexity, 50, 400);
            if (ImGui::Button(_LC("WaterSettings", "Apply complexity (rebuilds grid)")) && grid != nullptr)
            {
                go = grid->getOptions();
                go.Complexity = m_grid_complexity;
                grid->setOptions(go);
            }
        }
        EndGreyed(grid == nullptr);

        BeginGreyed(perlin == nullptr);
        {
            HX::Noise::Perlin::Options po;
            if (perlin != nullptr)
            {
                po = perlin->getOptions();
            }
            bool noise_dirty = false;
            noise_dirty |= ImGui::SliderFloat(_LC("WaterSettings", "Wave scale:"), &po.Scale, 0.05f, 2.f, "%.3f");
            noise_dirty |= ImGui::SliderFloat(_LC("WaterSettings", "Falloff:"), &po.Falloff, 0.1f, 0.9f, "%.2f");
            noise_dirty |= ImGui::SliderFloat(_LC("WaterSettings", "Animation speed:"), &po.Animspeed, 0.f, 5.f, "%.2f");
            noise_dirty |= ImGui::SliderFloat(_LC("WaterSettings", "Time multiplier:"), &po.Timemulti, 0.f, 5.f, "%.2f");
            if (noise_dirty && perlin != nullptr)
            {
                perlin->setOptions(po);
            }
        }
        EndGreyed(perlin == nullptr);
    }

    ImGui::PopItemWidth();

    m_is_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    App::GetGuiManager()->RequestGuiCaptureKeyboard(m_is_hovered);

    ImGui::End();

    if (!keep_open)
    {
        this->SetVisible(false);
    }
}
