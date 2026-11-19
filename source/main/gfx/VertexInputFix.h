/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2026 Petr Ohlidal

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
/// D3D11: every input a vertex shader reads must exist in the vertex declaration, otherwise the draw call
/// throws ("Vertex Shader X consumes semantic COLOR0 but no such VertexElement provided"). D3D9 silently reads
/// zeros there, and old content relies on it - e.g. NiceMetal (reads the damage blend from the vertex colour) on
/// flexbodies/props without vertex colours, alpha-tested shadow casters on lines without texture coordinates.
///
/// Fix: right before each draw, add a constant vertex stream for every input the shader reads but the geometry
/// lacks. Colours/texcoords are zero (= "no damage blend", what D3D9 delivered), normals point up.
/// The inputs of a program are parsed once from its HLSL source and cached.

#pragma once

#include "D3D11ShaderFix.h"

#include <Ogre.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace RoR {

class VertexInputFix : public Ogre::RenderObjectListener
{
public:
    static void Install(Ogre::SceneManager* scene_manager)
    {
        Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
        if (!rs || rs->getName().find("Direct3D11") == Ogre::String::npos)
            return; // D3D9/GL tolerate missing vertex inputs
        static VertexInputFix s_instance;
        scene_manager->addRenderObjectListener(&s_instance);
    }

    void notifyRenderSingleObject(Ogre::Renderable* rend, const Ogre::Pass* pass, const Ogre::AutoParamDataSource* source,
        const Ogre::LightList* pLightList, bool suppressRenderStateChanges) override
    {
        if (!pass->hasVertexProgram())
            return;
        const std::vector<Input>& inputs = this->GetInputs(pass->getVertexProgram().get());
        if (inputs.empty())
            return;

        Ogre::RenderOperation op;
        rend->getRenderOperation(op);
        Ogre::VertexData* vd = op.vertexData;
        if (!vd || !vd->vertexDeclaration || !vd->vertexBufferBinding)
            return;
        for (const Input& in : inputs)
        {
            if (vd->vertexDeclaration->findElementBySemantic(in.semantic, in.index) == nullptr)
                AddConstantStream(vd, in, pass->getVertexProgram()->getName());
        }
    }

private:
    struct Input
    {
        Ogre::VertexElementSemantic semantic;
        unsigned short index;
    };

    static bool ToInput(const std::string& sem_in, Input& out)
    {
        std::string sem = sem_in;
        size_t d = sem.size();
        while (d > 0 && sem[d - 1] >= '0' && sem[d - 1] <= '9') d--;
        const unsigned short idx = (d < sem.size()) ? (unsigned short)std::atoi(sem.c_str() + d) : 0;
        sem = sem.substr(0, d);
        if (sem == "COLOR")
        {
            out.semantic = (idx == 0) ? Ogre::VES_DIFFUSE : Ogre::VES_SPECULAR;
            out.index = 0;
            return idx <= 1;
        }
        out.index = idx;
        if (sem == "TEXCOORD")          out.semantic = Ogre::VES_TEXTURE_COORDINATES;
        else if (sem == "NORMAL")       out.semantic = Ogre::VES_NORMAL;
        else if (sem == "TANGENT")      out.semantic = Ogre::VES_TANGENT;
        else if (sem == "BINORMAL")     out.semantic = Ogre::VES_BINORMAL;
        else if (sem == "BLENDWEIGHT")  out.semantic = Ogre::VES_BLEND_WEIGHTS;
        else if (sem == "BLENDINDICES") out.semantic = Ogre::VES_BLEND_INDICES;
        else return false; // POSITION and system values are never added
        return true;
    }

    /// Splits a parameter list / struct body at top-level separators.
    static std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> parts;
        int depth = 0;
        size_t start = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') depth--;
            else if (s[i] == sep && depth == 0) { parts.push_back(s.substr(start, i - start)); start = i + 1; }
        }
        parts.push_back(s.substr(start));
        return parts;
    }

    static void ParseDecl(const std::string& src, const std::string& decl_raw, std::vector<Input>& inputs, int recursion)
    {
        using namespace D3D11ShaderFix;
        const std::string decl = Clean(decl_raw);
        if (decl.empty() || StartsWithWord(decl, "out") || StartsWithWord(decl, "uniform") || decl.find("uniform ") != std::string::npos)
            return;
        std::string sem;
        if (decl.compare(0, 3, "IN(") == 0) // OgreUnifiedShader.h: IN(vec4 vertex, POSITION)
        {
            const size_t c = decl.rfind(',');
            const size_t e = decl.rfind(')');
            if (c != std::string::npos && e != std::string::npos && e > c)
                sem = Upper(Trim(decl.substr(c + 1, e - c - 1)));
        }
        else
        {
            sem = Semantic(decl);
        }

        if (!sem.empty())
        {
            Input in;
            if (ToInput(sem, in))
                inputs.push_back(in);
            return;
        }

        // no semantic: maybe a struct parameter ("in VS_INPUT IN" / "VS_INPUT IN")
        if (recursion > 0)
            return;
        std::string d = decl;
        if (StartsWithWord(d, "in"))
            d = Trim(d.substr(2));
        const size_t sp = d.find_first_of(" \t\r\n");
        if (sp == std::string::npos)
            return;
        const std::string type = d.substr(0, sp);
        for (size_t p = FindWord(src, "struct", 0); p != std::string::npos; p = FindWord(src, "struct", p + 1))
        {
            const size_t n = src.find_first_not_of(" \t\r\n", p + 6);
            if (n == std::string::npos || src.compare(n, type.size(), type) != 0 || IsIdentChar(src[n + type.size()]))
                continue;
            const size_t b = src.find('{', n);
            const size_t e = (b == std::string::npos) ? b : src.find('}', b);
            if (e == std::string::npos)
                return;
            for (const std::string& member : Split(src.substr(b + 1, e - b - 1), ';'))
                ParseDecl(src, member, inputs, recursion + 1);
            return;
        }
    }

    const std::vector<Input>& GetInputs(Ogre::GpuProgram* prog)
    {
        auto found = m_inputs.find(prog);
        if (found != m_inputs.end())
            return found->second;

        std::vector<Input>& inputs = m_inputs[prog];
        if (prog->getLanguage() != "hlsl")
            return inputs;
        const std::string& src = prog->getSource();
        const Ogre::String entry = prog->getParameter("entry_point");
        if (src.empty() || entry.empty())
            return inputs;

        using namespace D3D11ShaderFix;
        for (size_t p = FindWord(src, entry, 0); p != std::string::npos; p = FindWord(src, entry, p + 1))
        {
            const size_t open = src.find_first_not_of(" \t\r\n", p + entry.size());
            if (open == std::string::npos || src[open] != '(')
                continue;
            int depth = 0;
            size_t close = open;
            for (; close < src.size(); ++close)
            {
                if (src[close] == '(') depth++;
                else if (src[close] == ')' && --depth == 0) break;
            }
            if (close >= src.size())
                break;
            // definition, not a call: '{' follows (optionally after ": SEMANTIC")
            size_t k = src.find_first_not_of(" \t\r\n", close + 1);
            if (k != std::string::npos && src[k] == ':')
                k = src.find('{', k);
            if (k == std::string::npos || src[k] != '{')
                continue;
            for (const std::string& param : Split(src.substr(open + 1, close - open - 1), ','))
                ParseDecl(src, param, inputs, 0);
            break;
        }
        return inputs;
    }

    static const char* SemanticName(Ogre::VertexElementSemantic s)
    {
        switch (s)
        {
        case Ogre::VES_DIFFUSE:             return "COLOR";
        case Ogre::VES_SPECULAR:            return "COLOR1/";
        case Ogre::VES_TEXTURE_COORDINATES: return "TEXCOORD";
        case Ogre::VES_NORMAL:              return "NORMAL";
        case Ogre::VES_TANGENT:             return "TANGENT";
        case Ogre::VES_BINORMAL:            return "BINORMAL";
        case Ogre::VES_BLEND_WEIGHTS:       return "BLENDWEIGHT";
        case Ogre::VES_BLEND_INDICES:       return "BLENDINDICES";
        default:                            return "?";
        }
    }

    static void AddConstantStream(Ogre::VertexData* vd, const Input& in, const Ogre::String& prog_name)
    {
        size_t num_verts = vd->vertexStart + vd->vertexCount;
        for (const auto& binding : vd->vertexBufferBinding->getBindings())
            num_verts = std::max<size_t>(num_verts, (size_t)binding.second->getNumVertices());
        if (num_verts == 0)
            return;

        Ogre::VertexElementType type = Ogre::VET_FLOAT4;
        float value[4] = { 0.f, 0.f, 0.f, 0.f };
        if (in.semantic == Ogre::VES_DIFFUSE || in.semantic == Ogre::VES_SPECULAR || in.semantic == Ogre::VES_BLEND_INDICES)
            type = Ogre::VET_UBYTE4_NORM;
        else if (in.semantic == Ogre::VES_NORMAL)
            value[1] = 1.f; // up
        else if (in.semantic == Ogre::VES_TANGENT)
            value[0] = value[3] = 1.f;
        else if (in.semantic == Ogre::VES_BINORMAL)
            value[2] = 1.f;
        else if (in.semantic == Ogre::VES_BLEND_WEIGHTS)
            value[0] = 1.f;

        const size_t vsize = Ogre::VertexElement::getTypeSize(type);
        std::vector<unsigned char> data(num_verts * vsize, 0);
        if (type == Ogre::VET_FLOAT4)
        {
            for (size_t i = 0; i < num_verts; ++i)
                memcpy(&data[i * vsize], value, vsize);
        }
        Ogre::HardwareVertexBufferSharedPtr buf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            vsize, num_verts, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        buf->writeData(0, buf->getSizeInBytes(), data.data(), true);

        const unsigned short source = vd->vertexBufferBinding->getNextIndex();
        vd->vertexDeclaration->addElement(source, 0, type, in.semantic, in.index);
        vd->vertexBufferBinding->setBinding(source, buf);

        Ogre::LogManager::getSingleton().logMessage("[RoR|VertexInputFix] '" + prog_name + "' reads "
            + SemanticName(in.semantic) + Ogre::StringConverter::toString(in.index)
            + " - geometry has none, added a constant stream (" + Ogre::StringConverter::toString(num_verts) + " vertices)");
    }

    std::unordered_map<Ogre::GpuProgram*, std::vector<Input>> m_inputs;
};

} // namespace RoR
