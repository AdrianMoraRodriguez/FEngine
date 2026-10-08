#pragma once

#include "editor/Panel.h"
#include "gfx/OffscreenBuffer.h"

namespace fe {

class ViewportPanel : public Panel {
public:
    explicit ViewportPanel(OffscreenBuffer& offscreen)
        : m_offscreen(offscreen) {}

    void onImGui() override;

private:
    OffscreenBuffer& m_offscreen;
};

} // namespace fe
