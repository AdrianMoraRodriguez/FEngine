#pragma once

/**
 * @file EditorFontData.h
 * @brief Embedded font data for the editor UI (generated, see EditorFontData.cpp).
 *
 * The arrays are plain TrueType files held in the executable. They are passed
 * to ImGui with FontDataOwnedByAtlas = false, because the data must stay valid
 * for as long as the font atlas lives.
 */

namespace fe::fontdata {

extern const unsigned char kInterRegular[42232];   ///< TrueType file, 42232 bytes.
extern const unsigned char kInterSemiBold[42312];   ///< TrueType file, 42312 bytes.
extern const unsigned char kLucide[20744];   ///< TrueType file, 20744 bytes.

} // namespace fe::fontdata
