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

// OpenGL / OpenGL 3+ without the Cg plugin: translates a Cg program (one entry point) to GLSL.
//
// The Cg function bodies are kept and run as GLSL: types are renamed (float4 -> vec4), Cg library
// functions (tex2D, mul, lerp, saturate...) come from a small prelude, and the implicit conversions
// Cg allows but GLSL does not (`float3 d = max(dot(l, n), 0);`) are made explicit for assignments,
// initializers and return statements. The entry function keeps its signature minus the `uniform`
// parameters (they become GLSL uniforms of the same name, so `param_named` keeps working);
// a generated `main()` feeds it from vertex attributes / varyings and writes its outputs.
//
// Linkage: vertex attributes use OGRE's custom attribute names (vertex, normal, uv0...).
// Varyings are named by semantic (`cg_v_TEXCOORD0`) and always vec4 - so a vertex shader writing
// float3 and a fragment shader reading float2 from the same TEXCOORD still link, like in D3D/Cg.
// Sampler units (`TEXUNITn`, `register(sN)` or declaration order of the used samplers) are returned
// to the caller, which sets them as `param_named <sampler> int <unit>` - GLSL has no other way.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace RoR {
namespace CgToGlsl {

struct Sampler
{
    std::string name;
    int unit;
};

struct Result
{
    bool ok = false;
    std::string glsl;
    std::string error;
    std::vector<Sampler> samplers;
};

namespace detail {

enum TokType { T_IDENT, T_NUMBER, T_PUNCT, T_DIRECTIVE };

struct Tok
{
    TokType type;
    std::string text;
};
typedef std::vector<Tok> Toks;

inline bool IsIdStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
inline bool IsIdChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

inline std::string Upper(std::string s)
{
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

/// Tokenizer. Comments are dropped, preprocessor lines are kept whole, Cg float suffixes (1.0f, 2.h) are removed.
inline Toks Lex(const std::string& src)
{
    Toks out;
    const size_t n = src.size();
    size_t i = 0;
    bool lineStart = true;
    while (i < n)
    {
        const char c = src[i];
        const char next = (i + 1 < n) ? src[i + 1] : '\0';
        if (c == '\n') { lineStart = true; ++i; continue; }
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '/' && next == '/') { while (i < n && src[i] != '\n') ++i; continue; }
        if (c == '/' && next == '*')
        {
            const size_t e = src.find("*/", i + 2);
            i = (e == std::string::npos) ? n : e + 2;
            continue;
        }
        if (c == '#' && lineStart)
        {
            std::string line;
            while (i < n && src[i] != '\n')
            {
                if (src[i] == '\\' && i + 1 < n && (src[i + 1] == '\n' || src[i + 1] == '\r'))
                {
                    i += (src[i + 1] == '\r' && i + 2 < n && src[i + 2] == '\n') ? 3 : 2;
                    line += ' ';
                    continue;
                }
                if (src[i] == '/' && i + 1 < n && src[i + 1] == '/')
                    break; // trailing comment
                if (src[i] != '\r')
                    line += src[i];
                ++i;
            }
            out.push_back({ T_DIRECTIVE, line });
            continue;
        }
        lineStart = false;
        if (IsIdStart(c))
        {
            size_t b = i;
            while (i < n && IsIdChar(src[i])) ++i;
            out.push_back({ T_IDENT, src.substr(b, i - b) });
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && std::isdigit((unsigned char)next)))
        {
            size_t b = i;
            const bool hex = (c == '0' && (next == 'x' || next == 'X'));
            while (i < n)
            {
                const char d = src[i];
                if (IsIdChar(d) || d == '.') { ++i; continue; }
                if (!hex && (d == '+' || d == '-') && (src[i - 1] == 'e' || src[i - 1] == 'E')) { ++i; continue; }
                break;
            }
            std::string num = src.substr(b, i - b);
            if (!hex)
            {
                while (!num.empty() && std::strchr("fFhHlLuU", num.back()))
                    num.pop_back();
                if (!num.empty() && num.back() == '.')
                    num += '0';
                if (!num.empty() && num[0] == '.')
                    num = "0" + num;
            }
            out.push_back({ T_NUMBER, num });
            continue;
        }
        static const char* puncts3[] = { "<<=", ">>=" };
        static const char* puncts2[] = { "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "->", "::" };
        std::string p;
        for (const char* t : puncts3) if (src.compare(i, 3, t) == 0) p = t;
        if (p.empty()) for (const char* t : puncts2) if (src.compare(i, 2, t) == 0) p = t;
        if (p.empty()) p = std::string(1, c);
        out.push_back({ T_PUNCT, p });
        i += p.size();
    }
    return out;
}

inline bool Is(const Toks& t, size_t i, const char* text) { return i < t.size() && t[i].type != T_DIRECTIVE && t[i].text == text; }
inline bool IsIdent(const Toks& t, size_t i) { return i < t.size() && t[i].type == T_IDENT; }

/// Index of the bracket closing the one at `open` (or t.size()).
inline size_t Match(const Toks& t, size_t open)
{
    const std::string o = t[open].text;
    const std::string c = (o == "(") ? ")" : (o == "[") ? "]" : "}";
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i)
    {
        if (t[i].type != T_PUNCT) continue;
        if (t[i].text == o) ++depth;
        else if (t[i].text == c && --depth == 0) return i;
    }
    return t.size();
}

/// Cg type name -> GLSL type name, or "" if not a builtin Cg type.
inline std::string MapType(const std::string& t)
{
    static std::map<std::string, std::string> table;
    if (table.empty())
    {
        for (const char* base : { "float", "half", "fixed", "double" })
        {
            const std::string b = base;
            table[b] = "float";
            for (int r = 1; r <= 4; ++r)
            {
                table[b + std::to_string(r)] = (r == 1) ? "float" : "vec" + std::to_string(r);
                for (int c = 1; c <= 4; ++c)
                {
                    if (r == 1 || c == 1) continue;
                    // Cg floatRxC has R rows and C columns, GLSL matCxR has C columns and R rows
                    table[b + std::to_string(r) + "x" + std::to_string(c)] =
                        (r == c) ? "mat" + std::to_string(r) : "mat" + std::to_string(c) + "x" + std::to_string(r);
                }
            }
        }
        for (const char* base : { "int", "uint" })
        {
            const std::string b = base;
            table[b] = "int";
            table[b + "1"] = "int";
            for (int r = 2; r <= 4; ++r) table[b + std::to_string(r)] = "ivec" + std::to_string(r);
        }
        table["bool"] = "bool";
        table["bool1"] = "bool";
        for (int r = 2; r <= 4; ++r) table["bool" + std::to_string(r)] = "bvec" + std::to_string(r);
        table["void"] = "void";
        table["sampler1D"] = "sampler1D";
        table["sampler2D"] = "sampler2D";
        table["sampler3D"] = "sampler3D";
        table["samplerCUBE"] = "samplerCube";
        table["samplerCube"] = "samplerCube";
        table["samplerRECT"] = "sampler2DRect";
        table["sampler"] = "sampler2D";
        // GLSL names used directly in Cg code are kept
        for (const char* g : { "vec2", "vec3", "vec4", "mat2", "mat3", "mat4", "ivec2", "ivec3", "ivec4" })
            table[g] = g;
    }
    auto it = table.find(t);
    return (it != table.end()) ? it->second : std::string();
}

inline bool IsSamplerType(const std::string& glslType) { return glslType.compare(0, 7, "sampler") == 0; }
inline bool IsMatrixType(const std::string& glslType) { return glslType.compare(0, 3, "mat") == 0; }

/// Number of components of a float/vec/int type, 0 if not numeric.
inline int Components(const std::string& glslType)
{
    if (glslType == "float" || glslType == "int" || glslType == "bool") return 1;
    if (glslType.size() == 4 && glslType.compare(0, 3, "vec") == 0) return glslType[3] - '0';
    if (glslType.size() == 5 && (glslType.compare(0, 4, "ivec") == 0 || glslType.compare(0, 4, "bvec") == 0)) return glslType[4] - '0';
    return 0;
}

/// Name of the prelude conversion function for an assignment target, "" if none.
inline std::string ConvFunc(const std::string& glslType)
{
    if (glslType == "float" || glslType == "int" || glslType == "vec2" || glslType == "vec3" || glslType == "vec4"
        || glslType == "mat2" || glslType == "mat3" || glslType == "mat4")
        return "cg_" + glslType;
    return std::string();
}

inline bool IsReserved(const std::string& id)
{
    static const std::set<std::string> words = {
        "input", "output", "sample", "filter", "common", "partition", "active", "asm", "class", "union", "enum",
        "typedef", "template", "this", "resource", "goto", "noinline", "public", "external", "interface",
        "long", "short", "unsigned", "superp", "sizeof", "cast", "namespace", "using", "attribute", "varying",
        "smooth", "flat", "noperspective", "centroid", "patch", "subroutine", "layout", "precise", "invariant",
        "lowp", "mediump", "highp", "precision", "buffer", "coherent", "restrict", "readonly", "writeonly",
        "texture", "main", "fvec2", "fvec3", "fvec4", "hvec2", "hvec3", "hvec4", "dvec2", "dvec3", "dvec4",
        "packed", "row_major", "column_major" };
    return words.count(id) > 0 || id.compare(0, 3, "gl_") == 0;
}

struct Var
{
    std::vector<std::string> quals; // in out inout uniform const static
    std::string type;               // Cg type token
    std::string name;
    std::string arr;                // "[N]" or ""
    std::string sem;                // upper case semantic, "" if none
    int unit = -1;                  // TEXUNITn / register(sN)
    Toks init;                      // initializer tokens (after '=')

    bool Has(const char* q) const { for (const auto& s : quals) if (s == q) return true; return false; }
};

struct Struct
{
    std::string name;
    std::vector<Var> members;
};

struct Func
{
    std::vector<std::string> quals;
    std::string ret;   // Cg type token
    std::string name;
    std::string retSem;
    std::vector<Var> params;
    size_t bodyBegin = 0, bodyEnd = 0; // tokens, '{' .. '}' inclusive; 0/0 for prototypes
    bool proto = false;
};

struct Item
{
    enum Kind { DIRECTIVE, STRUCT, FUNC, GLOBAL } kind;
    size_t index;               // into structs/funcs
    std::string directive;
    std::vector<Var> globals;   // GLOBAL: declarators
};

/// Parses `: SEMANTIC`, `: register(s0)`, `: TEXUNIT0` (possibly several) from t[b..e).
inline void ParseSemantics(const Toks& t, size_t b, size_t e, Var& v)
{
    for (size_t i = b; i < e; ++i)
    {
        if (!IsIdent(t, i)) continue;
        const std::string w = t[i].text;
        if (w == "register" && Is(t, i + 1, "("))
        {
            const size_t c = Match(t, i + 1);
            if (i + 2 < c && t[i + 2].text.size() > 1)
            {
                const std::string r = t[i + 2].text;
                if (r[0] == 's' || r[0] == 'S' || r[0] == 't' || r[0] == 'T')
                    v.unit = std::atoi(r.c_str() + 1);
            }
            i = c;
        }
        else if (Upper(w).compare(0, 7, "TEXUNIT") == 0)
            v.unit = std::atoi(w.c_str() + 7);
        else if (v.sem.empty())
            v.sem = Upper(w);
    }
}

/// Parses one declaration `[quals] type name [arr] [: sem] [= init]` from t[b..e).
inline bool ParseVar(const Toks& t, size_t b, size_t e, Var& v, const std::set<std::string>& structs, bool allowNoName)
{
    static const std::set<std::string> qualifiers = { "in", "out", "inout", "uniform", "const", "static", "extern", "shared", "volatile", "varying", "inline", "row_major", "column_major" };
    size_t i = b;
    while (i < e && IsIdent(t, i) && qualifiers.count(t[i].text)) v.quals.push_back(t[i++].text);
    if (i >= e || !IsIdent(t, i)) return false;
    v.type = t[i++].text;
    if (v.type == "unsigned" && IsIdent(t, i)) v.type = t[i++].text; // `unsigned int`
    if (MapType(v.type).empty() && !structs.count(v.type)) return false;
    if (i >= e || !IsIdent(t, i)) return allowNoName;
    v.name = t[i++].text;
    while (i < e && Is(t, i, "["))
    {
        const size_t c = Match(t, i);
        for (size_t k = i; k <= c && k < e; ++k) v.arr += t[k].text;
        i = c + 1;
    }
    size_t eq = e;
    for (size_t k = i; k < e; ++k) if (Is(t, k, "=")) { eq = k; break; }
    if (i < eq && Is(t, i, ":")) ParseSemantics(t, i + 1, eq, v);
    if (eq < e) v.init.assign(t.begin() + eq + 1, t.begin() + e);
    return true;
}

/// Splits t[b..e) at top level commas.
inline std::vector<std::pair<size_t, size_t>> SplitCommas(const Toks& t, size_t b, size_t e)
{
    std::vector<std::pair<size_t, size_t>> parts;
    size_t start = b;
    int depth = 0;
    for (size_t i = b; i < e; ++i)
    {
        if (t[i].type != T_PUNCT) continue;
        const std::string& s = t[i].text;
        if (s == "(" || s == "[" || s == "{") ++depth;
        else if (s == ")" || s == "]" || s == "}") --depth;
        else if (s == "," && depth == 0) { parts.push_back({ start, i }); start = i + 1; }
    }
    if (start < e) parts.push_back({ start, e });
    return parts;
}

class Translator
{
public:
    Translator(const std::string& src, const std::string& entry, bool fragment, int version)
        : m_t(Lex(src)), m_entryName(entry), m_fragment(fragment), m_version(version) {}

    Result Run()
    {
        Result res;
        if (!Parse(res.error))
            return res;
        const Func* entry = nullptr;
        for (const Func& f : m_funcs)
            if (f.name == m_entryName && !f.proto) entry = &f;
        if (!entry)
        {
            res.error = "entry point '" + m_entryName + "' not found";
            return res;
        }
        m_entry = entry;
        FindReachable();
        AssignSamplers(res);

        std::string mainFn = BuildMain();
        std::string out = Prelude();
        out += m_ioDecls;
        for (const Item& item : m_items)
        {
            switch (item.kind)
            {
            case Item::DIRECTIVE: out += "\n" + item.directive + "\n"; break;
            case Item::STRUCT: out += EmitStruct(m_structs[item.index]); break;
            case Item::GLOBAL: out += EmitGlobal(item.globals); break;
            case Item::FUNC:
            {
                const Func& f = m_funcs[item.index];
                if (!m_reachable.count(f.name)) break;
                if (&f == m_entry) out += EmitEntryUniforms();
                out += EmitFunc(f);
                break;
            }
            }
        }
        out += mainFn;
        res.glsl = out;
        res.ok = true;
        return res;
    }

private:
    // ---------------------------------------------------------------- parsing

    bool Parse(std::string& error)
    {
        const Toks& t = m_t;
        size_t i = 0;
        while (i < t.size())
        {
            if (t[i].type == T_DIRECTIVE)
            {
                Item it{ Item::DIRECTIVE, 0, t[i].text, {} };
                m_items.push_back(it);
                ++i;
                continue;
            }
            if (Is(t, i, ";")) { ++i; continue; }
            if (Is(t, i, "struct") && IsIdent(t, i + 1) && Is(t, i + 2, "{"))
            {
                Struct s;
                s.name = t[i + 1].text;
                m_structNames.insert(s.name);
                const size_t close = Match(t, i + 2);
                size_t start = i + 3;
                for (size_t k = i + 3; k < close; ++k)
                {
                    if (t[k].type == T_DIRECTIVE) { start = k + 1; continue; }
                    if (!Is(t, k, ";")) continue;
                    AddDeclarators(start, k, s.members);
                    start = k + 1;
                }
                m_items.push_back({ Item::STRUCT, m_structs.size(), "", {} });
                m_structs.push_back(s);
                i = close + 1;
                continue;
            }
            // Find the end of this top level construct: ';' or a function body
            size_t paren = t.size();
            size_t k = i;
            for (; k < t.size(); ++k)
            {
                if (Is(t, k, "(") && paren == t.size()) { paren = k; k = Match(t, k); continue; }
                if (Is(t, k, ";") || Is(t, k, "{") || Is(t, k, "=")) break;
            }
            const bool isFunc = (paren < t.size() && paren > i && IsIdent(t, paren - 1) && k < t.size() && !Is(t, k, "="));
            if (isFunc)
            {
                Func f;
                size_t q = i;
                while (q < paren - 2 && IsIdent(t, q) && (t[q].text == "static" || t[q].text == "inline" || t[q].text == "extern"))
                    f.quals.push_back(t[q++].text);
                f.ret = t[q].text;
                f.name = t[paren - 1].text;
                const size_t close = Match(t, paren);
                for (const auto& part : SplitCommas(t, paren + 1, close))
                {
                    Var v;
                    if (part.first < part.second && !(part.second - part.first == 1 && Is(t, part.first, "void")))
                    {
                        if (!ParseVar(t, part.first, part.second, v, m_structNames, false))
                        {
                            error = "cannot parse parameter of function '" + f.name + "'";
                            return false;
                        }
                        f.params.push_back(v);
                    }
                }
                size_t after = close + 1;
                if (Is(t, after, ":"))
                {
                    Var tmp;
                    size_t s = after + 1;
                    while (s < t.size() && !Is(t, s, "{") && !Is(t, s, ";")) ++s;
                    ParseSemantics(t, after + 1, s, tmp);
                    f.retSem = tmp.sem;
                    after = s;
                }
                if (Is(t, after, ";"))
                {
                    f.proto = true;
                    i = after + 1;
                }
                else if (Is(t, after, "{"))
                {
                    f.bodyBegin = after;
                    f.bodyEnd = Match(t, after);
                    i = f.bodyEnd + 1;
                }
                else
                {
                    error = "unexpected token after function '" + f.name + "'";
                    return false;
                }
                m_items.push_back({ Item::FUNC, m_funcs.size(), "", {} });
                m_funcs.push_back(f);
                continue;
            }
            // Global variable(s): up to ';' at depth 0
            size_t e = i;
            int depth = 0;
            for (; e < t.size(); ++e)
            {
                if (t[e].type != T_PUNCT) continue;
                const std::string& s = t[e].text;
                if (s == "(" || s == "[" || s == "{") ++depth;
                else if (s == ")" || s == "]" || s == "}") --depth;
                else if (s == ";" && depth == 0) break;
            }
            Item it{ Item::GLOBAL, 0, "", {} };
            AddDeclarators(i, e, it.globals);
            for (const Var& v : it.globals) m_globals[v.name] = v;
            if (!it.globals.empty())
                m_items.push_back(it);
            i = e + 1;
        }
        return true;
    }

    /// `[quals] type a [: sem] [= x], b ...` in t[b..e) -> vars
    void AddDeclarators(size_t b, size_t e, std::vector<Var>& out)
    {
        auto parts = SplitCommas(m_t, b, e);
        if (parts.empty()) return;
        Var first;
        if (!ParseVar(m_t, parts[0].first, parts[0].second, first, m_structNames, false))
            return;
        out.push_back(first);
        for (size_t p = 1; p < parts.size(); ++p)
        {
            // re-parse "type name..." with the type of the first declarator
            Toks tmp;
            for (const auto& q : first.quals) tmp.push_back({ T_IDENT, q });
            tmp.push_back({ T_IDENT, first.type });
            tmp.insert(tmp.end(), m_t.begin() + parts[p].first, m_t.begin() + parts[p].second);
            Var v;
            if (ParseVar(tmp, 0, tmp.size(), v, m_structNames, false))
                out.push_back(v);
        }
    }

    void FindReachable()
    {
        std::vector<const Func*> work = { m_entry };
        m_reachable.insert(m_entry->name);
        while (!work.empty())
        {
            const Func* f = work.back();
            work.pop_back();
            for (size_t i = f->bodyBegin; i < f->bodyEnd; ++i)
            {
                if (!IsIdent(m_t, i) || !Is(m_t, i + 1, "(") || m_reachable.count(m_t[i].text)) continue;
                for (const Func& g : m_funcs)
                {
                    if (g.name == m_t[i].text)
                    {
                        m_reachable.insert(g.name);
                        if (!g.proto) work.push_back(&g);
                    }
                }
            }
        }
    }

    bool UsedInReachable(const std::string& name) const
    {
        for (const Func& f : m_funcs)
        {
            if (!m_reachable.count(f.name) || f.proto) continue;
            for (size_t i = f.bodyBegin; i < f.bodyEnd; ++i)
                if (IsIdent(m_t, i) && m_t[i].text == name) return true;
        }
        return false;
    }

    bool IsEntryUniform(const Var& p) const
    {
        if (p.Has("uniform")) return true;
        // Cg/HLSL: entry parameters without semantic (and not a struct of varyings) are uniforms
        return !p.Has("out") && !p.Has("inout") && p.sem.empty() && !m_structNames.count(p.type);
    }

    void AssignSamplers(Result& res)
    {
        std::vector<const Var*> samplers;
        for (const Item& item : m_items)
            if (item.kind == Item::GLOBAL)
                for (const Var& v : item.globals)
                    if (IsSamplerType(MapType(v.type)) && UsedInReachable(v.name)) samplers.push_back(&v);
        for (const Var& p : m_entry->params)
            if (IsSamplerType(MapType(p.type)) && UsedInReachable(p.name)) samplers.push_back(&p);

        std::set<int> taken;
        for (const Var* v : samplers) if (v->unit >= 0) taken.insert(v->unit);
        int next = 0;
        for (const Var* v : samplers)
        {
            int unit = v->unit;
            if (unit < 0)
            {
                while (taken.count(next)) ++next;
                unit = next++;
            }
            res.samplers.push_back({ Rename(v->name), unit });
        }
    }

    // ---------------------------------------------------------------- emitting

    std::string Rename(const std::string& id) const
    {
        return IsReserved(id) ? id + "_cg" : id;
    }

    std::string GlslType(const std::string& cgType) const
    {
        const std::string m = MapType(cgType);
        return m.empty() ? Rename(cgType) : m;
    }

    std::string EmitStruct(const Struct& s)
    {
        std::string out = "struct " + Rename(s.name) + "\n{\n";
        for (const Var& m : s.members)
            out += "    " + GlslType(m.type) + " " + Rename(m.name) + m.arr + ";\n";
        return out + "};\n";
    }

    std::string EmitGlobal(const std::vector<Var>& vars)
    {
        std::string out;
        for (const Var& v : vars)
        {
            const std::string type = GlslType(v.type);
            std::string q;
            if (v.Has("const")) q = "const ";
            else if (v.Has("static")) q = "";
            else q = "uniform ";
            if (q == "uniform " && !UsedInReachable(v.name))
                continue; // uniform of another entry point
            out += q + type + " " + Rename(v.name) + v.arr;
            if (!v.init.empty() && q != "uniform ")
                out += " = " + EmitInit(v.init, type, v.arr);
            out += ";\n";
        }
        return out;
    }

    /// Initializer list `{a, b}` of arrays -> `T[N](a, b)`; anything else is emitted as an expression.
    std::string EmitInit(const Toks& init, const std::string& type, const std::string& arr)
    {
        if (!init.empty() && Is(init, 0, "{") && !arr.empty())
        {
            const size_t c = Match(init, 0);
            Toks inner(init.begin() + 1, init.begin() + c);
            return type + arr + "(" + Emit(inner, 0, inner.size()) + ")";
        }
        return Emit(init, 0, init.size());
    }

    std::string EmitParam(const Var& p) const
    {
        std::string q;
        for (const auto& s : p.quals)
            if (s == "in" || s == "out" || s == "inout" || s == "const") q += s + " ";
        return q + GlslType(p.type) + " " + Rename(p.name) + p.arr;
    }

    std::string EmitFunc(const Func& f)
    {
        const bool isEntry = (&f == m_entry);
        std::string out = GlslType(f.ret) + " " + (isEntry ? std::string("cg_entry") : Rename(f.name)) + "(";
        bool first = true;
        for (const Var& p : f.params)
        {
            if (isEntry && IsEntryUniform(p)) continue;
            out += (first ? "" : ", ") + EmitParam(p);
            first = false;
        }
        out += ")";
        if (f.proto)
            return out + ";\n";

        // Symbol table: globals, entry uniforms, params
        std::map<std::string, std::string> syms; // name -> GLSL type (struct name for structs), "[]" suffix for arrays
        for (const auto& g : m_globals) syms[g.first] = GlslType(g.second.type) + (g.second.arr.empty() ? "" : "[]");
        for (const Var& p : f.params) syms[p.name] = GlslType(p.type) + (p.arr.empty() ? "" : "[]");

        Toks body(m_t.begin() + f.bodyBegin, m_t.begin() + f.bodyEnd + 1);
        body = AddConversions(body, syms, GlslType(f.ret));
        return out + "\n" + Emit(body, 0, body.size()) + "\n";
    }

    std::string EmitEntryUniforms()
    {
        std::string out;
        for (const Var& p : m_entry->params)
            if (IsEntryUniform(p))
                out += "uniform " + GlslType(p.type) + " " + Rename(p.name) + p.arr + ";\n";
        return out;
    }

    // ---------------------------------------------------------------- implicit conversions

    /// Type of a struct member or swizzle applied to `type`.
    std::string MemberType(const std::string& type, const std::string& member) const
    {
        for (const Struct& s : m_structs)
        {
            if (Rename(s.name) != type) continue;
            for (const Var& m : s.members)
                if (m.name == member) return GlslType(m.type) + (m.arr.empty() ? "" : "[]");
            return std::string();
        }
        const int n = Components(type);
        if (n == 0 || member.empty() || member.size() > 4) return std::string();
        const bool isInt = type[0] == 'i';
        if (member.size() == 1) return isInt ? "int" : "float";
        return (isInt ? "ivec" : "vec") + std::to_string(member.size());
    }

    std::string IndexType(const std::string& type) const
    {
        if (type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0) return type.substr(0, type.size() - 2);
        if (type == "mat2" || type == "mat3" || type == "mat4") return "vec" + type.substr(3);
        if (type.compare(0, 3, "vec") == 0) return "float";
        if (type.compare(0, 4, "ivec") == 0) return "int";
        return std::string();
    }

    /// Wraps right hand sides of assignments/initializers and return values with `cg_<type>(...)`.
    Toks AddConversions(const Toks& t, std::map<std::string, std::string>& syms, const std::string& retType)
    {
        Toks out;
        std::string declType;       // type of the declaration statement in progress (for `float a = 1, b = 2;`)
        size_t declStmt = (size_t)-1;
        int paren = 0;
        size_t stmt = 0;            // number of statements seen
        std::vector<std::pair<size_t, std::string>> closers; // (index in t where the wrapped expression ends, text to insert)

        auto exprEnd = [&](size_t from, bool stopAtComma) -> size_t
        {
            int d = 0;
            for (size_t k = from; k < t.size(); ++k)
            {
                if (t[k].type != T_PUNCT) continue;
                const std::string& s = t[k].text;
                if (s == "(" || s == "[" || s == "{") ++d;
                else if (s == ")" || s == "]" || s == "}") { if (d == 0) return k; --d; }
                else if (d == 0 && (s == ";" || (stopAtComma && s == ","))) return k;
            }
            return t.size();
        };

        for (size_t i = 0; i < t.size(); ++i)
        {
            for (auto it = closers.begin(); it != closers.end();)
            {
                if (it->first == i) { out.push_back({ T_PUNCT, it->second }); it = closers.erase(it); }
                else ++it;
            }
            const Tok& tok = t[i];
            if (tok.type == T_PUNCT)
            {
                if (tok.text == "(") ++paren;
                else if (tok.text == ")") --paren;
                else if (tok.text == ";" || tok.text == "{" || tok.text == "}") { ++stmt; declType.clear(); }
            }

            // Declarations: register symbols
            if (tok.type == T_IDENT && IsIdent(t, i + 1) && (!MapType(tok.text).empty() || m_structNames.count(tok.text))
                && (Is(t, i + 2, ";") || Is(t, i + 2, "=") || Is(t, i + 2, ",") || Is(t, i + 2, "[") || Is(t, i + 2, ":") || Is(t, i + 2, ")")))
            {
                const std::string gt = GlslType(tok.text);
                const bool arr = Is(t, i + 2, "[");
                syms[t[i + 1].text] = gt + (arr ? "[]" : "");
                if (paren == 0) { declType = gt; declStmt = stmt; }
            }
            else if (tok.type == T_IDENT && i > 0 && Is(t, i - 1, ",") && !declType.empty() && declStmt == stmt && paren == 0
                && (Is(t, i + 1, ";") || Is(t, i + 1, "=") || Is(t, i + 1, ",") || Is(t, i + 1, "[")))
            {
                syms[tok.text] = declType + (Is(t, i + 1, "[") ? "[]" : "");
            }

            out.push_back(tok);

            if (tok.type == T_IDENT && tok.text == "return" && !Is(t, i + 1, ";"))
            {
                const std::string f = ConvFunc(retType);
                if (!f.empty())
                {
                    out.push_back({ T_IDENT, f });
                    out.push_back({ T_PUNCT, "(" });
                    closers.push_back({ exprEnd(i + 1, false), ")" });
                }
                continue;
            }

            if (!(tok.type == T_PUNCT && tok.text == "=" && paren == 0) || Is(t, i + 1, "{"))
                continue;

            // Left hand side: walk back over `a.b[c].d`
            size_t s = i;
            std::vector<std::pair<char, std::string>> chain; // ('.', member) / ('[', "") in reverse
            size_t k = i;
            while (k > 0)
            {
                --k;
                if (Is(t, k, "]"))
                {
                    int d = 0;
                    while (k > 0)
                    {
                        if (Is(t, k, "]")) ++d;
                        else if (Is(t, k, "[") && --d == 0) break;
                        --k;
                    }
                    chain.push_back({ '[', "" });
                    continue;
                }
                if (IsIdent(t, k))
                {
                    if (k > 0 && Is(t, k - 1, ".")) { chain.push_back({ '.', t[k].text }); --k; continue; }
                    s = k;
                    break;
                }
                break;
            }
            if (s == i) continue;
            std::string type;
            if (s > 0 && IsIdent(t, s - 1) && (!MapType(t[s - 1].text).empty() || m_structNames.count(t[s - 1].text)))
                type = GlslType(t[s - 1].text); // declaration
            else if (s > 0 && Is(t, s - 1, ",") && !declType.empty() && declStmt == stmt)
                type = declType;
            else
            {
                auto sym = syms.find(t[s].text);
                type = (sym != syms.end()) ? sym->second : std::string();
            }
            for (auto c = chain.rbegin(); c != chain.rend() && !type.empty(); ++c)
                type = (c->first == '.') ? MemberType(type, c->second) : IndexType(type);
            if (chain.empty() && s > 0 && IsIdent(t, s - 1) && !type.empty() && type.back() == ']')
                continue; // array declaration
            const std::string f = ConvFunc(type);
            if (f.empty()) continue;
            out.push_back({ T_IDENT, f });
            out.push_back({ T_PUNCT, "(" });
            closers.push_back({ exprEnd(i + 1, true), ")" });
        }
        return out;
    }

    // ---------------------------------------------------------------- expression emitting

    static bool NeedsSpace(const std::string& a, const std::string& b)
    {
        if (a.empty() || b.empty()) return false;
        const char x = a.back(), y = b[0];
        if (IsIdChar(x) && IsIdChar(y)) return true;
        if (std::strchr("+-*/%=<>!&|^,", x) && std::strchr("+-*/%=<>!&|^", y)) return true; // keep `a - -b` apart
        if (x == ',' || x == ';') return true;
        return false;
    }

    /// Operand of a cast: primary expression with postfix operators, starting at i. Returns end (exclusive).
    size_t OperandEnd(const Toks& t, size_t i, size_t e) const
    {
        if (i >= e) return i;
        if (Is(t, i, "-") || Is(t, i, "+") || Is(t, i, "!")) return OperandEnd(t, i + 1, e);
        size_t k = i;
        if (Is(t, k, "(")) k = Match(t, k) + 1;
        else if (t[k].type == T_IDENT || t[k].type == T_NUMBER)
        {
            ++k;
            if (t[k - 1].type == T_IDENT && Is(t, k, "(")) k = Match(t, k) + 1;
        }
        else return i + 1;
        while (k < e)
        {
            if (Is(t, k, ".") && IsIdent(t, k + 1)) k += 2;
            else if (Is(t, k, "[")) k = Match(t, k) + 1;
            else break;
        }
        return std::min(k, e);
    }

    std::string ZeroStruct(const std::string& name) const
    {
        for (const Struct& s : m_structs)
        {
            if (s.name != name) continue;
            std::string out = Rename(s.name) + "(";
            for (size_t m = 0; m < s.members.size(); ++m)
            {
                const std::string gt = GlslType(s.members[m].type);
                out += (m ? ", " : "") + (m_structNames.count(s.members[m].type) ? ZeroStruct(s.members[m].type) : gt + "(0)");
            }
            return out + ")";
        }
        return name + "()";
    }

    std::string Emit(const Toks& t, size_t b, size_t e) const
    {
        std::string out;
        std::string prev;
        auto put = [&](const std::string& s)
        {
            if (NeedsSpace(prev, s)) out += ' ';
            out += s;
            prev = s;
        };
        for (size_t i = b; i < e; ++i)
        {
            const Tok& tok = t[i];
            if (tok.type == T_DIRECTIVE)
            {
                out += "\n" + tok.text + "\n";
                prev.clear();
                continue;
            }
            if (tok.type == T_PUNCT)
            {
                // Cast `(type)operand` -> `type(operand)`
                if (tok.text == "(" && IsIdent(t, i + 1) && Is(t, i + 2, ")") && i + 3 < e
                    && (!MapType(t[i + 1].text).empty() || m_structNames.count(t[i + 1].text))
                    && (t[i + 3].type == T_IDENT || t[i + 3].type == T_NUMBER || Is(t, i + 3, "(") || Is(t, i + 3, "-")))
                {
                    const size_t oe = OperandEnd(t, i + 3, e);
                    if (m_structNames.count(t[i + 1].text))
                        put(ZeroStruct(t[i + 1].text)); // `(VS_OUT)0`
                    else
                    {
                        put(GlslType(t[i + 1].text));
                        put("(");
                        out += Emit(t, i + 3, oe);
                        prev = "x";
                        put(")");
                    }
                    i = oe - 1;
                    continue;
                }
                put(tok.text);
                if (tok.text == "{" || tok.text == "}" || tok.text == ";") { out += "\n"; prev.clear(); }
                continue;
            }
            if (tok.type == T_NUMBER) { put(tok.text); continue; }

            const std::string& w = tok.text;
            if (w == "static" || w == "inline" || w == "extern" || w == "shared" || w == "volatile" || w == "uniform")
                continue;
            const std::string mapped = MapType(w);
            if (!mapped.empty())
            {
                // Matrix constructor from rows -> GLSL takes columns
                if (IsMatrixType(mapped) && Is(t, i + 1, "("))
                {
                    const size_t c = Match(t, i + 1);
                    if (SplitCommas(t, i + 2, c).size() > 1)
                    {
                        put("transpose");
                        put("(");
                        put(mapped);
                        put("(");
                        out += Emit(t, i + 2, c);
                        prev = "x";
                        put(")");
                        put(")");
                        i = c;
                        continue;
                    }
                }
                put(mapped);
                continue;
            }
            if (w == "dot" && Is(t, i + 1, "("))
            {
                put("cg_dot"); // Cg truncates mismatched vectors, see prelude
                continue;
            }
            put(Rename(w));
        }
        return out;
    }

    // ---------------------------------------------------------------- main()

    static std::string Swizzle(const std::string& vec4Expr, const std::string& glslType)
    {
        const int n = Components(glslType);
        std::string e = vec4Expr;
        if (n == 1) e += ".x";
        else if (n == 2) e += ".xy";
        else if (n == 3) e += ".xyz";
        if (glslType[0] == 'i' || glslType[0] == 'b' || glslType == "int") return glslType + "(" + e + ")";
        return e;
    }

    static std::string NormalizeSemantic(std::string sem)
    {
        if (sem == "SV_POSITION" || sem == "HPOS" || sem == "POSITION0") return "POSITION";
        if (sem == "COLOR" || sem == "COL" || sem == "COL0" || sem == "DIFFUSE" || sem == "SV_TARGET" || sem == "SV_TARGET0") return "COLOR0";
        if (sem == "COL1" || sem == "SPECULAR") return "COLOR1";
        if (sem.compare(0, 9, "SV_TARGET") == 0) return "COLOR" + sem.substr(9);
        if (sem == "TEXCOORD" || sem == "TEX0") return "TEXCOORD0";
        if (sem == "NORMAL0") return "NORMAL";
        if (sem == "TANGENT0") return "TANGENT";
        if (sem == "BINORMAL0") return "BINORMAL";
        if (sem == "BLENDWEIGHT0") return "BLENDWEIGHT";
        if (sem == "BLENDINDICES0") return "BLENDINDICES";
        if (sem == "DEPTH0" || sem == "SV_DEPTH") return "DEPTH";
        if (sem == "VPOS" || sem == "WPOS") return "POSITION";
        return sem;
    }

    std::string AttributeName(const std::string& sem)
    {
        if (sem == "POSITION") return "vertex";
        if (sem == "NORMAL") return "normal";
        if (sem == "COLOR0") return "colour";
        if (sem == "COLOR1") return "secondary_colour";
        if (sem.compare(0, 8, "TEXCOORD") == 0) return "uv" + sem.substr(8);
        if (sem == "TANGENT") return "tangent";
        if (sem == "BINORMAL") return "binormal";
        if (sem == "BLENDWEIGHT") return "blendWeights";
        if (sem == "BLENDINDICES") return "blendIndices";
        return std::string();
    }

    void DeclareIo(const std::string& decl)
    {
        if (m_declared.insert(decl).second) m_ioDecls += decl + "\n";
    }

    const char* InQual() const { return (m_version >= 130) ? (m_fragment ? "in" : "in") : (m_fragment ? "varying" : "attribute"); }
    const char* VaryingOutQual() const { return (m_version >= 130) ? "out" : "varying"; }

    /// Expression reading input `sem` as `glslType`; "" if unsupported.
    std::string InputExpr(const std::string& rawSem, const std::string& glslType)
    {
        const std::string sem = NormalizeSemantic(rawSem);
        if (!m_fragment)
        {
            const std::string attr = AttributeName(sem);
            if (attr.empty()) return std::string();
            DeclareIo(std::string(InQual()) + " vec4 " + attr + ";");
            return Swizzle(attr, glslType);
        }
        if (sem == "POSITION") return Swizzle("gl_FragCoord", glslType);
        if (sem == "VFACE" || sem == "FACE" || sem == "SV_ISFRONTFACE") return "(gl_FrontFacing ? 1.0 : -1.0)";
        DeclareIo(std::string(InQual()) + " vec4 cg_v_" + sem + ";");
        return Swizzle("cg_v_" + sem, glslType);
    }

    /// Statement writing `value` (of `glslType`) to output `sem`; "" if unsupported.
    std::string OutputStmt(const std::string& rawSem, const std::string& value, const std::string& glslType)
    {
        const std::string sem = NormalizeSemantic(rawSem);
        if (!m_fragment)
        {
            if (sem == "POSITION") return "gl_Position = cg_to4(" + value + ");";
            if (sem == "PSIZE") return "gl_PointSize = cg_float(" + value + ");";
            DeclareIo(std::string(VaryingOutQual()) + " vec4 cg_v_" + sem + ";");
            m_written.insert(sem);
            return "cg_v_" + sem + " = cg_to4(" + value + ");";
        }
        if (sem == "DEPTH") return "gl_FragDepth = cg_float(" + value + ");";
        if (sem.compare(0, 5, "COLOR") == 0)
        {
            const std::string n = (sem.size() > 5) ? sem.substr(5) : "0";
            if (m_version >= 130)
            {
                DeclareIo("layout(location = " + n + ") out vec4 cg_FragColor" + n + ";");
                return "cg_FragColor" + n + " = cg_to4(" + value + ");";
            }
            return "gl_FragData[" + n + "] = cg_to4(" + value + ");";
        }
        return std::string();
    }

    std::string BuildMain()
    {
        std::string pre, post, args;
        int idx = 0;
        for (const Var& p : m_entry->params)
        {
            if (IsEntryUniform(p)) continue;
            const std::string local = "cg_a" + std::to_string(idx++);
            const std::string type = GlslType(p.type);
            pre += "    " + type + " " + local + p.arr + ";\n";
            args += (args.empty() ? "" : ", ") + local;
            const bool in = !p.Has("out");
            const bool out = p.Has("out") || p.Has("inout");
            const Struct* st = FindStruct(p.type);
            if (st)
            {
                for (const Var& m : st->members)
                {
                    if (m.sem.empty()) continue;
                    const std::string mt = GlslType(m.type);
                    const std::string field = local + "." + Rename(m.name);
                    if (in)
                    {
                        const std::string e = InputExpr(m.sem, mt);
                        if (!e.empty()) pre += "    " + field + " = " + e + ";\n";
                    }
                    if (out)
                    {
                        const std::string s = OutputStmt(m.sem, field, mt);
                        if (!s.empty()) post += "    " + s + "\n";
                    }
                }
            }
            else
            {
                if (in)
                {
                    const std::string e = InputExpr(p.sem, type);
                    if (!e.empty()) pre += "    " + local + " = " + e + ";\n";
                }
                if (out)
                {
                    const std::string s = OutputStmt(p.sem, local, type);
                    if (!s.empty()) post += "    " + s + "\n";
                }
            }
        }
        std::string call = "cg_entry(" + args + ");";
        const std::string retType = GlslType(m_entry->ret);
        if (retType != "void")
        {
            call = retType + " cg_r = " + call;
            if (const Struct* st = FindStruct(m_entry->ret))
            {
                for (const Var& m : st->members)
                {
                    if (m.sem.empty()) continue;
                    const std::string s = OutputStmt(m.sem, "cg_r." + Rename(m.name), GlslType(m.type));
                    if (!s.empty()) post += "    " + s + "\n";
                }
            }
            else
            {
                const std::string s = OutputStmt(m_entry->retSem.empty() ? std::string(m_fragment ? "COLOR0" : "POSITION") : m_entry->retSem, "cg_r", retType);
                if (!s.empty()) post += "    " + s + "\n";
            }
        }

        // Vertex shaders write every common varying, so any Cg fragment shader links with them
        // (GLSL fails to link a fragment shader reading a varying the vertex shader lacks; D3D/Cg just read zero).
        std::string defaults;
        if (!m_fragment)
        {
            for (const char* sem : { "TEXCOORD0", "TEXCOORD1", "TEXCOORD2", "TEXCOORD3", "TEXCOORD4", "TEXCOORD5", "TEXCOORD6", "TEXCOORD7", "COLOR0", "COLOR1" })
            {
                if (m_written.count(sem)) continue;
                DeclareIo(std::string(VaryingOutQual()) + " vec4 cg_v_" + sem + ";");
                defaults += std::string("    cg_v_") + sem + " = vec4(0.0);\n";
            }
        }
        return "void main()\n{\n" + pre + "    " + call + "\n" + post + defaults + "}\n";
    }

    const Struct* FindStruct(const std::string& name) const
    {
        for (const Struct& s : m_structs) if (s.name == name) return &s;
        return nullptr;
    }

    // ---------------------------------------------------------------- prelude

    std::string Prelude() const
    {
        const bool v130 = m_version >= 130;
        std::string p = "#version " + std::to_string(m_version) + (v130 ? " core\n" : "\n");
        p += "// Translated from Cg by Rigs of Rods (no Cg plugin)\n";
        const std::string t2 = v130 ? "texture" : "texture2D";
        const std::string t1 = v130 ? "texture" : "texture1D";
        const std::string t3 = v130 ? "texture" : "texture3D";
        const std::string tc = v130 ? "texture" : "textureCube";
        const std::string tp = v130 ? "textureProj" : "texture2DProj";
        p += R"(
#define saturate(x) clamp((x), 0.0, 1.0)
#define lerp mix
#define frac fract
#define rsqrt inversesqrt
#define atan2 atan
#define ddx dFdx
#define ddy dFdy
#define log10(x) (log2(x) * 0.30102999566)
float cg_float(float x) { return x; }
float cg_float(int x) { return float(x); }
float cg_float(bool x) { return x ? 1.0 : 0.0; }
float cg_float(vec2 x) { return x.x; }
float cg_float(vec3 x) { return x.x; }
float cg_float(vec4 x) { return x.x; }
int cg_int(int x) { return x; }
int cg_int(float x) { return int(x); }
int cg_int(bool x) { return x ? 1 : 0; }
vec2 cg_vec2(float x) { return vec2(x); }
vec2 cg_vec2(int x) { return vec2(float(x)); }
vec2 cg_vec2(vec2 x) { return x; }
vec2 cg_vec2(vec3 x) { return x.xy; }
vec2 cg_vec2(vec4 x) { return x.xy; }
vec3 cg_vec3(float x) { return vec3(x); }
vec3 cg_vec3(int x) { return vec3(float(x)); }
vec3 cg_vec3(vec3 x) { return x; }
vec3 cg_vec3(vec4 x) { return x.xyz; }
vec4 cg_vec4(float x) { return vec4(x); }
vec4 cg_vec4(int x) { return vec4(float(x)); }
vec4 cg_vec4(vec4 x) { return x; }
mat2 cg_mat2(mat2 x) { return x; }
mat2 cg_mat2(mat3 x) { return mat2(x); }
mat2 cg_mat2(mat4 x) { return mat2(x); }
mat3 cg_mat3(mat3 x) { return x; }
mat3 cg_mat3(mat4 x) { return mat3(x); }
mat4 cg_mat4(mat4 x) { return x; }
vec4 cg_to4(float x) { return vec4(x, 0.0, 0.0, 1.0); }
vec4 cg_to4(vec2 x) { return vec4(x, 0.0, 1.0); }
vec4 cg_to4(vec3 x) { return vec4(x, 1.0); }
vec4 cg_to4(vec4 x) { return x; }
vec2 mul(mat2 m, vec2 v) { return m * v; }
vec3 mul(mat3 m, vec3 v) { return m * v; }
vec4 mul(mat4 m, vec4 v) { return m * v; }
vec2 mul(vec2 v, mat2 m) { return v * m; }
vec3 mul(vec3 v, mat3 m) { return v * m; }
vec4 mul(vec4 v, mat4 m) { return v * m; }
mat2 mul(mat2 a, mat2 b) { return a * b; }
mat3 mul(mat3 a, mat3 b) { return a * b; }
mat4 mul(mat4 a, mat4 b) { return a * b; }
vec3 mul(vec4 v, mat3x4 m) { return v * m; }
vec4 mul(mat3x4 m, vec3 v) { return m * v; }
vec4 lit(float NdotL, float NdotH, float m)
{
    float specular = (NdotL > 0.0) ? pow(max(0.0, NdotH), m) : 0.0;
    return vec4(1.0, max(0.0, NdotL), specular, 1.0);
}
float fmod(float x, float y) { return x - y * sign(x / y) * floor(abs(x / y)); }
vec2 fmod(vec2 x, vec2 y) { return x - y * sign(x / y) * floor(abs(x / y)); }
vec3 fmod(vec3 x, vec3 y) { return x - y * sign(x / y) * floor(abs(x / y)); }
vec4 fmod(vec4 x, vec4 y) { return x - y * sign(x / y) * floor(abs(x / y)); }
void sincos(float x, out float s, out float c) { s = sin(x); c = cos(x); }
void sincos(vec2 x, out vec2 s, out vec2 c) { s = sin(x); c = cos(x); }
void sincos(vec3 x, out vec3 s, out vec3 c) { s = sin(x); c = cos(x); }
void sincos(vec4 x, out vec4 s, out vec4 c) { s = sin(x); c = cos(x); }
)";
        // dot() of vectors with different sizes: Cg truncates the longer one
        for (int a = 1; a <= 4; ++a)
        {
            for (int b = 1; b <= 4; ++b)
            {
                const int n = std::min(a, b);
                auto type = [](int c) { return (c == 1) ? std::string("float") : "vec" + std::to_string(c); };
                auto cut = [&](const char* v, int c) { return (c == n) ? std::string(v) : std::string(v) + "." + std::string("xyzw").substr(0, n); };
                p += "float cg_dot(" + type(a) + " a, " + type(b) + " b) { return dot(" + cut("a", a) + ", " + cut("b", b) + "); }\n";
            }
        }
        if (!v130)
            p += "float round(float x) { return floor(x + 0.5); }\nvec2 round(vec2 x) { return floor(x + 0.5); }\n"
                 "vec3 round(vec3 x) { return floor(x + 0.5); }\nvec4 round(vec4 x) { return floor(x + 0.5); }\n";
        p += "vec4 tex1D(sampler1D s, float uv) { return " + t1 + "(s, uv); }\n";
        p += "vec4 tex1D(sampler1D s, vec2 uv) { return " + t1 + "(s, uv.x); }\n";
        p += "vec4 tex2D(sampler2D s, vec2 uv) { return " + t2 + "(s, uv); }\n";
        p += "vec4 tex2D(sampler2D s, vec3 uv) { return " + t2 + "(s, uv.xy); }\n";
        p += "vec4 tex2D(sampler2D s, vec4 uv) { return " + t2 + "(s, uv.xy); }\n";
        p += "vec4 tex2Dproj(sampler2D s, vec4 uv) { return " + tp + "(s, uv); }\n";
        p += "vec4 tex2Dproj(sampler2D s, vec3 uv) { return " + tp + "(s, uv); }\n";
        p += "vec4 tex3D(sampler3D s, vec3 uv) { return " + t3 + "(s, uv); }\n";
        p += "vec4 texCUBE(samplerCube s, vec3 uv) { return " + tc + "(s, uv); }\n";
        p += "vec4 texCUBE(samplerCube s, vec4 uv) { return " + tc + "(s, uv.xyz); }\n";
        if (v130)
        {
            p += "vec4 tex2D(sampler2D s, vec2 uv, vec2 dx, vec2 dy) { return textureGrad(s, uv, dx, dy); }\n";
            p += "vec4 tex2Dlod(sampler2D s, vec4 uv) { return textureLod(s, uv.xy, uv.w); }\n";
            p += "vec4 texCUBElod(samplerCube s, vec4 uv) { return textureLod(s, uv.xyz, uv.w); }\n";
            if (m_fragment)
            {
                p += "vec4 tex2Dbias(sampler2D s, vec4 uv) { return texture(s, uv.xy, uv.w); }\n";
                p += "vec4 texCUBEbias(samplerCube s, vec4 uv) { return texture(s, uv.xyz, uv.w); }\n";
            }
        }
        else if (m_fragment)
        {
            p += "vec4 tex2D(sampler2D s, vec2 uv, vec2 dx, vec2 dy) { return texture2D(s, uv); }\n";
            p += "vec4 tex2Dlod(sampler2D s, vec4 uv) { return texture2D(s, uv.xy); }\n";
            p += "vec4 texCUBElod(samplerCube s, vec4 uv) { return textureCube(s, uv.xyz); }\n";
            p += "vec4 tex2Dbias(sampler2D s, vec4 uv) { return texture2D(s, uv.xy, uv.w); }\n";
            p += "vec4 texCUBEbias(samplerCube s, vec4 uv) { return textureCube(s, uv.xyz, uv.w); }\n";
        }
        else
        {
            // texture*Lod: GLSL 1.20 vertex shaders only - emitted only when used (some compilers want an extension)
            if (Uses("tex2Dlod")) p += "vec4 tex2Dlod(sampler2D s, vec4 uv) { return texture2DLod(s, uv.xy, uv.w); }\n";
            if (Uses("texCUBElod")) p += "vec4 texCUBElod(samplerCube s, vec4 uv) { return textureCubeLod(s, uv.xyz, uv.w); }\n";
        }
        if (m_fragment)
        {
            p += "void clip(float x) { if (x < 0.0) discard; }\n";
            p += "void clip(vec2 x) { if (any(lessThan(x, vec2(0.0)))) discard; }\n";
            p += "void clip(vec3 x) { if (any(lessThan(x, vec3(0.0)))) discard; }\n";
            p += "void clip(vec4 x) { if (any(lessThan(x, vec4(0.0)))) discard; }\n";
        }
        return p + "\n";
    }

    bool Uses(const char* id) const
    {
        for (const Tok& tok : m_t) if (tok.type == T_IDENT && tok.text == id) return true;
        return false;
    }

    Toks m_t;
    std::string m_entryName;
    bool m_fragment;
    int m_version;
    std::vector<Item> m_items;
    std::vector<Struct> m_structs;
    std::set<std::string> m_structNames;
    std::vector<Func> m_funcs;
    std::map<std::string, Var> m_globals;
    std::set<std::string> m_reachable;
    const Func* m_entry = nullptr;
    std::string m_ioDecls;
    std::set<std::string> m_declared;
    std::set<std::string> m_written;
};

} // namespace detail

/// Translates the Cg program `entry` in `cgSource` to GLSL `version` (120 for the OpenGL, 330 for the OpenGL 3+ render system).
inline Result Translate(const std::string& cgSource, const std::string& entry, bool fragment, int version)
{
    detail::Translator tr(cgSource, entry, fragment, version);
    return tr.Run();
}

} // namespace CgToGlsl
} // namespace RoR
