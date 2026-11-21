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

#include "SurveyMapTextureCreator.h"

#include "Actor.h"
#include "Application.h"
#include "GameContext.h"
#include "GfxScene.h"
#include "HydraxWater.h"
#include "IGfxWater.h"
#include "Terrain.h"
#include "TerrainObjectManager.h"

using namespace Ogre;
using namespace RoR;

static int counter = 0;

SurveyMapTextureCreator::SurveyMapTextureCreator(Ogre::Real terrain_height) :
    mCamera(nullptr),
    mRttTex(nullptr),
    mTerrainHeight(Math::Clamp(terrain_height + 100.0f, 150.0f, 2500.0f))
{
    counter++;
    mTextureName = "MapRttTex-" + TOSTRING(counter);
}

SurveyMapTextureCreator::~SurveyMapTextureCreator()
{
    if (mRttTex)
        mRttTex->removeAllViewports();
    if (mCamera)
        App::GetGfxScene()->GetSceneManager()->destroyCamera(mCamera);
    if (mWaterPlane)
    {
        Ogre::SceneNode* node = mWaterPlane->getParentSceneNode();
        App::GetGfxScene()->GetSceneManager()->destroyManualObject(mWaterPlane);
        if (node)
            App::GetGfxScene()->GetSceneManager()->destroySceneNode(node);
    }
    if (mWaterPlaneMat)
        Ogre::MaterialManager::getSingleton().remove(mWaterPlaneMat);
    if (mTexture)
        Ogre::TextureManager::getSingleton().remove(mTexture);
}

bool SurveyMapTextureCreator::init(int res, int fsaa)
{
    mTexture = Ogre::TextureManager::getSingleton().createManual(mTextureName,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, Ogre::TEX_TYPE_2D, res, res,
        /*numMipmaps:*/0, Ogre::PF_R8G8B8, Ogre::TU_RENDERTARGET, 0, false, /*fsaa:*/0); // was 'TU_RENDERTARGET' (= 32 mipmaps): D3D11 rejects mipmapped MSAA render targets (E_INVALIDARG)
    // No FSAA: the texture is read back with convertToImage(), and a multisampled texture reads back black on D3D11.
    // The 4096x4096 map is a static top-down image, anti-aliasing adds next to nothing there.
    (void)fsaa;

    if (!mTexture)
        return false;

    mRttTex = mTexture->getBuffer()->getRenderTarget();

    if (!mRttTex)
        return false;

    mRttTex->addListener(this);
    mRttTex->setAutoUpdated(false);

    mCamera = App::GetGfxScene()->GetSceneManager()->createCamera("MapRttCam-" + TOSTRING(counter));
    mCamera->setProjectionType(PT_ORTHOGRAPHIC);
    mCamera->setNearClipDistance(1.0f);
    // Finite far plane: an orthographic camera can't have an infinite one (OGRE only avoids the division by zero
    // then), the camera sits mTerrainHeight above sea level looking straight down.
    mCamera->setFarClipDistance(mTerrainHeight + 1000.0f);
    // Full terrain detail for the map: the coarse terrain LOD levels come out as long streaks in the top-down map
    // (the map camera picks LOD per tile from its distance, and the coarser the levels, the more streaks).
    // LOD bias also scales OGRE's terrain pixel error, so a large bias selects the full-detail level everywhere.
    // It is a one-off render, the extra triangles don't matter.
    mCamera->setLodBias(1000.f);

    Ogre::SceneNode* mCamera_snode = App::GetGfxScene()->GetSceneManager()->getRootSceneNode()->createChildSceneNode();
    mCamera_snode->attachObject(mCamera);
    mCamera_snode->setFixedYawAxis(false);
    mCamera_snode->setDirection(-Vector3::UNIT_Y);

    auto mViewport = mRttTex->addViewport(mCamera);
    mViewport->setBackgroundColour(ColourValue::Black);
    mViewport->setOverlaysEnabled(false);
    mViewport->setShadowsEnabled(false);
    mViewport->setSkiesEnabled(false);

    this->createHydraxStandIn();

    return true;
}

void SurveyMapTextureCreator::createHydraxStandIn()
{
    HydraxWater* hydrax = App::GetGameContext()->GetTerrain()->getHydraxManager();
    if (!hydrax || !hydrax->GetHydrax())
        return;

    // Flat, unshaded plane in the Hydrax water colour (self-illumination only, so it looks the same on every render system)
    const Ogre::Vector3 col = hydrax->GetHydrax()->getWaterColor();
    mWaterPlaneMat = Ogre::MaterialManager::getSingleton().create("MapRttWaterMat-" + TOSTRING(counter),
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Pass* pass = mWaterPlaneMat->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(true);
    pass->setAmbient(Ogre::ColourValue::Black);
    pass->setDiffuse(Ogre::ColourValue::Black);
    pass->setSpecular(Ogre::ColourValue::Black);
    pass->setSelfIllumination(Ogre::ColourValue(col.x, col.y, col.z));

    const Ogre::Vector3 size = App::GetGameContext()->GetTerrain()->getMaxTerrainSize();
    const float margin = std::max(size.x, size.z); // also covers the sea around the terrain
    const float y = App::GetGameContext()->GetTerrain()->getWaterHeight();
    const float x0 = -margin, x1 = size.x + margin, z0 = -margin, z1 = size.z + margin;

    Ogre::SceneManager* scm = App::GetGfxScene()->GetSceneManager();
    mWaterPlane = scm->createManualObject("MapRttWater-" + TOSTRING(counter));
    mWaterPlane->begin(mWaterPlaneMat->getName(), Ogre::RenderOperation::OT_TRIANGLE_LIST, mWaterPlaneMat->getGroup());
    mWaterPlane->position(x0, y, z0); mWaterPlane->normal(Ogre::Vector3::UNIT_Y);
    mWaterPlane->position(x0, y, z1); mWaterPlane->normal(Ogre::Vector3::UNIT_Y);
    mWaterPlane->position(x1, y, z1); mWaterPlane->normal(Ogre::Vector3::UNIT_Y);
    mWaterPlane->position(x1, y, z0); mWaterPlane->normal(Ogre::Vector3::UNIT_Y);
    mWaterPlane->quad(0, 1, 2, 3);
    mWaterPlane->end();
    mWaterPlane->setCastShadows(false);
    mWaterPlane->setVisible(false); // only while the map is rendered
    scm->getRootSceneNode()->createChildSceneNode()->attachObject(mWaterPlane);
}

void SurveyMapTextureCreator::update(Vector2 center, Vector2 size)
{
    if (!mRttTex)
        return;
 
    mCamera->setOrthoWindow(size.x, size.y);
    mCamera->getParentSceneNode()->setPosition(Vector3(center.x, mTerrainHeight, center.y));

    mRttTex->update();
}

void SurveyMapTextureCreator::preRenderTargetUpdate(const RenderTargetEvent &evt)
{
    // Trees/grass (PagedGeometry) face the camera - seen from straight above they are stretched into long streaks
    if (App::GetGameContext()->GetTerrain()->getObjectManager())
        App::GetGameContext()->GetTerrain()->getObjectManager()->SetPagedGeometryAttached(false);

    // Hydrax draws a projected grid built for the player's perspective camera, textured with the player's
    // refraction/reflection images - seen through the orthographic map camera that smears the player's view
    // across the map in long streaks. Hide it and draw the flat stand-in plane instead.
    HydraxWater* hydrax = App::GetGameContext()->GetTerrain()->getHydraxManager();
    if (hydrax && hydrax->GetHydrax())
    {
        // Only the mesh node: Hydrax::setVisible() would tear down and rebuild its RTTs
        mHydraxWasVisible = hydrax->GetHydrax()->isVisible() && hydrax->GetHydrax()->getMesh()->getSceneNode();
        if (mHydraxWasVisible)
            hydrax->GetHydrax()->getMesh()->getSceneNode()->setVisible(false);
        if (mWaterPlane)
            mWaterPlane->setVisible(mHydraxWasVisible);
        return;
    }

    auto water = App::GetGameContext()->GetTerrain()->getGfxWater();
    if (water)
    {
        water->SetForcedCameraTransform(mCamera->getFOVy(),
            mCamera->getParentSceneNode()->getPosition(), mCamera->getParentSceneNode()->getOrientation()); // FIXME: Legacy OGRE API! Use camera node instead!
        water->UpdateWater();
        water->ClearForcedCameraTransform();
    }
}

void SurveyMapTextureCreator::postRenderTargetUpdate(const RenderTargetEvent &evt)
{
    if (App::GetGameContext()->GetTerrain()->getObjectManager())
        App::GetGameContext()->GetTerrain()->getObjectManager()->SetPagedGeometryAttached(true);

    HydraxWater* hydrax = App::GetGameContext()->GetTerrain()->getHydraxManager();
    if (hydrax && hydrax->GetHydrax())
    {
        if (mWaterPlane)
            mWaterPlane->setVisible(false);
        if (mHydraxWasVisible)
            hydrax->GetHydrax()->getMesh()->getSceneNode()->setVisible(true);
        return;
    }

    auto water = App::GetGameContext()->GetTerrain()->getGfxWater();
    if (water)
    {
        water->UpdateWater();
    }
}

Ogre::TexturePtr SurveyMapTextureCreator::convertTextureToStatic(const std::string& texName, const std::string& rgName)
{
    // Read back via the render target: Texture::convertToImage() returns only zeros for a D3D11 render texture
    // (the map came out black although 1.7M triangles were drawn). copyContentsToMemory() works on all render systems.
    Ogre::Image img(Ogre::PF_BYTE_RGBA, mTexture->getWidth(), mTexture->getHeight());
    mRttTex->copyContentsToMemory(Ogre::Box(0, 0, mTexture->getWidth(), mTexture->getHeight()), img.getPixelBox(), Ogre::RenderTarget::FB_AUTO);

    // The map is opaque: drop the alpha channel (an X8R8G8B8 target reads back with alpha 0 = invisible map)
    Ogre::Image rgb(Ogre::PF_BYTE_RGB, img.getWidth(), img.getHeight());
    Ogre::PixelUtil::bulkPixelConversion(img.getPixelBox(), rgb.getPixelBox());
    return Ogre::TextureManager::getSingleton().loadImage(texName, rgName, rgb);
}
