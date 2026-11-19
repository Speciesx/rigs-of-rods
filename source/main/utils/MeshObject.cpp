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

/// @file
/// @author Thomas Fischer (thomas{AT}thomasfischer{DOT}biz)
/// @date   1st of May 2010

#include "MeshObject.h"

#include "Actor.h"
#include "Application.h"
#include "GfxScene.h"
#include "Terrain.h"

#include <OgreMeshLodGenerator.h>
#include <OgreLodConfig.h>

#ifdef _MSC_VER
#include <excpt.h>
#endif

using namespace Ogre;
using namespace RoR;

// OGRE 14's MeshLodGenerator crashes with an access violation on some meshes - not reproducibly (the GraveDigger
// '35halo.mesh' passed once and crashed the next time). On MSVC, catch exactly that with SEH and drop the LODs
// instead of taking the game down.
// The OGRE calls create temporaries with destructors (default shared_ptr arguments), which __try does not allow
// in the same function (C2712) - so they live in plain functions, called through a pointer by the SEH wrapper.
static void DoGenerateLodLevels(void* config)
{
    Ogre::MeshLodGenerator::getSingleton().generateLodLevels(*static_cast<Ogre::LodConfig*>(config));
}

static void DoGenerateAutoLodLevels(void* mesh)
{
    Ogre::MeshLodGenerator::getSingleton().generateAutoconfiguredLodLevels(*static_cast<Ogre::MeshPtr*>(mesh));
}

#ifdef _MSC_VER
static bool RunLodGeneratorGuarded(void (*fn)(void*), void* arg)
{
    __try
    {
        fn(arg);
        return true;
    }
    __except (GetExceptionCode() == 0xC0000005 /*EXCEPTION_ACCESS_VIOLATION*/ ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}
#else
static bool RunLodGeneratorGuarded(void (*fn)(void*), void* arg) { fn(arg); return true; }
#endif

static bool GenerateLodLevelsGuarded(Ogre::LodConfig* config) { return RunLodGeneratorGuarded(&DoGenerateLodLevels, config); }
static bool GenerateAutoLodLevelsGuarded(Ogre::MeshPtr* mesh) { return RunLodGeneratorGuarded(&DoGenerateAutoLodLevels, mesh); }

MeshObject::MeshObject(Ogre::String meshName, Ogre::String entityRG, Ogre::String entityName, Ogre::SceneNode *m_scene_node)
    : m_scene_node(m_scene_node), m_entity(nullptr), m_cast_shadows(true)
{
    this->createEntity(meshName, entityRG, entityName);
}

void MeshObject::setMaterialName(Ogre::String m)
{
    if (m_entity)
    {
        m_entity->setMaterialName(m);
    }
}

void MeshObject::setCastShadows(bool b)
{
    m_cast_shadows = b;
    if (m_scene_node && m_scene_node->numAttachedObjects())
    {
        m_scene_node->getAttachedObject(0)->setCastShadows(b);
    }
}

void MeshObject::setVisible(bool b)
{
    // Workaround: if the scenenode is not used (entity not attached) for some reason, try hiding the entity directly.
    if (m_scene_node && m_scene_node->getAttachedObjects().size() > 0)
        m_scene_node->setVisible(b);
    else if (m_entity)
        m_entity->setVisible(b);
}

void MeshObject::createEntity(Ogre::String meshName, Ogre::String entityRG, Ogre::String entityName)
{
    if (!m_scene_node)
        return;

    try
    {
        m_mesh = Ogre::MeshManager::getSingleton().getByName(meshName, entityRG);

        // Mesh hasn't been loaded yet
        if (m_mesh == nullptr)
        {
            m_mesh = Ogre::MeshManager::getSingleton().load(meshName, entityRG);

            // important: you need to add the LODs before creating the entity
            // now find possible LODs, needs to be done before calling createEntity()
            String basename, ext;
            StringUtil::splitBaseFilename(meshName, basename, ext);

            bool lod_available = false;
            Ogre::LodConfig config(m_mesh);

            // the classic LODs
            FileInfoListPtr files = ResourceGroupManager::getSingleton().findResourceFileInfo(entityRG, basename + "_lod*.mesh");
            for (FileInfoList::iterator iterFiles = files->begin(); iterFiles != files->end(); ++iterFiles)
            {
                String format = basename + "_lod%d.mesh";
                int i = -1;
                int r = sscanf(iterFiles->filename.c_str(), format.c_str(), &i);

                if (r <= 0 || i < 0)
                    continue;

                float distance = 3;

                // we need to tune this according to our sightrange
                if (App::gfx_sight_range->getInt() > Terrain::UNLIMITED_SIGHTRANGE)
                {
                    // unlimited
                    if (i == 1)
                        distance = 200;
                    else if (i == 2)
                        distance = 600;
                    else if (i == 3)
                        distance = 2000;
                    else if (i == 4)
                        distance = 5000;
                }
                else
                {
                    // limited
                    int sightrange = App::gfx_sight_range->getInt();
                    if (i == 1)
                        distance = std::max(20.0f, sightrange * 0.1f);
                    else if (i == 2)
                        distance = std::max(20.0f, sightrange * 0.2f);
                    else if (i == 3)
                        distance = std::max(20.0f, sightrange * 0.3f);
                    else if (i == 4)
                        distance = std::max(20.0f, sightrange * 0.4f);
                }
                config.createManualLodLevel(distance, iterFiles->filename);
                lod_available = true;
            }

            // the custom LODs
            FileInfoListPtr files2 = ResourceGroupManager::getSingleton().findResourceFileInfo(entityRG, basename + "_clod_*.mesh");
            for (FileInfoList::iterator iterFiles = files2->begin(); iterFiles != files2->end(); ++iterFiles)
            {
                // and custom LODs
                String format = basename + "_clod_%d.mesh";
                int i = -1;
                int r = sscanf(iterFiles->filename.c_str(), format.c_str(), &i);
                if (r <= 0 || i < 0)
                    continue;

                config.createManualLodLevel(i, iterFiles->filename);
                lod_available = true;
            }

            // OGRE 14 MeshLodGenerator crashes (access violation, not catchable) on meshes it cannot handle.
            // Only let it work on plain triangle-list meshes, and log before the call so a crash names the mesh.
            bool lod_safe = true;
            for (unsigned short si = 0; si < m_mesh->getNumSubMeshes(); ++si)
            {
                Ogre::SubMesh* sub = m_mesh->getSubMesh(si);
                if (sub->operationType != Ogre::RenderOperation::OT_TRIANGLE_LIST
                    || sub->indexData == nullptr || sub->indexData->indexCount < 3
                    || sub->indexData->indexBuffer->getType() == Ogre::HardwareIndexBuffer::IT_32BIT // suspected crash cause in the OGRE 14 LOD generator (e.g. Desert Trails LJDT_VisitorMap)
                    || (!sub->useSharedVertices && (sub->vertexData == nullptr || sub->vertexData->vertexCount == 0)))
                {
                    lod_safe = false;
                }
            }
            if (m_mesh->getNumSubMeshes() == 0 || (m_mesh->sharedVertexData != nullptr && m_mesh->sharedVertexData->vertexCount == 0))
                lod_safe = false;

            // Automatic LODs only where they pay off: skip small meshes (dashboard needles, lights - the generator
            // crashed on the Tatra T813 't813_dash_light.mesh'). Fewer than 1000 vertices gain nothing from LODs.
            size_t total_verts = 0;
            auto inspect = [&](const Ogre::VertexData* vd)
            {
                if (!vd) return;
                total_verts += vd->vertexCount;
            };
            inspect(m_mesh->sharedVertexData);
            for (unsigned short si = 0; si < m_mesh->getNumSubMeshes(); ++si)
            {
                if (!m_mesh->getSubMesh(si)->useSharedVertices)
                    inspect(m_mesh->getSubMesh(si)->vertexData);
            }
            const bool auto_lod_worth = (total_verts >= 1000);

            if (lod_safe && (lod_available || (App::gfx_auto_lod->getBool() && auto_lod_worth)))
            {
                LOG(fmt::format("[RoR|LOD] generating LOD levels for mesh '{}' (group '{}', manual LODs: {})", meshName, entityRG, lod_available));
                const bool ok = lod_available ? GenerateLodLevelsGuarded(&config) : GenerateAutoLodLevelsGuarded(&m_mesh);
                if (!ok)
                {
                    m_mesh->removeLodLevels();
                    LOG(fmt::format("[RoR|LOD] LOD generator crashed on mesh '{}' - continuing without LODs", meshName));
                }
            }
            else if (!lod_safe)
            {
                LOG(fmt::format("[RoR|LOD] skipping LOD generation for mesh '{}': unsupported layout", meshName));
            }
        }

        // now create an entity around the mesh and attach it to the scene graph
        m_entity = App::GetGfxScene()->GetSceneManager()->createEntity(entityName, meshName, entityRG);
        m_entity->setCastShadows(m_cast_shadows);

        m_scene_node->attachObject(m_entity);
        m_scene_node->setVisible(true);
    }
    catch (Ogre::Exception &e)
    {
        RoR::LogFormat("[RoR] Error creating entity of mesh '%s' (group: '%s'), message: %s",
                       meshName.c_str(), entityRG.c_str(), e.getFullDescription().c_str());
        return;
    }
}
