#pragma once

/**
 * @file Geometry.h
 * @brief Small geometric types and ray tests used for picking.
 */

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fe {

/// A half-line starting at @ref origin and going along @ref direction.
struct Ray {
    glm::vec3 origin    {0.0f};
    glm::vec3 direction {0.0f, 0.0f, -1.0f};   ///< Not required to be normalised.
};

/// Axis-aligned box, in whatever space its owner defines.
struct Aabb {
    glm::vec3 min {0.0f};
    glm::vec3 max {0.0f};
};

/**
 * @brief Tests a ray against a box that lives in a transformed space.
 *
 * The ray is brought into the box's local space with the inverse of
 * @p boxToWorld, so the test also works for rotated and scaled boxes.
 * The direction is deliberately not re-normalised: that keeps the
 * parameter @p outT expressed in the units of the original ray, so
 * distances from different boxes are directly comparable.
 *
 * @param ray        The ray, in world space.
 * @param box        The box, in its local space.
 * @param boxToWorld Transform from the box's local space to world space.
 * @param[out] outT  Distance along the ray to the entry point (0 if the
 *                   ray starts inside the box). Only written on a hit.
 * @return `true` if the ray hits the box.
 */
inline bool intersectRayBox(const Ray& ray, const Aabb& box,
                            const glm::mat4& boxToWorld, float& outT) {
    const glm::mat4 inverse = glm::inverse(boxToWorld);
    const glm::vec3 o = glm::vec3(inverse * glm::vec4(ray.origin,    1.0f));
    const glm::vec3 d = glm::vec3(inverse * glm::vec4(ray.direction, 0.0f));

    // A singular matrix (for example a zero scale) produces non-finite
    // values; such a box cannot be hit.
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(o[i]) || !std::isfinite(d[i])) return false;

    // Slab test: intersect the ray with the three pairs of parallel planes
    // and keep the interval where it is inside all of them.
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::max();

    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-8f) {
            // Parallel to this pair of planes: it either stays inside the
            // slab for the whole ray, or never enters it.
            if (o[i] < box.min[i] || o[i] > box.max[i]) return false;
        } else {
            float t1 = (box.min[i] - o[i]) / d[i];
            float t2 = (box.max[i] - o[i]) / d[i];
            if (t1 > t2) std::swap(t1, t2);

            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax) return false;
        }
    }

    outT = tMin;
    return true;
}

} // namespace fe
