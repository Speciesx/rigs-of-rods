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

#include "RTSSManager.h"

#include "Actor.h"
#include "CameraManager.h"
#include "GfxScene.h"
#include "AppContext.h"

#include <Ogre.h>
#include <Terrain/OgreTerrain.h>
#include <Overlay/OgreOverlayContainer.h>
#include <OgreMaterialManager.h>
#include <RTShaderSystem/OgreRTShaderSystem.h>

using namespace Ogre;
using namespace RoR;

RTSSManager::RTSSManager()
{
}

RTSSManager::~RTSSManager()
{
}

void RTSSManager::SetupRTSS()
{
    // Per-pixel lighting: the RTSS default is per-vertex, which lights whole (large) tree/leaf quads
    // as blocks when a spotlight (headlights) only touches some of their vertices.
    static bool perPixelAdded = false; // ShaderGenerator outlives terrains - add the template only once
    if (!perPixelAdded)
    {
        perPixelAdded = true;
        auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
        auto* renderState = shaderGen->getRenderState(Ogre::MSN_SHADERGEN);
        renderState->addTemplateSubRenderState(shaderGen->createSubRenderState(RTShader::SRS_PER_PIXEL_LIGHTING));
    }

    // RTSS PSSM3
    if(App::gfx_shadow_type->getEnum<GfxShadowType>() == GfxShadowType::PSSM)
    {
        // The shadow caster only writes depth - it needs no lighting. The default material has lighting on,
        // which makes RTSS add per-pixel lighting (reads NORMAL); D3D11 fails on meshes without normals (terrain).
        Ogre::MaterialPtr casterMat = Ogre::MaterialManager::getSingleton().getByName("Ogre/TextureShadowCaster");
        if (casterMat)
        {
            casterMat->setLightingEnabled(false);
        }

        // Quality presets (gfx_shadow_quality): shadow map size, number of PSSM splits, shadow distance, PCF filter
        struct ShadowPreset { unsigned short tex_size; size_t splits; float far_dist; const char* filter; };
        static const ShadowPreset PRESETS[] =
        {
            { 1024, 2, 150.f, "pcf4"  }, // Low
            { 1024, 3, 250.f, "pcf4"  }, // Medium
            { 2048, 3, 400.f, "pcf4"  }, // High (previous fixed setup: 2048, 3 splits)
            { 4096, 4, 600.f, "pcf16" }, // Ultra
        };
        const int quality = Ogre::Math::Clamp(App::gfx_shadow_quality->getInt(), 0, 3);
        const ShadowPreset& preset = PRESETS[quality];

        Ogre::SceneManager* sm = App::GetGfxScene()->GetSceneManager();
        sm->setShadowTechnique(SHADOWTYPE_TEXTURE_MODULATIVE_INTEGRATED);
        sm->setShadowFarDistance(preset.far_dist); // same as the last split - no casters beyond it
        sm->setShadowTextureCountPerLightType(Ogre::Light::LT_DIRECTIONAL, preset.splits);
        sm->setShadowTextureSettings(preset.tex_size, preset.splits, PF_DEPTH16); // 16 bit depth: half the memory/bandwidth of 32 bit
        sm->setShadowTextureSelfShadow(true);

        pssmSetup = new PSSMShadowCameraSetup();
        pssmSetup->calculateSplitPoints(preset.splits, 1, preset.far_dist);
        pssmSetup->setSplitPadding(App::GetCameraManager()->GetCamera()->getNearClipDistance());
        for (size_t i = 0; i < preset.splits; i++)
        {
            pssmSetup->setOptimalAdjustFactor(i, 2.f / (float)(1 << i)); // 2, 1, 0.5, 0.25
        }

        auto* mShaderGenerator = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
        auto* schemRenderState = mShaderGenerator->getRenderState(Ogre::MSN_SHADERGEN);

        sm->setShadowCameraSetup(ShadowCameraSetupPtr(pssmSetup));
        auto subRenderState = mShaderGenerator->createSubRenderState(RTShader::SRS_INTEGRATED_PSSM3);
        // The shader must know the splits - without them it samples only the first shadow map,
        // while the other splits are still rendered every frame (wasted) and distant shadows are missing.
        subRenderState->setParameter("split_points", pssmSetup->getSplitPoints());
        subRenderState->setParameter("filter", Ogre::String(preset.filter));
        schemRenderState->addTemplateSubRenderState(subRenderState);

        LOG(fmt::format("[RoR|Gfx] Shadows: quality {}, {}x{} x{} splits, distance {:.0f} m, {}",
            quality, preset.tex_size, preset.tex_size, preset.splits, preset.far_dist, preset.filter));
    }

    // PBR
    //auto subRenderStateNP = mShaderGenerator->createSubRenderState(Ogre::RTShader::SRS_NORMALMAP);
    //schemRenderState->addTemplateSubRenderState(subRenderStateNP);
    //subRenderStateNP->setParameter("texture", "Grass004_1K_NormalGL.jpg");

    //auto subRenderStateMR = mShaderGenerator->createSubRenderState(Ogre::RTShader::SRS_COOK_TORRANCE_LIGHTING);
    //schemRenderState->addTemplateSubRenderState(subRenderStateMR);
    //subRenderStateMR->setParameter("texture", "Grass004_1K_Roughness.jpg");
}

void RTSSManager::EnableRTSS(const MaterialPtr& mat)
{
    Ogre::RTShader::ShaderGenerator* mShaderGenerator = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    mShaderGenerator->createShaderBasedTechnique(*mat, Ogre::MaterialManager::DEFAULT_SCHEME_NAME, Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
}