#pragma once

/**
 * @file AlphaMode.h
 * @brief How a material's transparency is drawn.
 */

#include <cstdint>

namespace fe {

/**
 * @brief What the alpha channel of a material does.
 *
 * The same three modes as glTF, so a model's own setting maps onto them.
 */
enum class AlphaMode : uint8_t {
    Opaque = 0,   ///< Alpha is ignored: the surface is solid.
    Mask   = 1,   ///< Cut-out: pixels with alpha below the cutoff are not drawn at all; the rest are solid. Leaves, fences, decals.
    Blend  = 2,   ///< Translucent: the surface is mixed with whatever is behind it. Glass, water, smoke.
};

} // namespace fe
