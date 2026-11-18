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

#pragma once

#include "Application.h"
#include "RefCountingObject.h"
#include "RefCountingObjectPtr.h"
#include "SimData.h"

#include <Ogre.h>

namespace RoR {

/// @addtogroup Gameplay
/// @{

/// @addtogroup Aerial
/// @{

/// Returns false if the node has been torn off the airframe (no intact path of beams to node 0).
/// Stretched beams (> 3x rest length) count as severed, even if they did not formally break.
bool IsNodeAttachedToAirframe(Actor* actor, NodeNum_t node);

class AeroEngine : public RefCountingObject<AeroEngine>
{
public:
    virtual ~AeroEngine() override {};

    virtual void updateForces(float dt, int doUpdate) =0;

    virtual void setThrottle(float val) =0;

    virtual float getThrottle() =0;

    virtual void reset() =0;

    virtual void toggleReverse() =0;

    virtual void setReverse(bool val) =0;
    virtual bool getReverse() =0;

    virtual void flipStart() =0;

    virtual float getRPMpc() =0;
    virtual float getRPM() =0;
    virtual void setRPM(float _rpm) =0;
    virtual float getpropwash() =0;
    virtual Ogre::Vector3 getAxis() =0;
    virtual bool isFailed() =0;
    virtual bool isDetached() =0; //!< Torn off the airframe: no exhaust/smoke at all
    virtual AeroEngineType getType() =0;
    virtual bool getIgnition() =0;
    virtual void setIgnition(bool val) =0;
    virtual int getNoderef() =0;
    virtual NodeNum_t GetFrontNode() const =0;
    virtual NodeNum_t GetBackNode()  const =0;
    virtual bool getWarmup() =0;
    virtual float getRadius() =0;

    // Visuals
    virtual void updateVisuals(RoR::GfxActor* gfx_actor) = 0;
    virtual void setVisible(bool visible) = 0;
};

/// @} // addtogroup Aerial
/// @} // addtogroup Gameplay

} // namespace RoR
