// This file is part of the Caelum project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution.

#ifndef CAELUMPORT__CAELUM_PREREQUISITES_H
#define CAELUMPORT__CAELUM_PREREQUISITES_H

#include <Ogre.h>
#include <OgreBuildSettings.h>

namespace CaelumPort
{
    // Caelum needs a lot of precission for astronomical calculations.
    // Very few calculations use it, and the precission IS required.
    typedef double LongReal;

    using Ogre::Real;
    using Ogre::String;
}

#endif // CAELUMPORT__CAELUM_PREREQUISITES_H
