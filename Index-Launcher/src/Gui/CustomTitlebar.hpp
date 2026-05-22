#pragma once

namespace Index::LauncherUI {

    // Draws the launcher's custom titlebar (title text + minimize /
    // maximize-restore / close buttons) and publishes the draggable
    // caption rect + per-button non-client overrides to Window. Must
    // run inside an ImGui frame, BEFORE any other ImGui windows for the
    // current frame. No-op when the active Window's CustomTitlebar
    // flag is false.
    void RenderTitlebar();

    // Logical-pixel height of the titlebar row. The launcher panel
    // uses this to offset its content so the panel doesn't overlap
    // the titlebar.
    float GetTitlebarRowHeight();

}
