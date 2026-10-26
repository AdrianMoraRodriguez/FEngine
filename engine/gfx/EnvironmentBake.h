#pragma once

/**
 * @file EnvironmentBake.h
 * @brief Turns a panorama of the sky into the data the shaders light objects with.
 *
 * Image-based lighting treats a picture of the surroundings as a light source
 * that surrounds the scene. Two things are computed from it, once, on the CPU:
 *
 *  - **Reflections.** A cube map whose mip levels are progressively blurrier,
 *    each blurred the way a rougher surface blurs a mirror image: level 0 is
 *    the sky as it is, the last level is what a fully rough surface sees.
 *    The shader picks the level from the material's roughness.
 *  - **Diffuse light.** The light a matte surface receives from the whole sky,
 *    for any direction it faces, compressed into nine numbers per colour channel
 *    (second-order spherical harmonics). It is a very smooth function of the
 *    direction, which nine numbers describe well.
 *
 * Nothing here touches Vulkan, so it can run on a background thread.
 */

#include "import/ImageLoader.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fe {

/// Width in texels of a cube face at mip level 0, for an environment baked at
/// full quality. (A smaller power of two makes a quick, rough bake.)
constexpr uint32_t kEnvironmentFaceSize = 512;

/// The result of baking an environment, ready to be uploaded (see Environment).
struct EnvironmentBake {
    /// Width in texels of a cube face at mip level 0. A power of two, at least 16.
    uint32_t faceSize = 0;

    /// Number of mip levels: from `faceSize` down to 8 texels (for 512: 512, 256,
    /// 128, 64, 32, 16 and 8). Level `i` holds the reflection of a surface of
    /// roughness `i / (mipLevels - 1)`.
    uint32_t mipLevels = 0;

    /// Cube map texels as RGBA16F ("half" floats, stored as raw 16-bit
    /// values). Ordered by mip level, then face (+X, -X, +Y, -Y, +Z, -Z, the
    /// order Vulkan numbers cube layers), then row from top to bottom, then
    /// column. Four values per texel, alpha always 1.
    std::vector<uint16_t> pixels;

    /// Diffuse lighting coefficients: rgb in each entry, w unused. Entry `i`
    /// multiplies the `i`-th term of `environmentDiffuse()` in mesh.frag, with
    /// everything else (the basis constants, the diffuse lobe) folded in.
    std::array<glm::vec4, 9> sh{};

    bool valid() const { return !pixels.empty(); }

    /// Texels along one side of a cube face at mip level @p level.
    uint32_t faceSizeAt(uint32_t level) const { return faceSize >> level; }
};

/**
 * @brief Draws the default sky: a plain gradient, blue overhead, pale at the
 *        horizon and a dark ground below, as a panorama.
 *
 * Used when the scene has no environment picture of its own, and by the
 * previews.
 */
HdrImageData makeProceduralSky();

/**
 * @brief Bakes @p panorama into reflections and diffuse light.
 *
 * @param panorama An equirectangular (latitude-longitude) picture, twice as
 *                 wide as it is tall. The middle of the picture is the
 *                 direction -Z (where the editor camera starts looking), the
 *                 top is straight up (+Y), and the picture runs left to right
 *                 as the view turns towards +X.
 *                 Any size works; huge pictures are shrunk first.
 * @param faceSize Width of the cube faces at level 0: a power of two from 16 to
 *                 2048. The default is the quality the editor uses; a small
 *                 value gives a rough environment almost instantly.
 * @return The baked data, or an invalid bake (`!valid()`) if the picture is empty.
 *
 * Takes about a second. Thread-safe: it uses no global state.
 */
EnvironmentBake bakeEnvironment(const HdrImageData& panorama, uint32_t faceSize = kEnvironmentFaceSize);

} // namespace fe
