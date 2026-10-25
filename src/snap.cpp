#include "snap.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace desk {
namespace {
using Coordinate = int64_t;

bool Valid(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

Coordinate Gap(LONG firstStart, LONG firstEnd, LONG secondStart, LONG secondEnd) {
    return std::max<Coordinate>(0, std::max<Coordinate>(
        Coordinate(secondStart) - firstEnd, Coordinate(firstStart) - secondEnd));
}

const RECT* WorkArea(const RECT& proposed, const std::vector<RECT>& workAreas) {
    const RECT* best = nullptr;
    uint64_t bestArea = 0;
    long double bestDistance = std::numeric_limits<long double>::max();
    for (const auto& work : workAreas) {
        if (!Valid(work)) continue;
        const Coordinate width = std::max<Coordinate>(0,
            Coordinate(std::min(proposed.right, work.right)) - std::max(proposed.left, work.left));
        const Coordinate height = std::max<Coordinate>(0,
            Coordinate(std::min(proposed.bottom, work.bottom)) - std::max(proposed.top, work.top));
        const uint64_t area = uint64_t(width) * uint64_t(height);
        const long double dx = static_cast<long double>(
            Gap(proposed.left, proposed.right, work.left, work.right));
        const long double dy = static_cast<long double>(
            Gap(proposed.top, proposed.bottom, work.top, work.bottom));
        const long double distance = dx * dx + dy * dy;
        if (!best || area > bestArea || (area == bestArea && distance < bestDistance)) {
            best = &work;
            bestArea = area;
            bestDistance = distance;
        }
    }
    return best;
}

bool FitsCoordinate(Coordinate value) {
    return value >= std::numeric_limits<LONG>::min() &&
        value <= std::numeric_limits<LONG>::max();
}

struct Choice {
    Coordinate distance;
    Coordinate delta = 0;
    bool found = false;
    bool screen = false;
    LONG guide = 0, begin = 0, end = 0;

    void Consider(Coordinate candidate, bool fromScreen, bool feasible,
        LONG coordinate, LONG first, LONG last) {
        const Coordinate magnitude = candidate < 0 ? -candidate : candidate;
        if (!feasible || magnitude > distance) return;
        const Coordinate previousMagnitude = delta < 0 ? -delta : delta;
        // A coordinate tie is independent of peer enumeration order.
        if (!found || magnitude < previousMagnitude ||
            (magnitude == previousMagnitude &&
                ((fromScreen && !screen) || (fromScreen == screen && candidate < delta)))) {
            delta = candidate;
            found = true;
            screen = fromScreen;
            guide = coordinate; begin = first; end = last;
        }
    }
};
} // namespace

RECT PointerDragRect(const RECT& initial, POINT start, POINT current, SnapEdge edge) {
    const bool moving = edge == SnapEdge::Move;
    const bool left = moving || edge == SnapEdge::Left || edge == SnapEdge::TopLeft || edge == SnapEdge::BottomLeft;
    const bool right = moving || edge == SnapEdge::Right || edge == SnapEdge::TopRight || edge == SnapEdge::BottomRight;
    const bool top = moving || edge == SnapEdge::Top || edge == SnapEdge::TopLeft || edge == SnapEdge::TopRight;
    const bool bottom = moving || edge == SnapEdge::Bottom || edge == SnapEdge::BottomLeft || edge == SnapEdge::BottomRight;
    const Coordinate dx = Coordinate(current.x) - start.x, dy = Coordinate(current.y) - start.y;
    const Coordinate l = Coordinate(initial.left) + (left ? dx : 0);
    const Coordinate r = Coordinate(initial.right) + (right ? dx : 0);
    const Coordinate t = Coordinate(initial.top) + (top ? dy : 0);
    const Coordinate b = Coordinate(initial.bottom) + (bottom ? dy : 0);
    if (!FitsCoordinate(l) || !FitsCoordinate(r) || !FitsCoordinate(t) || !FitsCoordinate(b)) return initial;
    return {static_cast<LONG>(l), static_cast<LONG>(t), static_cast<LONG>(r), static_cast<LONG>(b)};
}

RECT SnapRect(const RECT& proposed, SnapEdge edge, const std::vector<RECT>& workAreas,
    const std::vector<RECT>& peers, const SnapSettings& settings, SnapGuides* guides) {
    if (guides) *guides = {};
    const bool moving = edge == SnapEdge::Move;
    const bool left = edge == SnapEdge::Left || edge == SnapEdge::TopLeft || edge == SnapEdge::BottomLeft;
    const bool right = edge == SnapEdge::Right || edge == SnapEdge::TopRight || edge == SnapEdge::BottomRight;
    const bool top = edge == SnapEdge::Top || edge == SnapEdge::TopLeft || edge == SnapEdge::TopRight;
    const bool bottom = edge == SnapEdge::Bottom || edge == SnapEdge::BottomLeft || edge == SnapEdge::BottomRight;
    if (!moving && !left && !right && !top && !bottom) return proposed;

    const Coordinate minimumWidth = std::max(1, settings.minimumWidth);
    const Coordinate minimumHeight = std::max(1, settings.minimumHeight);
    const Coordinate distance = std::max(0, settings.distance);
    RECT rect = proposed;
    // Respect WM_GETMINMAXINFO even if a candidate edge would cross the fixed edge.
    if (left && Coordinate(rect.right) - rect.left < minimumWidth && FitsCoordinate(Coordinate(rect.right) - minimumWidth))
        rect.left = static_cast<LONG>(Coordinate(rect.right) - minimumWidth);
    if (right && Coordinate(rect.right) - rect.left < minimumWidth && FitsCoordinate(Coordinate(rect.left) + minimumWidth))
        rect.right = static_cast<LONG>(Coordinate(rect.left) + minimumWidth);
    if (top && Coordinate(rect.bottom) - rect.top < minimumHeight && FitsCoordinate(Coordinate(rect.bottom) - minimumHeight))
        rect.top = static_cast<LONG>(Coordinate(rect.bottom) - minimumHeight);
    if (bottom && Coordinate(rect.bottom) - rect.top < minimumHeight && FitsCoordinate(Coordinate(rect.top) + minimumHeight))
        rect.bottom = static_cast<LONG>(Coordinate(rect.top) + minimumHeight);

    Choice x{distance}, y{distance};
    const auto considerX = [&](Coordinate delta, bool screen, const RECT& target, LONG coordinate) {
        bool feasible = true;
        if (moving) feasible = FitsCoordinate(Coordinate(rect.left) + delta) && FitsCoordinate(Coordinate(rect.right) + delta);
        else if (left) feasible = Coordinate(rect.right) - rect.left - delta >= minimumWidth;
        else if (right) feasible = Coordinate(rect.right) - rect.left + delta >= minimumWidth;
        else return;
        x.Consider(delta, screen, feasible, coordinate, target.top, target.bottom);
    };
    const auto considerY = [&](Coordinate delta, bool screen, const RECT& target, LONG coordinate) {
        bool feasible = true;
        if (moving) feasible = FitsCoordinate(Coordinate(rect.top) + delta) && FitsCoordinate(Coordinate(rect.bottom) + delta);
        else if (top) feasible = Coordinate(rect.bottom) - rect.top - delta >= minimumHeight;
        else if (bottom) feasible = Coordinate(rect.bottom) - rect.top + delta >= minimumHeight;
        else return;
        y.Consider(delta, screen, feasible, coordinate, target.left, target.right);
    };

    if (const RECT* work = WorkArea(rect, workAreas)) {
        if (moving || left) considerX(Coordinate(work->left) - rect.left, true, *work, work->left);
        if (moving || right) considerX(Coordinate(work->right) - rect.right, true, *work, work->right);
        if (moving || top) considerY(Coordinate(work->top) - rect.top, true, *work, work->top);
        if (moving || bottom) considerY(Coordinate(work->bottom) - rect.bottom, true, *work, work->bottom);
    }
    for (const auto& peer : peers) {
        if (!Valid(peer)) continue;
        if (Gap(rect.top, rect.bottom, peer.top, peer.bottom) <= distance) {
            if (moving || left) {
                considerX(Coordinate(peer.left) - rect.left, false, peer, peer.left);
                considerX(Coordinate(peer.right) - rect.left, false, peer, peer.right);
            }
            if (moving || right) {
                considerX(Coordinate(peer.right) - rect.right, false, peer, peer.right);
                considerX(Coordinate(peer.left) - rect.right, false, peer, peer.left);
            }
        }
        if (Gap(rect.left, rect.right, peer.left, peer.right) <= distance) {
            if (moving || top) {
                considerY(Coordinate(peer.top) - rect.top, false, peer, peer.top);
                considerY(Coordinate(peer.bottom) - rect.top, false, peer, peer.bottom);
            }
            if (moving || bottom) {
                considerY(Coordinate(peer.bottom) - rect.bottom, false, peer, peer.bottom);
                considerY(Coordinate(peer.top) - rect.bottom, false, peer, peer.top);
            }
        }
    }
    if (moving || left) rect.left = static_cast<LONG>(Coordinate(rect.left) + x.delta);
    if (moving || right) rect.right = static_cast<LONG>(Coordinate(rect.right) + x.delta);
    if (moving || top) rect.top = static_cast<LONG>(Coordinate(rect.top) + y.delta);
    if (moving || bottom) rect.bottom = static_cast<LONG>(Coordinate(rect.bottom) + y.delta);
    if (guides) {
        guides->vertical = x.found; guides->horizontal = y.found;
        const LONG vx = x.guide == std::numeric_limits<LONG>::max() ? x.guide - 1 : x.guide;
        const LONG hy = y.guide == std::numeric_limits<LONG>::max() ? y.guide - 1 : y.guide;
        if (x.found) guides->verticalRect = {vx, std::min(rect.top, x.begin), vx + 1, std::max(rect.bottom, x.end)};
        if (y.found) guides->horizontalRect = {std::min(rect.left, y.begin), hy, std::max(rect.right, y.end), hy + 1};
    }
    return rect;
}

} // namespace desk
