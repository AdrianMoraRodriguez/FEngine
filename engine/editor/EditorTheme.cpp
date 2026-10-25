#include "editor/EditorTheme.h"

#include "editor/EditorFontData.h"

namespace fe::theme {
namespace {

ImFont* g_semiBold = nullptr;

/// Builds a colour from a 0xRRGGBB value, so the palette below reads like a
/// design file instead of lists of floats.
constexpr ImVec4 hex(unsigned int rgb, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((rgb >>  8) & 0xFF) / 255.0f,
                  static_cast<float>( rgb        & 0xFF) / 255.0f,
                  alpha);
}

constexpr ImVec4 withAlpha(ImVec4 c, float alpha) {
    return ImVec4(c.x, c.y, c.z, alpha);
}

// Neutrals: a very slightly blue-tinted grey, darkest at the back.
constexpr ImVec4 kBgDeep    = hex(0x121317);   // behind the panels
constexpr ImVec4 kBgBar     = hex(0x16171C);   // title/menu bars, inactive tabs
constexpr ImVec4 kBgPanel   = hex(0x1B1C22);   // panel background
constexpr ImVec4 kBgPopup   = hex(0x20212A);   // menus, tooltips
constexpr ImVec4 kBgField   = hex(0x262832);   // inputs, buttons
constexpr ImVec4 kBgHover   = hex(0x30333F);
constexpr ImVec4 kBgActive  = hex(0x3A3E4C);
constexpr ImVec4 kBorder    = hex(0x2B2D37);

constexpr ImVec4 kText      = hex(0xE6E7EC);
constexpr ImVec4 kTextDim   = hex(0x7F8392);

// Accent: indigo-violet.
constexpr ImVec4 kAccent       = hex(0x7C6CF0);
constexpr ImVec4 kAccentHover  = hex(0x9185F5);

} // namespace

// ---------------------------------------------------------------------------
// Palette accessors
// ---------------------------------------------------------------------------

ImVec4 accent()              { return kAccent; }
ImVec4 accent(float alpha)   { return withAlpha(kAccent, alpha); }
ImVec4 textDim()             { return kTextDim; }

ImVec4 tintFolder()  { return hex(0xE6B566); }
ImVec4 tintModel()   { return hex(0xAC9FFF); }
ImVec4 tintTexture() { return hex(0x5FCFA9); }
ImVec4 tintScene()   { return hex(0xF38E70); }
ImVec4 tintCode()    { return hex(0x6DB7F7); }
ImVec4 tintFile()    { return hex(0x8A8E9D); }
ImVec4 tintLight()   { return hex(0xF4CB52); }
ImVec4 tintCamera()  { return hex(0x6DB7F7); }

ImFont* semiBold() {
    return g_semiBold ? g_semiBold : ImGui::GetFont();
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();

    // The data lives in the executable, so the atlas must not try to free it.
    ImFontConfig text;
    text.FontDataOwnedByAtlas = false;

    ImFontConfig icons;
    icons.FontDataOwnedByAtlas = false;
    icons.MergeMode            = true;       // add the icons to the font above
    icons.GlyphOffset          = ImVec2(0.0f, 1.0f);   // sit on the text baseline

    auto addTo = [&](const unsigned char* ttf, int size) {
        return io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(ttf), size, kFontSize, &text);
    };
    auto mergeIcons = [&]() {
        io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(fontdata::kLucide),
            static_cast<int>(sizeof(fontdata::kLucide)), kFontSize, &icons);
    };

    // The first font added becomes the default one.
    addTo(fontdata::kInterRegular, static_cast<int>(sizeof(fontdata::kInterRegular)));
    mergeIcons();

    g_semiBold = addTo(fontdata::kInterSemiBold,
                       static_cast<int>(sizeof(fontdata::kInterSemiBold)));
    mergeIcons();
}

// ---------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------

void apply() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // ---- Metrics: more air, softer corners ----
    s.WindowPadding     = ImVec2(10.0f, 10.0f);
    s.FramePadding      = ImVec2(8.0f, 5.0f);
    s.CellPadding       = ImVec2(6.0f, 4.0f);
    s.ItemSpacing       = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    s.IndentSpacing     = 16.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;

    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.TabBarBorderSize  = 1.0f;
    s.TabBarOverlineSize = 2.0f;

    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 5.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 5.0f;

    s.WindowTitleAlign        = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;   // no collapse arrow on docked tabs
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding    = ImVec2(10.0f, 4.0f);
    s.DockingSeparatorSize    = 2.0f;

    // ---- Colours ----
    c[ImGuiCol_Text]                  = kText;
    c[ImGuiCol_TextDisabled]          = kTextDim;
    c[ImGuiCol_WindowBg]              = kBgPanel;
    c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]               = kBgPopup;
    c[ImGuiCol_Border]                = kBorder;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = kBgField;
    c[ImGuiCol_FrameBgHovered]        = kBgHover;
    c[ImGuiCol_FrameBgActive]         = kBgActive;

    c[ImGuiCol_TitleBg]               = kBgBar;
    c[ImGuiCol_TitleBgActive]         = kBgBar;
    c[ImGuiCol_TitleBgCollapsed]      = kBgBar;
    c[ImGuiCol_MenuBarBg]             = kBgBar;

    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = kBgHover;
    c[ImGuiCol_ScrollbarGrabHovered]  = kBgActive;
    c[ImGuiCol_ScrollbarGrabActive]   = kTextDim;

    c[ImGuiCol_CheckMark]             = kAccentHover;
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = kAccentHover;

    c[ImGuiCol_Button]                = kBgField;
    c[ImGuiCol_ButtonHovered]         = kBgHover;
    c[ImGuiCol_ButtonActive]          = kBgActive;

    // "Header" colours paint selected/hovered rows of trees, lists and
    // collapsing headers: the accent at low opacity reads as "selected"
    // without shouting.
    c[ImGuiCol_Header]                = withAlpha(kAccent, 0.30f);
    c[ImGuiCol_HeaderHovered]         = withAlpha(kAccent, 0.18f);
    c[ImGuiCol_HeaderActive]          = withAlpha(kAccent, 0.42f);

    c[ImGuiCol_Separator]             = kBorder;
    c[ImGuiCol_SeparatorHovered]      = kAccentHover;
    c[ImGuiCol_SeparatorActive]       = kAccent;

    c[ImGuiCol_ResizeGrip]            = withAlpha(kAccent, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = withAlpha(kAccent, 0.60f);
    c[ImGuiCol_ResizeGripActive]      = kAccent;

    c[ImGuiCol_InputTextCursor]       = kText;

    c[ImGuiCol_Tab]                   = kBgBar;
    c[ImGuiCol_TabHovered]            = kBgHover;
    c[ImGuiCol_TabSelected]           = kBgPanel;
    c[ImGuiCol_TabSelectedOverline]   = kAccent;
    c[ImGuiCol_TabDimmed]             = kBgBar;
    c[ImGuiCol_TabDimmedSelected]     = kBgPanel;
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(kAccent, 0.45f);

    c[ImGuiCol_DockingPreview]        = withAlpha(kAccent, 0.45f);
    c[ImGuiCol_DockingEmptyBg]        = kBgDeep;

    c[ImGuiCol_PlotLines]             = kTextDim;
    c[ImGuiCol_PlotLinesHovered]      = kAccentHover;
    c[ImGuiCol_PlotHistogram]         = kAccent;
    c[ImGuiCol_PlotHistogramHovered]  = kAccentHover;

    c[ImGuiCol_TableHeaderBg]         = kBgBar;
    c[ImGuiCol_TableBorderStrong]     = kBorder;
    c[ImGuiCol_TableBorderLight]      = withAlpha(kBorder, 0.6f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.025f);

    c[ImGuiCol_TextLink]              = kAccentHover;
    c[ImGuiCol_TextSelectedBg]        = withAlpha(kAccent, 0.35f);
    c[ImGuiCol_TreeLines]             = kBorder;
    c[ImGuiCol_DragDropTarget]        = kAccentHover;
    c[ImGuiCol_NavCursor]             = kAccentHover;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.6f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.5f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55f);
}

} // namespace fe::theme
