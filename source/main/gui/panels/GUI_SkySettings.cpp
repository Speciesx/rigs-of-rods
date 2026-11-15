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

#include "GUI_SkySettings.h"

#include "Application.h"
#include "GameContext.h"
#include "GUIManager.h"
#include "GUIUtils.h"
#include "Language.h"
#include "SkyXManager.h"
#include "SkyXRainParticles.h"
#include "SkyXWeatherAudio.h"
#include "Terrain.h"

#ifdef USE_CAELUM
#include "SkyManager.h"
#endif

#include <algorithm>
#include <cmath>

namespace SX = SkyX;

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

void SkyControlsPanel::Draw()
{
    ImGui::SetNextWindowPosCenter(ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.f, ImGui::GetFontSize() * 30.f), ImGuiCond_FirstUseEver);
    bool keep_open = true;
    ImGui::Begin(_LC("SkySettings", "Sky settings"), &keep_open, ImGuiWindowFlags_NoCollapse);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 12.5f);

    // --- Which sky is active? ---
    const TerrainPtr& terrain = App::GetGameContext()->GetTerrain();
    SkyXManager* skyx_mgr = (terrain) ? terrain->getSkyXManager() : nullptr;
    SX::SkyX* skyx = (skyx_mgr != nullptr) ? skyx_mgr->GetSkyX() : nullptr;
    const bool skyx_active = (skyx != nullptr) && skyx->isCreated();

#ifdef USE_CAELUM
    SkyManager* caelum = (terrain && !skyx_active) ? terrain->getSkyManager() : nullptr;
#else
    void* caelum = nullptr;
#endif

#ifdef USE_CAELUM
    Caelum::CaelumSystem* csys = (caelum != nullptr) ? caelum->GetCaelumSys() : nullptr;
    Caelum::FlatCloudLayer* clayer = nullptr;
    if (csys != nullptr && csys->getCloudSystem() != nullptr && csys->getCloudSystem()->getLayerCount() > 0)
    {
        clayer = csys->getCloudSystem()->getLayer(0);
    }
#else
    void* csys = nullptr;
    void* clayer = nullptr;
#endif

    std::string active_name = _LC("SkySettings", "Sandstorm (no settings)");
    if (!terrain)
    {
        active_name = _LC("SkySettings", "none (no map loaded)");
    }
    else if (skyx_active)
    {
        active_name = "SkyX";
    }
    else if (caelum != nullptr)
    {
        active_name = "Caelum";
    }
    ImGui::Text("%s", _LC("SkySettings", "Active sky:"));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "%s", active_name.c_str());
    if (skyx_active)
    {
        ImGui::SameLine();
        if (ImGui::Button(_LC("SkySettings", "Reset SkyX to map values")))
        {
            skyx_mgr->ResetToMapDefaults();
            m_cloud_height_valid = false;
        }
    }
    ImGui::Separator();

    // --- Time (SkyX and Caelum share these controls) ---
    SX::BasicController* ctrl = skyx_active ? dynamic_cast<SX::BasicController*>(skyx->getController()) : nullptr;
    const bool can_time = (ctrl != nullptr) || (caelum != nullptr);

    BeginGreyed(!can_time);
    {
        float hour = 12.f;
        if (ctrl != nullptr)
        {
            hour = ctrl->getTime().x;
        }
#ifdef USE_CAELUM
        else if (caelum != nullptr)
        {
            // Julian day starts at noon; same clock as SkyManager::GetPrettyTime()
            const double jd = caelum->GetTime();
            hour = static_cast<float>((jd + 0.5 - std::floor(jd + 0.5)) * 24.0);
        }
#endif
        if (ImGui::SliderFloat(_LC("SkySettings", "Time of day (h):"), &hour, 0.f, 24.f, "%.2f") && can_time)
        {
            if (ctrl != nullptr)
            {
                Ogre::Vector3 t = ctrl->getTime();
                t.x = hour;
                ctrl->setTime(t);
            }
#ifdef USE_CAELUM
            else if (caelum != nullptr)
            {
                const double jd = caelum->GetTime();
                caelum->SetTime(std::floor(jd + 0.5) + static_cast<double>(hour) / 24.0 - 0.5);
            }
#endif
        }

        DrawGCheckbox(App::gfx_sky_time_cycle, _LC("SkySettings", "Cycle time"));
        ImGui::SameLine();
        bool time_frozen = App::GetGameContext()->GetSkyTimeFrozen();
        if (ImGui::Checkbox(_LC("SkySettings", "Freeze time"), &time_frozen))
        {
            App::GetGameContext()->SetSkyTimeFrozen(time_frozen);
        }
        DrawGIntSlider(App::gfx_sky_time_speed, _LC("SkySettings", "Time speed (x real-time):"), 10, 2000);
    }
    EndGreyed(!can_time);

    // --- Sun and moon (SkyX only) ---
    if (ImGui::CollapsingHeader(_LC("SkySettings", "Sun and moon")))
    {
        if (!skyx_active)
        {
            ImGui::TextDisabled("%s", _LC("SkySettings", "(SkyX only)"));
        }
        // Night brightness: 100 % = default, 0 % = pitch black night, more = moonlit night
        BeginGreyed(skyx_mgr == nullptr);
        {
            float night_pct = (skyx_mgr != nullptr) ? skyx_mgr->GetNightBrightness() * 100.f : 100.f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Night brightness:"), &night_pct, 0.f, 300.f, "%.0f %%") && skyx_mgr != nullptr)
            {
                skyx_mgr->SetNightBrightness(night_pct / 100.f);
            }
            ImGui::SameLine();
            ImGui::PushID("night_brightness_reset");
            if (ImGui::Button(_LC("SkySettings", "Reset")) && skyx_mgr != nullptr)
            {
                skyx_mgr->SetNightBrightness(1.f);
            }
            ImGui::PopID();
        }
        EndGreyed(skyx_mgr == nullptr);

        BeginGreyed(ctrl == nullptr);
        {
            // Astronomical sun and moon (ported from Caelum): trajectories follow from latitude, day of year and time of day
            bool astro = (ctrl != nullptr) && ctrl->isAstronomical();
            if (ImGui::Checkbox(_LC("SkySettings", "Astronomical sun and moon"), &astro) && ctrl != nullptr)
            {
                ctrl->setAstronomical(astro);
            }
            if (astro)
            {
                float latitude = ctrl->getLatitude();
                if (ImGui::SliderFloat(_LC("SkySettings", "Latitude (deg):"), &latitude, -90.f, 90.f, "%.1f"))
                {
                    ctrl->setLatitude(latitude);
                }
                int day = ctrl->getDayOfYear();
                if (ImGui::SliderInt(_LC("SkySettings", "Day of year:"), &day, 1, 365))
                {
                    ctrl->setDayOfYear(day);
                }
                const Ogre::Vector3 st = ctrl->getTime();
                ImGui::Text("%s %02d:%02d   %s %02d:%02d",
                    _LC("SkySettings", "Sunrise:"), static_cast<int>(st.y), static_cast<int>((st.y - std::floor(st.y)) * 60.f),
                    _LC("SkySettings", "Sunset:"), static_cast<int>(st.z), static_cast<int>((st.z - std::floor(st.z)) * 60.f));
            }

            BeginGreyed(astro);
            {
                Ogre::Vector3 t = (ctrl != nullptr) ? ctrl->getTime() : Ogre::Vector3(12.f, 7.5f, 20.5f);
                bool time_dirty = false;
                time_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Sunrise (h):"), &t.y, 0.f, 24.f, "%.2f");
                time_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Sunset (h):"), &t.z, 0.f, 24.f, "%.2f");
                if (time_dirty && ctrl != nullptr)
                {
                    ctrl->setTime(t);
                }

                float moon = (ctrl != nullptr) ? ctrl->getMoonPhase() : 0.f;
                if (ImGui::SliderFloat(_LC("SkySettings", "Moon phase (0 = clear):"), &moon, -1.f, 1.f, "%.2f") && ctrl != nullptr)
                {
                    ctrl->setMoonPhase(moon);
                }
            }
            EndGreyed(astro);
        }
        EndGreyed(ctrl == nullptr);
    }

    // --- Atmosphere (sky color, SkyX only) ---
    if (ImGui::CollapsingHeader(_LC("SkySettings", "Sky color and atmosphere")))
    {
        if (!skyx_active)
        {
            ImGui::TextDisabled("%s", _LC("SkySettings", "(SkyX only)"));
        }
        BeginGreyed(!skyx_active);
        {
            SX::AtmosphereManager::Options opt;
            if (skyx_active)
            {
                opt = skyx->getAtmosphereManager()->getOptions();
            }
            bool dirty = false;

            // The color channels are the wavelengths of red/green/blue light (lower value = that color scatters more)
            float wl[3] = { opt.WaveLength.x, opt.WaveLength.y, opt.WaveLength.z };
            if (ImGui::ColorEdit3(_LC("SkySettings", "Sky wavelengths (R,G,B):"), wl))
            {
                // Values below 0.3 would break the scattering calculation (division by ~0)
                opt.WaveLength = Ogre::Vector3(std::max(wl[0], 0.3f), std::max(wl[1], 0.3f), std::max(wl[2], 0.3f));
                dirty = true;
            }
            dirty |= ImGui::SliderFloat(_LC("SkySettings", "Rayleigh multiplier:"), &opt.RayleighMultiplier, 0.f, 0.01f, "%.5f");
            dirty |= ImGui::SliderFloat(_LC("SkySettings", "Mie multiplier:"), &opt.MieMultiplier, 0.f, 0.005f, "%.6f");
            dirty |= ImGui::SliderFloat(_LC("SkySettings", "Sun intensity:"), &opt.SunIntensity, 0.f, 100.f);
            dirty |= ImGui::SliderFloat(_LC("SkySettings", "Exposure:"), &opt.Exposure, 0.1f, 10.f);
            dirty |= ImGui::SliderFloat(_LC("SkySettings", "Height position:"), &opt.HeightPosition, 0.f, 1.f, "%.3f");
            dirty |= ImGui::SliderFloat(_LC("SkySettings", "Phase function (G):"), &opt.G, -0.999f, -0.5f, "%.3f");
            dirty |= ImGui::SliderInt(_LC("SkySettings", "Atmosphere samples:"), &opt.NumberOfSamples, 1, 8);
            if (dirty && skyx_active)
            {
                skyx->getAtmosphereManager()->setOptions(opt);
            }

            bool stars = skyx_active ? skyx->isStarfieldEnabled() : false;
            if (ImGui::Checkbox(_LC("SkySettings", "Starfield"), &stars) && skyx_active)
            {
                skyx->setStarfieldEnabled(stars);
            }
        }
        EndGreyed(!skyx_active);
    }

    // --- Clouds (SkyX volumetric clouds and the first Caelum cloud layer share density, altitude and wind) ---
    SX::VCloudsManager* vcm = skyx_active ? skyx->getVCloudsManager() : nullptr;
    SX::VClouds::VClouds* vc = nullptr;
    if (vcm != nullptr && vcm->isCreated())
    {
        vc = vcm->getVClouds();
    }
    bool can_clouds = (vc != nullptr) || (clayer != nullptr);

    bool apply_height = false; // SkyX can only change altitude/thickness by rebuilding the clouds, done when a slider is released
    if (ImGui::CollapsingHeader(_LC("SkySettings", "Clouds")))
    {
        // Switch the SkyX volumetric clouds on/off (builds/removes the cloud simulation, causes a short hitch)
        BeginGreyed(vcm == nullptr);
        {
            bool vol_enabled = (vc != nullptr);
            if (ImGui::Checkbox(_LC("SkySettings", "SkyX volumetric clouds enabled"), &vol_enabled) && vcm != nullptr)
            {
                if (vol_enabled)
                {
                    vcm->create();
                }
                else
                {
                    vcm->remove();
                }
                vc = vcm->isCreated() ? vcm->getVClouds() : nullptr;
                m_cloud_height_valid = false;
                can_clouds = (vc != nullptr) || (clayer != nullptr);
            }
        }
        EndGreyed(vcm == nullptr);

        if (!can_clouds)
        {
            ImGui::TextDisabled("%s", _LC("SkySettings", "(the active sky has no clouds)"));
        }

        // Shared controls
        BeginGreyed(!can_clouds);
        {
            if (vc != nullptr && !m_cloud_height_valid)
            {
                m_cloud_height = vc->getGeometryManager()->getHeight();
                m_cloud_height_valid = true;
            }

            float density = 0.1f;
            if (vc != nullptr)
            {
                density = vc->getWheater().x;
            }
#ifdef USE_CAELUM
            else if (clayer != nullptr)
            {
                density = clayer->getCloudCover();
            }
#endif
            if (ImGui::SliderFloat(_LC("SkySettings", "Cloud density:"), &density, 0.f, 1.f, "%.2f") && can_clouds)
            {
                if (vc != nullptr)
                {
                    const Ogre::Vector2 w = vc->getWheater();
                    vc->setWheater(density, w.y, false);
                }
#ifdef USE_CAELUM
                else if (clayer != nullptr)
                {
                    clayer->setCloudCover(density);
                }
#endif
            }

            // SkyX can only change the altitude when the clouds are rebuilt (button below), Caelum applies it live
            float altitude = (vc != nullptr) ? m_cloud_height.x : 0.f;
#ifdef USE_CAELUM
            if (vc == nullptr && clayer != nullptr)
            {
                altitude = clayer->getHeight();
            }
#endif
            if (ImGui::SliderFloat(_LC("SkySettings", "Cloud altitude (m):"), &altitude, 0.f, 10000.f, "%.0f") && can_clouds)
            {
                if (vc != nullptr)
                {
                    m_cloud_height.x = altitude;
                }
#ifdef USE_CAELUM
                else if (clayer != nullptr)
                {
                    clayer->setHeight(altitude);
                }
#endif
            }
            if (vc != nullptr && ImGui::IsItemDeactivatedAfterEdit())
            {
                apply_height = true;
            }

            float wind = 0.f;
            float wind_dir = 0.f;
            float wind_max = 300.f;
            const char* wind_format = "%.0f";
            if (vc != nullptr)
            {
                wind = vcm->getWindSpeed();
                wind_dir = vc->getWindDirection().valueDegrees();
            }
#ifdef USE_CAELUM
            else if (clayer != nullptr)
            {
                const Ogre::Vector2 speed = clayer->getCloudSpeed();
                wind = speed.length();
                wind_dir = Ogre::Math::ATan2(speed.y, speed.x).valueDegrees();
                if (wind_dir < 0.f)
                {
                    wind_dir += 360.f;
                }
                wind_max = 0.0002f; // Caelum counts texture offset per second
                wind_format = "%.6f";
            }
#endif
            bool wind_dirty = false;
            wind_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Wind speed:"), &wind, 0.f, wind_max, wind_format);
            wind_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Wind direction (deg):"), &wind_dir, 0.f, 360.f, "%.0f");
            if (wind_dirty && can_clouds)
            {
                if (vc != nullptr)
                {
                    // Through the manager: it re-applies its own wind speed (times the time multiplier) on every change
                    vcm->setWindSpeed(wind);
                    vc->setWindDirection(Ogre::Radian(Ogre::Degree(wind_dir)));
                }
#ifdef USE_CAELUM
                else if (clayer != nullptr)
                {
                    const float rad = Ogre::Degree(wind_dir).valueRadians();
                    clayer->setCloudSpeed(Ogre::Vector2(wind * std::cos(rad), wind * std::sin(rad)));
                }
#endif
            }
        }
        EndGreyed(!can_clouds);

        // SkyX volumetric clouds only
        ImGui::TextDisabled("%s", _LC("SkySettings", "SkyX volumetric clouds"));
        BeginGreyed(vc == nullptr);
        {
            float cloud_size = (vc != nullptr) ? vc->getWheater().y : 0.6f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Average cloud size:"), &cloud_size, 0.f, 1.f, "%.2f") && vc != nullptr)
            {
                const Ogre::Vector2 w = vc->getWheater();
                vc->setWheater(w.x, cloud_size, false);
            }

            float opacity = (vc != nullptr) ? vc->getGlobalOpacity() : 1.f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Opacity:"), &opacity, 0.f, 1.f, "%.2f") && vc != nullptr)
            {
                vc->setGlobalOpacity(opacity);
            }

            float field_scale = (vc != nullptr) ? vc->getCloudFieldScale() : 1.f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Cloud field scale:"), &field_scale, 0.2f, 5.f, "%.2f") && vc != nullptr)
            {
                vc->setCloudFieldScale(field_scale);
            }

            float noise_scale = (vc != nullptr) ? vc->getNoiseScale() : 4.2f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Noise scale:"), &noise_scale, 0.5f, 10.f, "%.2f") && vc != nullptr)
            {
                vc->setNoiseScale(noise_scale);
            }

            const Ogre::Vector3 tint_v = (vcm != nullptr) ? vcm->getColorTint() : Ogre::Vector3(1.f, 1.f, 1.f);
            float tint[3] = { tint_v.x, tint_v.y, tint_v.z };
            if (ImGui::ColorEdit3(_LC("SkySettings", "Cloud color tint:"), tint) && vcm != nullptr)
            {
                vcm->setColorTint(Ogre::Vector3(tint[0], tint[1], tint[2]));
            }

            Ogre::Vector4 lr_v = (vc != nullptr) ? vc->getLightResponse() : Ogre::Vector4(0.25f, 0.2f, 1.0f, 0.1f);
            float lr[4] = { lr_v.x, lr_v.y, lr_v.z, lr_v.w };
            if (ImGui::SliderFloat4(_LC("SkySettings", "Light response:"), lr, 0.f, 3.f, "%.2f") && vc != nullptr)
            {
                vc->setLightResponse(Ogre::Vector4(lr[0], lr[1], lr[2], lr[3]));
            }

            Ogre::Vector4 af_v = (vc != nullptr) ? vc->getAmbientFactors() : Ogre::Vector4(0.45f, 0.3f, 0.6f, 1.0f);
            float af[4] = { af_v.x, af_v.y, af_v.z, af_v.w };
            if (ImGui::SliderFloat4(_LC("SkySettings", "Ambient factors:"), af, 0.f, 2.f, "%.2f") && vc != nullptr)
            {
                vc->setAmbientFactors(Ogre::Vector4(af[0], af[1], af[2], af[3]));
            }

            ImGui::SliderFloat(_LC("SkySettings", "Cloud thickness (m):"), &m_cloud_height.y, 100.f, 10000.f, "%.0f");
            if (vc != nullptr && ImGui::IsItemDeactivatedAfterEdit())
            {
                apply_height = true;
            }
            if (ImGui::Button(_LC("SkySettings", "Apply altitude and thickness (rebuilds clouds)")))
            {
                apply_height = true;
            }
            if (apply_height && vcm != nullptr)
            {
                // 'vc' is invalid after this, it is not used again in this block
                vcm->remove();
                vcm->setHeight(m_cloud_height);
                vcm->create();
                m_cloud_height_valid = false;
            }
        }
        EndGreyed(vc == nullptr);
    }

    // --- SkyX weather: storm clouds, lightning and precipitation ---
    if (ImGui::CollapsingHeader(_LC("SkySettings", "Weather and lightning")))
    {
        SX::VClouds::VClouds* wvc = (vcm != nullptr && vcm->isCreated()) ? vcm->getVClouds() : nullptr;
        SX::VClouds::LightningManager* lm = (wvc != nullptr) ? wvc->getLightningManager() : nullptr;
        SX::PrecipitationController* wprec = skyx_active ? skyx->getPrecipitationController() : nullptr;

        if (!skyx_active)
        {
            ImGui::TextDisabled("%s", _LC("SkySettings", "(SkyX only)"));
        }
        else if (wvc == nullptr)
        {
            ImGui::TextDisabled("%s", _LC("SkySettings", "(enable the SkyX volumetric clouds first)"));
        }

        // One-click weather presets: dark dense clouds + rain + lightning, or clear sky
        BeginGreyed(wvc == nullptr);
        {
            if (ImGui::Button(_LC("SkySettings", "Storm")) && wvc != nullptr)
            {
                const Ogre::Vector2 w = wvc->getWheater();
                wvc->setWheater(0.9f, w.y, false);
                if (lm != nullptr)
                {
                    lm->setAverageLightningApparitionTime(2.f);
                    lm->setEnabled(true);
                }
                if (wprec != nullptr)
                {
                    wprec->setPresetType(SX::PRECTYPE_RAIN);
                    wprec->setIntensity(0.8f);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(_LC("SkySettings", "Clear weather")) && wvc != nullptr)
            {
                const Ogre::Vector2 w = wvc->getWheater();
                wvc->setWheater(0.15f, w.y, false);
                if (lm != nullptr)
                {
                    lm->setEnabled(false);
                }
                if (wprec != nullptr)
                {
                    wprec->setIntensity(0.f);
                }
            }
        }
        EndGreyed(wvc == nullptr);

        BeginGreyed(lm == nullptr);
        {
            bool lightning_on = (lm != nullptr) && lm->isEnabled();
            if (ImGui::Checkbox(_LC("SkySettings", "Lightning"), &lightning_on) && lm != nullptr)
            {
                lm->setEnabled(lightning_on);
            }

            float avg_time = (lm != nullptr) ? lm->getAverageLightningApparitionTime() : 1.5f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Average time between lightnings (s):"), &avg_time, 0.2f, 30.f, "%.1f", 2.f) && lm != nullptr)
            {
                lm->setAverageLightningApparitionTime(avg_time);
            }

            float speed = (lm != nullptr) ? lm->getLightningTimeMultiplier() : 2.f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Lightning speed:"), &speed, 0.1f, 10.f, "%.2f") && lm != nullptr)
            {
                lm->setLightningTimeMultiplier(speed);
            }

            const Ogre::Vector3 lc = (lm != nullptr) ? lm->getLightningColor() : Ogre::Vector3(1.f, 0.925f, 0.85f);
            float lcol[3] = { lc.x, lc.y, lc.z };
            if (ImGui::ColorEdit3(_LC("SkySettings", "Lightning color:"), lcol) && lm != nullptr)
            {
                lm->setLightningColor(Ogre::Vector3(lcol[0], lcol[1], lcol[2]));
            }
        }
        EndGreyed(lm == nullptr);
        if (skyx_mgr != nullptr && skyx_mgr->GetWeatherAudio() != nullptr)
        {
            bool sounds_on = skyx_mgr->GetWeatherAudio()->IsEnabled();
            if (ImGui::Checkbox(_LC("SkySettings", "Weather sounds (rain, wind, thunder)"), &sounds_on))
            {
                skyx_mgr->GetWeatherAudio()->SetEnabled(sounds_on);
            }
        }
        ImGui::TextDisabled("%s", _LC("SkySettings", "Rain: see 'Precipitation'. Cloud density and size: see 'Clouds'."));
    }

    // --- SkyX layered clouds ---
    if (ImGui::CollapsingHeader(_LC("SkySettings", "SkyX layered clouds")))
    {
        SX::CloudsManager* clouds_mgr = skyx_active ? skyx->getCloudsManager() : nullptr;

        // Switch on/off: adds one default layer / removes all layers
        BeginGreyed(clouds_mgr == nullptr);
        {
            bool layers_enabled = (clouds_mgr != nullptr) && !clouds_mgr->getCloudLayers().empty();
            if (ImGui::Checkbox(_LC("SkySettings", "Layered clouds enabled"), &layers_enabled) && clouds_mgr != nullptr)
            {
                if (layers_enabled)
                {
                    clouds_mgr->add(SX::CloudLayer::Options());
                }
                else
                {
                    clouds_mgr->removeAll();
                }
            }
        }
        EndGreyed(clouds_mgr == nullptr);

        // Fetched after the switch above, removeAll() invalidates old pointers
        SX::CloudLayer* layer = nullptr;
        if (clouds_mgr != nullptr && !clouds_mgr->getCloudLayers().empty())
        {
            layer = clouds_mgr->getCloudLayers().front();
        }

        if (layer == nullptr)
        {
            ImGui::TextDisabled("%s", skyx_active ? _LC("SkySettings", "(switched off)") : _LC("SkySettings", "(SkyX only)"));
        }
        BeginGreyed(layer == nullptr);
        {
            SX::CloudLayer::Options lo;
            if (layer != nullptr)
            {
                lo = layer->getOptions();
            }
            bool layer_dirty = false;
            layer_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Layer height:"), &lo.Height, 0.f, 1000.f, "%.1f");
            layer_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Layer scale:"), &lo.Scale, 0.0001f, 0.01f, "%.5f");
            layer_dirty |= ImGui::SliderFloat2(_LC("SkySettings", "Layer wind direction:"), &lo.WindDirection.x, -2.f, 2.f, "%.2f");
            layer_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Layer time multiplier:"), &lo.TimeMultiplier, 0.f, 1.f, "%.3f");
            layer_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Distance attenuation:"), &lo.DistanceAttenuation, 0.f, 1.f, "%.3f");
            layer_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Detail attenuation:"), &lo.DetailAttenuation, 0.f, 2.f, "%.2f");
            layer_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Height volume:"), &lo.HeightVolume, 0.f, 1.f, "%.3f");
            layer_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Volumetric displacement:"), &lo.VolumetricDisplacement, 0.f, 0.1f, "%.4f");
            if (layer_dirty && layer != nullptr)
            {
                layer->setOptions(lo);
            }
        }
        EndGreyed(layer == nullptr);
    }

#ifdef USE_CAELUM
    // --- Caelum: fog and haze ---
    if (ImGui::CollapsingHeader(_LC("SkySettings", "Fog and haze")))
    {
        if (csys == nullptr)
        {
            ImGui::TextDisabled("%s", _LC("SkySettings", "(Caelum only)"));
        }

        // Scene fog range, only when the sky script / map config lets Caelum manage the fog
        const bool fog_ctl = (csys != nullptr) && (csys->getManageSceneFog() != Ogre::FOG_NONE);
        BeginGreyed(!fog_ctl);
        {
            float fog_start = fog_ctl ? csys->getManageSceneFogStart() : 0.f;
            float fog_end = fog_ctl ? csys->getManageSceneFogEnd() : 0.f;
            bool fog_dirty = false;
            fog_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Fog start (m):"), &fog_start, 0.f, 20000.f, "%.0f", 2.f);
            fog_dirty |= ImGui::SliderFloat(_LC("SkySettings", "Fog end (m):"), &fog_end, 0.f, 20000.f, "%.0f", 2.f);
            if (fog_dirty && fog_ctl)
            {
                csys->setManageSceneFogStart(fog_start);
                csys->setManageSceneFogEnd(fog_end);
            }

            const Ogre::ColourValue fc = fog_ctl ? csys->getSceneFogColourMultiplier() : Ogre::ColourValue::White;
            float fog_color[3] = { fc.r, fc.g, fc.b };
            if (ImGui::ColorEdit3(_LC("SkySettings", "Fog color multiplier:"), fog_color) && fog_ctl)
            {
                csys->setSceneFogColourMultiplier(Ogre::ColourValue(fog_color[0], fog_color[1], fog_color[2], fc.a));
            }
        }
        EndGreyed(!fog_ctl);

        BeginGreyed(csys == nullptr);
        {
            float global_fog = (csys != nullptr) ? csys->getGlobalFogDensityMultiplier() : 1.f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Fog density multiplier:"), &global_fog, 0.f, 5.f, "%.2f") && csys != nullptr)
            {
                csys->setGlobalFogDensityMultiplier(global_fog);
            }
        }
        EndGreyed(csys == nullptr);

        Caelum::GroundFog* ground_fog = (csys != nullptr) ? csys->getGroundFog() : nullptr;
        BeginGreyed(ground_fog == nullptr);
        {
            float gf = (csys != nullptr) ? csys->getGroundFogDensityMultiplier() : 0.03f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Ground fog density:"), &gf, 0.f, 0.3f, "%.4f", 3.f) && csys != nullptr)
            {
                csys->setGroundFogDensityMultiplier(gf);
            }
        }
        EndGreyed(ground_fog == nullptr);

        Caelum::SkyDome* dome = (csys != nullptr) ? csys->getSkyDome() : nullptr;
        BeginGreyed(dome == nullptr);
        {
            bool haze = (dome != nullptr) ? dome->getHazeEnabled() : false;
            if (ImGui::Checkbox(_LC("SkySettings", "Sky haze"), &haze) && dome != nullptr)
            {
                dome->setHazeEnabled(haze);
            }
        }
        EndGreyed(dome == nullptr);
    }

    // --- Caelum: sun light and stars ---
    if (ImGui::CollapsingHeader(_LC("SkySettings", "Sun light and stars")))
    {
        if (csys == nullptr)
        {
            ImGui::TextDisabled("%s", _LC("SkySettings", "(Caelum only)"));
        }

        Caelum::BaseSkyLight* sun = (csys != nullptr) ? csys->getSun() : nullptr;
        BeginGreyed(sun == nullptr);
        {
            Ogre::ColourValue diffuse = (sun != nullptr) ? sun->getDiffuseMultiplier() : Ogre::ColourValue(3.f, 3.f, 2.7f);
            float d[3] = { diffuse.r, diffuse.g, diffuse.b };
            if (ImGui::SliderFloat3(_LC("SkySettings", "Sun diffuse multiplier:"), d, 0.f, 10.f, "%.2f") && sun != nullptr)
            {
                sun->setDiffuseMultiplier(Ogre::ColourValue(d[0], d[1], d[2], diffuse.a));
            }

            Ogre::ColourValue specular = (sun != nullptr) ? sun->getSpecularMultiplier() : Ogre::ColourValue(5.f, 5.f, 5.f);
            float sp[3] = { specular.r, specular.g, specular.b };
            if (ImGui::SliderFloat3(_LC("SkySettings", "Sun specular multiplier:"), sp, 0.f, 10.f, "%.2f") && sun != nullptr)
            {
                sun->setSpecularMultiplier(Ogre::ColourValue(sp[0], sp[1], sp[2], specular.a));
            }

            Ogre::ColourValue ambient = (sun != nullptr) ? sun->getAmbientMultiplier() : Ogre::ColourValue(0.5f, 0.5f, 0.5f);
            float am[3] = { ambient.r, ambient.g, ambient.b };
            if (ImGui::SliderFloat3(_LC("SkySettings", "Sun ambient multiplier:"), am, 0.f, 3.f, "%.2f") && sun != nullptr)
            {
                sun->setAmbientMultiplier(Ogre::ColourValue(am[0], am[1], am[2], ambient.a));
            }
        }
        EndGreyed(sun == nullptr);

        BeginGreyed(csys == nullptr);
        {
            const Ogre::ColourValue min_amb = (csys != nullptr) ? csys->getMinimumAmbientLight() : Ogre::ColourValue(0.1f, 0.1f, 0.3f);
            float amb[3] = { min_amb.r, min_amb.g, min_amb.b };
            if (ImGui::ColorEdit3(_LC("SkySettings", "Minimum ambient light:"), amb) && csys != nullptr)
            {
                csys->setMinimumAmbientLight(Ogre::ColourValue(amb[0], amb[1], amb[2], min_amb.a));
            }
        }
        EndGreyed(csys == nullptr);

        Caelum::PointStarfield* stars = (csys != nullptr) ? csys->getPointStarfield() : nullptr;
        BeginGreyed(stars == nullptr);
        {
            float star_size = (stars != nullptr) ? stars->getMag0PixelSize() : 16.f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Star size (pixels):"), &star_size, 1.f, 32.f, "%.1f") && stars != nullptr)
            {
                stars->setMag0PixelSize(star_size);
            }

            float mag_scale = (stars != nullptr) ? stars->getMagnitudeScale() : 2.512f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Star magnitude scale:"), &mag_scale, 1.1f, 6.f, "%.2f") && stars != nullptr)
            {
                stars->setMagnitudeScale(mag_scale);
            }
        }
        EndGreyed(stars == nullptr);
    }
#endif // USE_CAELUM

    // --- SkyX: precipitation (ported from Caelum) ---
    if (ImGui::CollapsingHeader(_LC("SkySettings", "Precipitation")))
    {
        SX::PrecipitationController* prec = skyx_active ? skyx->getPrecipitationController() : nullptr;
        if (prec == nullptr)
        {
            ImGui::TextDisabled("%s", _LC("SkySettings", "(SkyX only)"));
        }

        BeginGreyed(prec == nullptr);
        {
            // _LC() returns the c_str() of a temporary string, so the texts must be copied before they are stored
            const std::string type_names_str[8] = {
                _LC("SkySettings", "Drizzle"), _LC("SkySettings", "Rain"), _LC("SkySettings", "Snow"),
                _LC("SkySettings", "Snow grains"), _LC("SkySettings", "Ice crystals"), _LC("SkySettings", "Ice pellets"),
                _LC("SkySettings", "Hail"), _LC("SkySettings", "Small hail") };
            const char* type_names[8];
            for (int i = 0; i < 8; ++i)
            {
                type_names[i] = type_names_str[i].c_str();
            }

            int type_idx = 1; // Rain
            if (prec != nullptr)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (prec->getTextureName() == SX::PrecipitationController::getPresetParams(static_cast<SX::PrecipitationType>(i)).Name)
                    {
                        type_idx = i;
                        break;
                    }
                }
            }
            if (ImGui::Combo(_LC("SkySettings", "Type:"), &type_idx, type_names, 8) && prec != nullptr)
            {
                prec->setPresetType(static_cast<SX::PrecipitationType>(type_idx));
            }

            float intensity = (prec != nullptr) ? prec->getIntensity() : 0.f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Intensity:"), &intensity, 0.f, 1.f, "%.2f") && prec != nullptr)
            {
                prec->setIntensity(intensity);
            }

            // 3D rain streaks around the camera + strength of the flat screen-space layer
            SkyXRainParticles* rain3d = (skyx_mgr != nullptr) ? skyx_mgr->GetRainParticles() : nullptr;
            if (rain3d != nullptr && !rain3d->IsAvailable())
            {
                ImGui::TextDisabled("%s", _LC("SkySettings", "(3D raindrops unavailable, see RoR.log)"));
            }
            else if (rain3d != nullptr)
            {
                bool rain3d_on = rain3d->IsEnabled();
                if (ImGui::Checkbox(_LC("SkySettings", "3D raindrops"), &rain3d_on))
                {
                    rain3d->SetEnabled(rain3d_on);
                }
                bool splashes_on = rain3d->AreSplashesEnabled();
                if (ImGui::Checkbox(_LC("SkySettings", "Raindrop splashes"), &splashes_on))
                {
                    rain3d->SetSplashesEnabled(splashes_on);
                }
                float density_pct = rain3d->GetDensity() * 100.f;
                if (ImGui::SliderFloat(_LC("SkySettings", "Raindrop density:"), &density_pct, 10.f, 300.f, "%.0f %%"))
                {
                    rain3d->SetDensity(density_pct / 100.f);
                }
            }
            if (prec != nullptr)
            {
                float screen_pct = prec->getScreenEffectScale() * 100.f;
                if (ImGui::SliderFloat(_LC("SkySettings", "Screen rain layer:"), &screen_pct, 0.f, 100.f, "%.0f %%"))
                {
                    prec->setScreenEffectScale(screen_pct / 100.f);
                }
            }

            float speed = (prec != nullptr) ? prec->getSpeed() : 0.85f;
            if (ImGui::SliderFloat(_LC("SkySettings", "Falling speed:"), &speed, 0.05f, 2.f, "%.2f") && prec != nullptr)
            {
                prec->setSpeed(speed);
            }

            const Ogre::ColourValue pc = (prec != nullptr) ? prec->getColour() : Ogre::ColourValue(0.7f, 0.7f, 0.7f);
            float pcol[3] = { pc.r, pc.g, pc.b };
            if (ImGui::ColorEdit3(_LC("SkySettings", "Drop color:"), pcol) && prec != nullptr)
            {
                prec->setColour(Ogre::ColourValue(pcol[0], pcol[1], pcol[2], pc.a));
            }
        }
        EndGreyed(prec == nullptr);
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
