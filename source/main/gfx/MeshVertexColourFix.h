/*
    This source file is part of Rigs of Rods

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

// Content often uses materials with vertex colour tracking ('diffuse vertexcolour' etc.) on submeshes
// that have no vertex colours. The fixed function pipeline (D3D9) silently uses white then, but the
// RTSS shaders (D3D11, GL3+, Vulkan) read COLOR0 and D3D11 throws "consumes semantic COLOR0 but no
// such VertexElement provided" - a fatal error while loading the vehicle.
// Fix: when a mesh has been loaded, give such submeshes a copy of the material without vertex colour tracking.

#include <Ogre.h>

namespace RoR {

class MeshVertexColourFix : public Ogre::MeshSerializerListener
{
public:
    static void Install()
    {
        static MeshVertexColourFix s_instance;
        Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
        if (rs && rs->getCapabilities() && rs->getCapabilities()->hasCapability(Ogre::RSC_FIXED_FUNCTION))
            return; // D3D9/GL: fixed function handles missing vertex colours itself
        if (Ogre::MeshManager::getSingleton().getListener() == nullptr)
            Ogre::MeshManager::getSingleton().setListener(&s_instance);
    }

    void processMaterialName(Ogre::Mesh* mesh, Ogre::String* name) override {}
    void processSkeletonName(Ogre::Mesh* mesh, Ogre::String* name) override {}

    void processMeshCompleted(Ogre::Mesh* mesh) override
    {
        for (Ogre::SubMesh* sub : mesh->getSubMeshes())
        {
            const Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
            if (vd == nullptr || vd->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE) != nullptr)
                continue;

            const Ogre::String& matName = sub->getMaterialName();
            if (matName.empty())
                continue;
            Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName(matName, mesh->getGroup());
            if (!mat || !TracksVertexColour(mat))
                continue;

            const Ogre::String fixedName = matName + "/NoVertexColour";
            Ogre::MaterialPtr fixed = Ogre::MaterialManager::getSingleton().getByName(fixedName, mat->getGroup());
            if (!fixed)
            {
                fixed = mat->clone(fixedName);
                for (Ogre::Technique* tech : fixed->getTechniques())
                    for (Ogre::Pass* pass : tech->getPasses())
                        pass->setVertexColourTracking(Ogre::TVC_NONE);
                Ogre::LogManager::getSingleton().logMessage("[RoR|VertexColour] mesh '" + mesh->getName() + "' has no vertex colours, material '"
                    + matName + "' tracks them - using '" + fixedName + "'");
            }
            sub->setMaterialName(fixedName, fixed->getGroup());
        }
    }

private:
    static bool TracksVertexColour(const Ogre::MaterialPtr& mat)
    {
        for (Ogre::Technique* tech : mat->getTechniques())
            for (Ogre::Pass* pass : tech->getPasses())
                if (pass->getVertexColourTracking() != Ogre::TVC_NONE)
                    return true;
        return false;
    }
};

} // namespace RoR
