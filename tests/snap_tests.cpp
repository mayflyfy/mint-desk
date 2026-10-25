#include "../src/snap.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
int checks = 0;
void Check(const RECT& actual, const RECT& expected, const char* label) {
    ++checks;
    if (actual.left == expected.left && actual.top == expected.top &&
        actual.right == expected.right && actual.bottom == expected.bottom) return;
    std::fprintf(stderr, "FAIL: %s: got (%ld,%ld,%ld,%ld), expected (%ld,%ld,%ld,%ld)\n",
        label, actual.left, actual.top, actual.right, actual.bottom,
        expected.left, expected.top, expected.right, expected.bottom);
    ++failures;
}
} // namespace

int main() {
    using namespace desk;
    const std::vector<RECT> monitor{{0, 0, 1920, 1040}};
    const std::vector<RECT> none;
    Check(SnapRect({8, 9, 308, 309}, SnapEdge::Move, monitor, none), {0, 0, 300, 300},
        "drag near corner snaps both axes and preserves size");
    Check(SnapRect({12, 100, 312, 400}, SnapEdge::Move, monitor, none), {0, 100, 300, 400},
        "threshold is inclusive");
    Check(SnapRect({13, 100, 313, 400}, SnapEdge::Move, monitor, none), {13, 100, 313, 400},
        "outside threshold remains untouched");
    Check(SnapRect({1612, 734, 1912, 1034}, SnapEdge::Move, monitor, none), {1620, 740, 1920, 1040},
        "right and bottom use work area boundary");
    Check(SnapRect({0, 9, 300, 309}, SnapEdge::Move, monitor, none), {0, 0, 300, 300},
        "coincident horizontal edge still permits vertical snap");

    const std::vector<RECT> peer{{500, 100, 800, 400}};
    Check(SnapRect({192, 104, 492, 404}, SnapEdge::Move, none, peer), {200, 100, 500, 400},
        "moving adjacent card snaps touching edge and aligned top");
    Check(SnapRect({508, 406, 808, 706}, SnapEdge::Move, none, peer), {500, 400, 800, 700},
        "same sides align while snapping below peer");
    Check(SnapRect({811, 411, 1111, 711}, SnapEdge::Move, none, peer), {800, 400, 1100, 700},
        "near diagonal corner snaps two axes");
    Check(SnapRect({508, 900, 808, 1200}, SnapEdge::Move, none, peer), {508, 900, 808, 1200},
        "distant vertical card does not attract horizontal alignment");
    Check(SnapRect({1300, 108, 1600, 408}, SnapEdge::Move, none, peer), {1300, 108, 1600, 408},
        "distant horizontal card does not attract vertical alignment");
    Check(SnapRect({811, 413, 1111, 713}, SnapEdge::Move, none, peer), {811, 413, 1111, 713},
        "diagonal gap outside perpendicular threshold does not attract");

    Check(SnapRect({8, 8, 400, 400}, SnapEdge::Left, monitor, none), {0, 8, 400, 400},
        "left resizing leaves all fixed edges untouched");
    Check(SnapRect({8, 8, 400, 400}, SnapEdge::Top, monitor, none), {8, 0, 400, 400},
        "top resizing leaves horizontal edges untouched");
    Check(SnapRect({1500, 100, 1912, 500}, SnapEdge::Right, monitor, none), {1500, 100, 1920, 500},
        "right resizing snaps screen edge only");
    Check(SnapRect({100, 500, 500, 1030}, SnapEdge::Bottom, monitor, none), {100, 500, 500, 1040},
        "bottom resizing snaps screen edge only");
    Check(SnapRect({8, 8, 400, 400}, SnapEdge::TopLeft, monitor, none), {0, 0, 400, 400},
        "top-left resizing snaps both active edges");
    Check(SnapRect({1500, 8, 1912, 400}, SnapEdge::TopRight, monitor, none), {1500, 0, 1920, 400},
        "top-right resizing snaps both active edges");
    Check(SnapRect({8, 500, 400, 1030}, SnapEdge::BottomLeft, monitor, none), {0, 500, 400, 1040},
        "bottom-left resizing snaps both active edges");
    Check(SnapRect({1500, 500, 1912, 1030}, SnapEdge::BottomRight, monitor, none), {1500, 500, 1920, 1040},
        "bottom-right resizing snaps both active edges");
    Check(SnapRect({100, 100, 491, 393}, SnapEdge::BottomRight, none, peer), {100, 100, 500, 400},
        "resize active edges snap to peer adjacency and alignment");
    Check(SnapRect({790, 103, 1100, 403}, SnapEdge::Left, none, peer), {800, 103, 1100, 403},
        "resize left edge can attach to peer right edge");

    Check(SnapRect({200, 100, 400, 400}, SnapEdge::Left, none, none), {172, 100, 400, 400},
        "undersize width corrected by dragged edge");
    Check(SnapRect({100, 100, 200, 200}, SnapEdge::BottomRight, none, none), {100, 100, 328, 290},
        "corner resize enforces both minimum dimensions");
    Check(SnapRect({100, 100, 328, 400}, SnapEdge::Right, none, {{320, 100, 600, 400}}),
        {100, 100, 328, 400}, "snap candidate cannot violate minimum width");
    Check(SnapRect({5, 8, 105, 108}, SnapEdge::Move, monitor, none), {0, 0, 100, 100},
        "moving does not enlarge a small rectangle");

    Check(SnapRect({-2550, 10, -2100, 460}, SnapEdge::Move,
        {{-2560, 0, 0, 1400}, {0, 0, 1920, 1040}}, none), {-2560, 0, -2110, 450},
        "negative-coordinate monitor snaps to its own boundaries");
    Check(SnapRect({-25, 200, 425, 650}, SnapEdge::Move,
        {{-2560, 0, 0, 1400}, {0, 0, 1920, 1040}}, none), {-25, 200, 425, 650},
        "maximum intersection picks primary instead of distant side edge");
    Check(SnapRect({-310, 200, -10, 500}, SnapEdge::Move,
        {{-1920, 0, -8, 1040}, {0, 0, 1920, 1040}}, none), {-308, 200, -8, 500},
        "monitor selection avoids snapping across a display gap");
    Check(SnapRect({-310, 200, -10, 500}, SnapEdge::Move,
        {{-1920, 0, -8, 1040}, {0, 0, 1920, 1040}}, {{-2, 100, 298, 600}}),
        {-308, 200, -8, 500}, "closer work area edge wins over peer across display gap");
    Check(SnapRect({1010, 200, 1310, 500}, SnapEdge::Move,
        {{0, 0, 1000, 1000}, {2000, 0, 3000, 1000}}, none), {1010, 200, 1310, 500},
        "rectangle between displays never teleports over a gap");
    Check(SnapRect({8, 100, 308, 400}, SnapEdge::Move,
        {{0, 0, 1920, 1040}, {5, 0, 1915, 1040}}, none), {0, 100, 300, 400},
        "overlapping work areas use deterministic intersection tie");

    Check(SnapRect({-2544, 16, -2094, 466}, SnapEdge::Move,
        {{-2560, 0, 0, 1400}}, none, {18, 342, 285}), {-2560, 0, -2110, 450},
        "150 percent DPI threshold and physical size");
    Check(SnapRect({20, 20, 620, 620}, SnapEdge::Move,
        {{0, 0, 3840, 2080}}, none, {24, 456, 380}), {0, 0, 600, 600},
        "200 percent DPI threshold and physical size");
    Check(SnapRect({100, 100, 550, 600}, SnapEdge::Right, none,
        {{540, 100, 1000, 600}}, {24, 456, 380}), {100, 100, 556, 600},
        "scaled minimum size rejects otherwise nearby peer");

    Check(SnapRect({8, 100, 308, 400}, SnapEdge::Move, monitor, {{16, 100, 316, 400}}),
        {0, 100, 300, 400}, "screen edge wins equal-distance peer tie");
    const RECT candidate{500, 100, 800, 400};
    const std::vector<RECT> ties{{492, 100, 792, 400}, {508, 100, 808, 400}};
    Check(SnapRect(candidate, SnapEdge::Move, none, ties), {492, 100, 792, 400},
        "equal peer distances resolve deterministically");
    Check(SnapRect(candidate, SnapEdge::Move, none, {ties[1], ties[0]}), {492, 100, 792, 400},
        "peer order does not alter tie result");
    const RECT snapped = SnapRect({811, 411, 1111, 711}, SnapEdge::Move, none, peer);
    Check(SnapRect(snapped, SnapEdge::Move, none, peer), snapped,
        "repeated coincident snap is stable");
    Check(SnapRect({8, 8, 308, 308}, SnapEdge::Move, {{1, 1, 1, 1}}, {{1, 1, 1, 1}}),
        {8, 8, 308, 308}, "invalid target rectangles are ignored");

    // Repeated one-pixel pointer movements must escape attraction. Feeding the
    // last snapped HWND rectangle back into these steps would pin it forever.
    for (LONG step = 0; step <= 40; ++step) {
        const auto raw = PointerDragRect({0, 0, 300, 300}, {50, 20}, {50 + step, 20 + step}, SnapEdge::Move);
        const LONG position = step <= 12 ? 0 : step;
        Check(SnapRect(raw, SnapEdge::Move, monitor, none),
            {position, position, position + 300, position + 300}, "slow diagonal drag escapes a snapped screen corner");
        const auto adjacent = PointerDragRect({200, 100, 500, 400}, {250, 120}, {250 + step, 120}, SnapEdge::Move);
        Check(SnapRect(adjacent, SnapEdge::Move, none, peer),
            {200 + position, 100, 500 + position, 400}, "slow drag escapes peer contact without Alt");
        const auto resized = PointerDragRect({200, 100, 500, 400}, {500, 200}, {500 - step, 200}, SnapEdge::Right);
        Check(SnapRect(resized, SnapEdge::Right, none, peer),
            {200, 100, 500 - position, 400}, "slow resize escapes peer contact without moving fixed edges");
    }
    Check(PointerDragRect({-1000, -500, -700, -200}, {-900, -480}, {-870, -440}, SnapEdge::Move),
        {-970, -460, -670, -160}, "pointer anchor supports negative monitor coordinates");
    Check(PointerDragRect({100, 100, 500, 500}, {100, 100}, {120, 130}, SnapEdge::TopLeft),
        {120, 130, 500, 500}, "corner pointer resize preserves opposite edges");
    SnapGuides guides;
    Check(SnapRect({192, 104, 492, 404}, SnapEdge::Move, none, peer, {}, &guides),
        {200, 100, 500, 400}, "guide metadata leaves snap geometry unchanged");
    Check(guides.verticalRect, {500, 100, 501, 400}, "vertical guide follows the winning contact edge");
    Check(guides.horizontalRect, {200, 100, 800, 101}, "horizontal guide spans aligned cards");
    SnapRect({1000, 800, 1300, 1100}, SnapEdge::Move, none, peer, {}, &guides);
    Check(guides.vertical || guides.horizontal ? RECT{1,1,1,1} : RECT{}, {},
        "leaving all snap targets clears guide metadata");

    if (failures) return 1;
    std::printf("Snap geometry: %d checks passed.\n", checks);
    return 0;
}
