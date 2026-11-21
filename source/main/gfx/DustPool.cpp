/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer

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

#include <algorithm>
#include "DustPool.h"

#include <Ogre.h>

#include "Application.h"
#include "GameContext.h"
#include "GfxScene.h"
#include "Terrain.h"
#include "GfxWater.h"

using namespace Ogre;
using namespace RoR;

#ifndef _WIN32
// This definitions is needed because the variable is declared but not defined in DustPool
  const int DustPool::MAX_DUSTS;
#endif // !_WIN32

DustPool::DustPool(Ogre::SceneManager* sm, const char* dname, int dsize, const std::string& custom_name /* = "" */):
	allocated(0),
	size(std::min(dsize, static_cast<int>(MAX_DUSTS))),
	m_is_discarded(false)
{
    std::string poolname = fmt::format("DustPools/{}", custom_name.empty() ? dname : custom_name);
    parent_snode = sm->getRootSceneNode()->createChildSceneNode(poolname);
    for (int i = 0; i < MAX_DUSTS; i++)
        emitters[i] = nullptr;

    m_ps = sm->createParticleSystem(poolname, dname);
    if (!m_ps || m_ps->getNumEmitters() == 0)
    {
        size = 0;
        return;
    }
    parent_snode->attachObject(m_ps);
    m_ps->setCastShadows(false);
    m_ps->setVisibilityFlags(RoR::DEPTHMAP_DISABLED);
    // The emitters move all over the map: keep the bounds following the particles (OGRE stops after 10 s by default)
    m_ps->setBoundsAutoUpdated(true, 0.f);

    // The template's quota is meant for one emitter: scale it to the number of slots.
    m_ps->setParticleQuota(m_ps->getParticleQuota() * size);

    // Slot 0 uses the template's emitter, the other slots get copies of it.
    Ogre::ParticleEmitter* first = m_ps->getEmitter(0);
    emitters[0] = first;
    for (int i = 1; i < size; i++)
    {
        emitters[i] = m_ps->addEmitter(first->getType());
        first->copyParametersTo(emitters[i]);
    }
    for (int i = 0; i < size; i++)
        emitters[i]->setEnabled(false);
}

DustPool::~DustPool()
{
	ROR_ASSERT(m_is_discarded);
}

void DustPool::Discard(Ogre::SceneManager* sm)
{
    if (m_ps)
    {
        sm->destroyParticleSystem(m_ps); // also destroys the emitters
        m_ps = nullptr;
    }
    for (int i = 0; i < MAX_DUSTS; i++)
        emitters[i] = nullptr;
    sm->destroySceneNode(parent_snode);
	m_is_discarded = true;
}

void DustPool::setVisible(bool s)
{
    if (m_ps)
        m_ps->setVisible(s);
}

//Dust
void DustPool::malloc(Vector3 pos, Vector3 vel, ColourValue col)
{
    if (allocated < size)
    {
        positions[allocated] = pos;
        velocities[allocated] = vel;
        colours[allocated] = col;
        types[allocated] = DUST_NORMAL;
        allocated++;
    }
}

//Clumps
void DustPool::allocClump(Vector3 pos, Vector3 vel, ColourValue col)
{
    if (allocated < size)
    {
        positions[allocated] = pos;
        velocities[allocated] = vel;
        colours[allocated] = col;
        types[allocated] = DUST_CLUMP;
        allocated++;
    }
}

//Rubber smoke
void DustPool::allocSmoke(Vector3 pos, Vector3 vel)
{
    if (allocated < size)
    {
        positions[allocated] = pos;
        velocities[allocated] = vel;
        types[allocated] = DUST_RUBBER;
        allocated++;
    }
}

//
void DustPool::allocSparks(Vector3 pos, Vector3 vel)
{
    if (vel.length() < 0.1)
        return; // try to prevent emitting sparks while standing
    if (allocated < size)
    {
        positions[allocated] = pos;
        velocities[allocated] = vel;
        types[allocated] = DUST_SPARKS;
        allocated++;
    }
}

//Water vapour
void DustPool::allocVapour(Vector3 pos, Vector3 vel, float time)
{
    if (allocated < size)
    {
        positions[allocated] = pos;
        velocities[allocated] = vel;
        types[allocated] = DUST_VAPOUR;
        rates[allocated] = 5.0 - time;
        allocated++;
    }
}

void DustPool::allocDrip(Vector3 pos, Vector3 vel, float time)
{
    if (allocated < size)
    {
        positions[allocated] = pos;
        velocities[allocated] = vel;
        types[allocated] = DUST_DRIP;
        rates[allocated] = 5.0 - time;
        allocated++;
    }
}

void DustPool::allocSplash(Vector3 pos, Vector3 vel)
{
    if (allocated < size)
    {
        positions[allocated] = pos;
        velocities[allocated] = vel;
        types[allocated] = DUST_SPLASH;
        allocated++;
    }
}

void DustPool::allocRipple(Vector3 pos, Vector3 vel)
{
    if (allocated < size)
    {
        positions[allocated] = pos;
        velocities[allocated] = vel;
        types[allocated] = DUST_RIPPLE;
        allocated++;
    }
}

void DustPool::AdjustDustPoolSpeedFactor(GfxActor* gfx_actor /* = nullptr */)
{
    if (m_ps)
        App::GetGfxScene()->AdjustParticleSystemTimeFactor(m_ps, gfx_actor);
}

/// The 'DeflectorPlane' affector of the water particle scripts sits at world y=0. On maps with another water level (or with waves)
/// the particles used to fall through the water surface; this moves the plane to the current water surface.
void DustPool::SetDeflectorHeight(Ogre::ParticleSystem* ps, float height)
{
    for (unsigned short a = 0; a < ps->getNumAffectors(); a++)
    {
        Ogre::ParticleAffector* affector = ps->getAffector(a);
        if (affector->getType() == "DeflectorPlane")
        {
            affector->setParameter("plane_point", "0 " + Ogre::StringConverter::toString(height) + " 0");
        }
    }
}

// The particle materials are unlit ('lighting off'): without this, smoke/dust/spray glow white at night.
// Uses the SkyX ambient light (same as the 3D rain); maps without SkyX keep full brightness.
static float CalcParticleBrightness()
{
    Ogre::SceneManager* sm = App::GetGfxScene()->GetSceneManager();
    if (sm == nullptr || !sm->hasLight("Light1"))
        return 1.f;
    const Ogre::ColourValue c = sm->getLight("Light1")->getDiffuseColour();
    const float lum = std::min(std::max((c.r + c.g + c.b) / 3.f, 0.f), 1.f);
    return 0.12f + 0.88f * lum;
}

void DustPool::update()
{
    Wavefield* water = (RoR::App::GetGameContext()->GetTerrain() != nullptr) ? RoR::App::GetGameContext()->GetTerrain()->getWater() : nullptr;
    const float brightness = CalcParticleBrightness();

    // All slots share one deflector plane now: use the water surface at the first slot that needs it
    // (the water height differs only by the waves between the slots of one frame).
    bool deflector_set = false;
    auto set_deflector = [&](float height)
    {
        if (!deflector_set)
            SetDeflectorHeight(m_ps, height);
        deflector_set = true;
    };

    for (int i = 0; i < allocated; i++)
    {
        ParticleEmitter* emit = emitters[i];
        Vector3 ndir = velocities[i];
        Real vel = ndir.length();
        ColourValue col = colours[i];

        if (vel == 0)
            vel += 0.0001;
        ndir = ndir / vel;

        emit->setEnabled(true);

        if (types[i] != DUST_RIPPLE)
        {
            emit->setDirection(ndir);
            emit->setParticleVelocity(vel);
            emit->setPosition(positions[i]);
        }

        if (types[i] == DUST_NORMAL)
        {
            ndir.y = 0;
            ndir = ndir / 2.0;

            col.a = vel * 0.05;
            emit->setTimeToLive(vel * 0.05 / 0.1);
        }
        else if (types[i] == DUST_CLUMP)
        {
            ndir = ndir / 2.0;
            if (ndir.y < 0)
                ndir.y = -ndir.y;

            col.a = 1.0;
        }
        else if (types[i] == DUST_RUBBER)
        {
            ndir.y = 0;
            ndir = ndir / 4.0;

            col.a = sqrt(vel) * 0.1;
            col.b = 0.9;
            col.g = 0.9;
            col.r = 0.9;

            emit->setTimeToLive(vel * 0.025 / 0.1);
        }
        else if (types[i] == DUST_SPARKS)
        {
            //ugh
        }
        else if (types[i] == DUST_VAPOUR)
        {
            emit->setParticleVelocity(vel / 2.0);

            col.a = rates[i] * 0.03;
            col.b = 0.9;
            col.g = 0.9;
            col.r = 0.9;

            emit->setTimeToLive(rates[i] * 0.03 / 0.1);
        }
        else if (types[i] == DUST_DRIP)
        {
            emit->setEmissionRate(rates[i]);
            // Drips stop on the water surface (the deflector plane of the script sits at y=0 - they used to sink
            // to the sea floor and show up as a bright column under the boat).
            const float surface = (water != nullptr) ? water->CalcWavesHeight(positions[i]) : 0.f;
            set_deflector(surface);
            if (water != nullptr)
            {
                // The deflector plane alone doesn't hold them: it is shared by all slots and follows the waves,
                // so drops resting on it end up below it when it moves up, and sink. Let each drop die when it
                // reaches the surface instead - fall time from the drip point with the script's gravity (9.8).
                const float height = positions[i].y - surface;
                if (height <= 0.f)
                {
                    emit->setEnabled(false); // drip point is under water: no drops
                }
                else
                {
                    const float g = 9.8f;
                    const float vy = velocities[i].y; // drops start with the node velocity
                    emit->setTimeToLive((vy + std::sqrt(vy * vy + 2.f * g * height)) / g);
                }
            }
        }
        else if (types[i] == DUST_SPLASH)
        {
            // Particles born below the water surface are not caught by the deflector plane and sink: spawn them
            // on the surface and throw them upwards in the direction of movement.
            const float surface = (water != nullptr) ? water->CalcWavesHeight(positions[i]) : 0.f;
            if (positions[i].y < surface + 0.05f)
            {
                positions[i].y = surface + 0.05f;
                emit->setPosition(positions[i]);
            }
            // Spray: thrown up (and a bit along the movement), not shot ahead with the full boat speed
            ndir.y = 0.0;
            ndir = ndir * 0.5;
            ndir.y = 1.0;
            ndir.normalise();

            emit->setDirection(ndir);
            emit->setParticleVelocity(std::min<Real>(std::max<Real>(vel * 0.3, 1.0), 6.0));
            set_deflector(surface);

            col.a = sqrt(vel) * 0.04;
            col.b = 0.9;
            col.g = 0.9;
            col.r = 0.9;

            emit->setTimeToLive(std::min<Real>(vel * 0.025 / 0.1, 1.5));
        }
        else if (types[i] == DUST_RIPPLE)
        {
            positions[i].y = ((water != nullptr) ? water->CalcWavesHeight(positions[i]) : 0.f) - 0.02;
            emit->setPosition(positions[i]);
            set_deflector(positions[i].y);

            col.a = vel * 0.04;
            col.b = 0.9;
            col.g = 0.9;
            col.r = 0.9;

            emit->setTimeToLive(vel * 0.04 / 0.1);
        }

        if (types[i] != DUST_SPARKS) // sparks glow by themselves
        {
            col.r *= brightness;
            col.g *= brightness;
            col.b *= brightness;
        }
        emit->setColour(col);
    }
    for (int i = allocated; i < size; i++)
    {
        emitters[i]->setEnabled(false);
    }
    allocated = 0;
}
