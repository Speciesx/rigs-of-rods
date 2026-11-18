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

#include "SkyXRainParticles.h"

#include "Application.h"
#include "Collisions.h"
#include "GameContext.h"
#include "GfxActor.h"
#include "GfxScene.h"
#include "Terrain.h"

#include "gfx/skyx/VClouds/VClouds.h"

#include <OgreBillboard.h>
#include <OgreBillboardSet.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cmath>

using namespace RoR;

namespace {

const int   MAX_DROPS = 6000;
const int   DROPS_FULL = 2600;          // drops at intensity 1 and density 1
const int   MAX_SPLASHES = 400;
const float BOX_RADIUS = 30.f;          // half size of the rain area around the camera (m)
const float SPAWN_HEIGHT = 14.f;        // drops start this high above the camera ...
const float SPAWN_JITTER = 8.f;         // ... plus up to this much (spreads the drops over time)
const float FALL_SPEED = 22.f;          // m/s
const float MAX_WIND_DRIFT = 9.f;       // m/s of sideways drift at full wind
const float DROP_WIDTH = 0.045f;
const float DROP_LENGTH = 1.1f;
const float SPLASH_RADIUS = 22.f;       // splashes are only created this close to the camera
const int   SPLASHES_PER_FRAME = 40;

const char* const TEX_DROP   = "SkyXRainDropTex";
const char* const TEX_SPLASH = "SkyXRainSplashTex";
const char* const MAT_DROP   = "SkyXRainDropMat";
const char* const MAT_SPLASH = "SkyXRainSplashMat";

float Gauss(float x, float sigma) { return std::exp(-(x * x) / (2.f * sigma * sigma)); }

void EnsureTexture(const char* name, int w, int h, const std::vector<unsigned char>& rgba)
{
    Ogre::TextureManager& tm = Ogre::TextureManager::getSingleton();
    if (tm.resourceExists(name))
        return;
    Ogre::TexturePtr tex = tm.createManual(name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D, w, h, Ogre::MIP_UNLIMITED, Ogre::PF_BYTE_RGBA, Ogre::TU_AUTOMIPMAP);
    Ogre::PixelBox box(Ogre::Box(0, 0, w, h), Ogre::PF_BYTE_RGBA, const_cast<unsigned char*>(rgba.data()));
    tex->getBuffer()->blitFromMemory(box);
}

void EnsureMaterial(const char* name, const char* tex_name)
{
    Ogre::MaterialManager& mm = Ogre::MaterialManager::getSingleton();
    if (mm.resourceExists(name))
        return;
    Ogre::MaterialPtr mat = mm.create(name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    mat->setReceiveShadows(false);
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    // The drop/splash brightness (dark at night) is set as billboard (vertex) colour. Without tracking, the RTSS
    // (D3D11, GL3+, Vulkan) ignores the vertex colour of an unlit pass and the rain stays white.
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    pass->setDepthWriteEnabled(false);
    pass->setCullingMode(Ogre::CULL_NONE);
    Ogre::TextureUnitState* tus = pass->createTextureUnitState(tex_name);
    tus->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
}

/// Soft vertical streak, transparent towards both ends
void CreateDropTexture()
{
    const int W = 16, H = 64;
    std::vector<unsigned char> px(W * H * 4);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const float u = (x + 0.5f) / W * 2.f - 1.f;
            const float v = (y + 0.5f) / H;
            const float a = Gauss(u, 0.32f) * std::pow(std::sin(3.14159265f * v), 1.3f);
            unsigned char* p = &px[(y * W + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<unsigned char>(std::min(1.f, a) * 255.f);
        }
    }
    EnsureTexture(TEX_DROP, W, H, px);
}

/// Thin ripple ring with a few small droplets around it
void CreateSplashTexture()
{
    const int S = 64;
    std::vector<unsigned char> px(S * S * 4);
    for (int y = 0; y < S; ++y)
    {
        for (int x = 0; x < S; ++x)
        {
            const float u = (x + 0.5f) / S * 2.f - 1.f;
            const float v = (y + 0.5f) / S * 2.f - 1.f;
            const float r = std::sqrt(u * u + v * v);
            float a = Gauss(r - 0.5f, 0.07f) * 0.9f;
            for (int k = 0; k < 6; ++k)
            {
                const float ang = k * 1.0471976f + 0.4f;
                const float dx = u - std::cos(ang) * 0.8f;
                const float dy = v - std::sin(ang) * 0.8f;
                a += Gauss(std::sqrt(dx * dx + dy * dy), 0.05f) * 0.8f;
            }
            a += Gauss(r, 0.06f) * 0.5f;
            a *= std::min(1.f, std::max(0.f, (1.f - r) / 0.15f)); // fade out at the border
            unsigned char* p = &px[(y * S + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<unsigned char>(std::min(1.f, a) * 255.f);
        }
    }
    EnsureTexture(TEX_SPLASH, S, S, px);
}

} // anonymous namespace

SkyXRainParticles::SkyXRainParticles(SkyX::SkyX* skyx, Ogre::SceneManager* sm)
    : m_skyx(skyx), m_scene_mgr(sm)
{
    try
    {
        CreateDropTexture();
        CreateSplashTexture();
        EnsureMaterial(MAT_DROP, TEX_DROP);
        EnsureMaterial(MAT_SPLASH, TEX_SPLASH);

        m_node = m_scene_mgr->getRootSceneNode()->createChildSceneNode();

        const Ogre::AxisAlignedBox bounds(
            Ogre::Vector3(-BOX_RADIUS * 1.5f, -60.f, -BOX_RADIUS * 1.5f),
            Ogre::Vector3(BOX_RADIUS * 1.5f, 60.f, BOX_RADIUS * 1.5f));

        // Rain streaks: oriented along their direction of motion
        m_rain_set = m_scene_mgr->createBillboardSet("SkyXRainDrops", MAX_DROPS);
        m_rain_set->setMaterialName(MAT_DROP);
        m_rain_set->setDefaultDimensions(DROP_WIDTH, DROP_LENGTH);
        m_rain_set->setBillboardType(Ogre::BBT_ORIENTED_COMMON);
        m_rain_set->setCommonDirection(Ogre::Vector3(0.f, -1.f, 0.f));
        m_rain_set->setBillboardOrigin(Ogre::BBO_CENTER);
        m_rain_set->setAutoextend(false);
        m_rain_set->setCullIndividually(false);
        m_rain_set->setBounds(bounds, BOX_RADIUS * 2.f);
        m_rain_set->setCastShadows(false);
        m_rain_set->setVisibilityFlags(DEPTHMAP_DISABLED);
        m_node->attachObject(m_rain_set);

        // Splashes: flat quads lying on the surface
        m_splash_set = m_scene_mgr->createBillboardSet("SkyXRainSplashes", MAX_SPLASHES);
        m_splash_set->setMaterialName(MAT_SPLASH);
        m_splash_set->setDefaultDimensions(0.5f, 0.5f);
        m_splash_set->setBillboardType(Ogre::BBT_PERPENDICULAR_COMMON);
        m_splash_set->setCommonDirection(Ogre::Vector3::UNIT_Y);
        m_splash_set->setCommonUpVector(Ogre::Vector3::UNIT_X);
        m_splash_set->setBillboardOrigin(Ogre::BBO_CENTER);
        m_splash_set->setAutoextend(false);
        m_splash_set->setCullIndividually(false);
        m_splash_set->setBounds(bounds, BOX_RADIUS * 2.f);
        m_splash_set->setCastShadows(false);
        m_splash_set->setVisibilityFlags(DEPTHMAP_DISABLED);
        m_node->attachObject(m_splash_set);

        m_splashes.resize(MAX_SPLASHES);
        for (Splash& s : m_splashes)
        {
            s.bb = m_splash_set->createBillboard(Ogre::Vector3(0.f, -10000.f, 0.f), Ogre::ColourValue(1.f, 1.f, 1.f, 0.f));
        }

        m_node->setVisible(false);
    }
    catch (Ogre::Exception& e)
    {
        // The sky must keep working without the 3D rain
        LOG("[SkyX] 3D rain could not be created: " + e.getFullDescription());
        if (m_rain_set != nullptr)
        {
            m_scene_mgr->destroyBillboardSet(m_rain_set);
            m_rain_set = nullptr;
        }
        if (m_splash_set != nullptr)
        {
            m_scene_mgr->destroyBillboardSet(m_splash_set);
            m_splash_set = nullptr;
        }
        if (m_node != nullptr)
        {
            m_scene_mgr->destroySceneNode(m_node);
            m_node = nullptr;
        }
        m_splashes.clear();
    }
}

SkyXRainParticles::~SkyXRainParticles()
{
    if (m_node != nullptr)
    {
        m_node->detachAllObjects();
        m_scene_mgr->destroySceneNode(m_node);
    }
    if (m_rain_set != nullptr)
        m_scene_mgr->destroyBillboardSet(m_rain_set);
    if (m_splash_set != nullptr)
        m_scene_mgr->destroyBillboardSet(m_splash_set);
}

float SkyXRainParticles::SurfaceBelow(float x, float z, float top) const
{
    TerrainPtr terrain = App::GetGameContext()->GetTerrain();
    if (terrain == nullptr)
        return -10000.f;

    // Keep the queries inside the map
    const Ogre::Vector3 size = terrain->getMaxTerrainSize();
    const float cx = std::min(std::max(x, 0.f), size.x);
    const float cz = std::min(std::max(z, 0.f), size.z);

    float h = terrain->GetCollisions()->getSurfaceHeightBelow(cx, cz, top);
    if (m_has_water)
        h = std::max(h, m_water_height);
    return h;
}

bool SkyXRainParticles::InsideOccluder(float x, float y, float z) const
{
    for (const Ogre::AxisAlignedBox& bb : m_occluders)
    {
        const Ogre::Vector3& lo = bb.getMinimum();
        const Ogre::Vector3& hi = bb.getMaximum();
        if (x > lo.x && x < hi.x && z > lo.z && z < hi.z && y > lo.y && y < hi.y)
            return true;
    }
    return false;
}

float SkyXRainParticles::VehicleRoofHit(const Drop& d, float half_len) const
{
    const float head_y = d.pos.y - half_len;
    for (const Ogre::AxisAlignedBox& bb : m_occluders)
    {
        const Ogre::Vector3& lo = bb.getMinimum();
        const Ogre::Vector3& hi = bb.getMaximum();
        if (d.pos.x > lo.x && d.pos.x < hi.x && d.pos.z > lo.z && d.pos.z < hi.z && head_y <= hi.y && head_y > lo.y)
            return hi.y;
    }
    return -10000.f;
}

bool SkyXRainParticles::SpawnDrop(Drop& d, bool scatter)
{
    const float top = m_cam_pos.y + SPAWN_HEIGHT + SPAWN_JITTER;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const float x = m_box_center.x + Ogre::Math::RangeRandom(-BOX_RADIUS, BOX_RADIUS) - m_drift.x * 0.65f;
        const float z = m_box_center.z + Ogre::Math::RangeRandom(-BOX_RADIUS, BOX_RADIUS) - m_drift.z * 0.65f;
        const float kill = this->SurfaceBelow(x, z, top);
        if (kill > top - 1.5f)
            continue; // the surface is higher than the rain starts (building, steep hill): no rain here

        float y = scatter
            ? kill + 1.f + Ogre::Math::UnitRandom() * (top - kill - 1.f)
            : top - Ogre::Math::UnitRandom() * SPAWN_JITTER;
        if (y <= kill + 0.5f)
            y = top;

        if (this->InsideOccluder(x, y, z))
            continue; // inside a vehicle

        d.pos = Ogre::Vector3(x, y, z);
        d.kill_y = kill;
        d.speed_mul = 0.9f + 0.2f * Ogre::Math::UnitRandom();
        d.alive = true;
        return true;
    }
    d.alive = false;
    return false;
}

void SkyXRainParticles::AddSplash(const Drop& d)
{
    if (!m_splashes_enabled || m_splash_budget <= 0)
        return;
    const float dx = d.pos.x - m_cam_pos.x;
    const float dz = d.pos.z - m_cam_pos.z;
    if (dx * dx + dz * dz > SPLASH_RADIUS * SPLASH_RADIUS)
        return;
    if (Ogre::Math::UnitRandom() > 0.45f)
        return;

    m_splash_budget--;
    Splash& s = m_splashes[m_next_splash];
    m_next_splash = (m_next_splash + 1) % m_splashes.size(); // overwrites the oldest one if the pool is full

    const bool on_water = m_has_water && d.kill_y <= m_water_height + 0.02f;
    s.pos = Ogre::Vector3(d.pos.x, d.kill_y + 0.04f, d.pos.z);
    s.age = 0.f;
    s.life = Ogre::Math::RangeRandom(0.3f, 0.5f);
    s.max_size = on_water ? Ogre::Math::RangeRandom(0.5f, 0.9f) : Ogre::Math::RangeRandom(0.3f, 0.55f);
    s.active = true;
}

void SkyXRainParticles::SetActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    m_node->setVisible(active);
    if (active)
    {
        // Start with rain everywhere, not with a wall of drops falling from the top
        for (Drop& d : m_drops)
            this->SpawnDrop(d, /*scatter:*/true);
    }
    else
    {
        for (Splash& s : m_splashes)
        {
            s.active = false;
            s.bb->setColour(Ogre::ColourValue(1.f, 1.f, 1.f, 0.f));
        }
    }
}

void SkyXRainParticles::Update(float dt, const Ogre::Vector3& cam_pos, const Ogre::ColourValue& light, bool camera_underwater)
{
    dt = std::min(dt, 0.1f);

    // --- Is the camera sheltered? (also used to fade the screen-space rain, which is drawn over everything) ---
    {
        bool sheltered = camera_underwater;
        if (!sheltered)
        {
            for (GfxActor* gfx_actor : App::GetGfxScene()->GetGfxActors())
            {
                const ActorSB& sb = gfx_actor->GetSimDataBuffer();
                if (sb.simbuf_actor_state == ActorState::DISPOSED || sb.simbuf_actor_state == ActorState::NETWORKED_HIDDEN)
                    continue;
                const Ogre::AxisAlignedBox& bb = sb.simbuf_aabb;
                if (!bb.isNull() && !bb.isInfinite() && bb.contains(cam_pos))
                {
                    sheltered = true; // inside a vehicle
                    break;
                }
            }
        }
        if (!sheltered)
        {
            // roof above the camera?
            sheltered = this->SurfaceBelow(cam_pos.x, cam_pos.z, cam_pos.y + 40.f) > cam_pos.y + 0.5f;
        }
        const float target = sheltered ? 1.f : 0.f;
        const float step = (dt > 0.f ? dt : 0.016f) * 4.f;
        m_shelter = (m_shelter < target) ? std::min(m_shelter + step, target) : std::max(m_shelter - step, target);
    }

    if (m_rain_set == nullptr)
        return; // 3D rain unavailable, only the shelter above was needed

    // --- Which kind of precipitation? Streaks only for rain-like types, none for snow. ---
    SkyX::PrecipitationController* prec = m_skyx->getPrecipitationController();
    const std::string tex = prec->getTextureName();
    float type_factor = 1.f;
    if (tex.find("snow") != std::string::npos)
        type_factor = 0.f;
    else if (tex.find("drizzle") != std::string::npos)
        type_factor = 0.45f;
    else if (tex.find("ice") != std::string::npos)
        type_factor = 0.7f;

    const float intensity = std::min(std::max(prec->getIntensity(), 0.f), 1.f);
    const int target = std::min(MAX_DROPS, static_cast<int>(DROPS_FULL * m_density * intensity * type_factor));

    if (!m_enabled || target < 8 || camera_underwater)
    {
        this->SetActive(false);
        m_last_cam_pos = cam_pos;
        return;
    }

    // --- Camera state ---
    if (cam_pos.distance(m_last_cam_pos) > 100.f) // teleport
    {
        m_cam_vel = Ogre::Vector3::ZERO;
    }
    else if (dt <= 0.f)
    {
        m_cam_vel = Ogre::Vector3::ZERO; // paused
    }
    else
    {
        Ogre::Vector3 v = (cam_pos - m_last_cam_pos) / dt;
        if (v.length() > 60.f)
            v = v.normalisedCopy() * 60.f;
        m_cam_vel = m_cam_vel + (v - m_cam_vel) * std::min(1.f, dt * 4.f);
    }
    const bool teleported = cam_pos.distance(m_last_cam_pos) > 100.f;
    m_last_cam_pos = cam_pos;
    m_cam_pos = cam_pos;
    m_box_center = Ogre::Vector3(cam_pos.x + m_cam_vel.x * 0.35f, cam_pos.y, cam_pos.z + m_cam_vel.z * 0.35f);

    // --- Water level (static water) ---
    TerrainPtr terrain = App::GetGameContext()->GetTerrain();
    m_has_water = (terrain != nullptr) && (terrain->getWater() != nullptr);
    m_water_height = m_has_water ? terrain->getWaterHeight() : 0.f;

    // --- Wind slant (same wind as the screen-space precipitation) ---
    Ogre::Vector3 drift = Ogre::Vector3::ZERO;
    SkyX::VCloudsManager* vcm = m_skyx->getVCloudsManager();
    if (vcm != nullptr && vcm->isCreated() && vcm->getVClouds() != nullptr)
    {
        const Ogre::Vector3 wd = vcm->getVClouds()->getPrecipitationWindDirectionV3() * -1.f;
        const float strength = std::min(std::max(vcm->getWindSpeed() / 200.f, 0.f), 1.f);
        drift = Ogre::Vector3(wd.x, 0.f, wd.z) * (strength * MAX_WIND_DRIFT);
    }
    m_drift = drift;
    m_velocity = Ogre::Vector3(drift.x, -FALL_SPEED, drift.z);
    m_dir = m_velocity.normalisedCopy();

    // The streaks show the motion of the drops relative to the camera: when driving, the rain slants backwards
    const Ogre::Vector3 rel = m_velocity - m_cam_vel;
    const Ogre::Vector3 view_dir = rel.normalisedCopy();
    if (view_dir.distance(m_view_dir) > 0.004f)
    {
        m_view_dir = view_dir;
        m_rain_set->setCommonDirection(view_dir);
    }
    const float streak_scale = std::min(std::max(rel.length() / FALL_SPEED, 1.f), 2.2f);
    if (std::fabs(streak_scale - m_streak_scale) > 0.05f)
    {
        m_streak_scale = streak_scale;
        m_rain_set->setDefaultDimensions(DROP_WIDTH, DROP_LENGTH * streak_scale);
    }

    // --- Vehicles near the rain area shield the drops ---
    m_occluders.clear();
    for (GfxActor* gfx_actor : App::GetGfxScene()->GetGfxActors())
    {
        const ActorSB& sb = gfx_actor->GetSimDataBuffer();
        if (sb.simbuf_actor_state == ActorState::DISPOSED || sb.simbuf_actor_state == ActorState::NETWORKED_HIDDEN)
            continue;
        const Ogre::AxisAlignedBox& bb = sb.simbuf_aabb;
        if (bb.isNull() || bb.isInfinite())
            continue;
        const float reach = BOX_RADIUS * 1.2f;
        if (bb.getMaximum().x < m_box_center.x - reach || bb.getMinimum().x > m_box_center.x + reach
            || bb.getMaximum().z < m_box_center.z - reach || bb.getMinimum().z > m_box_center.z + reach)
            continue;
        m_occluders.push_back(bb);
    }

    // --- Colour: lit by the current light, so drops are dark at night ---
    const float lum = std::min(std::max((light.r + light.g + light.b) / 3.f, 0.f), 1.f);
    const float brightness = 0.12f + 0.88f * lum;
    const float alpha = 0.38f * std::min(1.f, 0.55f + intensity * 0.6f) * (type_factor < 1.f ? 0.8f : 1.f);
    const Ogre::ColourValue drop_col(0.82f * brightness, 0.86f * brightness, 0.92f * brightness, alpha);
    const bool colour_changed = std::fabs(drop_col.r - m_drop_colour.r) > 0.01f || std::fabs(drop_col.a - m_drop_colour.a) > 0.01f;
    if (colour_changed)
        m_drop_colour = drop_col;
    m_splash_colour = Ogre::ColourValue(0.85f * brightness, 0.9f * brightness, 0.95f * brightness, std::min(1.f, alpha * 1.6f));

    m_node->setPosition(cam_pos);
    this->SetActive(true);

    // --- Number of drops ---
    while (static_cast<int>(m_drops.size()) < target)
    {
        m_drops.emplace_back();
        Drop& d = m_drops.back();
        d.bb = m_rain_set->createBillboard(Ogre::Vector3::ZERO, m_drop_colour);
        if (d.bb == nullptr) // pool exhausted
        {
            m_drops.pop_back();
            break;
        }
        this->SpawnDrop(d, /*scatter:*/true);
    }
    while (static_cast<int>(m_drops.size()) > target)
    {
        m_rain_set->removeBillboard(m_drops.back().bb);
        m_drops.pop_back();
    }

    // --- Simulate the drops ---
    m_splash_budget = SPLASHES_PER_FRAME;
    const float half_len = DROP_LENGTH * 0.5f * (-m_dir.y); // vertical extent of the streak below its centre
    const float limit = BOX_RADIUS * 1.15f;
    const Ogre::Vector3 hidden(0.f, -10000.f, 0.f);

    for (Drop& d : m_drops)
    {
        if (colour_changed)
            d.bb->setColour(m_drop_colour);

        if (!d.alive)
        {
            if (!this->SpawnDrop(d, false))
            {
                d.bb->setPosition(hidden);
                continue;
            }
        }
        else
        {
            if (dt > 0.f)
                d.pos += m_velocity * (d.speed_mul * dt);

            if (teleported
                || std::fabs(d.pos.x - m_box_center.x) > limit
                || std::fabs(d.pos.z - m_box_center.z) > limit)
            {
                if (!this->SpawnDrop(d, /*scatter:*/true))
                {
                    d.bb->setPosition(hidden);
                    continue;
                }
            }
            else
            {
                const float roof = this->VehicleRoofHit(d, half_len);
                const bool hit_vehicle = (roof > -9999.f);
                if (hit_vehicle || d.pos.y - half_len <= d.kill_y)
                {
                    if (hit_vehicle)
                        d.kill_y = roof; // the drop landed on a vehicle
                    this->AddSplash(d);
                    if (!this->SpawnDrop(d, false))
                    {
                        d.bb->setPosition(hidden);
                        continue;
                    }
                }
            }
        }
        d.bb->setPosition(d.pos - cam_pos);
    }

    // --- Splashes ---
    for (Splash& s : m_splashes)
    {
        if (!s.active)
            continue;
        s.age += dt;
        if (s.age >= s.life)
        {
            s.active = false;
            s.bb->setColour(Ogre::ColourValue(1.f, 1.f, 1.f, 0.f));
            s.bb->setPosition(hidden);
            continue;
        }
        const float t = s.age / s.life;
        const float size = s.max_size * (0.25f + 0.75f * std::sqrt(t));
        Ogre::ColourValue c = m_splash_colour;
        c.a *= std::pow(1.f - t, 1.5f);
        s.bb->setPosition(s.pos - cam_pos);
        s.bb->setDimensions(size, size);
        s.bb->setColour(c);
    }
}
