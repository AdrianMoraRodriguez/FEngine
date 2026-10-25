#pragma once

/**
 * @file Icons.h
 * @brief Icon glyphs of the editor, as UTF-8 string literals.
 *
 * Icons come from the Lucide icon font, which is merged into the editor's
 * default font, so an icon is just text:
 * @code
 * ImGui::Text(FE_ICON_FOLDER "  Models");   // adjacent literals concatenate
 * @endcode
 * Macros (not constants) are used so that they can be concatenated with other
 * string literals at compile time.
 *
 * The font file only contains the icons listed here (it is subset to keep the
 * executable small). To add an icon, add its Lucide name to the subset list
 * when regenerating the font data and define it below.
 */

#define FE_ICON_ARROW_LEFT "\xee\x81\x88"
#define FE_ICON_ARROW_RIGHT "\xee\x81\x89"
#define FE_ICON_ARROW_UP "\xee\x81\x8a"
#define FE_ICON_ARROW_UP_DOWN "\xee\x8d\xbd"
#define FE_ICON_BOX "\xee\x81\xa1"
#define FE_ICON_CHECK "\xee\x81\xac"
#define FE_ICON_CHEVRON_DOWN "\xee\x81\xad"
#define FE_ICON_CHEVRON_RIGHT "\xee\x81\xaf"
#define FE_ICON_CIRCLE_DASHED "\xee\x92\xb0"
#define FE_ICON_CLAPPERBOARD "\xee\x8a\x9b"
#define FE_ICON_CLIPBOARD_PASTE "\xee\x8f\xa8"
#define FE_ICON_COPY "\xee\x82\x9e"
#define FE_ICON_CUBOID "\xee\x94\xa4"
#define FE_ICON_EXTERNAL_LINK "\xee\x82\xb9"
#define FE_ICON_EYE "\xee\x82\xba"
#define FE_ICON_EYE_OFF "\xee\x82\xbb"
#define FE_ICON_FILE "\xee\x83\x80"
#define FE_ICON_FILE_CODE "\xee\x83\x83"
#define FE_ICON_FILE_JSON "\xee\x8d\xab"
#define FE_ICON_FILE_TEXT "\xee\x83\x8c"
#define FE_ICON_FOCUS "\xee\x8a\x9e"
#define FE_ICON_FOLDER "\xee\x83\x97"
#define FE_ICON_FOLDER_OPEN "\xee\x89\x87"
#define FE_ICON_FOLDER_PLUS "\xee\x83\x99"
#define FE_ICON_GLOBE "\xee\x83\xa8"
#define FE_ICON_GRID_3X3 "\xee\x83\xa9"
#define FE_ICON_HOUSE "\xee\x83\xb5"
#define FE_ICON_IMAGE "\xee\x83\xb6"
#define FE_ICON_INFO "\xee\x83\xb9"
#define FE_ICON_LAYERS "\xee\x94\xa9"
#define FE_ICON_LAYOUT_GRID "\xee\x83\xbf"
#define FE_ICON_LIGHTBULB "\xee\x87\x82"
#define FE_ICON_LIST "\xee\x84\x86"
#define FE_ICON_MOUSE_POINTER_2 "\xee\x87\x83"
#define FE_ICON_MOVE "\xee\x84\xa1"
#define FE_ICON_MOVE_3D "\xee\x8b\xa5"
#define FE_ICON_PALETTE "\xee\x87\x9d"
#define FE_ICON_PENCIL "\xee\x87\xb9"
#define FE_ICON_PLUS "\xee\x84\xbd"
#define FE_ICON_REFRESH_CW "\xee\x85\x85"
#define FE_ICON_ROTATE_3D "\xee\x8b\xaa"
#define FE_ICON_SAVE "\xee\x85\x8d"
#define FE_ICON_SCALING "\xee\x8b\xac"
#define FE_ICON_SCISSORS "\xee\x85\x8e"
#define FE_ICON_SEARCH "\xee\x85\x91"
#define FE_ICON_SETTINGS "\xee\x85\x94"
#define FE_ICON_SLIDERS_HORIZONTAL "\xee\x8a\x9a"
#define FE_ICON_SPARKLES "\xee\x90\x92"
#define FE_ICON_SPOTLIGHT "\xee\x9a\x82"
#define FE_ICON_SUN "\xee\x85\xb8"
#define FE_ICON_TRASH_2 "\xee\x86\x8e"
#define FE_ICON_TRIANGLE_ALERT "\xee\x86\x93"
#define FE_ICON_VIDEO "\xee\x86\xa5"
#define FE_ICON_X "\xee\x86\xb2"
