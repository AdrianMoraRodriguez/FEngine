#pragma once

/**
 * @file MeshData.h
 * @brief CPU-side triangle mesh produced by the model importers.
 */

#include "gfx/Vertex.h"

#include <cstdint>
#include <vector>

namespace fe {

/// Triangle mesh data ready to be uploaded to the GPU.
struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;   ///< Three per triangle, counter-clockwise.
};

} // namespace fe
