/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2023 Petr Ohlidal

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
/// @brief Passes with a vertex program but no fragment program relied on the fixed-function pipeline for the
/// pixel stage (D3D9 had one). D3D11/GL3+/Vulkan have none, OGRE throws "RenderSystem does not support
/// FixedFunction ... has no Fragment Shader" and the terrain never finishes loading (WindmillMountain: the
/// ETTerrainMaterial 'Ambient' and colour-map passes). The RTSS does not touch passes that already have a program.
/// This generates the missing fragment program from the vertex program's outputs, emulating the default
/// fixed-function stage: vertex colour (or white) modulated with every texture unit.

#pragma once

#include "Application.h"
#include "D3D11ShaderFix.h"

#include <Ogre.h>
#include <string>
#include <vector>

namespace RoR {
namespace FfpFragmentFix {

struct Varying
{
    std::string type;
    std::string sem;  //!< upper case, e.g. "TEXCOORD0"
};

/// Splits a parameter list at top-level commas.
inline std::vector<std::string> SplitParams(const std::string& list)
{
    std::vector<std::string> params;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= list.size(); ++i)
    {
        if (i < list.size() && list[i] == '(') depth++;
        else if (i < list.size() && list[i] == ')') depth--;
        else if (i == list.size() || (list[i] == ',' && depth == 0))
        {
            params.push_back(list.substr(start, i - start));
            start = i + 1;
        }
    }
    return params;
}

/// First token of a declaration after the given qualifiers ("out float4 oPos : POSITION" -> "float4").
inline std::string DeclType(std::string decl)
{
    using namespace RoR::D3D11ShaderFix;
    decl = Trim(decl);
    for (const char* q : { "inout", "out", "in", "uniform", "const", "linear", "centroid", "nointerpolation", "noperspective" })
    {
        while (StartsWithWord(decl, q))
            decl = Trim(decl.substr(std::char_traits<char>::length(q)));
    }
    const size_t e = decl.find_first_of(" \t\r\n");
    return (e == std::string::npos) ? decl : decl.substr(0, e);
}

/// Collects the outputs (semantic + type) of the vertex program's entry function, in declaration order.
/// Handles `out` parameters and a returned struct. Returns false if the entry function was not found.
inline bool ParseVsOutputs(const std::string& src, const std::string& entry, std::vector<Varying>& out)
{
    using namespace RoR::D3D11ShaderFix;
    for (size_t at = FindWord(src, entry, 0); at != std::string::npos; at = FindWord(src, entry, at + 1))
    {
        const size_t open = src.find_first_not_of(" \t\r\n", at + entry.size());
        if (open == std::string::npos || src[open] != '(')
            continue;
        int depth = 0;
        size_t close = open;
        for (; close < src.size(); ++close)
        {
            if (src[close] == '(') depth++;
            else if (src[close] == ')') { if (--depth == 0) break; }
        }
        if (close >= src.size())
            return false;
        // a definition (not a call): ")" [":" SEM] "{"
        size_t k = src.find_first_not_of(" \t\r\n", close + 1);
        if (k != std::string::npos && src[k] == ':')
            k = src.find('{', k);
        if (k == std::string::npos || src[k] != '{')
            continue;

        for (const std::string& raw : SplitParams(src.substr(open + 1, close - open - 1)))
        {
            const std::string p = Clean(raw);
            if (!(StartsWithWord(p, "out") || StartsWithWord(p, "inout")))
                continue;
            const std::string sem = Semantic(p);
            if (!sem.empty())
                out.push_back({ DeclType(p), sem });
        }

        if (out.empty())
        {
            // `VS_OUTPUT main(...)`: return type is the word before the entry name
            size_t e = at;
            while (e > 0 && (src[e - 1] == ' ' || src[e - 1] == '\t' || src[e - 1] == '\r' || src[e - 1] == '\n')) e--;
            size_t b = e;
            while (b > 0 && IsIdentChar(src[b - 1])) b--;
            const std::string ret = src.substr(b, e - b);
            for (size_t s = FindWord(src, "struct", 0); s != std::string::npos; s = FindWord(src, "struct", s + 1))
            {
                const size_t n = src.find_first_not_of(" \t\r\n", s + 6);
                if (n == std::string::npos || src.compare(n, ret.size(), ret) != 0)
                    continue;
                const size_t ob = src.find('{', n);
                const size_t cb = (ob == std::string::npos) ? std::string::npos : src.find('}', ob);
                if (cb == std::string::npos)
                    break;
                const std::string body = src.substr(ob + 1, cb - ob - 1);
                size_t m = 0;
                while (m < body.size())
                {
                    size_t semi = body.find(';', m);
                    if (semi == std::string::npos) semi = body.size();
                    const std::string member = Clean(body.substr(m, semi - m));
                    const std::string sem = Semantic(member);
                    if (!sem.empty())
                        out.push_back({ DeclType(member), sem });
                    m = semi + 1;
                }
                break;
            }
        }
        return true;
    }
    return false;
}

inline int Components(const std::string& type)
{
    const char last = type.empty() ? '4' : type.back();
    return (last >= '1' && last <= '4') ? (last - '0') : 1;
}

/// D3D9-style HLSL pixel shader emulating the fixed-function stage for the given VS outputs.
inline std::string MakeFragmentSource(const std::vector<Varying>& vs_out, const Ogre::Pass* pass, std::string& signature)
{
    std::string globals, params, body;
    std::string color_var, uv_var[8];
    int uv_comp[8] = {};
    for (size_t i = 0; i < vs_out.size(); ++i)
    {
        const Varying& v = vs_out[i];
        const std::string name = "v" + std::to_string(i);
        params += std::string(params.empty() ? "" : ",\n") + "\t" + v.type + " " + name + " : " + v.sem;
        if ((v.sem == "COLOR" || v.sem == "COLOR0") && color_var.empty())
            color_var = name;
        if (v.sem.compare(0, 8, "TEXCOORD") == 0)
        {
            const int set = std::atoi(v.sem.c_str() + 8);
            if (set >= 0 && set < 8)
            {
                uv_var[set] = name;
                uv_comp[set] = Components(v.type);
            }
        }
    }

    body = "\tfloat4 c = " + (color_var.empty() ? std::string("float4(1, 1, 1, 1)") : "(float4)" + color_var) + ";\n";
    signature = color_var.empty() ? "w" : "c";
    int unit = 0;
    for (const Ogre::TextureUnitState* tus : pass->getTextureUnitStates())
    {
        if (unit >= 8)
            break;
        unsigned set = tus->getTextureCoordSet();
        if (set >= 8 || uv_var[set].empty())
            set = 0;
        if (uv_var[set].empty())
            break; // no coordinates at all - nothing to sample
        const bool cube = tus->getTextureType() == Ogre::TEX_TYPE_CUBE_MAP;
        const std::string s = "ror_ffp_tex" + std::to_string(unit);
        const std::string reg = " : register(s" + std::to_string(unit) + ")";
        const std::string& uv = uv_var[set];
        if (cube)
        {
            globals += "samplerCUBE " + s + reg + ";\n";
            body += "\tc *= texCUBE(" + s + ", " + (uv_comp[set] >= 3 ? uv + ".xyz" : "float3(" + uv + ", 0)") + ");\n";
        }
        else
        {
            globals += "sampler2D " + s + reg + ";\n";
            const std::string coord = (uv_comp[set] >= 2) ? uv + ".xy" : "float2(" + uv + ", 0)";
            body += "\tc *= tex2D(" + s + ", " + coord + ");\n";
        }
        signature += (cube ? "C" : "T") + std::to_string(set);
        unit++;
    }

    return globals + "\nfloat4 ror_ffp_fs(\n" + params + "\n) : COLOR\n{\n" + body + "\treturn c;\n}\n";
}

/// Adds a generated fragment program to every pass of the group's materials that has a vertex program
/// but no fragment program, if the render system has no fixed-function pipeline.
inline void FixGroup(const Ogre::String& group)
{
    Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
    if (!rs || !rs->getCapabilities() || rs->getCapabilities()->hasCapability(Ogre::RSC_FIXED_FUNCTION))
        return;
    if (!Ogre::HighLevelGpuProgramManager::getSingleton().isLanguageSupported("hlsl"))
        return; // TODO: GLSL variant for GL3+/Vulkan

    auto it = Ogre::MaterialManager::getSingleton().getResourceIterator();
    while (it.hasMoreElements())
    {
        Ogre::MaterialPtr mat = Ogre::static_pointer_cast<Ogre::Material>(it.getNext());
        if (!mat || mat->getGroup() != group)
            continue;
        for (Ogre::Technique* tech : mat->getTechniques())
        {
            for (Ogre::Pass* pass : tech->getPasses())
            {
                if (!pass->hasVertexProgram() || pass->hasFragmentProgram() || pass->hasGeometryProgram())
                    continue;

                const Ogre::GpuProgramPtr& vs = pass->getVertexProgram();
                if (!vs || vs->getLanguage() != "hlsl")
                {
                    RoR::LogFormat("[RoR|FfpFragmentFix] '%s': pass %d has a non-HLSL vertex program and no fragment program - not fixed",
                        mat->getName().c_str(), (int)pass->getIndex());
                    continue;
                }

                std::string entry_point = vs->getParameter("entry_point");
                if (entry_point.empty())
                    entry_point = "main";
                std::string vs_src = vs->getSource();
                if (vs_src.empty() && !vs->getSourceFile().empty())
                {
                    try
                    {
                        vs_src = Ogre::ResourceGroupManager::getSingleton().openResource(vs->getSourceFile(), vs->getGroup())->getAsString();
                    }
                    catch (Ogre::Exception&) {}
                }
                std::vector<Varying> outputs;
                if (!ParseVsOutputs(vs_src, entry_point, outputs) || outputs.empty())
                {
                    RoR::LogFormat("[RoR|FfpFragmentFix] '%s': could not read the outputs of vertex program '%s' - not fixed",
                        mat->getName().c_str(), vs->getName().c_str());
                    continue;
                }

                std::string signature;
                const std::string src = MakeFragmentSource(outputs, pass, signature);
                const std::string name = "RoR/FfpFS/" + vs->getName() + "/" + signature;

                Ogre::HighLevelGpuProgramManager& hgpm = Ogre::HighLevelGpuProgramManager::getSingleton();
                if (!Ogre::GpuProgramManager::getSingleton().getByName(name, group))
                {
                    Ogre::HighLevelGpuProgramPtr fs = hgpm.createProgram(name, group, "hlsl", Ogre::GPT_FRAGMENT_PROGRAM);
                    if (!fs) // name taken in another group (see ContentManager::resourceCollision())
                        continue;
                    fs->setSource(RoR::D3D11ShaderFix::FixSignatures(src));
                    fs->setParameter("entry_point", "ror_ffp_fs");
                    fs->setParameter("target", "ps_3_0");
                    fs->setParameter("enable_backwards_compatibility", "true");
                }
                pass->setFragmentProgram(name);
                RoR::LogFormat("[RoR|FfpFragmentFix] '%s' pass %d: no fragment program (fixed function) - using generated '%s'",
                    mat->getName().c_str(), (int)pass->getIndex(), name.c_str());
            }
        }
    }
}

} // namespace FfpFragmentFix
} // namespace RoR
