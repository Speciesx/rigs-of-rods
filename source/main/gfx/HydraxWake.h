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

/*
    @file
    @brief  Bow and stern waves of boats (and anything else moving through the water) for the Hydrax surface.

    A height field (N*N cells) follows the camera. Every frame the "occupancy" of the water surface by submerged
    nodes is splatted into the grid; its change since the last frame is the volume the hulls pushed away.
    That volume is taken from under the hull (narrow blur) and put around it (wide blur), so moving hulls raise
    a bow wave and leave a trough behind them, while resting hulls do nothing. A damped 2D wave equation carries
    the waves away (wave speed ~3 m/s: a boat at 10 m/s gets a V wake of about 17 degrees, close to the real
    Kelvin angle). The result is visual only: Hydrax adds it to the vertices, physics does not feel it.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace RoR {

class HydraxWake
{
public:
    static constexpr int   N = 256;           //!< Cells per side
    static constexpr float DX = 0.5f;     //!< Cell size [m] -> 128 m around the camera
    static constexpr float SPEED = 3.0f;  //!< Wave speed [m/s]
    static constexpr float STEP = 1.f / 40.f; //!< Fixed sim step [s]; SPEED*STEP/DX = 0.15 (stable below 0.7)
    static constexpr float DAMPING = 0.9f;    //!< Velocity damping [1/s]
    static constexpr float GAIN = 3.0f;       //!< Height per unit of displaced occupancy
    static constexpr float MAX_HEIGHT = 1.5f; //!< Clamp [m]
    static constexpr int   RECENTER = 8;      //!< Recenter after the camera moved this many cells
    static constexpr int   BORDER = 48;       //!< Absorbing border, faded out [cells]

    /// Runs the given tasks in parallel and returns when all are done (e.g. RoR::ThreadPool::Parallelize); optional
    void SetParallel(std::function<void(std::vector<std::function<void()>>&)> f, int num_chunks) { m_parallel = std::move(f); m_num_chunks = num_chunks; }

    /// Terrain height at a world position (x, z); optional, masks out land
    void SetTerrainFunction(std::function<float(float, float)> f) { m_terrain = std::move(f); m_mask_dirty = true; }

    // --- Per frame: BeginFrame(), per actor BeginActor(), AddNode() for its submerged nodes, EndActor(); EndFrame() ---

    void BeginFrame(float cam_x, float cam_z, float water_y)
    {
        if (m_h.empty())
            this->Allocate();
        if (water_y != m_water_y)
        {
            m_water_y = water_y;
            m_mask_dirty = true;
        }
        this->Follow(cam_x, cam_z);
        if (m_mask_dirty)
        {
            this->UpdateMask(0, N, 0, N);
            m_mask_dirty = false;
        }
        std::fill(m_occ.begin(), m_occ.end(), 0.f);
        m_occ_box = Box();
        m_num_nodes = 0;
        ++m_frame;
    }

    /// Starts an actor at world position (x, z); an actor that jumped (reset, respawn) makes no splash
    void BeginActor(int actor_id, float x, float z)
    {
        m_cur_actor = &m_actors[actor_id];
        const bool seen = (m_cur_actor->last_frame + 1 == m_frame);
        const bool teleported = seen && (std::abs(x - m_cur_actor->x) > 10.f || std::abs(z - m_cur_actor->z) > 10.f);
        m_cur_new = !seen || teleported;
        if (teleported)
        {
            // Remove the old footprint without a wave (it stays in both occupancy frames)
            this->Raster(m_occ, m_cur_actor->poly, m_cur_actor->weight, m_occ_box);
        }
        m_cur_actor->last_frame = m_frame;
        m_cur_actor->x = x;
        m_cur_actor->z = z;
        m_cur_actor->poly.clear();
        m_cur_actor->weight = 0.f;
        m_pts.clear();
    }

    /// Submerged node at world position (x, z); together they make the footprint of the hull
    void AddNode(float x, float z)
    {
        if (m_cur_actor != nullptr)
            m_pts.push_back(Pt{x, z});
    }

    /// Ends the actor: its footprint (convex hull of the nodes) displaces the water; `weight` scales the waves
    void EndActor(float weight)
    {
        if (m_cur_actor == nullptr)
            return;
        if (m_pts.size() >= 3 && weight > 0.f)
        {
            ConvexHull(m_pts, m_cur_actor->poly);
            m_cur_actor->weight = weight;
            this->Raster(m_occ, m_cur_actor->poly, weight, m_occ_box);
            if (m_cur_new)
                this->Raster(m_occ_prev, m_cur_actor->poly, weight, m_prev_box); // appears without a splash
            m_num_nodes += static_cast<int>(m_pts.size());
        }
        m_cur_actor = nullptr;
    }

    /// Advances the waves by `dt` seconds (0 = paused)
    void EndFrame(float dt)
    {
        m_cur_actor = nullptr;

        // Actors that are gone (despawned, left the water area) vanish without a splash
        for (auto it = m_actors.begin(); it != m_actors.end(); )
        {
            if (it->second.last_frame != m_frame)
            {
                this->Raster(m_occ, it->second.poly, it->second.weight, m_occ_box);
                it = m_actors.erase(it);
            }
            else
                ++it;
        }

        if (dt <= 0.f)
        {
            std::swap(m_occ, m_occ_prev);
            m_prev_box = m_occ_box;
            return;
        }

        // Displaced volume: taken from under the hull, put around it
        Box box = m_occ_box;
        box.Add(m_prev_box);
        const bool has_source = !box.Empty();
        if (has_source)
        {
            box.Grow(WIDE_R + 1);
            box.Clip(N);
            for (int z = box.z0; z < box.z1; ++z)
                for (int x = box.x0; x < box.x1; ++x)
                    m_diff[z * N + x] = m_occ[z * N + x] - m_occ_prev[z * N + x];
            this->Blur(m_diff, m_wide, m_tmp, box, m_wide_k, WIDE_R);
            this->Blur(m_diff, m_narrow, m_tmp, box, m_narrow_k, NARROW_R);
            float src_max = 0.f;
            for (int z = box.z0; z < box.z1; ++z)
                for (int x = box.x0; x < box.x1; ++x)
                {
                    const int i = z * N + x;
                    const float s = GAIN * (m_wide[i] - m_narrow[i]) * m_mask[i];
                    m_h[i] += s;
                    src_max = std::max(src_max, std::abs(s));
                    m_diff[i] = 0.f;
                }
            if (src_max > 1e-5f)
                m_active = true;
        }
        std::swap(m_occ, m_occ_prev);
        m_prev_box = m_occ_box;

        if (!m_active)
            return;

        m_accum = std::min(m_accum + dt, 4.f * STEP);
        float max_abs = 0.f;
        while (m_accum >= STEP)
        {
            m_accum -= STEP;
            max_abs = this->Step();
        }
        if (max_abs > 0.f && max_abs < 2e-3f && !has_source)
        {
            // Calm again: stop simulating until the next disturbance
            std::fill(m_h.begin(), m_h.end(), 0.f);
            std::fill(m_v.begin(), m_v.end(), 0.f);
            m_active = false;
        }
    }

    // --- Queries (thread safe while no frame is processed) ---

    bool IsActive() const { return m_active; }
    int  GetNumNodes() const { return m_num_nodes; } //!< Submerged nodes in the last frame

    /// Wake height at a world position, 0 outside the grid
    float GetHeight(float x, float z) const
    {
        if (!m_active)
            return 0.f;
        const float fx = (x - m_origin_x) / DX, fz = (z - m_origin_z) / DX;
        if (!(fx >= 0.f && fz >= 0.f && fx < N - 1 && fz < N - 1))
            return 0.f;
        const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
        const float tx = fx - ix, tz = fz - iz;
        const float* r0 = &m_out[iz * N + ix];
        const float* r1 = r0 + N;
        return (r0[0] * (1 - tx) + r0[1] * tx) * (1 - tz) + (r1[0] * (1 - tx) + r1[1] * tx) * tz;
    }

private:
    static constexpr int MAX_CHUNKS = 8;
    static constexpr int NARROW_R = 3; //!< sigma 0.6 m
    static constexpr int WIDE_R = 5;   //!< sigma 1.0 m

    struct Box
    {
        int x0 = N, z0 = N, x1 = 0, z1 = 0;
        bool Empty() const { return x0 >= x1 || z0 >= z1; }
        void Add(int x, int z) { x0 = std::min(x0, x); z0 = std::min(z0, z); x1 = std::max(x1, x + 2); z1 = std::max(z1, z + 2); }
        void Add(const Box& b) { if (!b.Empty()) { x0 = std::min(x0, b.x0); z0 = std::min(z0, b.z0); x1 = std::max(x1, b.x1); z1 = std::max(z1, b.z1); } }
        void Grow(int r) { x0 -= r; z0 -= r; x1 += r; z1 += r; }
        void Clip(int n) { x0 = std::max(x0, 0); z0 = std::max(z0, 0); x1 = std::min(x1, n); z1 = std::min(z1, n); }
        void Shift(int sx, int sz) { if (!Empty()) { x0 -= sx; x1 -= sx; z0 -= sz; z1 -= sz; Clip(N); } }
    };

    struct Pt { float x, z; };
    struct ActorState
    {
        uint64_t last_frame = 0;
        float x = 0.f, z = 0.f;
        std::vector<Pt> poly; //!< Footprint of the last frame (convex, counter-clockwise)
        float weight = 0.f;
    };

    static float Cross(const Pt& o, const Pt& a, const Pt& b) { return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x); }

    /// Andrew's monotone chain
    static void ConvexHull(std::vector<Pt>& pts, std::vector<Pt>& out)
    {
        std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) { return a.x < b.x || (a.x == b.x && a.z < b.z); });
        out.assign(2 * pts.size(), Pt{0.f, 0.f});
        size_t k = 0;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            while (k >= 2 && Cross(out[k - 2], out[k - 1], pts[i]) <= 0.f) --k;
            out[k++] = pts[i];
        }
        for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
        {
            while (k >= t && Cross(out[k - 2], out[k - 1], pts[i - 1]) <= 0.f) --k;
            out[k++] = pts[i - 1];
        }
        out.resize(k > 1 ? k - 1 : k);
    }

    /// Adds the area coverage of a convex polygon (anti-aliased: exact in x, 4 samples in z) to an occupancy grid
    void Raster(std::vector<float>& grid, const std::vector<Pt>& poly, float w, Box& box)
    {
        if (poly.size() < 3 || w <= 0.f)
            return;
        float zmin = poly[0].z, zmax = poly[0].z;
        for (const Pt& p: poly) { zmin = std::min(zmin, p.z); zmax = std::max(zmax, p.z); }
        const int r0 = std::max(1, static_cast<int>(std::floor((zmin - m_origin_z) / DX)));
        const int r1 = std::min(N - 2, static_cast<int>(std::ceil((zmax - m_origin_z) / DX)));
        const int SUB = 4;
        for (int r = r0; r <= r1; ++r)
        {
            float* row = &grid[r * N];
            for (int s = 0; s < SUB; ++s)
            {
                const float z = m_origin_z + (r - 0.5f + (s + 0.5f) / SUB) * DX;
                // x interval of the convex polygon at this z
                float xl = 1e30f, xr = -1e30f;
                for (size_t i = 0, n = poly.size(); i < n; ++i)
                {
                    const Pt& a = poly[i];
                    const Pt& b = poly[(i + 1) % n];
                    if ((a.z <= z && b.z > z) || (b.z <= z && a.z > z))
                    {
                        const float x = a.x + (z - a.z) / (b.z - a.z) * (b.x - a.x);
                        xl = std::min(xl, x);
                        xr = std::max(xr, x);
                    }
                }
                if (xl >= xr)
                    continue;
                const float fl = (xl - m_origin_x) / DX + 0.5f, fr = (xr - m_origin_x) / DX + 0.5f; // in cell units, cell i spans [i, i+1)
                const int c0 = std::max(1, static_cast<int>(std::floor(fl)));
                const int c1 = std::min(N - 2, static_cast<int>(std::floor(fr)));
                for (int c = c0; c <= c1; ++c)
                {
                    const float cover = std::min(fr, c + 1.f) - std::max(fl, static_cast<float>(c));
                    if (cover > 0.f)
                        row[c] += w * cover / SUB;
                }
                if (c0 <= c1)
                {
                    box.Add(c0, r);
                    box.Add(c1, r);
                }
            }
        }
    }

    void Allocate()
    {
        const size_t n = static_cast<size_t>(N) * N;
        for (std::vector<float>* a: { &m_h, &m_v, &m_out, &m_occ, &m_occ_prev, &m_diff, &m_wide, &m_narrow, &m_tmp, &m_tmp2 })
            a->assign(n, 0.f);
        m_mask.assign(n, 1.f);
        m_edge.resize(N);
        for (int i = 0; i < N; ++i)
        {
            const int d = std::min(i, N - 1 - i);
            const float t = std::min(1.f, d / static_cast<float>(BORDER));
            m_edge[i] = t * t * (3 - 2 * t);
        }
        auto gauss = [](std::vector<float>& k, int r, float sigma)
        {
            k.resize(2 * r + 1);
            float sum = 0.f;
            for (int i = -r; i <= r; ++i)
                sum += k[i + r] = std::exp(-(i * DX) * (i * DX) / (2 * sigma * sigma));
            for (float& v: k)
                v /= sum;
        };
        gauss(m_narrow_k, NARROW_R, 0.6f);
        gauss(m_wide_k, WIDE_R, 1.0f);
        m_mask_dirty = true;
    }

    /// Separable blur of `src` inside `box` (already grown by the radius)
    void Blur(const std::vector<float>& src, std::vector<float>& dst, std::vector<float>& tmp, const Box& box, const std::vector<float>& k, int r)
    {
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x)
            {
                float s = 0.f;
                for (int i = -r; i <= r; ++i)
                {
                    const int xx = x + i;
                    if (xx >= box.x0 && xx < box.x1)
                        s += k[i + r] * src[z * N + xx];
                }
                tmp[z * N + x] = s;
            }
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x)
            {
                float s = 0.f;
                for (int i = -r; i <= r; ++i)
                {
                    const int zz = z + i;
                    if (zz >= box.z0 && zz < box.z1)
                        s += k[i + r] * tmp[zz * N + x];
                }
                dst[z * N + x] = s;
            }
    }

    /// One step of the damped wave equation; returns the largest |height|
    float Step()
    {
        const float c2 = SPEED * SPEED * STEP / (DX * DX);
        const float damp = std::exp(-DAMPING * STEP);
        const int rows = N - 2;
        const int max_chunks = MAX_CHUNKS; // local copies: no ODR use of the static members before C++17
        const float max_height = MAX_HEIGHT;
        const int chunks = std::max(1, std::min(max_chunks, m_num_chunks));
        float chunk_max[MAX_CHUNKS] = {};

        auto velocity = [&](int z0, int z1)
        {
            for (int z = z0 + 1; z < z1 + 1; ++z)
                for (int x = 1; x < N - 1; ++x)
                {
                    const int i = z * N + x;
                    const float lap = m_h[i - 1] + m_h[i + 1] + m_h[i - N] + m_h[i + N] - 4.f * m_h[i];
                    // Border cells absorb the waves instead of reflecting them
                    const float edge = m_edge[x] * m_edge[z];
                    m_v[i] = (m_v[i] + c2 * lap) * damp * (0.85f + 0.15f * edge);
                }
        };
        auto height = [&](int z0, int z1, float& out_max)
        {
            float max_abs = 0.f;
            for (int z = z0 + 1; z < z1 + 1; ++z)
                for (int x = 1; x < N - 1; ++x)
                {
                    const int i = z * N + x;
                    const float edge = m_edge[x] * m_edge[z];
                    float h = (m_h[i] + m_v[i] * STEP) * m_mask[i] * (0.9f + 0.1f * edge);
                    h = std::max(-max_height, std::min(max_height, h));
                    const float a = std::abs(h);
                    if (a < 1e-6f)
                    {
                        // Flush the decayed tail to 0: denormal floats are ~100x slower
                        h = 0.f;
                        if (std::abs(m_v[i]) < 1e-5f)
                            m_v[i] = 0.f;
                    }
                    m_h[i] = h;
                    m_out[i] = h * edge;
                    max_abs = std::max(max_abs, a);
                }
            out_max = max_abs;
        };

        if (chunks <= 1 || !m_parallel)
        {
            velocity(0, rows);
            height(0, rows, chunk_max[0]);
            return chunk_max[0];
        }
        std::vector<std::function<void()>> tasks;
        for (int c = 0; c < chunks; ++c)
        {
            const int z0 = rows * c / chunks, z1 = rows * (c + 1) / chunks;
            tasks.push_back([&velocity, z0, z1]() { velocity(z0, z1); });
        }
        m_parallel(tasks);
        tasks.clear();
        for (int c = 0; c < chunks; ++c)
        {
            const int z0 = rows * c / chunks, z1 = rows * (c + 1) / chunks;
            float* out = &chunk_max[c];
            tasks.push_back([&height, z0, z1, out]() { height(z0, z1, *out); });
        }
        m_parallel(tasks);
        float max_abs = 0.f;
        for (int c = 0; c < chunks; ++c)
            max_abs = std::max(max_abs, chunk_max[c]);
        return max_abs;
    }

    /// Keeps the grid centered on the camera (moves the data by whole cells)
    void Follow(float cam_x, float cam_z)
    {
        const int cx = static_cast<int>(std::floor(cam_x / DX)), cz = static_cast<int>(std::floor(cam_z / DX));
        if (!m_placed)
        {
            m_center_x = cx; m_center_z = cz;
            m_placed = true;
            this->UpdateOrigin();
            m_mask_dirty = true;
            return;
        }
        const int sx = cx - m_center_x, sz = cz - m_center_z;
        if (std::abs(sx) < RECENTER && std::abs(sz) < RECENTER)
            return;
        m_center_x = cx; m_center_z = cz;
        this->UpdateOrigin();
        if (std::abs(sx) >= N || std::abs(sz) >= N)
        {
            for (std::vector<float>* a: { &m_h, &m_v, &m_out, &m_occ_prev })
                std::fill(a->begin(), a->end(), 0.f);
            m_prev_box = Box();
            m_mask_dirty = true;
            return;
        }
        for (std::vector<float>* a: { &m_h, &m_v, &m_out, &m_occ_prev, &m_mask })
        {
            for (int z = 0; z < N; ++z)
                for (int x = 0; x < N; ++x)
                {
                    const int ox = x + sx, oz = z + sz;
                    m_tmp2[z * N + x] = (ox >= 0 && ox < N && oz >= 0 && oz < N) ? (*a)[oz * N + ox] : 0.f;
                }
            a->swap(m_tmp2);
        }
        m_prev_box.Shift(sx, sz);
        // Land mask of the cells that came in
        if (sx > 0) this->UpdateMask(N - sx, N, 0, N); else if (sx < 0) this->UpdateMask(0, -sx, 0, N);
        if (sz > 0) this->UpdateMask(0, N, N - sz, N); else if (sz < 0) this->UpdateMask(0, N, 0, -sz);
    }

    void UpdateOrigin()
    {
        m_origin_x = (m_center_x - N / 2) * DX;
        m_origin_z = (m_center_z - N / 2) * DX;
    }

    /// Water depth -> 0 on land, 1 in water deeper than 1 m
    void UpdateMask(int x0, int x1, int z0, int z1)
    {
        for (int z = z0; z < z1; ++z)
            for (int x = x0; x < x1; ++x)
            {
                float m = 1.f;
                if (m_terrain)
                {
                    const float depth = m_water_y - m_terrain(m_origin_x + x * DX, m_origin_z + z * DX);
                    const float t = std::max(0.f, std::min(1.f, (depth - 0.1f) / 0.9f));
                    m = t * t * (3 - 2 * t);
                }
                m_mask[z * N + x] = m;
            }
    }

    std::function<float(float, float)> m_terrain;
    std::function<void(std::vector<std::function<void()>>&)> m_parallel;
    int m_num_chunks = 1;
    std::vector<float> m_h, m_v, m_out, m_mask, m_occ, m_occ_prev, m_diff, m_wide, m_narrow, m_tmp, m_tmp2;
    std::vector<float> m_edge, m_narrow_k, m_wide_k;
    std::unordered_map<int, ActorState> m_actors;
    ActorState* m_cur_actor = nullptr;
    std::vector<Pt> m_pts;
    bool m_cur_new = false;
    Box m_occ_box, m_prev_box;
    uint64_t m_frame = 1;
    int m_center_x = 0, m_center_z = 0;
    float m_origin_x = 0.f, m_origin_z = 0.f;
    float m_water_y = 0.f;
    float m_accum = 0.f;
    int m_num_nodes = 0;
    bool m_placed = false;
    bool m_mask_dirty = true;
    bool m_active = false;
};

} // namespace RoR
