// Display.hpp - the output geometry the JetExamples scenes lay themselves out
// against (as picojet's examples/runtime/Display.hpp): 320x200, with a band
// of 20 rows above and below on the 320x240 panel (Runtime.cpp).
#pragma once

namespace Display {

constexpr int SCREEN_WIDTH  = 320;
constexpr int SCREEN_HEIGHT = 200;
constexpr int EDGE_INSET_X  = 0;
constexpr int EDGE_INSET_Y  = 0;
constexpr int RENDER_WIDTH  = SCREEN_WIDTH  - EDGE_INSET_X;
constexpr int RENDER_HEIGHT = SCREEN_HEIGHT - EDGE_INSET_Y;
constexpr int RENDER_TOP    = (SCREEN_HEIGHT - RENDER_HEIGHT) / 2;
constexpr int RENDER_LEFT   = (SCREEN_WIDTH  - RENDER_WIDTH ) / 2;

/* Whole frames, not interlaced fields. */
constexpr int RENDER_FIELDS = 1;

} // namespace Display
