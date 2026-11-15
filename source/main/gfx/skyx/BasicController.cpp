/*
--------------------------------------------------------------------------------
This source file is part of SkyX.
Visit http://www.paradise-studios.net/products/skyx/

Copyright (C) 2009-2012 Xavier Verguín González <xavyiy@gmail.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place - Suite 330, Boston, MA 02111-1307, USA, or go to
http://www.gnu.org/copyleft/lesser.txt.
--------------------------------------------------------------------------------
*/

#include "BasicController.h"

#include <algorithm>
#include <cmath>

namespace SkyX
{
	BasicController::BasicController(const bool& deleteBySkyX)
		: Controller(deleteBySkyX)
		, mTime(Ogre::Vector3(14.0f, 7.50f, 20.50f))
		, mSunDirection(Ogre::Vector3(0,1,0))
		, mMoonDirection(Ogre::Vector3(0,-1,0))
		, mEastDirection(Ogre::Vector2(0,1))
		, mMoonPhase(0)
		, mAstronomical(false)
		, mLatitude(45.f)
		, mDayOfYear(150)
		, mSunTimesLatitude(-1000.f)
		, mSunTimesDayOfYear(-1)
	{
	}

	void BasicController::update(const Ogre::Real& simDeltaTime)
	{
		mTime.x += simDeltaTime;

		if (mTime.x > 24)
		{
			mTime.x -= 24;
		} 
		else if (mTime.x < 0)
		{
			mTime.x += 24;
		}

		if (mAstronomical)
		{
			this->updateAstronomy();
			return;
		}

		// 24h day: 
		// 0______A(Sunrise)_______B(Sunset)______24
		//                     

		float y,
			X = mTime.x,
			A = mTime.y,
			B = mTime.z,
			AB  = A+24-B,
			AB_ = B-A,
			XB  = X+24-B;

		if (X<A || X>B)
		{
			if (X<A)
			{
                y = -XB / AB;
			}
			else
			{
				y = -(X-B) / AB;
			}
            
			if (y > -0.5f)
			{
				y *= 2;
			}
			else
			{
				y = -(1 + y)*2;
			}
		}
		else
		{
			y = (X-A)/(B-A);

			if (y < 0.5f)
			{
				y *= 2;
			}
			else
			{
				y = (1 - y)*2;
			}
		}

		Ogre::Vector2 East = mEastDirection;

		if (X > A && X < B)
		{
			if (X > (A + AB_/2))
			{
				East = -East;
			}
		}
		else
		{
			if (X<=A)
			{
				if (XB < (24-AB_)/2)
				{
					East = -East;
				}
			}
			else
			{
				if ((X-B) < (24-AB_)/2)
				{
					East = -East;
				}
			}
		}

		float ydeg = (Ogre::Math::PI/2)*y,
		      sn = Ogre::Math::Sin(ydeg),
		      cs = Ogre::Math::Cos(ydeg);

		mSunDirection = Ogre::Vector3(East.x*cs, sn, East.y*cs);
		mMoonDirection = -mSunDirection;
	}

	void BasicController::setAstronomical(bool enabled)
	{
		mAstronomical = enabled;
		update(0);
	}

	void BasicController::setLatitude(Ogre::Real degrees)
	{
		mLatitude = std::max<Ogre::Real>(-90.f, std::min<Ogre::Real>(90.f, degrees));
		update(0);
	}

	void BasicController::setDayOfYear(int day)
	{
		mDayOfYear = std::max(1, std::min(365, day));
		update(0);
	}

	Ogre::Vector3 BasicController::horizontalToWorld(const Ogre::Degree& azimuth, const Ogre::Degree& altitude) const
	{
		// East = mEastDirection (X,Z world coords), North = East rotated so that East x North = Up
		const Ogre::Vector2 east = mEastDirection.normalisedCopy();
		const Ogre::Vector2 north(east.y, -east.x);
		const float ca = Ogre::Math::Cos(altitude);
		const float sa = Ogre::Math::Sin(altitude);
		const float cz = Ogre::Math::Cos(azimuth);
		const float sz = Ogre::Math::Sin(azimuth);
		return Ogre::Vector3(
			(north.x*cz + east.x*sz)*ca,
			sa,
			(north.y*cz + east.y*sz)*ca);
	}

	void BasicController::updateAstronomy()
	{
		using CaelumPort::Astronomy;
		using CaelumPort::LongReal;

		// Observer at longitude 0, so 'hour' is solar time; any non-leap year will do.
		const LongReal jdMidnight = Astronomy::getJulianDayFromGregorianDateTime(2001, 1, 1, 0, 0, 0.0) + (mDayOfYear - 1);
		const Ogre::Degree longitude(0.f);
		const Ogre::Degree latitude(mLatitude);

		Ogre::Degree azimuth, altitude;
		Astronomy::getHorizontalSunPosition(jdMidnight + mTime.x/24.0, longitude, latitude, azimuth, altitude);
		mSunDirection = horizontalToWorld(azimuth, altitude);

		Astronomy::getHorizontalMoonPosition(jdMidnight + mTime.x/24.0, longitude, latitude, azimuth, altitude);
		mMoonDirection = horizontalToWorld(azimuth, altitude);

		// Moon phase from elongation (ecliptic longitude of the moon minus that of the sun): new moon = covered (+-1), full moon = clear (0)
		const LongReal jd = jdMidnight + mTime.x/24.0;
		LongReal moonLonRad, moonLatRad;
		Astronomy::getEclipticMoonPositionRad(jd, moonLonRad, moonLatRad);
		const LongReal sunLonDeg = 280.460 + 0.9856474 * (jd - Astronomy::J2000);
		const LongReal moonLonDeg = moonLonRad * 180.0 / 3.14159265358979323846;
		LongReal elongation = std::fmod(moonLonDeg - sunLonDeg, 360.0);
		if (elongation < 0.0)
		{
			elongation += 360.0;
		}
		mMoonPhase = static_cast<Ogre::Real>((elongation - 180.0) / 180.0);

		// Sunrise and sunset: scan the day for altitude zero crossings (only when latitude or day changed)
		if (mSunTimesLatitude != mLatitude || mSunTimesDayOfYear != mDayOfYear)
		{
			mSunTimesLatitude = mLatitude;
			mSunTimesDayOfYear = mDayOfYear;

			const int STEPS = 480; // 3 minutes
			float sunrise = -1.f, sunset = -1.f;
			bool anyAbove = false, anyBelow = false;
			Ogre::Degree az, alt;
			Astronomy::getHorizontalSunPosition(jdMidnight, longitude, latitude, az, alt);
			float prevAlt = alt.valueDegrees();
			for (int i = 1; i <= STEPS; ++i)
			{
				const float hour = 24.f * i / STEPS;
				Astronomy::getHorizontalSunPosition(jdMidnight + hour/24.0, longitude, latitude, az, alt);
				const float curAlt = alt.valueDegrees();
				anyAbove |= (curAlt > 0.f);
				anyBelow |= (curAlt <= 0.f);
				if (prevAlt <= 0.f && curAlt > 0.f && sunrise < 0.f)
				{
					sunrise = hour - 24.f/STEPS * curAlt / (curAlt - prevAlt);
				}
				else if (prevAlt > 0.f && curAlt <= 0.f && sunset < 0.f)
				{
					sunset = hour - 24.f/STEPS * curAlt / (curAlt - prevAlt);
				}
				prevAlt = curAlt;
			}

			if (sunrise >= 0.f && sunset >= 0.f)
			{
				mTime.y = sunrise;
				mTime.z = sunset;
			}
			else if (anyAbove && !anyBelow) // polar day
			{
				mTime.y = 0.f;
				mTime.z = 24.f;
			}
			else // polar night
			{
				mTime.y = 12.f;
				mTime.z = 12.f;
			}
		}
	}
}
