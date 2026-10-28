#include "gfx/MeshDistanceField.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace fe {
namespace {

constexpr float kInfinity = std::numeric_limits<float>::max();

/// Cells around a triangle (beyond its box) whose distance to it is worked out exactly.
constexpr int kBand = 3;

/// The point of triangle (a, b, c) nearest to p (Ericson, Real-Time Collision Detection).
glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a,
                                 const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }

    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

/// float to IEEE half (round to nearest; values beyond the range saturate).
uint16_t floatToHalf(float value) {
    value = std::clamp(value, -65000.0f, 65000.0f);
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint32_t sign     = (bits >> 16) & 0x8000u;
    int32_t        exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t       mantissa = bits & 0x7FFFFFu;

    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);   // too small: zero
        mantissa |= 0x800000u;                                   // a subnormal half
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t half = mantissa >> shift;
        if ((mantissa >> (shift - 1)) & 1u) ++half;
        return static_cast<uint16_t>(sign | half);
    }

    uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u) ++half;   // rounding may carry into the exponent, which is right
    return static_cast<uint16_t>(half);
}

bool cancelled(const std::atomic<bool>* flag) {
    return flag && flag->load(std::memory_order_relaxed);
}

} // namespace

// ---------------------------------------------------------------------------
// SdfVolume
// ---------------------------------------------------------------------------

float SdfVolume::sample(const glm::vec3& p) const {
    if (!valid() || voxelSize <= 0.0f) return kInfinity;

    // Position in cell units, where cell i has its centre at i.
    glm::vec3 g = (p - boxMin) / voxelSize - 0.5f;
    const glm::vec3 hi(static_cast<float>(dims.x - 1), static_cast<float>(dims.y - 1), static_cast<float>(dims.z - 1));
    g = glm::clamp(g, glm::vec3(0.0f), hi);

    const glm::uvec3 i0(static_cast<uint32_t>(g.x), static_cast<uint32_t>(g.y), static_cast<uint32_t>(g.z));
    const glm::uvec3 i1 = glm::min(i0 + 1u, dims - 1u);
    const glm::vec3  f  = g - glm::vec3(i0);

    const auto at = [&](uint32_t x, uint32_t y, uint32_t z) { return distances[index(x, y, z)]; };
    const float c00 = glm::mix(at(i0.x, i0.y, i0.z), at(i1.x, i0.y, i0.z), f.x);
    const float c10 = glm::mix(at(i0.x, i1.y, i0.z), at(i1.x, i1.y, i0.z), f.x);
    const float c01 = glm::mix(at(i0.x, i0.y, i1.z), at(i1.x, i0.y, i1.z), f.x);
    const float c11 = glm::mix(at(i0.x, i1.y, i1.z), at(i1.x, i1.y, i1.z), f.x);
    return glm::mix(glm::mix(c00, c10, f.y), glm::mix(c01, c11, f.y), f.z);
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

SdfVolume buildMeshSdf(const std::vector<glm::vec3>& positions,
                       const std::vector<uint32_t>&  indices,
                       const SdfSettings&            settings,
                       const std::atomic<bool>*      cancel) {
    SdfVolume volume;
    const size_t triangleCount = indices.size() / 3;
    if (triangleCount == 0 || positions.empty()) return volume;

    // ---- The box: the mesh plus a margin, split in cubic cells ----
    glm::vec3 lo(kInfinity), hi(-kInfinity);
    for (uint32_t i : indices) {
        if (i >= positions.size()) return volume;   // a broken mesh
        lo = glm::min(lo, positions[i]);
        hi = glm::max(hi, positions[i]);
    }

    const uint32_t maxRes = std::clamp(settings.maxResolution, 8u, 128u);
    const glm::vec3 extent = hi - lo;
    const float longest = std::max({extent.x, extent.y, extent.z, 1e-4f});

    // Two cells of margin on every side, so the field also covers the space
    // just around the mesh, where rays arrive.
    const float voxel = longest / static_cast<float>(maxRes - 4);

    glm::uvec3 dims;
    for (int a = 0; a < 3; ++a)
        dims[a] = std::clamp(static_cast<uint32_t>(std::ceil(extent[a] / voxel)) + 4u, 4u, maxRes);

    const glm::vec3 centre = 0.5f * (lo + hi);
    const glm::vec3 size   = glm::vec3(dims) * voxel;

    volume.dims      = dims;
    volume.voxelSize = voxel;
    volume.boxMin    = centre - 0.5f * size;
    volume.boxMax    = centre + 0.5f * size;

    const size_t cellCount = static_cast<size_t>(dims.x) * dims.y * dims.z;
    std::vector<float>& d = volume.distances;
    d.assign(cellCount, kInfinity);

    const auto cellCentre = [&](uint32_t x, uint32_t y, uint32_t z) {
        return volume.boxMin + (glm::vec3(x, y, z) + 0.5f) * voxel;
    };

    // ---- 1. Exact distance to every triangle, for the cells near it ----
    for (size_t t = 0; t < triangleCount; ++t) {
        if (cancelled(cancel)) return SdfVolume{};

        const glm::vec3& a = positions[indices[3 * t]];
        const glm::vec3& b = positions[indices[3 * t + 1]];
        const glm::vec3& c = positions[indices[3 * t + 2]];

        const glm::vec3 tLo = glm::min(a, glm::min(b, c)) - kBand * voxel;
        const glm::vec3 tHi = glm::max(a, glm::max(b, c)) + kBand * voxel;

        glm::ivec3 from, to;
        for (int ax = 0; ax < 3; ++ax) {
            from[ax] = std::max(0, static_cast<int>(std::floor((tLo[ax] - volume.boxMin[ax]) / voxel)));
            to[ax]   = std::min(static_cast<int>(dims[ax]) - 1,
                                static_cast<int>(std::ceil((tHi[ax] - volume.boxMin[ax]) / voxel)));
        }

        for (int z = from.z; z <= to.z; ++z)
            for (int y = from.y; y <= to.y; ++y)
                for (int x = from.x; x <= to.x; ++x) {
                    const glm::vec3 p = cellCentre(x, y, z);
                    const float dist = glm::length(p - closestPointOnTriangle(p, a, b, c));
                    float& cell = d[volume.index(x, y, z)];
                    if (dist < cell) cell = dist;
                }
    }

    // ---- 2. Spread the distances to the cells no triangle was near ----
    // Two sweeps (forwards then backwards), each cell taking the best of its
    // neighbours' distance plus the step to reach it. Exact enough far away,
    // where only the general slope of the field matters.
    {
        struct Step { int dx, dy, dz; float length; };
        std::vector<Step> forward, backward;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    const float length = voxel * std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz));
                    // The neighbours already visited in a forward sweep are the "earlier" ones.
                    const bool earlier = (dz < 0) || (dz == 0 && dy < 0) || (dz == 0 && dy == 0 && dx < 0);
                    (earlier ? forward : backward).push_back({dx, dy, dz, length});
                }

        const auto relax = [&](int x, int y, int z, const std::vector<Step>& steps) {
            float& cell = d[volume.index(x, y, z)];
            for (const Step& s : steps) {
                const int nx = x + s.dx, ny = y + s.dy, nz = z + s.dz;
                if (nx < 0 || ny < 0 || nz < 0 ||
                    nx >= static_cast<int>(dims.x) || ny >= static_cast<int>(dims.y) || nz >= static_cast<int>(dims.z))
                    continue;
                const float other = d[volume.index(nx, ny, nz)];
                if (other < kInfinity && other + s.length < cell) cell = other + s.length;
            }
        };

        for (int z = 0; z < static_cast<int>(dims.z); ++z) {
            if (cancelled(cancel)) return SdfVolume{};
            for (int y = 0; y < static_cast<int>(dims.y); ++y)
                for (int x = 0; x < static_cast<int>(dims.x); ++x) relax(x, y, z, forward);
        }
        for (int z = static_cast<int>(dims.z) - 1; z >= 0; --z) {
            if (cancelled(cancel)) return SdfVolume{};
            for (int y = static_cast<int>(dims.y) - 1; y >= 0; --y)
                for (int x = static_cast<int>(dims.x) - 1; x >= 0; --x) relax(x, y, z, backward);
        }
    }

    // ---- 3. The sign: a line along +x that crosses the surface an odd number of times is inside ----
    // The columns are shifted by a tiny irregular amount so that none of them
    // runs exactly along an edge or through a vertex, which would be counted
    // twice (or not at all) for axis-aligned meshes.
    {
        const double shiftY = 0.0123 * voxel, shiftZ = 0.0171 * voxel;
        std::vector<std::vector<float>> crossings(static_cast<size_t>(dims.y) * dims.z);

        for (size_t t = 0; t < triangleCount; ++t) {
            if (cancelled(cancel)) return SdfVolume{};

            const glm::vec3& a = positions[indices[3 * t]];
            const glm::vec3& b = positions[indices[3 * t + 1]];
            const glm::vec3& c = positions[indices[3 * t + 2]];

            // The triangle seen along x: its y and z are the plane's coordinates.
            const double ay = a.y, az = a.z, by = b.y, bz = b.z, cy = c.y, cz = c.z;
            const double area = (by - ay) * (cz - az) - (bz - az) * (cy - ay);
            if (std::abs(area) < 1e-14) continue;   // edge-on: it never crosses a column

            const int y0 = std::max(0, static_cast<int>(std::ceil((std::min({ay, by, cy}) - shiftY - volume.boxMin.y) / voxel - 0.5)));
            const int y1 = std::min(static_cast<int>(dims.y) - 1, static_cast<int>(std::floor((std::max({ay, by, cy}) - shiftY - volume.boxMin.y) / voxel - 0.5)));
            const int z0 = std::max(0, static_cast<int>(std::ceil((std::min({az, bz, cz}) - shiftZ - volume.boxMin.z) / voxel - 0.5)));
            const int z1 = std::min(static_cast<int>(dims.z) - 1, static_cast<int>(std::floor((std::max({az, bz, cz}) - shiftZ - volume.boxMin.z) / voxel - 0.5)));

            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) {
                    const double py = volume.boxMin.y + (y + 0.5) * voxel + shiftY;
                    const double pz = volume.boxMin.z + (z + 0.5) * voxel + shiftZ;

                    // Barycentric weights of the column's point in the projected triangle.
                    const double wa = ((by - py) * (cz - pz) - (bz - pz) * (cy - py)) / area;
                    const double wb = ((cy - py) * (az - pz) - (cz - pz) * (ay - py)) / area;
                    const double wc = 1.0 - wa - wb;
                    if (wa < 0.0 || wb < 0.0 || wc < 0.0) continue;

                    const double x = wa * a.x + wb * b.x + wc * c.x;
                    crossings[static_cast<size_t>(z) * dims.y + y].push_back(static_cast<float>(x));
                }
        }

        for (uint32_t z = 0; z < dims.z; ++z) {
            if (cancelled(cancel)) return SdfVolume{};
            for (uint32_t y = 0; y < dims.y; ++y) {
                std::vector<float>& line = crossings[static_cast<size_t>(z) * dims.y + y];

                // An odd total means the surface is not closed here (or the
                // mesh is a sheet): there is no inside to speak of.
                if (line.empty() || (line.size() & 1u)) continue;
                std::sort(line.begin(), line.end());

                size_t next = 0;   // first crossing ahead of the current cell
                for (uint32_t x = 0; x < dims.x; ++x) {
                    const float cx = volume.boxMin.x + (static_cast<float>(x) + 0.5f) * voxel;
                    while (next < line.size() && line[next] < cx) ++next;
                    const size_t ahead = line.size() - next;
                    if (ahead & 1u) {
                        float& cell = d[volume.index(x, y, z)];
                        cell = -cell;
                    }
                }
            }
        }
    }

    return volume;
}

uint16_t sdfFloatToHalf(float value) { return floatToHalf(value); }

} // namespace fe
