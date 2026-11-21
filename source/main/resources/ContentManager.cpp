/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2018 Petr Ohlidal

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

#include "ContentManager.h"
#include "MeshVertexColourFix.h"

#include "Application.h"
#include "ErrorUtils.h"
#include "SoundScriptManager.h"
#include "SkinFileFormat.h"
#include "Language.h"
#include "PlatformUtils.h"
#include "AppContext.h"
#include "GfxScene.h"
#include "CacheSystem.h"
#include "OgreShaderParticleRenderer.h"

#include <Overlay/OgreOverlayManager.h>
#include <Overlay/OgreOverlay.h>
#include <OgreSGTechniqueResolverListener.h>
#include <RTShaderSystem/OgreRTShaderSystem.h>

// Removed by Skybon as part of OGRE 1.9 port 
// Disabling temporarily for 1.8.1 as well. ~ only_a_ptr, 2015-11
// TODO: Study the system, then re-enable or remove entirely.
//#include "OgreBoxEmitterFactory.h"

#ifdef USE_ANGELSCRIPT
#include "FireExtinguisherAffectorFactory.h"
#include "ExtinguishableFireAffectorFactory.h"
#endif // USE_ANGELSCRIPT

#include "Utils.h"

#include <OgreFileSystem.h>
#include <OgreHighLevelGpuProgramManager.h>
#include <OgreScriptCompiler.h>
#include "CgToGlsl.h"
#include "D3D11ShaderFix.h"
#include "FfpFragmentFix.h"
#include <regex>
#include <map>
#include <set>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <OgreMeshLodGenerator.h>

using namespace Ogre;
using namespace RoR;

// ================================================================================
// Static variables
// ================================================================================

#define DECLARE_RESOURCE_PACK(_FIELD_, _NAME_, _RESOURCE_GROUP_) \
    const ContentManager::ResourcePack ContentManager::ResourcePack::_FIELD_(_NAME_, _RESOURCE_GROUP_);

DECLARE_RESOURCE_PACK( OGRE_CORE,             "OgreCore",             "OgreCoreRG");
DECLARE_RESOURCE_PACK( WALLPAPERS,            "wallpapers",           "Wallpapers");
DECLARE_RESOURCE_PACK( AIRFOILS,              "airfoils",             "AirfoilsRG");
DECLARE_RESOURCE_PACK( CUBEMAPS,              "cubemaps",             "CubemapsRG");
DECLARE_RESOURCE_PACK( DASHBOARDS,            "dashboards",           "DashboardsRG");
DECLARE_RESOURCE_PACK( FAMICONS,              "famicons",             "FamiconsRG");
DECLARE_RESOURCE_PACK( FLAGS,                 "flags",                "FlagsRG");
DECLARE_RESOURCE_PACK( FONTS,                 "fonts",                "FontsRG");
DECLARE_RESOURCE_PACK( HYDRAX,                "hydrax",               "HydraxRG");
DECLARE_RESOURCE_PACK( ICONS,                 "icons",                "IconsRG");
DECLARE_RESOURCE_PACK( MATERIALS,             "materials",            "MaterialsRG");
DECLARE_RESOURCE_PACK( MESHES,                "meshes",               "MeshesRG");
DECLARE_RESOURCE_PACK( MYGUI,                 "mygui",                "MyGuiRG");
DECLARE_RESOURCE_PACK( OVERLAYS,              "overlays",             "OverlaysRG");
DECLARE_RESOURCE_PACK( PAGED,                 "paged",                "PagedRG");
DECLARE_RESOURCE_PACK( PARTICLES,             "particles",            "ParticlesRG");
DECLARE_RESOURCE_PACK( PSSM,                  "pssm",                 "PssmRG");
DECLARE_RESOURCE_PACK( RTSHADER,              "rtshader",             "RtShaderRG");
DECLARE_RESOURCE_PACK( SCRIPTS,               "scripts",              "ScriptsRG");
DECLARE_RESOURCE_PACK( SOUNDS,                "sounds",               "SoundsRG");
DECLARE_RESOURCE_PACK( TEXTURES,              "textures",             "TexturesRG");
DECLARE_RESOURCE_PACK( SKYX,                  "SkyX",                 "SkyXRG");

// ================================================================================
// Functions
// ================================================================================

void ContentManager::AddResourcePack(ResourcePack const& resource_pack, std::string const& override_rgn)
{
    Ogre::ResourceGroupManager& rgm = Ogre::ResourceGroupManager::getSingleton();

    Ogre::String rg_name;
    if (!override_rgn.empty()) // Custom RG defined?
    {
        rg_name = override_rgn;
    }
    else // Use default RG
    {
        if (rgm.resourceGroupExists(resource_pack.resource_group_name)) // Already loaded?
        {
            return; // Nothing to do, nothing to report
        }
        rg_name = resource_pack.resource_group_name;
    }

    std::stringstream log_msg;
    log_msg << "[RoR|ContentManager] Loading resource pack \"" << resource_pack.name << "\" to group \"" << rg_name << "\"";
    std::string dir_path = PathCombine(App::sys_resources_dir->getStr(), resource_pack.name);
    std::string zip_path = dir_path + ".zip";
    if (FileExists(zip_path))
    {
        log_msg << " (ZIP archive)";
        LOG(log_msg.str());
        rgm.addResourceLocation(zip_path, "Zip", rg_name);
    }
    else
    {
        if (FolderExists(dir_path))
        {
            log_msg << " (directory)";
            LOG(log_msg.str());
            rgm.addResourceLocation(dir_path, "FileSystem", rg_name);
        }
        else
        {
            log_msg << " failed, data not found.";
            throw std::runtime_error(log_msg.str());
        }
    }

    if (override_rgn.empty()) // Only init the default RG
    {
        rgm.initialiseResourceGroup(rg_name);
    }
}

void ContentManager::InitContentManager()
{
    ResourceGroupManager::getSingleton().addResourceLocation(
        App::sys_config_dir->getStr(), "FileSystem", RGN_CONFIG, /*recursive=*/false, /*readOnly=*/false);
    ResourceGroupManager::getSingleton().addResourceLocation(
        App::sys_savegames_dir->getStr(), "FileSystem", RGN_SAVEGAMES, /*recursive=*/false, /*readOnly=*/false);
    ResourceGroupManager::getSingleton().addResourceLocation(
        App::sys_scripts_dir->getStr(), "FileSystem", RGN_SCRIPTS, /*recursive:*/false, /*readonly:*/false);
    ResourceGroupManager::getSingleton().addResourceLocation(
        App::sys_logs_dir->getStr(), "FileSystem", RGN_LOGS, /*recursive:*/false, /*readonly:*/false);

    // Server scripts must be isolated from any others.
    ResourceGroupManager::getSingleton().createResourceGroup(RGN_SERVER_SCRIPTS, /* inGlobalPool: */false);
    ResourceGroupManager::getSingleton().addResourceLocation(
        App::sys_server_scripts_dir->getStr(), "FileSystem", RGN_SERVER_SCRIPTS, /*recursive:*/false, /*readonly:*/false);
    // Recurse one level down, that's all the https://github.com/RigsOfRods/RoRServerScripts package needs.
    // NOTE: We can't use OGRE's 'recursive' mode because it prepends subdir names to the resource name, and we need bare filenames.
    FileInfoListPtr dirs = ResourceGroupManager::getSingleton().findResourceFileInfo(RGN_SERVER_SCRIPTS, "*", /*dirs:*/true);
    for (const auto& dir_fileinfo : *dirs)
    {
        if (!dir_fileinfo.archive)
            continue;
        String fullpath = PathCombine(dir_fileinfo.archive->getName(), dir_fileinfo.filename);
        ResourceGroupManager::getSingleton().addResourceLocation(fullpath, "FileSystem", RGN_SERVER_SCRIPTS, /*recursive:*/false, /*readonly:*/false);
    }

    Ogre::ScriptCompilerManager::getSingleton().setListener(this);
    Ogre::ResourceGroupManager::getSingleton().addResourceGroupListener(this); // see resourceGroupScriptingEnded()
    MeshVertexColourFix::Install(); // RTSS: materials tracking vertex colours on meshes without them

    // Initialize "managed materials" first
    //   These are base materials referenced by user content
    //   They must be initialized before any content is loaded,
    //   otherwise material links are unresolved and loading ends with an exception
    this->InitManagedMaterials(RGN_MANAGED_MATS);

    // set listener if none has already been set
    if (!Ogre::ResourceGroupManager::getSingleton().getLoadingListener())
        Ogre::ResourceGroupManager::getSingleton().setLoadingListener(this);

    // by default, display everything in the depth map
    Ogre::MovableObject::setDefaultVisibilityFlags(DEPTHMAP_ENABLED);


    this->AddResourcePack(ResourcePack::MYGUI);
    this->AddResourcePack(ResourcePack::DASHBOARDS);


#ifdef _WIN32
    // TODO: FIX UNDER LINUX!
    // register particle classes
    LOG("RoR|ContentManager: Registering Particle Box Emitter");
    ParticleSystemRendererFactory* mParticleSystemRendererFact = OGRE_NEW ShaderParticleRendererFactory();
    ParticleSystemManager::getSingleton().addRendererFactory(mParticleSystemRendererFact);

    // Removed by Skybon as part of OGRE 1.9 port 
    // Disabling temporarily for 1.8.1 as well.  ~ only_a_ptr, 2015-11
    //ParticleEmitterFactory *mParticleEmitterFact = OGRE_NEW BoxEmitterFactory();
    //ParticleSystemManager::getSingleton().addEmitterFactory(mParticleEmitterFact);

#endif // _WIN32

#ifdef USE_ANGELSCRIPT
    // FireExtinguisherAffector
    ParticleAffectorFactory* pAffFact = OGRE_NEW FireExtinguisherAffectorFactory();
    ParticleSystemManager::getSingleton().addAffectorFactory(pAffFact);

    // ExtinguishableFireAffector
    pAffFact = OGRE_NEW ExtinguishableFireAffectorFactory();
    ParticleSystemManager::getSingleton().addAffectorFactory(pAffFact);
#endif // USE_ANGELSCRIPT

    // sound is a bit special as we mark the base sounds so we don't clear them accidentally later on
#ifdef USE_OPENAL
    LOG("RoR|ContentManager: Creating Sound Manager");
    App::CreateSoundScriptManager();
    App::GetSoundScriptManager()->setLoadingBaseSounds(true);
#endif // USE_OPENAL

    AddResourcePack(ResourcePack::SOUNDS);

    // streams path, to be processed later by the cache system
    LOG("RoR|ContentManager: Loading filesystems");

    // set default mipmap level (NB some APIs ignore this)
    if (TextureManager::getSingletonPtr())
        TextureManager::getSingleton().setDefaultNumMipmaps(5);

    TextureFilterOptions tfo = TFO_NONE;
    switch (App::gfx_texture_filter->getEnum<GfxTexFilter>())
    {
    case GfxTexFilter::ANISOTROPIC: tfo = TFO_ANISOTROPIC;        break;
    case GfxTexFilter::TRILINEAR:   tfo = TFO_TRILINEAR;          break;
    case GfxTexFilter::BILINEAR:    tfo = TFO_BILINEAR;           break;
    case GfxTexFilter::NONE:        tfo = TFO_NONE;               break;
    }
    MaterialManager::getSingleton().setDefaultAnisotropy(Math::Clamp(App::gfx_anisotropy->getInt(), 1, 16));
    MaterialManager::getSingleton().setDefaultTextureFiltering(tfo);

    // load all resources now, so the zip files are also initiated
    LOG("RoR|ContentManager: Calling initialiseAllResourceGroups()");
    try
    {
        ResourceGroupManager::getSingleton().initialiseAllResourceGroups();
    }
    catch (Ogre::Exception& e)
    {
        LOG("RoR|ContentManager: catched error while initializing Resource groups: " + e.getFullDescription());
    }
#ifdef USE_OPENAL
    App::GetSoundScriptManager()->setLoadingBaseSounds(false);
#endif // USE_OPENAL

    new Ogre::MeshLodGenerator();
}

void ContentManager::InitModCache(CacheValidity validity)
{
    // Sets up RGN_CONTENT which encompasses all mods, scans it for changes and deletes it again.
    // IMPORTANT NOTE ON 'readOnly' FLAG:
    //   We need mods in subdirs to be writable for the Tuning menu to work.
    //   Apart from `Resources` and resource groups, OGRE also keeps `Archives` in `ArchiveManager`
    //   These aren't unloaded on destroying resource groups, and keep a 'readOnly' flag (defaults to true).
    //   Upon loading/creating new resource groups, OGRE complains (=assert on Debug, exception on Release) if the submitted flag doesn't match.
    //   It's possible to manually unload archives to reset the flag, but for simplicity we just always load subdirs as 'writable', even during modcache update.
    // ------------------------------------------------------------------------------------------

    ResourceGroupManager::getSingleton().addResourceLocation(
        App::sys_cache_dir->getStr(), "FileSystem", RGN_CACHE, /*recursive=*/false, /*readOnly=*/false);
    ResourceGroupManager::getSingleton().addResourceLocation(
        App::sys_thumbnails_dir->getStr(), "FileSystem", RGN_THUMBNAILS, /*recursive=*/false, /*readOnly=*/false);
    ResourceGroupManager::getSingleton().addResourceLocation(
        App::sys_repo_attachments_dir->getStr(), "FileSystem", RGN_REPO_ATTACHMENTS, /*recursive=*/false, /*readOnly=*/false);

    // Add top-level ZIPs/directories to RGN_CONTENT (non-recursive)

    if (!App::app_extra_mod_path->getStr().empty())
    {
        std::string extra_mod_path = App::app_extra_mod_path->getStr();
        ResourceGroupManager::getSingleton().addResourceLocation(extra_mod_path           , "FileSystem", RGN_CONTENT);
    }
    for (const std::string& dirname : App::GetCacheSystem()->GetContentDirs())
    {
        ResourceGroupManager::getSingleton().addResourceLocation(PathCombine(App::sys_user_dir->getStr(), dirname), "FileSystem", RGN_CONTENT);
    }
    ResourceGroupManager::getSingleton().addResourceLocation(PathCombine(App::sys_process_dir->getStr(), "content") , "FileSystem", RGN_CONTENT);
    std::string objects = PathCombine("resources", "beamobjects.zip");
    ResourceGroupManager::getSingleton().addResourceLocation(PathCombine(App::sys_process_dir->getStr(), objects)   , "Zip"       , RGN_CONTENT);
    std::string dashboards = PathCombine("resources", "dashboards.zip");
    ResourceGroupManager::getSingleton().addResourceLocation(PathCombine(App::sys_process_dir->getStr(), dashboards), "Zip", RGN_CONTENT);
    std::string gadgets = PathCombine("resources", "gadgets.zip");
    ResourceGroupManager::getSingleton().addResourceLocation(PathCombine(App::sys_process_dir->getStr(), gadgets), "Zip", RGN_CONTENT);
    
    // Create RGN_TEMP in recursive mode to find all subdirectories.

    ResourceGroupManager::getSingleton().createResourceGroup(RGN_TEMP, false);
    if (!App::app_extra_mod_path->getStr().empty())
    {
        std::string extra_mod_path = App::app_extra_mod_path->getStr();
        ResourceGroupManager::getSingleton().addResourceLocation(extra_mod_path           , "FileSystem", RGN_TEMP, true);
    }
    for (const std::string& dirname : App::GetCacheSystem()->GetContentDirs())
    {
        ResourceGroupManager::getSingleton().addResourceLocation(PathCombine(App::sys_user_dir->getStr(), dirname), "FileSystem", RGN_TEMP, true);
    }
    ResourceGroupManager::getSingleton().addResourceLocation(PathCombine(App::sys_process_dir->getStr(), "content") , "FileSystem", RGN_TEMP, true);

    // Traverse RGN_TEMP and add all subdirectories to RGN_CONTENT.
    // NOTE we can't use OGRE's 'recursive' mode because it prepends subdir names to the resource name, and we need bare filenames.

    FileInfoListPtr dirs = ResourceGroupManager::getSingleton().findResourceFileInfo(RGN_TEMP, "*", /*dirs:*/true);
    for (const auto& dir_fileinfo : *dirs)
    {
        if (!dir_fileinfo.archive)
            continue;
        String fullpath = PathCombine(dir_fileinfo.archive->getName(), dir_fileinfo.filename);
        ResourceGroupManager::getSingleton().addResourceLocation(fullpath, "FileSystem", RGN_CONTENT, /*recursive:*/false, /*readonly:*/false);
    }
    ResourceGroupManager::getSingleton().destroyResourceGroup(RGN_TEMP);

    // Traverse RGN_CONTENT and detect updates

    if (validity == CacheValidity::UNKNOWN)
    {
        validity = App::GetCacheSystem()->EvaluateCacheValidity(); // Must be called while RGN_CONTENT is alive.
    }
    App::GetCacheSystem()->LoadModCache(validity);

    ResourceGroupManager::getSingleton().destroyResourceGroup(RGN_CONTENT);
    
}

Ogre::DataStreamPtr ContentManager::resourceLoading(const Ogre::String& name, const Ogre::String& group, Ogre::Resource* resource)
{
    return Ogre::DataStreamPtr();
}

void ContentManager::resourceStreamOpened(const Ogre::String& name, const Ogre::String& group, Ogre::Resource* resource, Ogre::DataStreamPtr& dataStream)
{
}

bool ContentManager::resourceCollision(Ogre::Resource* resource, Ogre::ResourceManager* resourceManager)
{
    // RoR loads each resource bundle (see CacheSystem.h for info)
    // into dedicated resource group outside the global pool [see CacheSystem::LoadResource()]
    // This means resource collision is pretty much content creator's fault, with 2 exceptions:
    // * asset packs (introduced 2024) are mixed into the requesting mod's resource group.
    // * bundled resources (e.g. beamobjects.zip) are also mixed into the mod's resource group.
    RoR::LogFormat("[RoR|ContentManager] Skipping resource with duplicate name: '%s' (origin: '%s')",
        resource->getName().c_str(), resource->getOrigin().c_str());
    return false; // Instruct OGRE to drop the new resource and keep the original.
}

void ContentManager::resourceGroupScriptingEnded(const Ogre::String& groupName)
{
    // Render systems without fixed-function pipeline (D3D11, GL3+, Vulkan): passes with only a vertex program
    // need a generated fragment program, otherwise OGRE throws when the material is first rendered.
    FfpFragmentFix::FixGroup(groupName);
}

static bool IsD3D11Active()
{
    Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
    return rs && rs->getName().find("Direct3D11") != Ogre::String::npos;
}

static bool IsD3D9Active()
{
    Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
    return rs && rs->getName().find("Direct3D9") != Ogre::String::npos;
}

/// D3D9 without the Cg plugin (OGRE 14.6 dependencies are built without Cg): Cg programs are compiled
/// by the D3D9 HLSL compiler instead - the Cg sources are D3D9-style HLSL (see D3D11ShaderFix::CgToHlsl()).
static bool IsCgViaHlslOnD3D9()
{
    return IsD3D9Active() && !Ogre::HighLevelGpuProgramManager::getSingleton().isLanguageSupported("cg");
}

/// OpenGL / OpenGL 3+ without the Cg plugin: Cg programs are translated to GLSL (see CgToGlsl.h).
/// Returns the GLSL version to translate to, 0 if not applicable.
static int GetCgViaGlslVersion()
{
    Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
    if (!rs || Ogre::HighLevelGpuProgramManager::getSingleton().isLanguageSupported("cg"))
        return 0;
    const Ogre::String& name = rs->getName();
    if (name.find("OpenGL") == Ogre::String::npos || name.find("ES") != Ogre::String::npos)
        return 0;
    return (name.find("3+") != Ogre::String::npos) ? 330 : 120;
}

/// Cg program name -> translated GLSL source, filled in `preConversion()`, consumed in `handleEvent()`.
static std::map<Ogre::String, Ogre::String> g_cg_glsl_sources;

static Ogre::ConcreteNodePtr MakeScriptNode(Ogre::ConcreteNode* parent, const Ogre::String& token, Ogre::ConcreteNodeType type)
{
    Ogre::ConcreteNodePtr node = std::make_shared<Ogre::ConcreteNode>();
    node->token = token;
    node->file = parent->file;
    node->line = parent->line;
    node->type = type;
    node->parent = parent;
    return node;
}

/// Finds the `{` child of an object node (`vertex_program x cg { ... }` or `default_params { ... }`).
static Ogre::ConcreteNode* FindScriptBody(Ogre::ConcreteNode* obj)
{
    for (auto& child : obj->children)
        if (child->type == Ogre::CNT_LBRACE) return child.get();
    return nullptr;
}

/// OpenGL without the Cg plugin: translates Cg vertex/fragment programs to GLSL while the script is parsed -
/// the entry point and sampler units are only known here. The program properties GLSL does not know
/// (`entry_point`, `profiles`) are removed, `compile_arguments -DX` become `preprocessor_defines`,
/// and every used sampler gets `param_named <sampler> int <unit>` (GLSL has no `TEXUNITn`/`register(sN)`).
static void ConvertCgProgramsForGL(Ogre::ConcreteNodeList& nodes, const Ogre::String& group, int glslVersion)
{
    for (auto& node : nodes)
    {
        const bool vp = (node->token == "vertex_program"), fp = (node->token == "fragment_program");
        if (!(vp || fp) || node->children.size() < 3)
            continue;
        auto childIt = node->children.begin();
        const Ogre::String progName = (*childIt)->token;
        if ((*std::next(childIt))->token != "cg")
            continue;
        Ogre::ConcreteNode* body = FindScriptBody(node.get());
        if (!body)
            continue;

        Ogre::String source, entry = "main", defines;
        Ogre::ConcreteNode* defaultParams = nullptr;
        for (auto& prop : body->children)
        {
            if (prop->token == "source" && !prop->children.empty())
                source = prop->children.front()->token;
            else if (prop->token == "entry_point" && !prop->children.empty())
                entry = prop->children.front()->token;
            else if (prop->token == "compile_arguments")
            {
                for (auto& arg : prop->children)
                    if (arg->token.compare(0, 2, "-D") == 0 && arg->token.size() > 2)
                        defines += (defines.empty() ? "" : ";") + arg->token.substr(2);
            }
            else if (prop->token == "default_params")
                defaultParams = prop.get();
        }
        if (source.empty())
            continue;

        RoR::CgToGlsl::Result res;
        try
        {
            Ogre::DataStreamPtr stream = Ogre::ResourceGroupManager::getSingleton().openResource(source, group);
            res = RoR::CgToGlsl::Translate(stream->getAsString(), entry, fp, glslVersion);
        }
        catch (Ogre::Exception& e)
        {
            res.error = e.getDescription();
        }
        if (!res.ok)
        {
            RoR::LogFormat("[RoR|GL] Cannot translate Cg program '%s' (%s, entry '%s') to GLSL: %s",
                progName.c_str(), source.c_str(), entry.c_str(), res.error.c_str());
            continue;
        }
        g_cg_glsl_sources[progName] = res.glsl;

        body->children.remove_if([](const Ogre::ConcreteNodePtr& prop)
            { return prop->token == "entry_point" || prop->token == "profiles" || prop->token == "compile_arguments" || prop->token == "target"; });
        if (!defines.empty())
        {
            Ogre::ConcreteNodePtr prop = MakeScriptNode(body, "preprocessor_defines", Ogre::CNT_WORD);
            prop->children.push_back(MakeScriptNode(prop.get(), defines, Ogre::CNT_WORD));
            body->children.push_back(prop);
        }

        if (!res.samplers.empty() && !defaultParams)
        {
            Ogre::ConcreteNodePtr dp = MakeScriptNode(body, "default_params", Ogre::CNT_WORD);
            dp->children.push_back(MakeScriptNode(dp.get(), "{", Ogre::CNT_LBRACE));
            dp->children.push_back(MakeScriptNode(dp.get(), "}", Ogre::CNT_RBRACE));
            body->children.push_back(dp);
            defaultParams = dp.get();
        }
        Ogre::ConcreteNode* dpBody = defaultParams ? FindScriptBody(defaultParams) : nullptr;
        for (const auto& sampler : res.samplers)
        {
            if (!dpBody)
                break;
            bool present = false;
            for (auto& param : dpBody->children)
                present |= (param->token == "param_named" && !param->children.empty() && param->children.front()->token == sampler.name);
            if (present)
                continue;
            Ogre::ConcreteNodePtr param = MakeScriptNode(dpBody, "param_named", Ogre::CNT_WORD);
            param->children.push_back(MakeScriptNode(param.get(), sampler.name, Ogre::CNT_WORD));
            param->children.push_back(MakeScriptNode(param.get(), "int", Ogre::CNT_WORD));
            param->children.push_back(MakeScriptNode(param.get(), std::to_string(sampler.unit), Ogre::CNT_WORD));
            dpBody->children.push_back(param);
        }
        RoR::LogFormat("[RoR|GL] Cg program '%s' (%s, entry '%s') translated to GLSL %d, %d sampler(s)",
            progName.c_str(), source.c_str(), entry.c_str(), glslVersion, (int)res.samplers.size());
    }
}

/// D3D9, Cg compiled as HLSL: Cg silently accepts partially written outputs (`VS_OUTPUT OUT; OUT.pos = ...; return OUT;`),
/// the HLSL compiler rejects them (error X4000: variable 'OUT' used without having been completely initialized).
/// Zero-initialize local variables of every struct type declared in the source: `T name;` -> `T name = (T)0;`
static Ogre::String ZeroInitStructLocals(const Ogre::String& src)
{
    static const std::regex structDecl(R"(\bstruct\s+([A-Za-z_]\w*))");
    std::set<std::string> types;
    for (std::sregex_iterator it(src.begin(), src.end(), structDecl), end; it != end; ++it)
        types.insert((*it)[1].str());
    if (types.empty())
        return src;

    // Walk the source statement by statement; only touch statements at brace depth >= 1 that are not inside a struct body.
    static const std::regex localDecl(R"(^(\s*)([A-Za-z_]\w*)(\s+)([A-Za-z_]\w*)\s*$)");
    Ogre::String out;
    out.reserve(src.size() + 256);
    std::vector<bool> braceIsStruct; // stack: true = struct body
    size_t stmtStart = 0;
    for (size_t i = 0; i < src.size(); ++i)
    {
        const char c = src[i];
        if (c == '/' && i + 1 < src.size() && (src[i + 1] == '/' || src[i + 1] == '*')) // skip comments
        {
            const size_t e = (src[i + 1] == '/') ? src.find('\n', i) : src.find("*/", i + 2);
            i = (e == Ogre::String::npos) ? src.size() - 1 : (src[i + 1] == '/' ? e : e + 1);
            continue;
        }
        if (c != '{' && c != '}' && c != ';')
            continue;

        const std::string stmt = src.substr(stmtStart, i - stmtStart);
        out += stmt;
        const size_t nl = stmt.find_last_of('\n');
        const std::string last = (nl == std::string::npos) ? stmt : stmt.substr(nl + 1); // skips comments/preprocessor lines before it
        std::smatch m;
        bool inStruct = false;
        for (bool b : braceIsStruct) inStruct |= b;
        if (c == ';' && !braceIsStruct.empty() && !inStruct
            && std::regex_match(last, m, localDecl) && types.count(m[2].str()))
        {
            out += " = (" + m[2].str() + ")0";
        }
        out += c;
        stmtStart = i + 1;

        if (c == '{')
        {
            braceIsStruct.push_back(std::regex_search(stmt, structDecl));
        }
        else if (c == '}' && !braceIsStruct.empty())
            braceIsStruct.pop_back();
    }
    out += src.substr(stmtStart);
    return out;
}

/// D3D9, Cg compiled as HLSL: the Cg profile list (`profiles vs_1_1 arbvp1`) becomes an HLSL `target`.
/// Always shader model 3: Cg profiles like fp30/arbfp1 allow far more instructions than ps_2_0, and
/// D3D9 requires vs_3_0 and ps_3_0 to be used together. `compile_arguments -DNAME` become `preprocessor_defines`.
static void ConvertCgProfilesForD3D9(Ogre::ConcreteNodeList& nodes)
{
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        if ((*it)->token == "profiles" && !(*it)->children.empty())
        {
            Ogre::String chosen;
            for (auto& value : (*it)->children)
            {
                const Ogre::String& p = value->token;
                if (p.compare(0, 3, "vs_") == 0 || p.find("vp") != Ogre::String::npos || p == "glslv")
                    chosen = "vs_3_0";
                else if (p.compare(0, 3, "ps_") == 0 || p.find("fp") != Ogre::String::npos || p == "glslf")
                    chosen = "ps_3_0";
                if (!chosen.empty())
                    break;
            }
            if (!chosen.empty())
            {
                RoR::LogFormat("[RoR|D3D9] Cg profiles -> HLSL target '%s' (%s:%d)", chosen.c_str(), (*it)->file.c_str(), (int)(*it)->line);
                (*it)->token = "target";
                (*it)->children.front()->token = chosen;
                (*it)->children.erase(std::next((*it)->children.begin()), (*it)->children.end());
            }
        }
        else if ((*it)->token == "compile_arguments" && !(*it)->children.empty())
        {
            Ogre::String defines;
            for (auto& value : (*it)->children)
            {
                if (value->token.compare(0, 2, "-D") == 0 && value->token.size() > 2)
                    defines += (defines.empty() ? "" : ";") + value->token.substr(2);
            }
            if (!defines.empty())
            {
                (*it)->token = "preprocessor_defines";
                (*it)->children.front()->token = defines;
                (*it)->children.erase(std::next((*it)->children.begin()), (*it)->children.end());
            }
        }
        ConvertCgProfilesForD3D9((*it)->children);
    }
}

// D3D11 knows no shader model 1.x/2.x: scripts written for D3D9 use `target vs_1_1` / `ps_2_0` (SkyX, Hydrax, ...).
// Upgrade them to shader model 3 - D3D11 maps it to 4.0 when 'enable_backwards_compatibility' is set (see handleEvent()).
static void UpgradeLegacyShaderTargets(Ogre::ConcreteNodeList& nodes)
{
    // In the OGRE script parser, the values of a property are the *children* of the property node:
    // `target vs_1_1` => node 'target' with child 'vs_1_1'.
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        // Cg programs (`profiles vs_1_1 arbvp1`) are compiled as HLSL on D3D11 (see handleEvent()): turn the
        // Cg profile list into an HLSL `target` - the D3D profile if listed, else derived from the GL profile.
        if ((*it)->token == "profiles" && !(*it)->children.empty())
        {
            Ogre::String chosen;
            for (auto& value : (*it)->children)
            {
                const Ogre::String& p = value->token;
                if (p.compare(0, 3, "vs_") == 0 || p.compare(0, 3, "ps_") == 0)
                {
                    chosen = p;
                    break;
                }
            }
            if (chosen.empty())
            {
                for (auto& value : (*it)->children)
                {
                    const Ogre::String& p = value->token;
                    if (p.find("vp") != Ogre::String::npos || p == "glslv")
                        chosen = "vs_3_0";
                    else if (p.find("fp") != Ogre::String::npos || p == "glslf")
                        chosen = "ps_3_0";
                    if (!chosen.empty())
                        break;
                }
            }
            if (!chosen.empty())
            {
                (*it)->token = "target";
                (*it)->children.front()->token = chosen;
                (*it)->children.erase(std::next((*it)->children.begin()), (*it)->children.end());
            }
        }

        if ((*it)->token == "target")
        {
            for (auto& value : (*it)->children)
            {
                Ogre::String& t = value->token;
                const Ogre::String old_target = t;
                if (t == "vs_1_1" || t == "vs_2_0" || t == "vs_2_a" || t == "vs_2_x")
                    t = "vs_3_0";
                else if (t == "ps_1_1" || t == "ps_1_2" || t == "ps_1_3" || t == "ps_1_4" || t == "ps_2_0" || t == "ps_2_a" || t == "ps_2_b" || t == "ps_2_x")
                    t = "ps_3_0";
                if (t != old_target)
                {
                    RoR::LogFormat("[RoR|D3D11] shader target '%s' -> '%s' (%s:%d)", old_target.c_str(), t.c_str(), value->file.c_str(), (int)value->line);
                }
            }
        }
        UpgradeLegacyShaderTargets((*it)->children);
    }
}

void ContentManager::preConversion(Ogre::ScriptCompiler *compiler, Ogre::ConcreteNodeListPtr nodes)
{
    if (IsD3D11Active())
    {
        UpgradeLegacyShaderTargets(*nodes);
    }
    else if (IsCgViaHlslOnD3D9())
    {
        ConvertCgProfilesForD3D9(*nodes);
    }
    else if (const int glslVersion = GetCgViaGlslVersion())
    {
        ConvertCgProgramsForGL(*nodes, compiler->getResourceGroup(), glslVersion);
    }
}

bool ContentManager::handleEvent(ScriptCompiler *compiler, ScriptCompilerEvent *evt, void *retval)
{
    if (evt->mType == CreateMaterialScriptCompilerEvent::eventType)
    {
        // Workaround for OGRE script compiler not properly checking that material name is not empty.
        // See https://github.com/RigsOfRods/rigs-of-rods/issues/2349
        auto* matEvent = static_cast<CreateMaterialScriptCompilerEvent*>(evt);
        if (matEvent->mName.empty())
        {
            RoR::LogFormat("[RoR] Got malformed material (empty name) from file: '%s' - forcing OGRE to fail loading.",
                matEvent->mFile.c_str());
            // Report "handled" but create nothing -> OGRE will interrupt the loading
            //   with message "failed to find or create material" [in MaterialTranslator::translate()]
            return true;
        }
    }
    else if (evt->mType == CreateParticleSystemScriptCompilerEvent::eventType)
    {
        // Workaround for OGRE ignoring resource groups when registering particle templates
        // See https://github.com/RigsOfRods/rigs-of-rods/pull/2398
        auto* particleEvent = static_cast<CreateParticleSystemScriptCompilerEvent*>(evt);
        if (Ogre::ParticleSystemManager::getSingleton().getTemplate(particleEvent->mName) != nullptr)
        {
            // Duplicate name -> OGRE would throw exception and fail initializing whole resource group
            RoR::LogFormat("[RoR] Duplicate particle system name '%s' in file: '%s' - forcing OGRE to fail loading.",
                particleEvent->mName.c_str(), particleEvent->mFile.c_str());
            return true; // Instruct OGRE to skip the particle system
        }
    }
    else if (evt->mType == CreateGpuProgramScriptCompilerEvent::eventType && IsD3D11Active())
    {
        // D3D11: scripts contain D3D9-style HLSL (`POSITION` outputs, `tex2D`, ...). Compile it in backwards compatibility mode.
        // Cg: D3D11 has no Cg profiles - the Cg sources are D3D9-style HLSL, compile them as HLSL as well
        // (same program name, so materials and mods keep working unchanged). See D3D11ShaderFix::CgToHlsl().
        auto* progEvent = static_cast<CreateGpuProgramScriptCompilerEvent*>(evt);
        const bool isCg = (progEvent->mSyntax == "cg");
        if (progEvent->mSyntax != "hlsl" && !isCg)
            return false;
        RoR::LogFormat("[RoR|D3D11] compiling %s program '%s' as HLSL in backwards compatibility mode",
            isCg ? "Cg" : "HLSL", progEvent->mName.c_str());

        HighLevelGpuProgramPtr prog = HighLevelGpuProgramManager::getSingleton().createProgram(
            progEvent->mName, progEvent->mResourceGroup, "hlsl", progEvent->mProgramType);
        if (prog) // Null if duplicate, see `resourceCollision()`
        {
            if (!progEvent->mSource.empty())
            {
                // Normally done by OGRE, but not when we handle the event.
                // Load the text ourselves to fix the VS/PS register layout (see D3D11ShaderFix.h).
                try
                {
                    DataStreamPtr stream = ResourceGroupManager::getSingleton().openResource(progEvent->mSource, progEvent->mResourceGroup);
                    Ogre::String text = stream->getAsString();
                    if (isCg)
                        text = RoR::D3D11ShaderFix::CgToHlsl(text);
                    prog->setSource(RoR::D3D11ShaderFix::FixSignatures(text));
                }
                catch (Ogre::Exception&)
                {
                    prog->setSourceFile(progEvent->mSource);
                }
            }
            prog->setParameter("enable_backwards_compatibility", "true");
        }
        *static_cast<GpuProgram**>(retval) = prog.get(); // Script properties (entry_point, target...) are applied afterwards.
        return true;
    }
    else if (evt->mType == CreateGpuProgramScriptCompilerEvent::eventType && IsCgViaHlslOnD3D9())
    {
        // D3D9 without the Cg plugin: compile Cg programs with the D3D9 HLSL compiler under the same name,
        // so materials and mods keep working unchanged. D3D9 links VS and PS by semantic - no signature fixes needed.
        auto* progEvent = static_cast<CreateGpuProgramScriptCompilerEvent*>(evt);
        if (progEvent->mSyntax != "cg")
            return false;
        RoR::LogFormat("[RoR|D3D9] compiling Cg program '%s' as HLSL (no Cg plugin)", progEvent->mName.c_str());

        HighLevelGpuProgramPtr prog = HighLevelGpuProgramManager::getSingleton().createProgram(
            progEvent->mName, progEvent->mResourceGroup, "hlsl", progEvent->mProgramType);
        if (prog) // Null if duplicate, see `resourceCollision()`
        {
            if (!progEvent->mSource.empty())
            {
                try
                {
                    DataStreamPtr stream = ResourceGroupManager::getSingleton().openResource(progEvent->mSource, progEvent->mResourceGroup);
                    prog->setSource(ZeroInitStructLocals(RoR::D3D11ShaderFix::CgToHlsl(stream->getAsString())));
                }
                catch (Ogre::Exception&)
                {
                    prog->setSourceFile(progEvent->mSource);
                }
            }
        }
        *static_cast<GpuProgram**>(retval) = prog.get(); // Script properties (entry_point, target...) are applied afterwards.
        return true;
    }
    else if (evt->mType == CreateGpuProgramScriptCompilerEvent::eventType && GetCgViaGlslVersion())
    {
        // OpenGL without the Cg plugin: create the GLSL program translated in `preConversion()` under the Cg program's name.
        auto* progEvent = static_cast<CreateGpuProgramScriptCompilerEvent*>(evt);
        auto found = g_cg_glsl_sources.find(progEvent->mName);
        if (progEvent->mSyntax != "cg" || found == g_cg_glsl_sources.end())
            return false;

        HighLevelGpuProgramPtr prog = HighLevelGpuProgramManager::getSingleton().createProgram(
            progEvent->mName, progEvent->mResourceGroup, "glsl", progEvent->mProgramType);
        if (prog) // Null if duplicate, see `resourceCollision()`
        {
            prog->setSource(found->second);
        }
        g_cg_glsl_sources.erase(found);
        *static_cast<GpuProgram**>(retval) = prog.get(); // Script properties (default_params...) are applied afterwards.
        return true;
    }
#if OGRE_VERSION < 0x010C08 // OGRE 1.12.8+ does this itself, see `D3D9HLSLProgram::getTarget()`
    else if (evt->mType == CreateHighLevelGpuProgramScriptCompilerEvent::eventType)
    {
        // Backport of the fallback HLSL target, so scripts written for newer OGRE may omit `target`.
        // OGRE 1.11 leaves it empty, which fails `isSupported()` already when the material is compiled.
        // Unlike OGRE, prefer shader model 3: 'OgreUnifiedShader.h' fragment shaders declare a `VPOS` input
        // and commonly `FOG`, neither of which ps_2_0 accepts. D3D11 maps these to its own profiles.
        auto* progEvent = static_cast<CreateHighLevelGpuProgramScriptCompilerEvent*>(evt);
        if (progEvent->mLanguage != "hlsl")
            return false;

        HighLevelGpuProgramPtr prog = HighLevelGpuProgramManager::getSingleton().createProgram(
            progEvent->mName, progEvent->mResourceGroup, progEvent->mLanguage, progEvent->mProgramType);
        if (prog) // Null if duplicate, see `resourceCollision()`
        {
            prog->setSourceFile(progEvent->mSource); // Normally done by OGRE, but not when we handle the event.
            const String prefix = (progEvent->mProgramType == GPT_VERTEX_PROGRAM) ? "vs_" : "ps_";
            const bool sm3 = GpuProgramManager::getSingleton().isSyntaxSupported(prefix + "3_0");
            prog->setParameter("target", prefix + (sm3 ? "3_0" : "2_0"));
        }
        *static_cast<HighLevelGpuProgram**>(retval) = prog.get(); // Script properties, i.e. an explicit `target`, are applied afterwards.
        return true;
    }
#endif

    return false; // Report "not handled"
}

void ContentManager::InitManagedMaterials(std::string const & rg_name)
{
    Ogre::String managed_materials_dir = PathCombine(App::sys_resources_dir->getStr(), "managed_materials");

    ResourceGroupManager::getSingleton().addResourceLocation(PathCombine(managed_materials_dir, "texture"), "FileSystem", rg_name);

    // Last
    ResourceGroupManager::getSingleton().addResourceLocation(managed_materials_dir, "FileSystem", rg_name);

    if (rg_name == RGN_MANAGED_MATS) // Only initialize the global resource group
        ResourceGroupManager::getSingleton().initialiseResourceGroup(rg_name);
}



void ContentManager::LoadGameplayResources()
{
    if (!m_base_resource_loaded)
    {
        this->AddResourcePack(ContentManager::ResourcePack::AIRFOILS);
        this->AddResourcePack(ContentManager::ResourcePack::TEXTURES);
        this->AddResourcePack(ContentManager::ResourcePack::FAMICONS);
        this->AddResourcePack(ContentManager::ResourcePack::MATERIALS);
        this->AddResourcePack(ContentManager::ResourcePack::MESHES);
        this->AddResourcePack(ContentManager::ResourcePack::OVERLAYS);
        this->AddResourcePack(ContentManager::ResourcePack::PARTICLES);

        m_base_resource_loaded = true;
    }

    if (App::gfx_water_mode->getEnum<GfxWaterMode>() == GfxWaterMode::HYDRAX)
        this->AddResourcePack(ContentManager::ResourcePack::HYDRAX);

    if (App::gfx_sky_mode->getEnum<GfxSkyMode>() == GfxSkyMode::SKYX)
        this->AddResourcePack(ContentManager::ResourcePack::SKYX);

    if (App::gfx_vegetation_mode->getEnum<GfxVegetation>() != RoR::GfxVegetation::NONE)
        this->AddResourcePack(ContentManager::ResourcePack::PAGED);

}

std::string ContentManager::ListAllUserContent()
{
    std::stringstream buf;

    auto dir_list = Ogre::ResourceGroupManager::getSingleton().listResourceFileInfo(RGN_CONTENT, true);
    for (auto dir: *dir_list)
    {
        buf << dir.filename << std::endl;
    }

    // Any filename + listed extensions, ignore case
    std::regex file_whitelist("^.\\.(airplane|boat|car|fixed|load|machine|skin|terrn2|train|truck)$", std::regex::icase);

    auto file_list = Ogre::ResourceGroupManager::getSingleton().listResourceFileInfo(RGN_CONTENT, false);
    for (auto file: *file_list)
    {
        if ((file.archive != nullptr) || std::regex_match(file.filename, file_whitelist))
        {
            buf << file.filename << std::endl;
        }
    }

    return buf.str();
}

bool ContentManager::LoadAndParseJson(std::string const& filename, std::string const& rg_name, rapidjson::Document& j_doc)
{
    try
    {
        Ogre::DataStreamPtr stream = Ogre::ResourceGroupManager::getSingleton().openResource(filename, rg_name);
        Ogre::String json_str = stream->getAsString();
        rapidjson::MemoryStream j_stream(json_str.data(), json_str.length());
        j_doc.ParseStream<rapidjson::kParseNanAndInfFlag>(j_stream);
    }
    catch (Ogre::FileNotFoundException)
    {
        return false; // Error already logged by OGRE
    }
    catch (std::exception& e)
    {
        RoR::LogFormat("[RoR] Failed to open or read json file '%s' (resource group '%s'), message: '%s'",
                       filename.c_str(), rg_name.c_str(), e.what());
        return false;
    }

    if (j_doc.HasParseError())
    {
        RoR::LogFormat("[RoR] Error parsing JSON file '%s' (resource group '%s')",
                       filename.c_str(), rg_name.c_str());
        return false;
    }

    return true;
}

bool ContentManager::SerializeAndWriteJson(std::string const& filename, std::string const& rg_name, rapidjson::Document& j_doc)
{
    // Serialize JSON to string
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer, rapidjson::UTF8<>, rapidjson::UTF8<>,
                      rapidjson::CrtAllocator, rapidjson::kWriteNanAndInfFlag>
                      writer(buffer);
    j_doc.Accept(writer);

    // Write JSON to file
    try
    {
        Ogre::DataStreamPtr stream
            = Ogre::ResourceGroupManager::getSingleton().createResource(
                filename, rg_name, /*overwrite=*/true);
        size_t written = stream->write(buffer.GetString(), buffer.GetSize());
        if (written < buffer.GetSize())
        {
            RoR::LogFormat("[RoR] Error writing JSON file '%s' (resource group '%s'), ",
                           "only written %u out of %u bytes!",
                           filename.c_str(), rg_name.c_str(), written, buffer.GetSize());
            return false;
        }
        return true;
    }
    catch (std::exception& e)
    {
        RoR::LogFormat("[RoR] Error writing JSON file '%s' (resource group '%s'), message: '%s'",
                       filename.c_str(), rg_name.c_str(), e.what());
        return false;
    }
}

bool ContentManager::DeleteDiskFile(std::string const& filename, std::string const& rg_name)
{
    try
    {
        Ogre::ResourceGroupManager::getSingleton().deleteResource(filename, rg_name);
        return true;
    }
    catch (std::exception& e)
    {
        RoR::LogFormat("[RoR|ModCache] Error deleting file '%s' (resource group '%s'), message: '%s'",
                        filename.c_str(), rg_name.c_str(), e.what());
        return false;
    }
}

