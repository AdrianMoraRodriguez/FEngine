#include "gfx/EnvironmentBake.h"

#include "core/Log.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <future>

namespace fe {
namespace {

constexpr float kPi = 3.14159265358979f;

/// Brightest value kept from the picture. The sun in a photograph can be
/// 100000 times brighter than the sky: that energy is kept, but capped, as
/// otherwise the few texels of the sun make the blurred reflections speckled.
/// (The scene's own sun is a light of the scene, not part of the picture.)
constexpr float kMaxRadiance = 1000.0f;

/// Largest value a 16-bit float can hold.
constexpr float kHalfMax = 65000.0f;

// -----------------------------------------------------------------------------
// Cube faces
// -----------------------------------------------------------------------------

/// The axes of a cube face, in the convention Vulkan gives cube map layers
/// (the same table as cubeFaceView() in SceneRenderer.cpp): for a texel at
/// (sc, tc) in -1..1 across the face, the direction is forward + sc * right +
/// tc * up. `tc` grows downwards in the image, like the rows do.
struct FaceAxes { glm::vec3 right, up, forward; };

const FaceAxes kFaces[6] = {
    {{ 0.0f, 0.0f,-1.0f}, { 0.0f,-1.0f, 0.0f}, { 1.0f, 0.0f, 0.0f}},   // +X
    {{ 0.0f, 0.0f, 1.0f}, { 0.0f,-1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},   // -X
    {{ 1.0f, 0.0f, 0.0f}, { 0.0f, 0.0f, 1.0f}, { 0.0f, 1.0f, 0.0f}},   // +Y
    {{ 1.0f, 0.0f, 0.0f}, { 0.0f, 0.0f,-1.0f}, { 0.0f,-1.0f, 0.0f}},   // -Y
    {{ 1.0f, 0.0f, 0.0f}, { 0.0f,-1.0f, 0.0f}, { 0.0f, 0.0f, 1.0f}},   // +Z
    {{-1.0f, 0.0f, 0.0f}, { 0.0f,-1.0f, 0.0f}, { 0.0f, 0.0f,-1.0f}},   // -Z
};

/// Unit direction through the centre of texel (x, y) of a face that is
/// @p size texels wide.
glm::vec3 texelDirection(int face, uint32_t x, uint32_t y, uint32_t size) {
    const float sc = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.0f;
    const float tc = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.0f;
    const FaceAxes& a = kFaces[face];
    return glm::normalize(a.forward + sc * a.right + tc * a.up);
}

/// Which face a direction points at, and where on it (u, v in 0..1).
void directionToFace(const glm::vec3& d, int& face, float& u, float& v) {
    const glm::vec3 a = glm::abs(d);
    float sc, tc, ma;
    if (a.x >= a.y && a.x >= a.z) {
        ma = a.x;
        if (d.x > 0.0f) { face = 0; sc = -d.z; tc = -d.y; }
        else            { face = 1; sc =  d.z; tc = -d.y; }
    } else if (a.y >= a.z) {
        ma = a.y;
        if (d.y > 0.0f) { face = 2; sc =  d.x; tc =  d.z; }
        else            { face = 3; sc =  d.x; tc = -d.z; }
    } else {
        ma = a.z;
        if (d.z > 0.0f) { face = 4; sc =  d.x; tc = -d.y; }
        else            { face = 5; sc = -d.x; tc = -d.y; }
    }
    u = 0.5f * (sc / ma + 1.0f);
    v = 0.5f * (tc / ma + 1.0f);
}

/// One mip level of an unfiltered cube: six square faces of linear colour.
struct CubeLevel {
    uint32_t               size = 0;
    std::vector<glm::vec3> texels;   // face, then row, then column

    glm::vec3& at(int face, uint32_t x, uint32_t y) {
        return texels[(static_cast<size_t>(face) * size + y) * size + x];
    }
    const glm::vec3& at(int face, uint32_t x, uint32_t y) const {
        return texels[(static_cast<size_t>(face) * size + y) * size + x];
    }

    /// Bilinear lookup inside one face. Taps past the edge repeat the edge
    /// texel (not the neighbouring face), a small error that only the blurry
    /// levels ever see.
    glm::vec3 sample(int face, float u, float v) const {
        const float fx = u * static_cast<float>(size) - 0.5f;
        const float fy = v * static_cast<float>(size) - 0.5f;
        const float x0f = std::floor(fx), y0f = std::floor(fy);
        const float tx = fx - x0f, ty = fy - y0f;
        const int last = static_cast<int>(size) - 1;
        const auto clampi = [last](float f) { return std::clamp(static_cast<int>(f), 0, last); };
        const uint32_t x0 = clampi(x0f), x1 = clampi(x0f + 1.0f);
        const uint32_t y0 = clampi(y0f), y1 = clampi(y0f + 1.0f);
        const glm::vec3 top    = glm::mix(at(face, x0, y0), at(face, x1, y0), tx);
        const glm::vec3 bottom = glm::mix(at(face, x0, y1), at(face, x1, y1), tx);
        return glm::mix(top, bottom, ty);
    }
};

/// Halves a cube level by averaging blocks of 2 x 2 texels.
CubeLevel halve(const CubeLevel& in) {
    CubeLevel out;
    out.size = in.size / 2;
    out.texels.resize(static_cast<size_t>(6) * out.size * out.size);
    for (int f = 0; f < 6; ++f)
        for (uint32_t y = 0; y < out.size; ++y)
            for (uint32_t x = 0; x < out.size; ++x)
                out.at(f, x, y) = 0.25f * (in.at(f, 2 * x, 2 * y)     + in.at(f, 2 * x + 1, 2 * y) +
                                           in.at(f, 2 * x, 2 * y + 1) + in.at(f, 2 * x + 1, 2 * y + 1));
    return out;
}

/// Smooth lookup in a chain of ever smaller levels: bilinear inside two
/// neighbouring levels, blended by the fractional part of @p lod.
glm::vec3 sampleChain(const std::vector<CubeLevel>& chain, const glm::vec3& dir, float lod) {
    int face; float u, v;
    directionToFace(dir, face, u, v);
    lod = std::clamp(lod, 0.0f, static_cast<float>(chain.size() - 1));
    const int   lo = static_cast<int>(lod);
    const int   hi = std::min(lo + 1, static_cast<int>(chain.size()) - 1);
    const float t  = lod - static_cast<float>(lo);
    const glm::vec3 a = chain[lo].sample(face, u, v);
    return t > 0.0f ? glm::mix(a, chain[hi].sample(face, u, v), t) : a;
}

// -----------------------------------------------------------------------------
// The panorama
// -----------------------------------------------------------------------------

/// A panorama with sane values: finite, not negative, not absurdly bright.
struct Panorama {
    uint32_t               width = 0, height = 0;
    std::vector<glm::vec3> texels;

    const glm::vec3& at(int x, int y) const { return texels[static_cast<size_t>(y) * width + x]; }

    /// Bilinear lookup in a direction. The picture wraps around horizontally
    /// and stops at the poles.
    glm::vec3 sample(const glm::vec3& d) const {
        const float u = std::atan2(d.x, -d.z) / (2.0f * kPi) + 0.5f;
        const float v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / kPi;
        const float fx = u * static_cast<float>(width)  - 0.5f;
        const float fy = v * static_cast<float>(height) - 0.5f;
        const float x0f = std::floor(fx), y0f = std::floor(fy);
        const float tx = fx - x0f, ty = fy - y0f;
        const int w = static_cast<int>(width), h = static_cast<int>(height);
        const auto wrapX  = [w](int x) { return ((x % w) + w) % w; };
        const auto clampY = [h](int y) { return std::clamp(y, 0, h - 1); };
        const int x0 = wrapX(static_cast<int>(x0f)), x1 = wrapX(static_cast<int>(x0f) + 1);
        const int y0 = clampY(static_cast<int>(y0f)), y1 = clampY(static_cast<int>(y0f) + 1);
        return glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx),
                        glm::mix(at(x0, y1), at(x1, y1), tx), ty);
    }
};

Panorama sanitize(const HdrImageData& in) {
    Panorama p;
    p.width  = in.width;
    p.height = in.height;
    p.texels.resize(static_cast<size_t>(in.width) * in.height);
    for (size_t i = 0; i < p.texels.size(); ++i) {
        glm::vec3 c(in.pixels[3 * i], in.pixels[3 * i + 1], in.pixels[3 * i + 2]);
        for (int k = 0; k < 3; ++k)
            c[k] = std::isfinite(c[k]) ? std::clamp(c[k], 0.0f, kMaxRadiance) : 0.0f;
        p.texels[i] = c;
    }
    return p;
}

/// Averages blocks of 2 x 2 texels. Pictures much larger than the cube are
/// shrunk this way first, so that the cube does not alias them.
Panorama halve(const Panorama& in) {
    Panorama out;
    out.width  = std::max(in.width  / 2, 1u);
    out.height = std::max(in.height / 2, 1u);
    out.texels.resize(static_cast<size_t>(out.width) * out.height);
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            const int x0 = static_cast<int>(std::min(2 * x,     in.width  - 1));
            const int x1 = static_cast<int>(std::min(2 * x + 1, in.width  - 1));
            const int y0 = static_cast<int>(std::min(2 * y,     in.height - 1));
            const int y1 = static_cast<int>(std::min(2 * y + 1, in.height - 1));
            out.texels[static_cast<size_t>(y) * out.width + x] =
                0.25f * (in.at(x0, y0) + in.at(x1, y0) + in.at(x0, y1) + in.at(x1, y1));
        }
    return out;
}

// -----------------------------------------------------------------------------
// Prefiltering
// -----------------------------------------------------------------------------

/// The i-th of n points of the Hammersley set: well spread points of the unit square.
glm::vec2 hammersley(uint32_t i, uint32_t n) {
    uint32_t bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return {static_cast<float>(i) / static_cast<float>(n),
            static_cast<float>(bits) * 2.3283064365386963e-10f};   // / 2^32
}

/// Blurs one face of the cube for a surface of the given roughness.
///
/// For each texel, the surface is looking straight at the sky in that
/// direction, and the reflection collects light from all around it, weighted
/// by the GGX highlight shape of the shader. Instead of taking thousands of
/// samples from the sharp picture, a few are taken from the right *smaller*
/// level of the chain: one that is blurry enough that a single sample stands
/// for the whole patch of sky it represents ("filtered importance sampling").
void prefilterFace(const std::vector<CubeLevel>& chain, float roughness, uint32_t size,
                   uint32_t sampleCount, int face, glm::vec3* out) {
    const float alpha  = roughness * roughness;
    const float a2     = alpha * alpha;
    // Solid angle that one texel of the sharpest level covers.
    const float texelSolidAngle = 4.0f * kPi / (6.0f * static_cast<float>(chain[0].size) *
                                                       static_cast<float>(chain[0].size));

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const glm::vec3 n = texelDirection(face, x, y, size);

            const glm::vec3 helper = std::abs(n.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
            const glm::vec3 tangent   = glm::normalize(glm::cross(helper, n));
            const glm::vec3 bitangent = glm::cross(n, tangent);

            glm::vec3 sum(0.0f);
            float     weight = 0.0f;
            for (uint32_t i = 0; i < sampleCount; ++i) {
                // A direction h, chosen so that more of them fall where the
                // highlight is strong.
                const glm::vec2 xi = hammersley(i, sampleCount);
                const float phi      = 2.0f * kPi * xi.x;
                const float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a2 - 1.0f) * xi.y));
                const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
                const glm::vec3 h = tangent * (sinTheta * std::cos(phi)) +
                                    bitangent * (sinTheta * std::sin(phi)) + n * cosTheta;

                // The light direction that mirrors the view (= n) around h.
                const glm::vec3 l = 2.0f * cosTheta * h - n;
                const float nDotL = glm::dot(n, l);
                if (nDotL <= 0.0f) continue;

                // How likely that direction was: with v = n the half-vector
                // terms cancel to D / 4. A rarely chosen direction stands for a
                // larger patch of sky, hence a blurrier level of the chain.
                const float d = a2 / (kPi * std::pow(cosTheta * cosTheta * (a2 - 1.0f) + 1.0f, 2.0f));
                const float pdf = d * 0.25f;
                const float sampleSolidAngle = 1.0f / (static_cast<float>(sampleCount) * pdf + 1e-4f);
                const float lod = 0.5f * std::log2(sampleSolidAngle / texelSolidAngle) + 1.0f;

                sum    += sampleChain(chain, l, lod) * nDotL;
                weight += nDotL;
            }
            out[static_cast<size_t>(y) * size + x] = weight > 0.0f ? sum / weight : sum;
        }
    }
}

/// The reflection cube at the given level of the final chain.
CubeLevel prefilterLevel(const std::vector<CubeLevel>& chain, uint32_t level, uint32_t levelCount) {
    if (level == 0) return chain[0];

    CubeLevel out;
    out.size = chain[0].size >> level;
    out.texels.resize(static_cast<size_t>(6) * out.size * out.size);

    const float roughness = static_cast<float>(level) / static_cast<float>(levelCount - 1);
    // Blurrier levels need more samples to stay smooth.
    const uint32_t samples = 32 + 16 * level;

    // One task per face.
    std::vector<std::future<void>> tasks;
    for (int face = 0; face < 6; ++face) {
        glm::vec3* dst = out.texels.data() + static_cast<size_t>(face) * out.size * out.size;
        tasks.push_back(std::async(std::launch::async, [&chain, roughness, size = out.size, samples, face, dst] {
            prefilterFace(chain, roughness, size, samples, face, dst);
        }));
    }
    for (auto& t : tasks) t.get();
    return out;
}

// -----------------------------------------------------------------------------
// Diffuse light
// -----------------------------------------------------------------------------

/// Nine coefficients of the sky's light as seen from a point, in the same
/// order and with the same meaning as `environmentDiffuse()` in mesh.frag.
std::array<glm::vec4, 9> computeDiffuse(const CubeLevel& level) {
    // The nine real spherical harmonics of bands 0 to 2: constants and the
    // polynomials they multiply, in the order the shader uses.
    constexpr float k0 = 0.282095f, k1 = 0.488603f, k2 = 1.092548f, k20 = 0.315392f, k22 = 0.546274f;
    const float constants[9] = {k0, k1, k1, k1, k2, k2, k20, k2, k22};

    // How strongly the diffuse lobe keeps each band, divided by pi: band 0
    // fully, band 1 by 2/3, band 2 by 1/4. (The 1/pi turns irradiance into
    // the "radiance the surface sends back" the shader multiplies the colour by.)
    const float lobe[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f,
                           0.25f, 0.25f, 0.25f, 0.25f, 0.25f};

    double sums[9][3] = {};
    for (int face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < level.size; ++y) {
            for (uint32_t x = 0; x < level.size; ++x) {
                const glm::vec3 d = texelDirection(face, x, y, level.size);
                const glm::vec3 c = level.at(face, x, y);

                // Solid angle of the texel: its area on the cube's face, shrunk
                // by distance and tilt as seen from the centre.
                const float sc = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(level.size) - 1.0f;
                const float tc = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(level.size) - 1.0f;
                const float texel = 2.0f / static_cast<float>(level.size);
                const float dw = texel * texel / std::pow(1.0f + sc * sc + tc * tc, 1.5f);

                const float poly[9] = {1.0f, d.y, d.z, d.x, d.x * d.y, d.y * d.z,
                                       3.0f * d.z * d.z - 1.0f, d.x * d.z, d.x * d.x - d.y * d.y};
                for (int i = 0; i < 9; ++i)
                    for (int k = 0; k < 3; ++k)
                        sums[i][k] += static_cast<double>(c[k]) * poly[i] * constants[i] * dw;
            }
        }
    }

    std::array<glm::vec4, 9> sh{};
    for (int i = 0; i < 9; ++i)
        sh[i] = glm::vec4(static_cast<float>(sums[i][0]), static_cast<float>(sums[i][1]),
                          static_cast<float>(sums[i][2]), 0.0f) * (lobe[i] * constants[i]);
    return sh;
}

uint16_t toHalf(float v) {
    return glm::packHalf1x16(std::clamp(v, 0.0f, kHalfMax));
}

} // namespace

HdrImageData makeProceduralSky() {
    HdrImageData image;
    image.width  = 512;
    image.height = 256;
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 3);

    const glm::vec3 zenith        (0.20f, 0.38f, 0.85f);
    const glm::vec3 horizon       (0.80f, 0.86f, 0.93f);
    const glm::vec3 groundNear    (0.42f, 0.40f, 0.37f);
    const glm::vec3 groundDeep    (0.10f, 0.095f, 0.09f);

    for (uint32_t y = 0; y < image.height; ++y) {
        // Row 0 is straight up, the last row straight down.
        const float up = std::cos((static_cast<float>(y) + 0.5f) / static_cast<float>(image.height) * kPi);
        glm::vec3 sky    = glm::mix(horizon, zenith, std::pow(std::max(up, 0.0f), 0.45f));
        glm::vec3 ground = glm::mix(groundNear, groundDeep, std::pow(std::max(-up, 0.0f), 0.6f));
        const glm::vec3 c = glm::mix(ground, sky, glm::smoothstep(-0.03f, 0.03f, up));
        for (uint32_t x = 0; x < image.width; ++x) {
            float* p = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 3];
            p[0] = c.r; p[1] = c.g; p[2] = c.b;
        }
    }
    return image;
}

EnvironmentBake bakeEnvironment(const HdrImageData& source, uint32_t faceSize) {
    EnvironmentBake bake;
    if (faceSize < 16 || faceSize > 2048 || (faceSize & (faceSize - 1)) != 0) {
        FE_ERROR("Environment: face size %u is not a power of two between 16 and 2048", faceSize);
        return bake;
    }
    if (source.width == 0 || source.height == 0 ||
        source.pixels.size() < static_cast<size_t>(source.width) * source.height * 3) {
        FE_ERROR("Environment: the panorama is empty");
        return bake;
    }

    Panorama panorama = sanitize(source);
    // About four times the face width is the panorama's resolution that
    // matches the cube's; more would only alias.
    while (panorama.width >= 8 * faceSize && panorama.height > 1)
        panorama = halve(panorama);

    // ---- Level 0: the sky as it is, cut into six faces ----
    CubeLevel sharp;
    sharp.size = faceSize;
    sharp.texels.resize(static_cast<size_t>(6) * sharp.size * sharp.size);
    {
        std::vector<std::future<void>> tasks;
        for (int face = 0; face < 6; ++face)
            tasks.push_back(std::async(std::launch::async, [&sharp, &panorama, face] {
                for (uint32_t y = 0; y < sharp.size; ++y)
                    for (uint32_t x = 0; x < sharp.size; ++x)
                        sharp.at(face, x, y) = panorama.sample(texelDirection(face, x, y, sharp.size));
            }));
        for (auto& t : tasks) t.get();
    }

    // ---- A chain of ever smaller copies, the source of the blurry levels ----
    std::vector<CubeLevel> chain;
    chain.push_back(std::move(sharp));
    while (chain.back().size > 4)
        chain.push_back(halve(chain.back()));

    // ---- Diffuse light, from a small level: it is a very smooth function ----
    size_t diffuseLevel = 0;
    for (size_t i = 0; i < chain.size(); ++i)
        if (chain[i].size >= 32) diffuseLevel = i;
    bake.sh = computeDiffuse(chain[diffuseLevel]);

    // ---- Reflections: one blurrier level after another ----
    // From the face size down to 8 texels.
    uint32_t levels = 1;
    while ((faceSize >> levels) >= 8) ++levels;

    bake.faceSize  = faceSize;
    bake.mipLevels = levels;
    for (uint32_t level = 0; level < levels; ++level) {
        const CubeLevel filtered = prefilterLevel(chain, level, levels);
        for (const glm::vec3& c : filtered.texels) {
            bake.pixels.push_back(toHalf(c.r));
            bake.pixels.push_back(toHalf(c.g));
            bake.pixels.push_back(toHalf(c.b));
            bake.pixels.push_back(glm::packHalf1x16(1.0f));
        }
    }
    return bake;
}

} // namespace fe
