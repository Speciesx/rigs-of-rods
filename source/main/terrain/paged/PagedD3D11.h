/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2024 Petr Ohlidal

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

#pragma once

// Helpers to run the PagedGeometry vertex shaders (written in D3D9-style HLSL) on Direct3D11:
//  * uniforms must be globals (not entry point parameters)
//  * the position output must be SV_Position, shader model must be 4.0
//  * there is no fixed function pipeline: a pixel shader is required (texture * colour + alpha test)

#include <Ogre.h>

namespace PagedD3D11
{

inline bool IsActive()
{
    Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
    return rs && rs->getName().find("Direct3D11") != Ogre::String::npos;
}

inline const char* VsTarget() { return IsActive() ? "vs_4_0" : "vs_1_1"; }

inline Ogre::String Trim(const Ogre::String& s)
{
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == Ogre::String::npos)
        return Ogre::String();
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

/// Converts the D3D9-style HLSL vertex program source (single entry function 'void name(...) {...}').
inline Ogre::String ConvertVertexSource(const Ogre::String& src)
{
    const size_t fn = src.find("void ");
    if (fn == Ogre::String::npos) return src;
    const size_t open = src.find('(', fn);
    if (open == Ogre::String::npos) return src;
    const size_t brace = src.find('{', open);
    if (brace == Ogre::String::npos) return src;
    const size_t close = src.rfind(')', brace);
    if (close == Ogre::String::npos || close < open) return src;

    const Ogre::String list = src.substr(open + 1, close - open - 1);
    Ogre::String globals, params;
    size_t pos = 0;
    while (pos <= list.size())
    {
        size_t comma = list.find(',', pos);
        if (comma == Ogre::String::npos) comma = list.size();
        Ogre::String p = Trim(list.substr(pos, comma - pos));
        pos = comma + 1;
        if (p.empty())
            continue;
        if (p.compare(0, 8, "uniform ") == 0)
        {
            globals += Trim(p.substr(8)) + ";\n";
        }
        else
        {
            const Ogre::String sem = "POSITION";
            if (p.compare(0, 4, "out ") == 0 && p.size() > sem.size()
                && p.compare(p.size() - sem.size(), sem.size(), sem) == 0)
            {
                p = p.substr(0, p.size() - sem.size()) + "SV_Position";
            }
            params += (params.empty() ? "" : ",\n") + p;
        }
    }

    Ogre::String body = src.substr(brace);
    // float array index: D3D11 wants an integer
    const Ogre::String idx = "[normal.z]";
    for (size_t at = body.find(idx); at != Ogre::String::npos; at = body.find(idx, at))
    {
        body.replace(at, idx.size(), "[(int)normal.z]");
        at += 5;
    }

    return src.substr(0, fn) + globals + src.substr(fn, open + 1 - fn) + params + ")\n" + body;
}

/// Builds a pixel shader whose inputs match the vertex shader outputs exactly (same order, same semantics).
/// D3D11 links VS and PS by register: a PS declaring its inputs in another order reads the wrong values.
inline Ogre::String BuildFragmentSource(const Ogre::String& vsSrc)
{
    const size_t fn = vsSrc.find("void ");
    const size_t open = (fn == Ogre::String::npos) ? fn : vsSrc.find('(', fn);
    const size_t brace = (open == Ogre::String::npos) ? open : vsSrc.find('{', open);
    const size_t close = (brace == Ogre::String::npos) ? brace : vsSrc.rfind(')', brace);
    if (close == Ogre::String::npos || close < open)
        return Ogre::String();

    const Ogre::String list = vsSrc.substr(open + 1, close - open - 1);
    Ogre::String params, colorName, uvName;
    size_t pos = 0;
    while (pos <= list.size())
    {
        size_t comma = list.find(',', pos);
        if (comma == Ogre::String::npos) comma = list.size();
        Ogre::String p = Trim(list.substr(pos, comma - pos));
        pos = comma + 1;
        if (p.compare(0, 4, "out ") != 0)
            continue;
        p = Trim(p.substr(4)); // "float4 oColor : COLOR"
        const size_t colon = p.find(':');
        if (colon == Ogre::String::npos)
            continue;
        const Ogre::String decl = Trim(p.substr(0, colon));      // "float4 oColor"
        const Ogre::String sem = Trim(p.substr(colon + 1));      // "COLOR"
        const Ogre::String name = Trim(decl.substr(decl.find_last_of(" \t") + 1));
        if (sem == "COLOR" || sem == "COLOR0") colorName = name;
        if (sem == "TEXCOORD0") uvName = name;
        params += (params.empty() ? "" : ",\n") + Ogre::String("\tin ") + decl + " : " + sem;
    }
    if (uvName.empty())
        return Ogre::String();

    return
        "Texture2D diffuseMap : register(t0); \n"
        "SamplerState diffuseSampler : register(s0); \n"
        "float alphaRef; \n"
        "float4 main(\n" + params + ") : SV_Target \n"
        "{ \n"
        "\tfloat4 c = diffuseMap.Sample(diffuseSampler, " + uvName + ".xy)" + (colorName.empty() ? Ogre::String() : " * " + colorName) + "; \n"
        "\tclip(c.a - alphaRef); \n"
        "\treturn c; \n"
        "}";
}

/// Sets a pixel shader on the pass (texture unit 0 * vertex colour, alpha test from the pass),
/// generated from the pass's vertex program so the VS/PS signatures match.
inline void ApplyFragmentProgram(Ogre::Pass* pass)
{
    using namespace Ogre;
    if (!pass->hasVertexProgram())
        return;
    const String psSource = BuildFragmentSource(pass->getVertexProgram()->getSource());
    if (psSource.empty())
        return;
    const String psName = "Paged_PS_d3d11_" + StringConverter::toString((uint32)FastHash(psSource.c_str(), psSource.size()));
    if (!HighLevelGpuProgramManager::getSingleton().getByName(psName))
    {
        HighLevelGpuProgramPtr ps = HighLevelGpuProgramManager::getSingleton().createProgram(
            psName, ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, "hlsl", GPT_FRAGMENT_PROGRAM);
        ps->setSource(psSource);
        ps->setParameter("target", "ps_4_0");
        ps->setParameter("entry_point", "main");
        ps->load();
    }
    pass->setFragmentProgram(psName);
    const float alphaRef = (pass->getAlphaRejectFunction() == CMPF_ALWAYS_PASS) ? 0.f : pass->getAlphaRejectValue() / 255.f;
    pass->getFragmentProgramParameters()->setNamedConstant("alphaRef", alphaRef);
}

<<<<<<< HEAD
/// D3D11: the default shadow caster has no alpha test, so alpha-tested foliage casts solid quads.
/// Gives the technique its own caster material: texture alpha + alpha test, nothing else (depth only).
inline void ApplyAlphaShadowCaster(Ogre::Technique* tech)
{
    using namespace Ogre;
    try
    {
    if (tech->getNumPasses() == 0)
        return;
    Pass* src = tech->getPass(0);
    if (src->getAlphaRejectFunction() == CMPF_ALWAYS_PASS || src->getNumTextureUnitStates() == 0)
        return;

    const String vsName = "Paged_ShadowCaster_VS_d3d11";
    const String psName = "Paged_ShadowCaster_PS_d3d11";
    HighLevelGpuProgramManager& hmgr = HighLevelGpuProgramManager::getSingleton();
    if (!hmgr.getByName(vsName))
    {
        HighLevelGpuProgramPtr vs = hmgr.createProgram(vsName, ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, "hlsl", GPT_VERTEX_PROGRAM);
        vs->setSource(
            "float4x4 worldViewProj; \n"
            "void main(float4 iPos : POSITION, float2 iUv : TEXCOORD0, \n"
            "\tout float4 oPos : SV_Position, out float2 oUv : TEXCOORD0) \n"
            "{ \n"
            "\toPos = mul(worldViewProj, iPos); \n"
            "\toUv = iUv; \n"
            "}");
        vs->setParameter("target", "vs_4_0");
        vs->setParameter("entry_point", "main");
        vs->load();

        HighLevelGpuProgramPtr ps = hmgr.createProgram(psName, ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, "hlsl", GPT_FRAGMENT_PROGRAM);
        ps->setSource(
            "Texture2D diffuseMap : register(t0); \n"
            "SamplerState diffuseSampler : register(s0); \n"
            "float alphaRef; \n"
            "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target \n"
            "{ \n"
            "\tclip(diffuseMap.Sample(diffuseSampler, uv).a - alphaRef); \n"
            "\treturn float4(0, 0, 0, 1); \n"
            "}");
        ps->setParameter("target", "ps_4_0");
        ps->setParameter("entry_point", "main");
        ps->load();
    }

    const String casterName = tech->getParent()->getName() + "/D3D11ShadowCaster";
    MaterialPtr caster = MaterialManager::getSingleton().getByName(casterName);
    const bool created = !caster;
    if (!caster)
    {
        caster = MaterialManager::getSingleton().create(casterName, tech->getParent()->getGroup());
        Pass* p = caster->getTechnique(0)->getPass(0);
        p->setLightingEnabled(false);
        p->setCullingMode(src->getCullingMode());
        p->setAlphaRejectSettings(src->getAlphaRejectFunction(), src->getAlphaRejectValue());
        p->createTextureUnitState(src->getTextureUnitState(0)->getTextureName());
        p->setVertexProgram(vsName);
        p->setFragmentProgram(psName);
        p->getVertexProgramParameters()->setNamedAutoConstant("worldViewProj", GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
        p->getFragmentProgramParameters()->setNamedConstant("alphaRef", src->getAlphaRejectValue() / 255.f);
    }
    // Must be loaded (compiled) - the shadow renderer uses its best technique directly, an unloaded material has none.
    caster->load();
    if (!caster->getBestTechnique())
    {
        LogManager::getSingleton().logMessage("[RoR|D3D11] alpha shadow caster '" + casterName + "' has no usable technique - skipped");
        return;
    }
    tech->setShadowCasterMaterial(caster);
    if (created)
        LogManager::getSingleton().logMessage("[RoR|D3D11] alpha shadow caster created: " + casterName);
    }
    catch (Ogre::Exception& e)
    {
        LogManager::getSingleton().logMessage("[RoR|D3D11] alpha shadow caster failed: " + e.getFullDescription());
    }
}

=======
>>>>>>> fbbc381a55766fed82ef5e8127451dde0c1a7e79
} // namespace PagedD3D11
