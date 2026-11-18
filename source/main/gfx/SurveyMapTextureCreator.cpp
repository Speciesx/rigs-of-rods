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

    Ogre::SceneNode* mCamera_snode = App::GetGfxScene()->GetSceneManager()->getRootSceneNode()->createChildSceneNode();
    mCamera_snode->attachObject(mCamera);
    mCamera_snode->setFixedYawAxis(false);
    mCamera_snode->setDirection(-Vector3::UNIT_Y);

    auto mViewport = mRttTex->addViewport(mCamera);
    mViewport->setBackgroundColour(ColourValue::Black);
    mViewport->setOverlaysEnabled(false);
    mViewport->setShadowsEnabled(false);
    mViewport->setSkiesEnabled(false);

    return true;
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
