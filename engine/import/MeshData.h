#pragma once

/**
 * @file MeshData.h
 * @brief CPU-side triangle mesh produced by the model importers.
 */

#include "core/AlphaMode.h"
#include "gfx/Vertex.h"
#include "import/ImageLoader.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fe {

/**
 * @brief How the surface of one part of a model looks (metallic-roughness PBR).
 *
 * The base colour factor is not here: it is multiplied into the vertex
 * colours while importing. Textures are indices into MeshData::images, or -1
 * when the material has none (the factors alone then decide).
 */
struct ModelMaterial {
    int albedo     = -1;   ///< Colour (sRGB). Multiplies the vertex colour.
    int metalRough = -1;   ///< Green: roughness. Blue: metalness. (glTF layout.)
    int normal     = -1;   ///< Tangent-space normal map (green up, as in glTF).
    int emissive   = -1;   ///< Light the surface gives off (sRGB).

    float     metallic    = 0.0f;   ///< Multiplies the metalness texture (or is the metalness).
    float     roughness   = 0.6f;   ///< Multiplies the roughness texture (or is the roughness).
    float     normalScale = 1.0f;   ///< Strength of the normal map.
    glm::vec3 emissiveFactor{0.0f}; ///< Linear colour, multiplies the emissive texture.

    AlphaMode alphaMode   = AlphaMode::Opaque;   ///< How the albedo's alpha is used.
    float     alphaCutoff = 0.5f;                ///< Mask mode: pixels with less alpha than this are dropped.
    float     opacity     = 1.0f;                ///< Multiplies the albedo's alpha (the base colour factor's alpha in glTF).
    bool      doubleSided = false;               ///< Draw the back faces too (leaves, cloth, thin sheets).
};

/**
 * @brief A run of consecutive indices that share one material.
 *
 * A model with several materials is one vertex/index buffer drawn in several
 * ranges, each with its own material.
 */
struct MeshPart {
    uint32_t firstIndex = 0;   ///< Position of the first index of the run.
    uint32_t indexCount = 0;   ///< Number of indices (three per triangle).
    int      material   = -1;  ///< Index into MeshData::materials, or -1 for the default material.
};

/// Triangle mesh data ready to be uploaded to the GPU.
struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;   ///< Three per triangle, counter-clockwise.

    /// The index ranges and their materials. May be empty (one range, default material).
    std::vector<MeshPart> parts;

    /// The materials the parts refer to.
    std::vector<ModelMaterial> materials;

    /// The images the materials refer to. Each is decoded only when it is
    /// first drawn, so listing one costs nothing.
    std::vector<ImageSource> images;

    /**
     * @brief Records that indices [firstIndex, firstIndex + indexCount) use @p material.
     *
     * Joins the run to the previous one when it continues it with the same
     * material, so a model does not end up with thousands of tiny draw calls.
     */
    void addPart(uint32_t firstIndex, uint32_t indexCount, int material) {
        if (indexCount == 0) return;
        if (!parts.empty() && parts.back().material == material &&
            parts.back().firstIndex + parts.back().indexCount == firstIndex) {
            parts.back().indexCount += indexCount;
            return;
        }
        parts.push_back({firstIndex, indexCount, material});
    }
};

} // namespace fe
