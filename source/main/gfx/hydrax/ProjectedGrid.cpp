/*
--------------------------------------------------------------------------------
This source file is part of Hydrax.
Visit ---

Copyright (C) 2008 Xavier Verguín González <xavierverguin@hotmail.com>
                                           <xavyiy@gmail.com>

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

Based on the Projected Grid concept from Claes Johanson thesis:
http://graphics.cs.lth.se/theses/projects/projgrid/
and Ren Cheng Ogre3D implementation:
http://www.cnblogs.com/ArenAK/archive/2007/11/07/951713.html
--------------------------------------------------------------------------------
*/

#include <ProjectedGrid.h>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#	include <emmintrin.h>
#	define HYDRAX_PG_SSE2 1
#endif

#include <algorithm>
#include <functional>
#include <vector>

#include "Application.h"
#include "ThreadPool.h"

#define _def_MaxFarClipDistance 99999

namespace Hydrax{namespace Module
{
	// RIGSOFRODS: splits [0, count) into up to 4 chunks and runs f(begin, end) on the thread pool; the calling thread does the first chunk.
	// f must only write to its own range. Falls back to a plain call if there is no pool, multithreading is off or the work is small.
	template<class F> static void PGParallelFor(int count, int min_per_chunk, F& f)
	{
		RoR::ThreadPool* pool = RoR::App::GetThreadPool();
		int chunks = 1;
		if (pool != nullptr && RoR::App::gfx_water_multithread->getBool())
		{
			chunks = std::min(std::max(RoR::App::app_num_workers->getInt(), 1), 4);
		}
		chunks = std::min(chunks, std::max(count / std::max(min_per_chunk, 1), 1));
		if (chunks <= 1)
		{
			f(0, count);
			return;
		}
		std::vector<std::function<void()>> tasks;
		for (int c = 0; c < chunks; ++c)
		{
			const int begin = static_cast<int>(static_cast<long long>(count) * c / chunks);
			const int end = static_cast<int>(static_cast<long long>(count) * (c + 1) / chunks);
			tasks.push_back([&f, begin, end]() { f(begin, end); });
		}
		pool->Parallelize(tasks);
	}

	Mesh::VertexType _PG_getVertexTypeFromNormalMode(const MaterialManager::NormalMode& NormalMode)
	{
		if (NormalMode == MaterialManager::NM_VERTEX)
		{
			return Mesh::VT_POS_NORM;
		}

		// NM_RTT
		return Mesh::VT_POS;
	}

	Ogre::String _PG_getNormalModeString(const MaterialManager::NormalMode& NormalMode)
	{
		if (NormalMode == MaterialManager::NM_VERTEX)
		{
			return "Vertex";
		}

		return "Rtt";
	}

	ProjectedGrid::ProjectedGrid(Hydrax *h, Noise::Noise *n, const Ogre::Plane &BasePlane, const MaterialManager::NormalMode& NormalMode)
		: Module("ProjectedGrid" + _PG_getNormalModeString(NormalMode),
		         n, Mesh::Options(256, Size(0), _PG_getVertexTypeFromNormalMode(NormalMode)), NormalMode)
		, mHydrax(h)
		, mVertices(0)
		, mVerticesChoppyBuffer(0)
		, mBasePlane(BasePlane)
		, mNormal(BasePlane.normal)
		, mPos(Ogre::Vector3(0,0,0))
		, mProjectingCamera(0)
		, mTmpRndrngCamera(0)
		, mRenderingCamera(h->getCamera())
	{
	}

	ProjectedGrid::ProjectedGrid(Hydrax *h, Noise::Noise *n, const Ogre::Plane &BasePlane, const MaterialManager::NormalMode& NormalMode, const Options &Options)
		: Module("ProjectedGrid" + _PG_getNormalModeString(NormalMode),
		         n, Mesh::Options(Options.Complexity, Size(0), _PG_getVertexTypeFromNormalMode(NormalMode)), NormalMode)
		, mHydrax(h)
		, mVertices(0)
		, mVerticesChoppyBuffer(0)
		, mBasePlane(BasePlane)
		, mNormal(BasePlane.normal)
		, mPos(Ogre::Vector3(0,0,0))
		, mProjectingCamera(0)
		, mTmpRndrngCamera(0)
		, mRenderingCamera(h->getCamera())
	{
		setOptions(Options);
	}

	ProjectedGrid::~ProjectedGrid()
	{
		remove();

		HydraxLOG(getName() + " destroyed.");
	}

	void ProjectedGrid::setOptions(const Options &Options)
	{
		// Size(0) -> Infinite mesh
		mMeshOptions.MeshSize     = Size(0);
		mMeshOptions.MeshStrength = Options.Strength;
		mMeshOptions.MeshComplexity = Options.Complexity;

		mHydrax->getMesh()->setOptions(mMeshOptions);
		mHydrax->_setStrength(Options.Strength);

		// Re-create geometry if it's needed
		if (isCreated() && Options.Complexity != mOptions.Complexity)
		{
			remove();
			mOptions = Options;
			create();

			if (mNormalMode == MaterialManager::NM_RTT)
			{
				if (!mNoise->createGPUNormalMapResources(mHydrax->getGPUNormalMapManager()))
				{
					HydraxLOG(mNoise->getName() + " doesn't support GPU Normal map generation.");
				}
			}

		    Ogre::String MaterialNameTmp = mHydrax->getMesh()->getMaterialName();
		    mHydrax->getMesh()->remove();
		    mHydrax->getMesh()->setOptions(getMeshOptions());
		    mHydrax->getMesh()->setMaterialName(MaterialNameTmp);
		    mHydrax->getMesh()->create();

			// Force to recalculate the geometry on next frame
			mLastPosition = Ogre::Vector3(0,0,0);
			mLastOrientation = Ogre::Quaternion();

			return;
		}

		mOptions = Options;
	}

	void ProjectedGrid::create()
	{
		HydraxLOG("Creating " + getName() + " module.");

		Module::create();

		if (getNormalMode() == MaterialManager::NM_VERTEX)
		{
		    mVertices = new Mesh::POS_NORM_VERTEX[mOptions.Complexity*mOptions.Complexity];

			Mesh::POS_NORM_VERTEX* Vertices = static_cast<Mesh::POS_NORM_VERTEX*>(mVertices);

			for (int i = 0; i < mOptions.Complexity*mOptions.Complexity; i++)
			{
				Vertices[i].nx = 0;
				Vertices[i].ny = -1;
				Vertices[i].nz = 0;
			}

			mVerticesChoppyBuffer = new Mesh::POS_NORM_VERTEX[mOptions.Complexity*mOptions.Complexity];
		}
		else if(getNormalMode() == MaterialManager::NM_RTT)
		{
			mVertices = new Mesh::POS_VERTEX[mOptions.Complexity*mOptions.Complexity];
		}

	    _setDisplacementAmplitude(0.0f);

		mTmpRndrngCamera  = new Ogre::Camera("PG_TmpRndrngCamera", NULL);
		mProjectingCamera = new Ogre::Camera("PG_ProjectingCamera", NULL);

		HydraxLOG(getName() + " created.");
	}

	void ProjectedGrid::remove()
	{
		if (!isCreated())
		{
			return;
		}

		Module::remove();

		if (mVertices)
		{
			if (getNormalMode() == MaterialManager::NM_VERTEX)
			{
				delete [] static_cast<Mesh::POS_NORM_VERTEX*>(mVertices);
				mVertices = 0;
			}
			else if (getNormalMode() == MaterialManager::NM_RTT)
			{
				delete [] static_cast<Mesh::POS_VERTEX*>(mVertices);
				mVertices = 0;
			}
		}

		if (mVerticesChoppyBuffer)
		{
			delete [] mVerticesChoppyBuffer;
		}

		if (mTmpRndrngCamera)
		{
			delete mTmpRndrngCamera;
			delete mProjectingCamera;
		}

		mLastPosition = Ogre::Vector3(0,0,0);
		mLastOrientation = Ogre::Quaternion();
	}

	void ProjectedGrid::saveCfg(Ogre::String &Data)
	{
		Module::saveCfg(Data);

		Data += CfgFileManager::_getCfgString("PG_ChoopyStrength", mOptions.ChoppyStrength);
		Data += CfgFileManager::_getCfgString("PG_ChoppyWaves", mOptions.ChoppyWaves);
		Data += CfgFileManager::_getCfgString("PG_Complexity", mOptions.Complexity);
		Data += CfgFileManager::_getCfgString("PG_Elevation", mOptions.Elevation);
		Data += CfgFileManager::_getCfgString("PG_ForceRecalculateGeometry", mOptions.ForceRecalculateGeometry);
		Data += CfgFileManager::_getCfgString("PG_Smooth", mOptions.Smooth);
		Data += CfgFileManager::_getCfgString("PG_Strength", mOptions.Strength); Data += "\n";
	}

	bool ProjectedGrid::loadCfg(Ogre::ConfigFile &CfgFile)
	{
		if (!Module::loadCfg(CfgFile))
		{
			return false;
		}

        HydraxLOG("\tReading options...");
		setOptions(
			Options(CfgFileManager::_getIntValue(CfgFile,   "PG_Complexity"),
			        CfgFileManager::_getFloatValue(CfgFile, "PG_Strength"),
					CfgFileManager::_getFloatValue(CfgFile, "PG_Elevation"),
					CfgFileManager::_getBoolValue(CfgFile,  "PG_Smooth"),
					CfgFileManager::_getBoolValue(CfgFile,  "PG_ForceRecalculateGeometry"),
					CfgFileManager::_getBoolValue(CfgFile,  "PG_ChoppyWaves"),
					CfgFileManager::_getFloatValue(CfgFile, "PG_ChoopyStrength")));

        HydraxLOG("\tOptions readed.");

		return true;
	}

	void ProjectedGrid::update(const Ogre::Real &timeSinceLastFrame)
	{
		if (!isCreated())
		{
			return;
		}

		Module::update(timeSinceLastFrame);

		Ogre::Vector3 RenderingCameraPos = mRenderingCamera->getDerivedPosition();

		if (mLastPosition    != RenderingCameraPos    ||
			mLastOrientation != mRenderingCamera->getDerivedOrientation() ||
			mOptions.ForceRecalculateGeometry)
		{
			if (mLastPosition != RenderingCameraPos)
			{
				Ogre::Vector3 HydraxPos = Ogre::Vector3(RenderingCameraPos.x,mHydrax->getPosition().y,RenderingCameraPos.z);

		        mHydrax->getMesh()->getSceneNode()->setPosition(HydraxPos);
		        mHydrax->getRttManager()->getPlanesSceneNode()->setPosition(HydraxPos);

		        // For world-space -> object-space conversion
				mHydrax->setSunPosition(mHydrax->getSunPosition());
			}

			float RenderingFarClipDistance = mRenderingCamera->getFarClipDistance();

		    if (RenderingFarClipDistance > _def_MaxFarClipDistance)
		    {
			    mRenderingCamera->setFarClipDistance(_def_MaxFarClipDistance);
		    }

			mFlatDone = false;
			mLastMinMax = _getMinMax(&mRange);

		    if (mLastMinMax)
		    {
			    _renderGeometry(mRange, mProjectingCamera->getViewMatrix(), RenderingCameraPos);

			    mHydrax->getMesh()->updateGeometry(mOptions.Complexity*mOptions.Complexity, mVertices);
		    }

			mRenderingCamera->setFarClipDistance(RenderingFarClipDistance);
		}
		else if (mLastMinMax && !(mOptions.Strength == 0.f && mFlatDone))
		{
			// Every vertex only writes itself: the wave heights are computed in parallel
			const int total = mOptions.Complexity*mOptions.Complexity;
			const Ogre::Real baseY = -mBasePlane.d;
			if (getNormalMode() == MaterialManager::NM_VERTEX)
			{
				Mesh::POS_NORM_VERTEX* Vertices = static_cast<Mesh::POS_NORM_VERTEX*>(mVertices);
				const bool choppy = mOptions.ChoppyWaves;
				auto heights = [&](int begin, int end)
				{
					for (int i = begin; i < end; i++)
					{
						if (choppy)
						{
							Vertices[i] = mVerticesChoppyBuffer[i];
						}
						Vertices[i].y = baseY + _getWaveHeight(RenderingCameraPos.x + Vertices[i].x, RenderingCameraPos.z + Vertices[i].z);
					}
				};
				PGParallelFor(total, 4096, heights);
			}
			else if (getNormalMode() == MaterialManager::NM_RTT)
			{
				Mesh::POS_VERTEX* Vertices = static_cast<Mesh::POS_VERTEX*>(mVertices);
				auto heights = [&](int begin, int end)
				{
					for (int i = begin; i < end; i++)
					{
						Vertices[i].y = baseY + _getWaveHeight(RenderingCameraPos.x + Vertices[i].x, RenderingCameraPos.z + Vertices[i].z);
					}
				};
				PGParallelFor(total, 4096, heights);
			}

			// Smooth the heightdata
		    if (mOptions.Smooth)
		    {
				if (getNormalMode() == MaterialManager::NM_VERTEX)
				{
					Mesh::POS_NORM_VERTEX* Vertices = static_cast<Mesh::POS_NORM_VERTEX*>(mVertices);

					for(int v=1; v<(mOptions.Complexity-1); v++)
					{
						for(int u=1; u<(mOptions.Complexity-1); u++)
						{
							Vertices[v*mOptions.Complexity + u].y =
								 0.2f *
								(Vertices[v    *mOptions.Complexity + u    ].y +
								 Vertices[v    *mOptions.Complexity + (u+1)].y +
								 Vertices[v    *mOptions.Complexity + (u-1)].y +
								 Vertices[(v+1)*mOptions.Complexity + u    ].y +
								 Vertices[(v-1)*mOptions.Complexity + u    ].y);
						}
					}
				}
				else if(getNormalMode() == MaterialManager::NM_RTT)
				{
					Mesh::POS_VERTEX* Vertices = static_cast<Mesh::POS_VERTEX*>(mVertices);

					for(int v=1; v<(mOptions.Complexity-1); v++)
					{
						for(int u=1; u<(mOptions.Complexity-1); u++)
						{
							Vertices[v*mOptions.Complexity + u].y =
								 0.2f *
								(Vertices[v    *mOptions.Complexity + u    ].y +
								 Vertices[v    *mOptions.Complexity + (u+1)].y +
								 Vertices[v    *mOptions.Complexity + (u-1)].y +
								 Vertices[(v+1)*mOptions.Complexity + u    ].y +
								 Vertices[(v-1)*mOptions.Complexity + u    ].y);
						}
					}
				}
		    }

			_calculeNormals();

			_performChoppyWaves();

			mHydrax->getMesh()->updateGeometry(mOptions.Complexity*mOptions.Complexity, mVertices);

			// Without waves the surface doesn't change until the camera moves
			mFlatDone = (mOptions.Strength == 0.f);
		}

		mLastPosition = RenderingCameraPos;
		mLastOrientation = mRenderingCamera->getDerivedOrientation();
	}

	bool ProjectedGrid::_renderGeometry(const Ogre::Matrix4& m,const Ogre::Matrix4& _viewMat, const Ogre::Vector3& WorldPos)
	{
		t_corners0 = _calculeWorldPosition(Ogre::Vector2( 0.0f, 0.0f),m,_viewMat);
		t_corners1 = _calculeWorldPosition(Ogre::Vector2(+1.0f, 0.0f),m,_viewMat);
		t_corners2 = _calculeWorldPosition(Ogre::Vector2( 0.0f,+1.0f),m,_viewMat);
		t_corners3 = _calculeWorldPosition(Ogre::Vector2(+1.0f,+1.0f),m,_viewMat);

		const float du = 1.0f/(mOptions.Complexity-1),
			        dv = 1.0f/(mOptions.Complexity-1);

		int iv, iu;

		{
			const int C = mOptions.Complexity;
			const Ogre::Real baseY = -mBasePlane.d;
			// Position and height of one vertex of the projected grid
			auto computeVertex = [&](int row, int col, float& outX, float& outY, float& outZ)
			{
				const float rv = row * dv;
				const float r1v = 1.0f - rv;
				const float cu = col * du;
				const float c1u = 1.0f - cu;
				float rx = r1v*(c1u*t_corners0.x + cu*t_corners1.x) + rv*(c1u*t_corners2.x + cu*t_corners3.x);
				float rz = r1v*(c1u*t_corners0.z + cu*t_corners1.z) + rv*(c1u*t_corners2.z + cu*t_corners3.z);
				const float rw = r1v*(c1u*t_corners0.w + cu*t_corners1.w) + rv*(c1u*t_corners2.w + cu*t_corners3.w);
				const float inv = 1.0f/rw;
				rx *= inv;
				rz *= inv;
				outX = rx;
				outZ = rz;
				outY = baseY + _getWaveHeight(WorldPos.x + rx, WorldPos.z + rz);
			};

			// Every row only writes its own vertices: the rows are computed in parallel
			if (getNormalMode() == MaterialManager::NM_VERTEX)
			{
				Mesh::POS_NORM_VERTEX* Vertices = static_cast<Mesh::POS_NORM_VERTEX*>(mVertices);
				auto rows = [&](int begin, int end)
				{
					for (int row = begin; row < end; row++)
						for (int col = 0; col < C; col++)
						{
							Mesh::POS_NORM_VERTEX& vtx = Vertices[row*C + col];
							computeVertex(row, col, vtx.x, vtx.y, vtx.z);
						}
				};
				PGParallelFor(C, 16, rows);

				if (mOptions.ChoppyWaves)
				{
					for(int k = 0; k < C*C; k++)
					{
						mVerticesChoppyBuffer[k] = Vertices[k];
					}
				}
			}
			else if (getNormalMode() == MaterialManager::NM_RTT)
			{
				Mesh::POS_VERTEX* Vertices = static_cast<Mesh::POS_VERTEX*>(mVertices);
				auto rows = [&](int begin, int end)
				{
					for (int row = begin; row < end; row++)
						for (int col = 0; col < C; col++)
						{
							Mesh::POS_VERTEX& vtx = Vertices[row*C + col];
							computeVertex(row, col, vtx.x, vtx.y, vtx.z);
						}
				};
				PGParallelFor(C, 16, rows);
			}
		}

		// Smooth the heightdata
		if (mOptions.Smooth)
		{
			if (getNormalMode() == MaterialManager::NM_VERTEX)
			{
				Mesh::POS_NORM_VERTEX* Vertices = static_cast<Mesh::POS_NORM_VERTEX*>(mVertices);

				for(iv=1; iv<(mOptions.Complexity-1); iv++)
				{
					for(iu=1; iu<(mOptions.Complexity-1); iu++)
					{
						Vertices[iv*mOptions.Complexity + iu].y =
							 0.2f *
							(Vertices[iv    *mOptions.Complexity + iu    ].y +
							 Vertices[iv    *mOptions.Complexity + (iu+1)].y +
							 Vertices[iv    *mOptions.Complexity + (iu-1)].y +
							 Vertices[(iv+1)*mOptions.Complexity + iu    ].y +
							 Vertices[(iv-1)*mOptions.Complexity + iu    ].y);
					}
				}
			}
			else if(getNormalMode() == MaterialManager::NM_RTT)
			{
				Mesh::POS_VERTEX* Vertices = static_cast<Mesh::POS_VERTEX*>(mVertices);

				for(iv=1; iv<(mOptions.Complexity-1); iv++)
				{
					for(iu=1; iu<(mOptions.Complexity-1); iu++)
					{
						Vertices[iv*mOptions.Complexity + iu].y =
							 0.2f *
							(Vertices[iv    *mOptions.Complexity + iu    ].y +
							 Vertices[iv    *mOptions.Complexity + (iu+1)].y +
							 Vertices[iv    *mOptions.Complexity + (iu-1)].y +
							 Vertices[(iv+1)*mOptions.Complexity + iu    ].y +
							 Vertices[(iv-1)*mOptions.Complexity + iu    ].y);
					}
				}
			}
		}

		_calculeNormals();

		_performChoppyWaves();

		return true;
	}

	void ProjectedGrid::_calculeNormals()
	{
		if (getNormalMode() != MaterialManager::NM_VERTEX)
		{
			return;
		}

		const int n = mOptions.Complexity;

		Mesh::POS_NORM_VERTEX* Vertices = static_cast<Mesh::POS_NORM_VERTEX*>(mVertices);

		// Each row only writes its own normals and reads positions, so rows can be processed independently
		auto rows = [&](int begin, int end)
		{
			for(int v=begin; v<end; v++)
			{
				const Mesh::POS_NORM_VERTEX* prev = Vertices + (v-1)*n;
				Mesh::POS_NORM_VERTEX*       row  = Vertices + v*n;
				const Mesh::POS_NORM_VERTEX* next = Vertices + (v+1)*n;

				for(int u=1; u<(n-1); u++)
				{
#ifdef HYDRAX_PG_SSE2
					// x,y,z of a vertex as (x, y, z, 0) - doesn't touch the normal, which other threads may be writing
					auto load3 = [](const float* p) { return _mm_movelh_ps(_mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(p)), _mm_load_ss(p + 2)); };
					const __m128 v1 = _mm_sub_ps(load3(&row[u+1].x), load3(&row[u-1].x)); // right - left
					const __m128 v2 = _mm_sub_ps(load3(&next[u].x), load3(&prev[u].x));   // next row - previous row
					// normal = v2 x v1, same operations as Ogre::Vector3::crossProduct
					const __m128 a = _mm_mul_ps(_mm_shuffle_ps(v2, v2, _MM_SHUFFLE(3,0,2,1)), _mm_shuffle_ps(v1, v1, _MM_SHUFFLE(3,1,0,2)));
					const __m128 b = _mm_mul_ps(_mm_shuffle_ps(v2, v2, _MM_SHUFFLE(3,1,0,2)), _mm_shuffle_ps(v1, v1, _MM_SHUFFLE(3,0,2,1)));
					const __m128 nrm = _mm_sub_ps(a, b);
					_mm_storel_pi(reinterpret_cast<__m64*>(&row[u].nx), nrm);            // nx, ny
					_mm_store_ss(&row[u].nz, _mm_movehl_ps(nrm, nrm));                   // nz
#else
					// vec1 = right - left, vec2 = next row - previous row
					const float v1x = row[u+1].x - row[u-1].x;
					const float v1y = row[u+1].y - row[u-1].y;
					const float v1z = row[u+1].z - row[u-1].z;

					const float v2x = next[u].x - prev[u].x;
					const float v2y = next[u].y - prev[u].y;
					const float v2z = next[u].z - prev[u].z;

					// normal = vec2 x vec1 (same operations as Ogre::Vector3::crossProduct)
					row[u].nx = v2y*v1z - v2z*v1y;
					row[u].ny = v2z*v1x - v2x*v1z;
					row[u].nz = v2x*v1y - v2y*v1x;
#endif
				}
			}
		};

		// Rows 1 .. n-2: split into the range [0, n-2) shifted by one
		auto shifted = [&](int begin, int end) { rows(begin + 1, end + 1); };
		PGParallelFor(n - 2, 16, shifted);
	}

	void ProjectedGrid::_performChoppyWaves()
	{
		if (getNormalMode() != MaterialManager::NM_VERTEX || !mOptions.ChoppyWaves)
		{
			return;
		}

		int v, u,
		    Underwater = 1;

		if (mHydrax->_isCurrentFrameUnderwater())
		{
			Underwater = -1;
		}

		const float CHOPPY_MAX_CELL = 4.f; // metres
		float Dis1,  Dis2;//,
		   // Dis1_, Dis2_;

		Ogre::Vector3 CameraDir, Norm;
		Ogre::Vector2 Dir, Perp, Norm2;

		CameraDir = mRenderingCamera->getDerivedDirection();
		Dir       = Ogre::Vector2(CameraDir.x, CameraDir.z).normalisedCopy();
		Perp      = Dir.perpendicular();

		if (Dir.x < 0 ) Dir.x = -Dir.x;
		if (Dir.y < 0 ) Dir.y = -Dir.y;

		if (Perp.x < 0 ) Perp.x = -Perp.x;
		if (Perp.y < 0 ) Perp.y = -Perp.y;

		Mesh::POS_NORM_VERTEX* Vertices = static_cast<Mesh::POS_NORM_VERTEX*>(mVertices);

		for(v=1; v<(mOptions.Complexity-1); v++)
		{
			Dis1 =  (Ogre::Vector2(mVerticesChoppyBuffer[v*mOptions.Complexity + 1].x,
					               mVerticesChoppyBuffer[v*mOptions.Complexity + 1].z) -
					 Ogre::Vector2(mVerticesChoppyBuffer[(v+1)*mOptions.Complexity + 1].x,
				                   mVerticesChoppyBuffer[(v+1)*mOptions.Complexity + 1].z)).length();
			// The cells of the projected grid grow to kilometres towards the horizon; unclamped, the choppy shift of the
			// far rows would swing by hundreds of metres every frame (trembling water edge)
			Dis1 = std::min(Dis1, CHOPPY_MAX_CELL);

			/*Dis1_ = (Ogre::Vector2(mVerticesChoppyBuffer[v*mOptions.Complexity + 1].x,
                                   mVerticesChoppyBuffer[v*mOptions.Complexity + 1].z) -
				   	 Ogre::Vector2(mVerticesChoppyBuffer[(v-1)*mOptions.Complexity + 1].x,
					               mVerticesChoppyBuffer[(v-1)*mOptions.Complexity + 1].z)).length();

			Dis1 = (Dis1+Dis1_)/2;*/

			for(u=1; u<(mOptions.Complexity-1); u++)
			{
				Dis2 = (Ogre::Vector2(mVerticesChoppyBuffer[v*mOptions.Complexity + u].x,
					                  mVerticesChoppyBuffer[v*mOptions.Complexity + u].z) -
					    Ogre::Vector2(mVerticesChoppyBuffer[v*mOptions.Complexity + u+1].x,
					                  mVerticesChoppyBuffer[v*mOptions.Complexity + u+1].z)).length();
				Dis2 = std::min(Dis2, CHOPPY_MAX_CELL);
/*
				Dis2_ = (Ogre::Vector2(mVerticesChoppyBuffer[v*mOptions.Complexity + u].x,
					                   mVerticesChoppyBuffer[v*mOptions.Complexity + u].z) -
					     Ogre::Vector2(mVerticesChoppyBuffer[v*mOptions.Complexity + u-1].x,
				                       mVerticesChoppyBuffer[v*mOptions.Complexity + u-1].z)).length();

				Dis2 = (Dis2+Dis2_)/2;*/

				Norm = Ogre::Vector3(Vertices[v*mOptions.Complexity + u].nx,
					                 Vertices[v*mOptions.Complexity + u].ny,
								     Vertices[v*mOptions.Complexity + u].nz).
					   			     normalisedCopy();

				Norm2 = Ogre::Vector2(Norm.x, Norm.z)  *
					                 ( (Dir  * Dis1)   +
					                   (Perp * Dis2))  *
				 				      mOptions.ChoppyStrength;

				Vertices[v*mOptions.Complexity + u].x = mVerticesChoppyBuffer[v*mOptions.Complexity + u].x + Norm2.x * Underwater;
				Vertices[v*mOptions.Complexity + u].z = mVerticesChoppyBuffer[v*mOptions.Complexity + u].z + Norm2.y * Underwater;
			}
		}
	}

	// Check the point of intersection with the plane (0,1,0,0) and return the position in homogenous coordinates
	Ogre::Vector4 ProjectedGrid::_calculeWorldPosition(const Ogre::Vector2 &uv, const Ogre::Matrix4& m, const Ogre::Matrix4& _viewMat)
	{
		Ogre::Vector4 origin(uv.x,uv.y,-1,1);
		Ogre::Vector4 direction(uv.x,uv.y,1,1);

		origin = m*origin;
		direction = m*direction;

		Ogre::Vector3 _org(origin.x/origin.w,origin.y/origin.w,origin.z/origin.w);
		Ogre::Vector3 _dir(direction.x/direction.w,direction.y/direction.w,direction.z/direction.w);
		_dir -= _org;
		_dir.normalise();

		Ogre::Ray _ray(_org,_dir);
		std::pair<bool,Ogre::Real> _result = _ray.intersects(mBasePlane);
		float l = _result.second;
		Ogre::Vector3 worldPos = _org + _dir*l;
		Ogre::Vector4 _tempVec = _viewMat*Ogre::Vector4(worldPos);
		float _temp = -_tempVec.z/_tempVec.w;
		Ogre::Vector4 retPos(worldPos);
		retPos /= _temp;

		return retPos;
	}

	bool ProjectedGrid::_getMinMax(Ogre::Matrix4 *range)
	{
		_setDisplacementAmplitude(mOptions.Strength);

		float x_min,y_min,x_max,y_max;
		Ogre::Vector3 frustum[8],proj_points[24];

		int i,
			n_points = 0,
			src, dst;

		int cube[] =
		   {0,1,	0,2,	2,3,	1,3,
		    0,4,	2,6,	3,7,	1,5,
		    4,6,	4,5,	5,7,	6,7};

		Ogre::Vector3 _testLine;
		Ogre::Real _dist;
		Ogre::Ray _ray;

		std::pair<bool,Ogre::Real> _result;

		// Set temporal rendering camera parameters
		mTmpRndrngCamera->setFrustumOffset(mRenderingCamera->getFrustumOffset());
		mTmpRndrngCamera->setAspectRatio(mRenderingCamera->getAspectRatio());
		mTmpRndrngCamera->setDirection(mRenderingCamera->getDerivedDirection());
		mTmpRndrngCamera->setFarClipDistance(mRenderingCamera->getFarClipDistance());
		mTmpRndrngCamera->setFOVy(mRenderingCamera->getFOVy());
		mTmpRndrngCamera->setNearClipDistance(mRenderingCamera->getNearClipDistance());
		mTmpRndrngCamera->setOrientation(mRenderingCamera->getDerivedOrientation());
		mTmpRndrngCamera->setPosition(0, mRenderingCamera->getDerivedPosition().y - mHydrax->getPosition().y, 0);

		Ogre::Matrix4 invviewproj = (mTmpRndrngCamera->getProjectionMatrixWithRSDepth()*mTmpRndrngCamera->getViewMatrix()).inverse();
		frustum[0] = invviewproj * Ogre::Vector3(-1,-1,0);
		frustum[1] = invviewproj * Ogre::Vector3(+1,-1,0);
		frustum[2] = invviewproj * Ogre::Vector3(-1,+1,0);
		frustum[3] = invviewproj * Ogre::Vector3(+1,+1,0);
		frustum[4] = invviewproj * Ogre::Vector3(-1,-1,+1);
		frustum[5] = invviewproj * Ogre::Vector3(+1,-1,+1);
		frustum[6] = invviewproj * Ogre::Vector3(-1,+1,+1);
		frustum[7] = invviewproj * Ogre::Vector3(+1,+1,+1);

		// Check intersections with upper_bound and lower_bound
		for(i=0; i<12; i++)
		{
			src=cube[i*2]; dst=cube[i*2+1];
			_testLine = frustum[dst]-frustum[src];
			_dist = _testLine.normalise();
			_ray = Ogre::Ray(frustum[src], _testLine);
			_result = Ogre::Math::intersects(_ray,mUpperBoundPlane);
			if ((_result.first) && (_result.second<_dist+0.00001))
			{
				proj_points[n_points++] = frustum[src] + _result.second * _testLine;
			}
			_result = Ogre::Math::intersects(_ray,mLowerBoundPlane);
			if ((_result.first) && (_result.second<_dist+0.00001))
			{
				proj_points[n_points++] = frustum[src] + _result.second * _testLine;
			}
		}

		// Check if any of the frustums vertices lie between the upper_bound and lower_bound planes
		for(i=0; i<8; i++)
		{
			if(mUpperBoundPlane.getDistance(frustum[i])/mLowerBoundPlane.getDistance(frustum[i]) < 0)
			{
				proj_points[n_points++] = frustum[i];
			}
		}

		// Set projecting camera parameters
		mProjectingCamera->setFrustumOffset(mTmpRndrngCamera->getFrustumOffset());
		mProjectingCamera->setAspectRatio(mTmpRndrngCamera->getAspectRatio());
		mProjectingCamera->setDirection(mTmpRndrngCamera->getDerivedDirection());
		mProjectingCamera->setFarClipDistance(mTmpRndrngCamera->getFarClipDistance());
		mProjectingCamera->setFOVy(mTmpRndrngCamera->getFOVy());
		mProjectingCamera->setNearClipDistance(mTmpRndrngCamera->getNearClipDistance());
		mProjectingCamera->setOrientation(mTmpRndrngCamera->getDerivedOrientation());
		mProjectingCamera->setPosition(mTmpRndrngCamera->getDerivedPosition());

		// Make sure the camera isn't too close to the plane
		float height_in_plane = mBasePlane.getDistance(mProjectingCamera->getRealPosition());

		bool keep_it_simple = false,
			 underwater     = false;

		if (height_in_plane < 0.0f)
		{
			underwater = true;
		}

		if (keep_it_simple)
		{
			mProjectingCamera->setDirection(mTmpRndrngCamera->getDerivedDirection());
		}
		else
		{
			Ogre::Vector3 aimpoint, aimpoint2;

			if (height_in_plane < (mOptions.Strength + mOptions.Elevation))
			{
				if (underwater)
				{
					mProjectingCamera->setPosition(mProjectingCamera->getRealPosition()+mLowerBoundPlane.normal*(mOptions.Strength + mOptions.Elevation - 2*height_in_plane));
				}
				else
				{
					mProjectingCamera->setPosition(mProjectingCamera->getRealPosition()+mLowerBoundPlane.normal*(mOptions.Strength + mOptions.Elevation - height_in_plane));
				}
			}

			// Aim the projector at the point where the camera view-vector intersects the plane
			// if the camera is aimed away from the plane, mirror it's view-vector against the plane
			if (((mBasePlane.normal).dotProduct(mTmpRndrngCamera->getDerivedDirection()) < 0.0f) || ((mBasePlane.normal).dotProduct(mTmpRndrngCamera->getDerivedPosition()) < 0.0f ) )
			{
				_ray = Ogre::Ray(mTmpRndrngCamera->getDerivedPosition(), mTmpRndrngCamera->getDerivedDirection());
				_result = Ogre::Math::intersects(_ray,mBasePlane);

				if(!_result.first)
				{
					_result.second = -_result.second;
				}

				aimpoint = mTmpRndrngCamera->getDerivedPosition() + _result.second * mTmpRndrngCamera->getDerivedDirection();
			}
			else
			{
				Ogre::Vector3 flipped = mTmpRndrngCamera->getDerivedDirection() - 2*mNormal* (mTmpRndrngCamera->getDerivedDirection()).dotProduct(mNormal);
				flipped.normalise();
				_ray = Ogre::Ray( mTmpRndrngCamera->getDerivedPosition(), flipped);
				_result = Ogre::Math::intersects(_ray,mBasePlane);

				aimpoint = mTmpRndrngCamera->getDerivedPosition() + _result.second * flipped;
			}

			// Force the point the camera is looking at in a plane, and have the projector look at it
			// works well against horizon, even when camera is looking upwards
			// doesn't work straight down/up
			float af = fabs((mBasePlane.normal).dotProduct(mTmpRndrngCamera->getDerivedDirection()));
			aimpoint2 = mTmpRndrngCamera->getDerivedPosition() + 10.0*mTmpRndrngCamera->getDerivedDirection();
			aimpoint2 = aimpoint2 - mNormal* (aimpoint2.dotProduct(mNormal));

			// Fade between aimpoint & aimpoint2 depending on view angle
			aimpoint = aimpoint*af + aimpoint2*(1.0f-af);

			mProjectingCamera->setDirection(aimpoint-mProjectingCamera->getRealPosition());
		}

		for(i=0; i<n_points; i++)
		{
			// Project the point onto the surface plane
			proj_points[i] = proj_points[i] - mBasePlane.normal*mBasePlane.getDistance(proj_points[i]);
			proj_points[i] = mProjectingCamera->getViewMatrix() * proj_points[i];
			proj_points[i] = mProjectingCamera->getProjectionMatrixWithRSDepth() * proj_points[i];
		}

		// Get max/min x & y-values to determine how big the "projection window" must be
		if (n_points > 0)
		{
			x_min = proj_points[0].x;
			x_max = proj_points[0].x;
			y_min = proj_points[0].y;
			y_max = proj_points[0].y;

			for(i=1; i<n_points; i++)
			{
				if (proj_points[i].x > x_max) x_max = proj_points[i].x;
				if (proj_points[i].x < x_min) x_min = proj_points[i].x;
				if (proj_points[i].y > y_max) y_max = proj_points[i].y;
				if (proj_points[i].y < y_min) y_min = proj_points[i].y;
			}

			// Build the packing matrix that spreads the grid across the "projection window"
			Ogre::Matrix4 pack(x_max-x_min,	0,				0,		x_min,
				               0,			y_max-y_min,	0,		y_min,
				               0,			0,				1,		0,
				               0,			0,				0,		1);

			Ogre::Matrix4 invviewproj = (mProjectingCamera->getProjectionMatrixWithRSDepth()*mProjectingCamera->getViewMatrix()).inverse();
			*range = invviewproj * pack;

			return true;
		}

		return false;
	}

	void ProjectedGrid::_setDisplacementAmplitude(const float &Amplitude)
	{
		mUpperBoundPlane = Ogre::Plane( mNormal, mPos + Amplitude * mNormal);
		mLowerBoundPlane = Ogre::Plane( mNormal, mPos - Amplitude * mNormal);
	}

	Ogre::Real ProjectedGrid::_getWaveHeight(const Ogre::Real& x, const Ogre::Real& z)
	{
		Ogre::Real h = mNoise->getValue(x, z) * mOptions.Strength;

		const auto& TerrainHeight = mHydrax->getTerrainHeightFunction();
		if (TerrainHeight)
		{
			// Water depth at this point. The waves fade out over half of the wave height (at least 0.5 m) so that they
			// vanish where the water gets shallow and never rise above the beach.
			const Ogre::Real depth = (mHydrax->getPosition().y - mBasePlane.d) - TerrainHeight(x, z);
			const Ogre::Real fade = Ogre::Math::Abs(mOptions.Strength) * 0.5f > 0.5f ? Ogre::Math::Abs(mOptions.Strength) * 0.5f : 0.5f;
			const Ogre::Real k = depth <= 0 ? 0 : (depth >= fade ? 1 : depth / fade);
			h *= k * k * (3 - 2 * k); // smoothstep
		}

		return h;
	}

	float ProjectedGrid::getHeigth(const Ogre::Vector2 &Position)
	{
		return mHydrax->getPosition().y + _getWaveHeight(Position.x, Position.y);
	}
}}
