/*
    This source file is part of Rigs of Rods
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

// D3D11 links vertex shader outputs and pixel shader inputs by register, in declaration order.
// D3D9-style HLSL (SkyX, Hydrax, ...) has `out float4 oPosition : POSITION` in the vertex shader,
// but the pixel shader does not declare it - so all its inputs are shifted by one register and read garbage.
// Fix: vertex shaders get their POSITION output moved to the first output, pixel shaders get a leading
// `float4 ... : SV_Position` input. Plain text transformation, the shader code itself is not touched.

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace RoR {
namespace D3D11ShaderFix {

inline std::string Trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

inline std::string Upper(std::string s)
{
    for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}

inline bool IsIdentChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// Finds the next whole-word occurrence of `word` at or after `from`.
inline size_t FindWord(const std::string& s, const std::string& word, size_t from)
{
    for (size_t p = s.find(word, from); p != std::string::npos; p = s.find(word, p + 1))
    {
        const bool l = (p == 0) || !IsIdentChar(s[p - 1]);
        const bool r = (p + word.size() >= s.size()) || !IsIdentChar(s[p + word.size()]);
        if (l && r) return p;
    }
    return std::string::npos;
}

/// Semantic after the last top-level ':' of a parameter ("out float4 oPos : POSITION" -> "POSITION").
inline std::string Semantic(const std::string& param)
{
    int depth = 0;
    size_t colon = std::string::npos;
    for (size_t i = 0; i < param.size(); ++i)
    {
        if (param[i] == '(') depth++;
        else if (param[i] == ')') depth--;
        else if (param[i] == ':' && depth == 0) colon = i;
    }
    if (colon == std::string::npos) return std::string();
    std::string sem = Upper(Trim(param.substr(colon + 1)));
    const size_t sp = sem.find_first_of(" \t(");
    return (sp == std::string::npos) ? sem : sem.substr(0, sp);
}

inline bool StartsWithWord(const std::string& s, const char* word)
{
    const size_t n = std::char_traits<char>::length(word);
    return s.compare(0, n, word) == 0 && (s.size() == n || s[n] == ' ' || s[n] == '\t' || s[n] == '\r' || s[n] == '\n');
}

/// Parameter text without comments and preprocessor lines (for classification only).
inline std::string Clean(const std::string& p)
{
    std::string r;
    size_t i = 0;
    bool lineStart = true;
    while (i < p.size())
    {
        if (p.compare(i, 2, "//") == 0) { const size_t e = p.find('\n', i); i = (e == std::string::npos) ? p.size() : e; continue; }
        if (p.compare(i, 2, "/*") == 0) { const size_t e = p.find("*/", i + 2); i = (e == std::string::npos) ? p.size() : e + 2; continue; }
        if (lineStart && p[i] == '#') { const size_t e = p.find('\n', i); i = (e == std::string::npos) ? p.size() : e; continue; }
        if (p[i] == '\n') lineStart = true;
        else if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r') lineStart = false;
        r += p[i++];
    }
    return Trim(r);
}

inline bool IsPositionSemantic(const std::string& sem)
{
    return sem == "POSITION" || sem == "POSITION0" || sem == "SV_POSITION" || sem == "VPOS";
}

/// OGRE writes float3 auto constants (e.g. camera_position_object_space) as 4 floats. D3D9 gives every
/// uniform its own register, but D3D11 packs a following scalar into the 4th component of a float3, which then
/// gets overwritten every frame (Hydrax: uFullReflectionDistance := 1 -> water shows only reflection).
/// Adds an unused padding float after each `uniform float3 name` parameter so nothing shares its register.
inline std::string PadFloat3Uniforms(const std::string& list)
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
    bool changed = false;
    for (std::string& raw : params)
    {
        const std::string p = Clean(raw);
        if (!StartsWithWord(p, "uniform"))
            continue;
        std::string rest = Trim(p.substr(7));
        if (!StartsWithWord(rest, "float3") && !StartsWithWord(rest, "half3"))
            continue;
        rest = Trim(rest.substr(rest.find_first_of(" \t\r\n")));
        size_t e = 0;
        while (e < rest.size() && IsIdentChar(rest[e])) e++;
        const std::string name = rest.substr(0, e);
        if (name.empty() || !Trim(rest.substr(e)).empty())
            continue; // array, semantic or register binding - leave alone
        // insert right after the name in the raw text (keeps comments / #if lines around it)
        size_t at = std::string::npos;
        for (size_t q = FindWord(raw, name, 0); q != std::string::npos; q = FindWord(raw, name, q + 1))
        {
            const size_t ls = raw.rfind('\n', q);
            const std::string line = raw.substr(ls == std::string::npos ? 0 : ls + 1, q - (ls == std::string::npos ? 0 : ls + 1));
            if (line.find("//") == std::string::npos && line.find('#') == std::string::npos)
            {
                at = q + name.size();
                break;
            }
        }
        if (at == std::string::npos)
            continue;
        raw.insert(at, ",\n\tuniform float " + name + "__d3d11_pad");
        changed = true;
    }
    if (!changed)
        return list;
    std::string res;
    for (size_t i = 0; i < params.size(); ++i) res += (i ? "," : "") + params[i];
    return res;
}

/// Sort key of an interpolated value: position first, then TEXCOORDn, then COLORn, then anything else.
/// COLOR comes after the texcoords because pixel shaders often skip it (nicemetal: `SimpleMetal_PS` reads no
/// COLOR from `NiceMetal_VS`) - D3D11 accepts a PS that leaves out trailing VS outputs, not ones in the middle.
inline int VaryingOrder(const std::string& sem)
{
    if (IsPositionSemantic(sem)) return 0;
    size_t d = sem.size();
    while (d > 0 && sem[d - 1] >= '0' && sem[d - 1] <= '9') d--;
    const int idx = (d < sem.size()) ? std::atoi(sem.c_str() + d) : 0;
    const std::string base = sem.substr(0, d);
    if (base == "TEXCOORD") return 100 + idx;
    if (base == "COLOR") return 300 + idx;
    return 1000;
}

/// D3D11 links VS outputs to PS inputs by register, which the compiler assigns in declaration order - unlike D3D9,
/// which matched them by semantic. Cg/D3D9 sources declare them in any order (nicemetal: VS `COLOR, TEXCOORD0..4`,
/// PS `TEXCOORD0, COLOR, TEXCOORD1..4` -> the PS read the wrong values: textures as flat, flickering colours).
/// Puts the VS outputs (`outputs` = true) or the PS inputs into one canonical order. The parameters keep their
/// slots in the list, only the interpolated ones are permuted among themselves.
inline void SortVaryings(std::vector<std::string>& params, bool outputs)
{
    std::vector<int> slots;
    std::vector<std::pair<int, std::string>> items;
    for (int i = 0; i < (int)params.size(); ++i)
    {
        const std::string p = Clean(params[i]);
        if (p.empty() || StartsWithWord(p, "uniform")) continue;
        if (StartsWithWord(p, "out") != outputs) continue;
        const std::string sem = Semantic(p);
        if (sem.empty()) continue; // struct parameter: its members are ordered by the struct
        slots.push_back(i);
        items.push_back({ VaryingOrder(sem), params[i] });
    }
    std::stable_sort(items.begin(), items.end(), [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) { return a.first < b.first; });
    for (size_t k = 0; k < slots.size(); ++k)
    {
        std::string text = items[k].second;
        // a moved parameter must start on its own line: the slot may follow a `// comment` (whose commas
        // split the list into pieces), which would otherwise swallow it
        const size_t first = text.find_first_not_of(" \t");
        if (params[slots[k]] != text && (first == std::string::npos || (text[first] != '\n' && text[first] != '\r')))
            text = "\n\t" + text;
        params[slots[k]] = text;
    }
}

inline std::string JoinParams(const std::vector<std::string>& params)
{
    std::string res;
    for (size_t i = 0; i < params.size(); ++i) res += (i ? "," : "") + params[i];
    return res;
}

inline std::string FixFunctionParams(const std::string& list, bool returnsColor)
{
    // split by top-level commas
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

    int outPos = -1, firstOut = -1;
    bool outColor = returnsColor, inPos = false;
    for (int i = 0; i < (int)params.size(); ++i)
    {
        const std::string p = Clean(params[i]);
        const std::string sem = Semantic(p);
        if (p.empty() || StartsWithWord(p, "uniform")) continue;
        if (StartsWithWord(p, "out"))
        {
            if (firstOut < 0) firstOut = i;
            if (IsPositionSemantic(sem)) outPos = i;
            if (sem.compare(0, 5, "COLOR") == 0 || sem.compare(0, 9, "SV_TARGET") == 0) outColor = true;
        }
        else if (IsPositionSemantic(sem))
        {
            inPos = true;
        }
    }

    if (outPos >= 0)
    {
        // Vertex shader: POSITION must be the first output, the others in canonical order
        SortVaryings(params, true);
        return JoinParams(params);
    }

    if (outColor)
    {
        // Pixel shader: inputs in canonical order, the position declared as first input
        SortVaryings(params, false);
        const std::string sorted = JoinParams(params);
        if (inPos)
            return sorted;
        const bool empty = Clean(list).empty() || Clean(list) == "void";
        return std::string("\n\tfloat4 d3d11_SvPosition : SV_Position") + (empty ? std::string() : "," + sorted);
    }

    return list;
}

/// D3D9-style samplers (`sampler2D x : register(s1)`) compiled in D3D11 backwards compatibility mode get their
/// texture register (t#) assigned by the compiler, not from `register(s#)` - the textures end up in the wrong slots.
/// Converts every sampler that is only used in tex1D/tex2D/tex3D/texCUBE(sampler, coord) calls into an explicit
/// Texture + SamplerState pair with matching t#/s# registers. Other samplers are left untouched.
inline std::string ConvertSamplers(const std::string& src)
{
    struct Decl { size_t begin, end; std::string type, name; int reg; bool param; };
    std::vector<Decl> decls;
    static const char* types[] = { "samplerCUBE", "sampler1D", "sampler2D", "sampler3D", "sampler" };

    // 1. find declarations
    for (const char* t : types)
    {
        for (size_t p = FindWord(src, t, 0); p != std::string::npos; p = FindWord(src, t, p + 1))
        {
            size_t b = p;
            // include a preceding 'uniform'
            size_t q = src.find_last_not_of(" \t\r\n", p == 0 ? 0 : p - 1);
            if (q != std::string::npos && q >= 6 && src.compare(q - 6, 7, "uniform") == 0 && (q < 7 || !IsIdentChar(src[q - 7])))
                b = q - 6;
            size_t n0 = src.find_first_not_of(" \t\r\n", p + std::char_traits<char>::length(t));
            if (n0 == std::string::npos || !IsIdentChar(src[n0])) continue;
            size_t n1 = n0;
            while (n1 < src.size() && IsIdentChar(src[n1])) n1++;
            Decl d;
            d.type = t;
            d.name = src.substr(n0, n1 - n0);
            d.reg = -1;
            size_t e = src.find_first_not_of(" \t\r\n", n1);
            if (e != std::string::npos && src[e] == ':')
            {
                const size_t r = src.find("register", e);
                const size_t close = (r == std::string::npos) ? r : src.find(')', r);
                if (close == std::string::npos) continue;
                const size_t sPos = src.find_first_of("sS", src.find('(', r) + 1);
                if (sPos != std::string::npos && sPos < close)
                    d.reg = std::atoi(src.c_str() + sPos + 1);
                e = src.find_first_not_of(" \t\r\n", close + 1);
            }
            if (e == std::string::npos) continue;
            if (src[e] == ';') { d.param = false; d.end = e + 1; }
            else if (src[e] == ',' || src[e] == ')' || src[e] == '#' || src[e] == '/') { d.param = true; d.end = (src[e] == ',' || src[e] == ')') ? e : e; }
            else continue;
            if (d.param)
            {
                // end of the declaration text = end of the register(...) or the name
                size_t endText = n1;
                const size_t c2 = src.find_first_not_of(" \t\r\n", n1);
                if (c2 != std::string::npos && src[c2] == ':') endText = src.find(')', src.find("register", c2)) + 1;
                d.end = endText;
            }
            d.begin = b;
            // skip declarations inside comments
            const size_t ls = src.rfind('\n', b);
            const std::string line = src.substr(ls == std::string::npos ? 0 : ls + 1, b - (ls == std::string::npos ? 0 : ls + 1));
            if (line.find("//") != std::string::npos) continue;
            decls.push_back(d);
        }
    }
    if (decls.empty())
        return src;

    // 2. keep only samplers used exclusively as first argument of texXX(...)
    std::vector<Decl> ok;
    for (const Decl& d : decls)
    {
        bool valid = true, used = false;
        std::string tex;
        for (size_t p = FindWord(src, d.name, 0); p != std::string::npos; p = FindWord(src, d.name, p + 1))
        {
            bool isDecl = false;
            for (const Decl& o : decls) if (p >= o.begin && p < o.end && o.name == d.name) isDecl = true;
            if (isDecl) continue;
            size_t q = src.find_last_not_of(" \t\r\n", p - 1);
            size_t a = (q == std::string::npos || src[q] != '(') ? std::string::npos : src.find_last_not_of(" \t\r\n", q - 1);
            size_t f = a;
            while (f != std::string::npos && f > 0 && IsIdentChar(src[f - 1])) f--;
            const std::string fn = (a == std::string::npos) ? std::string() : src.substr(f, a + 1 - f);
            const size_t comma = src.find_first_not_of(" \t\r\n", p + d.name.size());
            if ((fn != "tex1D" && fn != "tex2D" && fn != "tex3D" && fn != "texCUBE") || comma == std::string::npos || src[comma] != ',')
            {
                valid = false; // used in another way (tex2Dlod, passed to a function, ...)
                break;
            }
            if (!tex.empty() && tex != fn) { valid = false; break; }
            tex = fn;
            used = true;
        }
        if (!valid)
            return src; // keep the whole source as is - partial conversion could break the register layout
        Decl c = d;
        if (c.type == "sampler")
            c.type = used ? (tex == "tex1D" ? "sampler1D" : tex == "tex3D" ? "sampler3D" : tex == "texCUBE" ? "samplerCUBE" : "sampler2D") : "sampler2D";
        ok.push_back(c);
    }

    // registers for samplers without explicit register: declaration order, skipping explicit ones
    std::vector<Decl> ordered = ok;
    std::sort(ordered.begin(), ordered.end(), [](const Decl& x, const Decl& y) { return x.begin < y.begin; });
    std::vector<int> usedRegs;
    for (const Decl& d : ordered) if (d.reg >= 0) usedRegs.push_back(d.reg);
    int next = 0;
    for (Decl& d : ordered)
    {
        if (d.reg >= 0) continue;
        while (std::find(usedRegs.begin(), usedRegs.end(), next) != usedRegs.end()) next++;
        d.reg = next;
        usedRegs.push_back(next);
    }

    // 3. rewrite. A parameter sampler only lives in its own function: Cg/D3D9 sources reuse the same name
    //    with another register in another entry point (nicemetal: `Specular_Map` is s1 in main_nicemetal_fp,
    //    s0 in reflect_nicemetal_fp). Such names get one global per register (NAME_rN), and calls are only
    //    rewritten inside the function that declares the parameter.
    struct Edit { size_t begin, end; std::string text; };
    std::vector<Edit> edits;
    auto bodyOf = [&src](const Decl& d, size_t& b, size_t& e)
    {
        b = src.find('{', d.end);
        e = src.size();
        if (b == std::string::npos) { b = src.size(); return; }
        int depth = 0;
        for (size_t i = b; i < src.size(); ++i)
        {
            if (src[i] == '{') depth++;
            else if (src[i] == '}' && --depth == 0) { e = i + 1; break; }
        }
    };
    auto globalName = [&ordered](const Decl& d)
    {
        for (const Decl& o : ordered)
            if (o.name == d.name && (o.reg != d.reg || o.type != d.type))
                return d.name + "_r" + std::to_string(d.reg);
        return d.name;
    };
    auto inDecl = [&decls](size_t p)
    {
        for (const Decl& o : decls) if (p >= o.begin && p < o.end) return true;
        return false;
    };

    std::string globals = "// D3D11: explicit texture/sampler registers (RoR D3D11ShaderFix)\n";
    std::vector<std::string> declared;
    for (const Decl& d : ordered)
    {
        const std::string g = globalName(d);
        // 1D lookups: OGRE loads such textures (e.g. Hydrax 'Fresnel.bmp') as 2D, so sample a 2D texture at v=0.5
        const bool is1D = (d.type == "sampler1D");
        if (std::find(declared.begin(), declared.end(), g) == declared.end())
        {
            declared.push_back(g);
            const std::string texType = d.type == "sampler3D" ? "Texture3D" : d.type == "samplerCUBE" ? "TextureCube" : "Texture2D";
            const std::string r = std::to_string(d.reg);
            globals += texType + " " + g + "__t : register(t" + r + ");\n";
            globals += "SamplerState " + g + "__s : register(s" + r + ");\n";
            if (is1D)
                globals += "float4 " + g + "__tex1D(float c) { return " + g + "__t.Sample(" + g + "__s, float2(c, 0.5)); }\n";
        }

        // the declaration itself
        edits.push_back({ d.begin, d.end, d.param ? ("uniform float " + d.name + "__d3d11_unused") : std::string() });

        // calls: texXX(NAME, coord) -> G__t.Sample(G__s, coord), in the owning function (parameter) or anywhere
        // outside functions that declare a parameter of the same name (global sampler)
        size_t from = 0, to = src.size();
        if (d.param)
            bodyOf(d, from, to);
        for (size_t p = FindWord(src, d.name, from); p != std::string::npos && p < to; p = FindWord(src, d.name, p + 1))
        {
            if (inDecl(p))
                continue;
            if (!d.param)
            {
                bool shadowed = false;
                for (const Decl& o : ordered)
                {
                    if (!o.param || o.name != d.name) continue;
                    size_t b, e;
                    bodyOf(o, b, e);
                    if (p >= b && p < e) { shadowed = true; break; }
                }
                if (shadowed) continue;
            }
            const size_t q = src.find_last_not_of(" \t\r\n", p - 1);
            if (q == std::string::npos || src[q] != '(') continue;
            const size_t a = src.find_last_not_of(" \t\r\n", q - 1);
            size_t f = a;
            while (f > 0 && IsIdentChar(src[f - 1])) f--;
            const size_t comma = src.find(',', p);
            edits.push_back({ f, comma + 1, is1D ? (g + "__tex1D(") : (g + "__t.Sample(" + g + "__s,") });
        }
    }

    // apply back to front so offsets stay valid
    std::sort(edits.begin(), edits.end(), [](const Edit& x, const Edit& y) { return x.begin > y.begin; });
    std::string out = src;
    for (const Edit& e : edits)
        out.replace(e.begin, e.end - e.begin, e.text);
    return globals + out;
}

/// Cg -> HLSL: the Cg sources of RoR and of mods are D3D9-style HLSL with a few Cg-only words.
/// - `fixed`, `fixed2`.. types do not exist in HLSL -> `half`
/// - `: TEXUNITn` sampler semantic -> `: register(sn)` (handled further by ConvertSamplers())
inline std::string CgToHlsl(const std::string& src)
{
    std::string out =
        "#define fixed half\n#define fixed2 half2\n#define fixed3 half3\n#define fixed4 half4\n"
        "#define fixed3x3 half3x3\n#define fixed4x4 half4x4\n";
    out.reserve(src.size() + out.size() + 64);
    const std::string word = "TEXUNIT";
    size_t pos = 0;
    // TEXUNITn is one token (FindWord() needs the whole word), so match the prefix by hand
    for (size_t at = src.find(word); at != std::string::npos; at = src.find(word, at))
    {
        const bool startOk = (at == 0) || !IsIdentChar(src[at - 1]);
        size_t end = at + word.size();
        while (end < src.size() && src[end] >= '0' && src[end] <= '9')
            ++end;
        const bool endOk = (end > at + word.size()) && (end == src.size() || !IsIdentChar(src[end]));
        if (startOk && endOk)
        {
            out += src.substr(pos, at - pos);
            out += "register(s" + src.substr(at + word.size(), end - at - word.size()) + ")";
            pos = end;
        }
        at = end;
    }
    out += src.substr(pos);
    return out;
}

/// Applies the fix to every function definition in the source.
inline std::string FixSignatures(const std::string& src_in)
{
    const std::string src = ConvertSamplers(src_in);
    std::string out;
    out.reserve(src.size() + 256);
    size_t pos = 0;
    int braceDepth = 0;
    size_t i = 0;
    while (i < src.size())
    {
        const char c = src[i];
        // skip comments
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/')
        {
            const size_t e = src.find('\n', i);
            i = (e == std::string::npos) ? src.size() : e;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*')
        {
            const size_t e = src.find("*/", i + 2);
            i = (e == std::string::npos) ? src.size() : e + 2;
            continue;
        }
        if (c == '{') { braceDepth++; i++; continue; }
        if (c == '}') { braceDepth--; i++; continue; }
        if (c == '(' && braceDepth == 0)
        {
            // find matching ')'
            int d = 0;
            size_t j = i;
            for (; j < src.size(); ++j)
            {
                if (src[j] == '(') d++;
                else if (src[j] == ')') { d--; if (d == 0) break; }
            }
            if (j >= src.size()) break;
            // function definition? ")" [":" SEM] "{"
            size_t k = src.find_first_not_of(" \t\r\n", j + 1);
            bool returnsColor = false;
            if (k != std::string::npos && src[k] == ':')
            {
                const size_t b = src.find('{', k);
                if (b == std::string::npos) { i = j + 1; continue; }
                const std::string sem = Upper(Trim(src.substr(k + 1, b - k - 1)));
                returnsColor = sem.compare(0, 5, "COLOR") == 0 || sem.compare(0, 9, "SV_TARGET") == 0;
                k = b;
            }
            if (k != std::string::npos && src[k] == '{')
            {
                out += src.substr(pos, i + 1 - pos);
                out += FixFunctionParams(PadFloat3Uniforms(src.substr(i + 1, j - i - 1)), returnsColor);
                pos = j;
            }
            i = j + 1;
            continue;
        }
        i++;
    }
    out += src.substr(pos);
    return out;
}

} // namespace D3D11ShaderFix
} // namespace RoR
