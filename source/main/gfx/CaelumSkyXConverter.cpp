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

#include "CaelumSkyXConverter.h"

#include "Application.h"
#include "Console.h"
#include "SkyXManager.h"
#include "gfx/skyx/CaelumPort/Astronomy.h"

#include <Ogre.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <locale>
#include <map>
#include <sstream>
#include <vector>

using namespace RoR;

namespace {

// --------------------------------------------------------------------------------
// Minimal reader for OGRE-style scripts (the format Caelum uses)
// --------------------------------------------------------------------------------

const char* const TOK_NEWLINE = "\n";

struct ScriptNode
{
    std::vector<std::string> header;   //!< Property: name + values; Object: type, name, ':', parent
    std::vector<ScriptNode>  children; //!< Only objects have children
    bool                     is_object = false;
};

std::vector<std::string> Tokenize(std::string const& src)
{
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&]() { if (!cur.empty()) { tokens.push_back(cur); cur.clear(); } };

    for (size_t i = 0; i < src.size(); ++i)
    {
        const char c = src[i];
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') // line comment
        {
            flush();
            while (i < src.size() && src[i] != '\n') { ++i; }
            tokens.push_back(TOK_NEWLINE);
        }
        else if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') // block comment
        {
            flush();
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) { ++i; }
            ++i;
        }
        else if (c == '"') // quoted string
        {
            flush();
            ++i;
            while (i < src.size() && src[i] != '"' && src[i] != '\n') { cur += src[i++]; }
            tokens.push_back(cur);
            cur.clear();
        }
        else if (c == '{' || c == '}' || c == ':')
        {
            flush();
            tokens.push_back(std::string(1, c));
        }
        else if (c == '\n')
        {
            flush();
            tokens.push_back(TOK_NEWLINE);
        }
        else if (std::isspace(static_cast<unsigned char>(c)))
        {
            flush();
        }
        else
        {
            cur += c;
        }
    }
    flush();
    return tokens;
}

void ParseBlock(std::vector<std::string> const& tok, size_t& pos, std::vector<ScriptNode>& out)
{
    std::vector<std::string> statement;
    while (pos < tok.size())
    {
        const std::string& t = tok[pos++];
        if (t == "}")
        {
            if (!statement.empty()) { ScriptNode n; n.header = statement; out.push_back(n); }
            return;
        }
        else if (t == "{")
        {
            ScriptNode n;
            n.is_object = true;
            n.header = statement;
            statement.clear();
            ParseBlock(tok, pos, n.children);
            out.push_back(n);
        }
        else if (t == TOK_NEWLINE)
        {
            if (statement.empty())
                continue;
            // The opening brace of an object may be on the next line.
            size_t peek = pos;
            while (peek < tok.size() && tok[peek] == TOK_NEWLINE) { ++peek; }
            if (peek < tok.size() && tok[peek] == "{")
            {
                pos = peek; // Next iteration consumes '{' and uses `statement` as header.
                continue;
            }
            ScriptNode n;
            n.header = statement;
            out.push_back(n);
            statement.clear();
        }
        else
        {
            statement.push_back(t);
        }
    }
    if (!statement.empty()) { ScriptNode n; n.header = statement; out.push_back(n); }
}

double ParseDouble(std::string const& s, double fallback = 0.0)
{
    std::istringstream iss(s);
    iss.imbue(std::locale::classic()); // Scripts always use '.' as decimal separator
    double v = fallback;
    iss >> v;
    return iss.fail() ? fallback : v;
}

bool ParseBool(std::string const& s)
{
    std::string l = s;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return l == "true" || l == "yes" || l == "on" || l == "1";
}

/// Caelum accepts 'latitude 45' as well as 'latitude 45 6 17' (degrees minutes seconds).
float ParseDegMinSec(std::vector<std::string> const& h)
{
    if (h.size() < 2)
        return 0.f;
    const double deg = ParseDouble(h[1]);
    const double min = (h.size() > 2) ? ParseDouble(h[2]) : 0.0;
    const double sec = (h.size() > 3) ? ParseDouble(h[3]) : 0.0;
    const double abs_val = std::fabs(deg) + min / 60.0 + sec / 3600.0;
    return static_cast<float>((deg < 0.0 || h[1][0] == '-') ? -abs_val : abs_val);
}

struct SkySystemEntry
{
    const ScriptNode* node = nullptr;
    std::string       name;
    std::string       parent;
    bool              is_abstract = false;
};

/// Reads type/name/parent from an object header like `abstract caelum_sky_system Name : Parent`
bool ReadSkySystemHeader(ScriptNode const& node, SkySystemEntry& out)
{
    const auto& h = node.header;
    size_t i = 0;
    if (i < h.size() && h[i] == "abstract") { out.is_abstract = true; ++i; }
    if (i >= h.size() || h[i] != "caelum_sky_system")
        return false;
    ++i;
    if (i < h.size() && h[i] != ":") { out.name = h[i]; ++i; }
    if (i < h.size() && h[i] == ":") { ++i; }
    if (i < h.size()) { out.parent = h[i]; }
    out.node = &node;
    return true;
}

void ApplyCloudSystem(ScriptNode const& cloud_system, CaelumSkyDef& def)
{
    // A `cloud_system` replaces whatever the parent sky defined.
    def.has_clouds = true;
    def.cloud_coverage = 0.f;
    def.has_cloud_speed = false;
    def.cloud_speed = Ogre::Vector2::ZERO;

    for (ScriptNode const& layer: cloud_system.children)
    {
        if (!layer.is_object || layer.header.empty() || layer.header[0] != "cloud_layer")
            continue;

        float coverage = 0.3f; // Caelum default
        bool has_speed = false;
        Ogre::Vector2 speed = Ogre::Vector2::ZERO;
        for (ScriptNode const& prop: layer.children)
        {
            if (prop.header.empty())
                continue;
            if (prop.header[0] == "coverage" && prop.header.size() >= 2)
                coverage = static_cast<float>(ParseDouble(prop.header[1], coverage));
            else if (prop.header[0] == "cloud_speed" && prop.header.size() >= 3)
            {
                speed = Ogre::Vector2(static_cast<float>(ParseDouble(prop.header[1])), static_cast<float>(ParseDouble(prop.header[2])));
                has_speed = true;
            }
        }

        if (coverage >= def.cloud_coverage)
        {
            def.cloud_coverage = Ogre::Math::Clamp(coverage, 0.f, 1.f);
            if (has_speed)
            {
                def.has_cloud_speed = true;
                def.cloud_speed = speed;
            }
        }
    }
}

void ApplyPrecipitation(ScriptNode const& precipitation, CaelumSkyDef& def)
{
    def.has_precipitation = true;
    def.precipitation_intensity = 0.5f; // Caelum default
    for (ScriptNode const& prop: precipitation.children)
    {
        const auto& h = prop.header;
        if (h.size() < 2)
            continue;
        if (h[0] == "intensity")
        {
            def.precipitation_intensity = Ogre::Math::Clamp(static_cast<float>(ParseDouble(h[1])), 0.f, 1.f);
        }
        else if (h[0] == "texture")
        {
            def.precipitation_texture = h[1];
        }
        else if (h[0] == "precipitation_colour" && h.size() >= 4)
        {
            def.precipitation_colour = Ogre::ColourValue(
                static_cast<float>(ParseDouble(h[1])), static_cast<float>(ParseDouble(h[2])), static_cast<float>(ParseDouble(h[3])),
                (h.size() >= 5) ? static_cast<float>(ParseDouble(h[4])) : 1.f);
            def.has_precipitation_colour = true;
        }
        else if (h[0] == "falling_speed")
        {
            def.precipitation_speed = static_cast<float>(ParseDouble(h[1]));
            def.has_precipitation_speed = true;
        }
        else if (h[0] == "camera_speed_scale")
        {
            const float x = static_cast<float>(ParseDouble(h[1]));
            def.precipitation_camera_speed_scale = (h.size() >= 4)
                ? Ogre::Vector3(x, static_cast<float>(ParseDouble(h[2])), static_cast<float>(ParseDouble(h[3])))
                : Ogre::Vector3(x, x, x);
            def.has_precipitation_camera_speed_scale = true;
        }
        else if (h[0] == "auto_disable_threshold" || h[0] == "auto_disable_intensity")
        {
            def.precipitation_auto_disable = static_cast<float>(ParseDouble(h[1]));
            def.has_precipitation_auto_disable = true;
        }
    }
}

void ApplySkySystem(SkySystemEntry const& entry, std::map<std::string, SkySystemEntry> const& all, CaelumSkyDef& def, int depth)
{
    if (depth > 16) // Guard against inheritance loops
        return;

    if (!entry.parent.empty())
    {
        auto itor = all.find(entry.parent);
        if (itor != all.end())
            ApplySkySystem(itor->second, all, def, depth + 1);
        else
            LOG(fmt::format("[RoR|Sky] Caelum script: parent sky '{}' of '{}' not found, ignoring.", entry.parent, entry.name));
    }

    for (ScriptNode const& child: entry.node->children)
    {
        if (child.header.empty())
            continue;
        const std::string& key = child.header[0];

        if (child.is_object)
        {
            if (key == "cloud_system")
                ApplyCloudSystem(child, def);
            else if (key == "precipitation")
                ApplyPrecipitation(child, def);
            // Other components (sun, moon, sky_dome, starfield, depth_composer...) are handled by SkyX itself.
            continue;
        }

        if (key == "julian_day" && child.header.size() >= 2)
        {
            def.julian_day = ParseDouble(child.header[1], def.julian_day);
            def.has_julian_day = true;
        }
        else if (key == "time_scale" && child.header.size() >= 2)
        {
            def.time_scale = static_cast<float>(ParseDouble(child.header[1], def.time_scale));
            def.has_time_scale = true;
        }
        else if (key == "latitude")
        {
            def.latitude_deg = Ogre::Math::Clamp(ParseDegMinSec(child.header), -90.f, 90.f);
            def.has_latitude = true;
        }
        else if (key == "longitude")
        {
            def.longitude_deg = ParseDegMinSec(child.header);
            def.has_longitude = true;
        }
        else if (key == "manage_scene_fog" && child.header.size() >= 2)
        {
            def.manage_scene_fog = ParseBool(child.header[1]);
        }
        else if (key == "scene_fog_density_multiplier" && child.header.size() >= 2)
        {
            def.scene_fog_density_multiplier = static_cast<float>(ParseDouble(child.header[1]));
        }
    }
}

std::string StripExtension(std::string const& s)
{
    const size_t dot = s.rfind('.');
    return (dot == std::string::npos) ? s : s.substr(0, dot);
}

} // anonymous namespace

// --------------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------------

bool RoR::ParseCaelumSkyScript(std::string const& filename, CaelumSkyDef& out_def)
{
    std::string src;
    try
    {
        Ogre::DataStreamPtr stream = Ogre::ResourceGroupManager::getSingleton().openResource(
            filename, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        src = stream->getAsString();
    }
    catch (...)
    {
        LOG(fmt::format("[RoR|Sky] Caelum script '{}' not found, using default SkyX sky.", filename));
        return false;
    }

    std::vector<std::string> tokens = Tokenize(src);
    std::vector<ScriptNode> root;
    size_t pos = 0;
    while (pos < tokens.size())
    {
        ParseBlock(tokens, pos, root); // Returns early on stray '}' - just continue
    }

    std::map<std::string, SkySystemEntry> systems;
    std::vector<std::string> order;
    for (ScriptNode const& node: root)
    {
        if (!node.is_object)
            continue;
        SkySystemEntry entry;
        if (ReadSkySystemHeader(node, entry) && !entry.name.empty())
        {
            if (systems.find(entry.name) == systems.end())
                order.push_back(entry.name);
            systems[entry.name] = entry;
        }
    }

    // Pick the sky: same name as the file (with or without extension), otherwise first concrete one.
    const SkySystemEntry* chosen = nullptr;
    for (const std::string& candidate: { filename, StripExtension(filename) })
    {
        auto itor = systems.find(candidate);
        if (itor != systems.end() && !itor->second.is_abstract)
        {
            chosen = &itor->second;
            break;
        }
    }
    if (!chosen)
    {
        for (const std::string& name: order)
        {
            if (!systems[name].is_abstract)
            {
                chosen = &systems[name];
                break;
            }
        }
    }
    if (!chosen)
    {
        LOG(fmt::format("[RoR|Sky] Caelum script '{}' contains no usable 'caelum_sky_system', using default SkyX sky.", filename));
        return false;
    }

    out_def = CaelumSkyDef();
    out_def.name = chosen->name;
    ApplySkySystem(*chosen, systems, out_def, 0);

    LOG(fmt::format("[RoR|Sky] Converted Caelum sky '{}' from '{}' to SkyX (julian day: {}, lat/lon: {:.2f}/{:.2f}, clouds: {:.2f}, precipitation: {:.2f})",
        out_def.name, filename, out_def.has_julian_day ? std::to_string(out_def.julian_day) : "default",
        out_def.latitude_deg, out_def.longitude_deg, out_def.has_clouds ? out_def.cloud_coverage : 0.f, out_def.precipitation_intensity));
    return true;
}

void RoR::CaelumJulianDayToDayOfYearAndHour(double julian_day, float longitude_deg, int& out_day_of_year, float& out_hour)
{
    // Julian days start at noon UTC. SkyX' astronomy works in solar time at longitude 0,
    // so the observer's local solar time is passed (Caelum longitude is east positive).
    double jd_local = julian_day + longitude_deg / 360.0;
    const double day_frac = (jd_local + 0.5) - std::floor(jd_local + 0.5);
    out_hour = static_cast<float>(day_frac * 24.0);

    int year = 0, month = 0, day = 0;
    CaelumPort::Astronomy::getGregorianDateFromJulianDay(jd_local, year, month, day);
    const int jd_jan1 = CaelumPort::Astronomy::getJulianDayFromGregorianDate(year, 1, 1);
    const int jd_today = CaelumPort::Astronomy::getJulianDayFromGregorianDate(year, month, day);
    out_day_of_year = std::max(1, std::min(365, jd_today - jd_jan1 + 1));
}

void RoR::ApplyCaelumSkyDefToSkyX(CaelumSkyDef const& def, SkyXManager* skyx_mgr)
{
    if (!skyx_mgr || !skyx_mgr->GetSkyX() || !skyx_mgr->GetBasicController())
        return;

    SkyX::SkyX* skyx = skyx_mgr->GetSkyX();
    SkyX::BasicController* controller = skyx_mgr->GetBasicController();

    // --- Date, time and observer position -> astronomical sun & moon (moon phase and sunrise/sunset follow) ---
    if (def.has_julian_day || def.has_latitude || def.has_longitude)
    {
        int day_of_year = 150;
        float hour = 12.f;
        CaelumJulianDayToDayOfYearAndHour(def.julian_day, def.longitude_deg, day_of_year, hour);
        controller->setLatitude(def.latitude_deg);
        controller->setDayOfYear(day_of_year);
        controller->setAstronomical(true);
        Ogre::Vector3 t = controller->getTime();
        t.x = hour;
        controller->setTime(t);
    }

    // --- Speed of time: Caelum = sim seconds per real second, SkyX = sim hours per real second ---
    if (def.has_time_scale)
    {
        skyx_mgr->SetBaseTimeMultiplier(def.time_scale / 3600.f);
    }

    // --- Precipitation (SkyX port of Caelum's precipitation, same textures) ---
    SkyX::PrecipitationController* prec = skyx->getPrecipitationController();
    if (prec)
    {
        if (def.has_precipitation)
        {
            // Caelum's default precipitation type is drizzle
            prec->setTextureName(def.precipitation_texture.empty() ? std::string("precipitation_drizzle.png") : def.precipitation_texture);
            if (def.has_precipitation_colour)
                prec->setColour(def.precipitation_colour);
            if (def.has_precipitation_speed)
                prec->setSpeed(def.precipitation_speed);
            if (def.has_precipitation_camera_speed_scale)
                prec->setCameraSpeedScale(def.precipitation_camera_speed_scale);
            if (def.has_precipitation_auto_disable)
                prec->setAutoDisableThreshold(def.precipitation_auto_disable);
            prec->setIntensity(def.precipitation_intensity);
        }
        else
        {
            prec->setIntensity(0.f); // Caelum sky without precipitation = dry
        }
    }

    // --- Clouds + weather ---
    SkyX::VCloudsManager* vclouds_mgr = skyx->getVCloudsManager();
    if (vclouds_mgr && vclouds_mgr->isCreated() && vclouds_mgr->getVClouds())
    {
        SkyX::VClouds::VClouds* vclouds = vclouds_mgr->getVClouds();

        // Caelum without `cloud_system` = clear sky.
        float humidity = def.has_clouds ? def.cloud_coverage : 0.f;
        float clouds_size = Ogre::Math::Clamp(0.35f + 0.5f * humidity, 0.1f, 0.9f);

        std::string tex = def.precipitation_texture;
        std::transform(tex.begin(), tex.end(), tex.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        const bool wet = tex.empty() // drizzle
            || tex.find("rain") != std::string::npos || tex.find("drizzle") != std::string::npos
            || tex.find("hail") != std::string::npos;

        if (def.has_precipitation && def.precipitation_intensity > 0.f)
        {
            // Precipitation needs an overcast sky
            humidity = std::max(humidity, 0.55f + 0.45f * def.precipitation_intensity);
            clouds_size = std::max(clouds_size, 0.7f);
        }

        vclouds->setWheater(Ogre::Math::Clamp(humidity, 0.f, 1.f), clouds_size, /*DelayedResponse:*/false);

        if (def.has_cloud_speed && def.cloud_speed.squaredLength() > 0.f)
        {
            // Caelum's default layer speed is (0.000005, -0.000009) UV units/s, which roughly matches SkyX' default wind of 80.
            const float CAELUM_DEFAULT_SPEED = 1.03e-5f;
            const float SKYX_DEFAULT_WIND = 80.f;
            const float wind = Ogre::Math::Clamp(def.cloud_speed.length() / CAELUM_DEFAULT_SPEED * SKYX_DEFAULT_WIND, 5.f, 400.f);
            vclouds_mgr->setWindSpeed(wind);
            vclouds->setWindDirection(Ogre::Math::ATan2(def.cloud_speed.y, def.cloud_speed.x));
        }

        // Thunderstorm for strong rain/hail (lightning + thunder sounds of SkyX)
        if (vclouds->getLightningManager())
        {
            const bool storm = def.has_precipitation && wet && def.precipitation_intensity >= 0.5f;
            vclouds->getLightningManager()->setEnabled(storm);
        }
    }
    else
    {
        LOG("[RoR|Sky] SkyX volumetric clouds are not enabled, Caelum clouds are ignored.");
    }

    // "Reset SkyX to map values" in the sky settings must restore the converted sky, not SkyXDefault.skx
    skyx_mgr->CommitMapDefaults();
}
