#pragma once

/**
 * @file EditorTheme.h
 * @brief Visual identity of the FEngine editor: fonts, colours and metrics.
 *
 * Everything that decides how the editor looks lives here, so that changing
 * the look of the whole editor means editing one file rather than hunting for
 * colours across the panels.
 *
 * Usage (once, after ImGui::CreateContext() and before the first frame):
 * @code
 * fe::theme::loadFonts();
 * fe::theme::apply();
 * @endcode
 *
 * Two kinds of values are exposed to the panels:
 *  - The palette (@ref fe::theme::accent, @ref fe::theme::textDim, ...), for
 *    drawing custom widgets such as the asset tiles or the toolbar.
 *  - Icon tints (@ref fe::theme::tintFolder, ...), one colour per kind of
 *    thing, so a folder or a light looks the same in every panel.
 *
 * @note Requires Dear ImGui 1.92 or newer: fonts are loaded from memory and
 *       resized on demand, which is what lets one font draw both 14 px labels
 *       and 32 px asset icons.
 */

#include <imgui.h>

namespace fe::theme {

/// Base size, in pixels, of the editor's text.
inline constexpr float kFontSize = 15.0f;

/**
 * @brief Loads the editor fonts into ImGui's font atlas.
 *
 * Adds Inter Regular as the default font and Inter SemiBold as a second one
 * (see @ref semiBold). The Lucide icon font is merged into both, so icon
 * macros from Icons.h work in any text drawn with either.
 *
 * Call after ImGui::CreateContext(). The font data is embedded in the
 * executable (see EditorFontData.h), so no files are needed at run time.
 */
void loadFonts();

/**
 * @brief Applies the FEngine colours and metrics to the current ImGui style.
 *
 * Call after ImGui::CreateContext(). It replaces every colour of the style,
 * so call it instead of ImGui::StyleColorsDark(), not after it.
 */
void apply();

/// @return The SemiBold font, for section titles and emphasis. Never null
///         once loadFonts() has run; falls back to the default font.
ImFont* semiBold();

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

/// Main accent colour (indigo-violet): selection, focus, active tool.
ImVec4 accent();
/// Accent with its alpha replaced; handy for translucent selection fills.
ImVec4 accent(float alpha);
/// Colour for secondary text and inactive icons.
ImVec4 textDim();

// ---------------------------------------------------------------------------
// Icon tints, one per kind of object
// ---------------------------------------------------------------------------

ImVec4 tintFolder();   ///< Folders (amber).
ImVec4 tintModel();    ///< 3D models and mesh entities (violet).
ImVec4 tintTexture();  ///< Images (green).
ImVec4 tintScene();    ///< Scene files (coral).
ImVec4 tintCode();     ///< Shaders and source files (sky blue).
ImVec4 tintFile();     ///< Any other file (grey).
ImVec4 tintLight();    ///< Lights (yellow).
ImVec4 tintCamera();   ///< Cameras (blue).

} // namespace fe::theme
