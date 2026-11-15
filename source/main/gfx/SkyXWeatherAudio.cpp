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

#include "SkyXWeatherAudio.h"

#include "Application.h"

#ifdef USE_OPENAL
#   include "Sound.h"
#   include "SoundManager.h"
#   include "SoundScriptManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace RoR;

#ifdef USE_OPENAL

namespace {

// OpenAL sources are never freed by the SoundManager, so the weather sounds are created only once per SoundManager.
struct WeatherSounds
{
    SoundManager* owner = nullptr;
    SoundPtr rain;
    SoundPtr wind;
    static const int NUM_THUNDER_VOICES = 6;  // voice i plays variant (i % 3), so thunders can overlap
    SoundPtr thunder[NUM_THUNDER_VOICES];
    bool rain_playing = false;
    bool wind_playing = false;
};

WeatherSounds g_sounds;

SoundManager* GetSM()
{
    SoundScriptManager* ssm = App::GetSoundScriptManager();
    return (ssm != nullptr) ? ssm->getSoundManager() : nullptr;
}

/// Creates the sounds on first use (or after the SoundManager was recreated). Returns false if there is no audio.
bool EnsureSounds()
{
    SoundManager* sm = GetSM();
    if (sm == nullptr)
        return false;
    if (g_sounds.owner == sm)
        return g_sounds.rain != nullptr;

    g_sounds = WeatherSounds();
    g_sounds.owner = sm;
    g_sounds.rain = sm->createSound("skyx_rain.wav");
    g_sounds.wind = sm->createSound("skyx_wind.wav");
    for (int i = 0; i < WeatherSounds::NUM_THUNDER_VOICES; ++i)
    {
        const char* files[3] = { "skyx_thunder1.wav", "skyx_thunder2.wav", "skyx_thunder3.wav" };
        g_sounds.thunder[i] = sm->createSound(files[i % 3]); // same file = shared buffer, separate source
    }
    // Ambient sounds are listener-relative: no Doppler shift, no obstruction filter, no panning lag when the camera moves
    for (SoundPtr* s : { &g_sounds.rain, &g_sounds.wind })
    {
        if (*s != nullptr)
            (*s)->setRelative(true);
    }
    for (int i = 0; i < WeatherSounds::NUM_THUNDER_VOICES; ++i)
    {
        if (g_sounds.thunder[i] != nullptr)
            g_sounds.thunder[i]->setRelative(true);
    }
    if (g_sounds.rain != nullptr)
    {
        g_sounds.rain->setLoop(true);
        g_sounds.rain->setGain(0.f);
    }
    if (g_sounds.wind != nullptr)
    {
        g_sounds.wind->setLoop(true);
        g_sounds.wind->setGain(0.f);
    }
    return g_sounds.rain != nullptr;
}

float Approach(float current, float target, float step)
{
    if (current < target) return std::min(current + step, target);
    return std::max(current - step, target);
}

void UpdateLoop(SoundPtr& snd, bool& playing, float gain)
{
    if (snd == nullptr)
        return;
    if (gain > 0.005f)
    {
        snd->setGain(gain);
        if (!playing)
        {
            snd->play();
            playing = true;
        }
    }
    else if (playing)
    {
        snd->setGain(0.f);
        snd->stop();
        playing = false;
    }
}

} // anonymous namespace

#endif // USE_OPENAL

SkyXWeatherAudio::SkyXWeatherAudio(SkyX::SkyX* skyx) : m_skyx(skyx)
{
}

SkyXWeatherAudio::~SkyXWeatherAudio()
{
    this->UnregisterListener();
    this->StopAll();
}

void SkyXWeatherAudio::RegisterListener()
{
    // The LightningManager forgets its listeners whenever the clouds are removed, so this is checked regularly.
    SkyX::VClouds::LightningManager* lm = m_skyx->getVCloudsManager()->getVClouds()->getLightningManager();
    const std::vector<SkyX::VClouds::LightningManager::Listener*>& ls = lm->getListeners();
    if (std::find(ls.begin(), ls.end(), this) == ls.end())
        lm->addListener(this);
}

void SkyXWeatherAudio::UnregisterListener()
{
    if (m_skyx == nullptr)
        return;
    m_skyx->getVCloudsManager()->getVClouds()->getLightningManager()->removeListener(this);
}

void SkyXWeatherAudio::StopAll()
{
#ifdef USE_OPENAL
    if (g_sounds.owner != nullptr)
    {
        if (g_sounds.rain != nullptr) { g_sounds.rain->setGain(0.f); g_sounds.rain->stop(); }
        if (g_sounds.wind != nullptr) { g_sounds.wind->setGain(0.f); g_sounds.wind->stop(); }
        for (int i = 0; i < WeatherSounds::NUM_THUNDER_VOICES; ++i)
            if (g_sounds.thunder[i] != nullptr) g_sounds.thunder[i]->stop();
        g_sounds.rain_playing = false;
        g_sounds.wind_playing = false;
    }
#endif
    m_pending.clear();
    m_rain_gain = 0.f;
    m_wind_gain = 0.f;
}

void SkyXWeatherAudio::lightningAdded(SkyX::VClouds::Lightning* l)
{
#ifdef USE_OPENAL
    if (!m_enabled || l == nullptr || !EnsureSounds())
        return;

    const Ogre::Vector3 listener = g_sounds.owner->GetListenerPosition();
    const Ogre::Vector3 bolt = l->getSceneNode()->_getDerivedPosition();
    Ogre::Vector3 dir = bolt - listener;
    const float dist = dir.length();
    if (dist > 0.001f)
        dir /= dist;
    else
        dir = Ogre::Vector3::UNIT_Y;

    PendingThunder t;
    t.delay = std::min(dist / 340.f, 8.f);               // speed of sound (capped, so far strikes are still heard)
    t.gain = Ogre::Math::Clamp(0.9f - dist / 14000.f, 0.15f, 0.6f);
    t.variant = (dist < 2500.f) ? 0 : ((dist < 6000.f) ? 1 : 2);
    if (dist < 2500.f && (rand() & 1))
        t.variant = 1; // some variety for close strikes
    t.direction = dir;
    if (m_pending.size() < 8)
        m_pending.push_back(t);
#endif
}

void SkyXWeatherAudio::Update(float dt)
{
#ifdef USE_OPENAL
    if (!EnsureSounds())
        return;

    if (!m_enabled)
    {
        this->StopAll();
        return;
    }

    this->RegisterListener();

    // Paused simulation (pause menu, END key): silence, like the vehicle sounds. Gains and pending thunders are kept.
    if (dt <= 0.f)
    {
        UpdateLoop(g_sounds.rain, g_sounds.rain_playing, 0.f);
        UpdateLoop(g_sounds.wind, g_sounds.wind_playing, 0.f);
        for (int i = 0; i < WeatherSounds::NUM_THUNDER_VOICES; ++i)
            if (g_sounds.thunder[i] != nullptr && g_sounds.thunder[i]->isPlaying())
                g_sounds.thunder[i]->stop();
        return;
    }

    const bool underwater = g_sounds.owner->ListenerIsUnderwater();
    const float muffle = underwater ? 0.12f : 1.f;

    // --- Target gains ---
    float rain_target = 0.f;
    SkyX::PrecipitationController* prec = m_skyx->getPrecipitationController();
    const float intensity = Ogre::Math::Clamp(prec->getIntensity(), 0.f, 1.f);
    if (intensity > 0.f)
    {
        const std::string tex = prec->getTextureName();
        float type_gain = 1.f;
        if (tex.find("snow") != std::string::npos)
            type_gain = 0.f; // snow falls silently
        else if (tex.find("ice") != std::string::npos)
            type_gain = 0.6f;
        else if (tex.find("drizzle") != std::string::npos)
            type_gain = 0.55f;
        rain_target = type_gain * std::sqrt(intensity) * 0.8f;
    }

    float wind_target = 0.f;
    SkyX::VCloudsManager* vcm = m_skyx->getVCloudsManager();
    if (vcm->isCreated())
    {
        const float speed = Ogre::Math::Clamp(vcm->getWindSpeed() / 200.f, 0.f, 1.f);
        const float humidity = vcm->getVClouds()->getWheater().x;
        wind_target = speed * (0.35f + 0.65f * humidity) * 0.7f;
    }
    if (intensity > 0.5f)
        wind_target = std::max(wind_target, 0.15f * intensity); // storms are never quiet

    // Smooth transitions (frozen while paused, dt == 0)
    m_rain_gain = Approach(m_rain_gain, rain_target * muffle, dt * 0.4f);
    m_wind_gain = Approach(m_wind_gain, wind_target * muffle, dt * 0.4f);
    UpdateLoop(g_sounds.rain, g_sounds.rain_playing, m_rain_gain);
    UpdateLoop(g_sounds.wind, g_sounds.wind_playing, m_wind_gain);

    // --- Thunder ---
    for (size_t i = 0; i < m_pending.size();)
    {
        m_pending[i].delay -= dt;
        if (m_pending[i].delay <= 0.f)
        {
            const PendingThunder& t = m_pending[i];
            // Pick a voice that is not playing yet (preferably of the wanted variant), so a new thunder never cuts off the previous one.
            // Only if all voices are busy, the preferred one is restarted.
            int voice = -1;
            for (int v = t.variant; v < WeatherSounds::NUM_THUNDER_VOICES && voice < 0; v += 3)
                if (g_sounds.thunder[v] != nullptr && !g_sounds.thunder[v]->isPlaying())
                    voice = v;
            for (int v = 0; v < WeatherSounds::NUM_THUNDER_VOICES && voice < 0; ++v)
                if (g_sounds.thunder[v] != nullptr && !g_sounds.thunder[v]->isPlaying())
                    voice = v;
            if (voice < 0)
                voice = t.variant;
            SoundPtr& snd = g_sounds.thunder[voice];
            if (snd != nullptr)
            {
                snd->stop();
                snd->setLoop(false);
                snd->setPitch(0.85f + 0.15f * (float(rand() % 100) / 100.f)); // lower and slower, a bit different each time
                snd->setGain(t.gain * muffle);
                snd->play();
            }
            m_pending.erase(m_pending.begin() + i);
        }
        else
        {
            ++i;
        }
    }
#endif
}
