#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>

namespace desk {

// Values 1..8 deliberately match the native WM_SIZING/WMSZ_* values.
enum class SnapEdge {
    Move = 0, Left = 1, Right = 2, Top = 3, TopLeft = 4,
    TopRight = 5, Bottom = 6, BottomLeft = 7, BottomRight = 8
};

struct SnapSettings {
    int distance = 12;
    int minimumWidth = 228;
    int minimumHeight = 190;
};

// All rectangles and settings use physical pixels. The caller applies DPI scaling.
// Moving preserves size. Resizing changes only the edge(s) being dragged.
// Only the nearest / greatest-intersection work area attracts a rectangle.
RECT SnapRect(const RECT& proposed, SnapEdge edge,
    const std::vector<RECT>& workAreas, const std::vector<RECT>& peers,
    const SnapSettings& settings = {});

} // namespace desk
