#pragma once

/**
 * @file ToneMapper.h
 * @brief The curves that squeeze HDR light into what a screen can show.
 */

namespace fe {

/**
 * @brief Which curve PostProcess uses to bring scene light into the 0..1 range.
 *
 * The scene is lit in linear light that can be far brighter than 1 (the sun,
 * an emissive texture). A screen cannot show that, so the light goes through
 * one of these curves just before it is displayed. Saved in scene files as the
 * number, so the values must never change.
 */
enum class ToneMapper : int {
    Neutral  = 0,   ///< Khronos "PBR Neutral": colours below about 0.76 are untouched, brighter ones roll off keeping their hue. Faithful to textures.
    Aces     = 1,   ///< ACES-like filmic curve (Narkowicz fit): more contrast and a punchy look, bright colours drift towards white.
    Reinhard = 2,   ///< x / (1 + x): soft and simple, but washes colours out a little.
    Linear   = 3,   ///< No curve: values above 1 are cut off. For checking raw light levels.
};

/// Number of tone mappers, for validating numbers read from a file.
constexpr int kToneMapperCount = 4;

} // namespace fe
