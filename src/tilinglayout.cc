// tilinglayout.cc — computes the geometry of each leaf of a FrameTree
// from a work-area rectangle.
#include "tilinglayout.h"

#include <algorithm>
#include <vector>

#include "wmframe.h"

using std::vector;

bool isTilingCandidate(const YFrameWindow* frame) {
    if (frame == nullptr || frame->client() == nullptr)
        return false;
    // a `tiling off` window-option forbids tiling this window
    if (frame->tilingForcedFloating())
        return false;
    // sticky / all-workspaces windows stay floating (like hlwm's sticky)
    if (frame->isAllWorkspaces())
        return false;
    // transient windows (dialogs/menus) stay floating
    if (frame->client()->isTransient())
        return false;
    // modal windows (WinStateModal or Motif input-modal) stay floating too
    if (frame->hasState(WinStateModal))
        return false;
    MwmHints* mwmHints = frame->client()->mwmHints();
    if (mwmHints && mwmHints->inputModal())
        return false;
    if (frame->isMinimized() || frame->isHidden() || frame->isRollup())
        return false;
    if (frame->isFullscreen())
        return false;
    // skip dock/desktop/etc. window types
    WindowType wt;
    if (frame->client()->getNetWMWindowType(&wt)) {
        switch (wt) {
            case wtDock:
            case wtDesktop:
            case wtSplash:
            case wtMenu:
            case wtToolbar:
            case wtTooltip:
            case wtNotification:
            case wtDND:
            case wtPopupMenu:
            case wtCombo:
            case wtDropdownMenu:
                return false;
            default:
                break;
        }
    }
    return true;
}

/*! Split a rectangle at fraction f in the given direction, leaving a
 * gap between the two halves. Returns the two rectangles.
 */
static void splitRect(const YRect& rect, double f, bool horizontal,
                      int gap, YRect& r1, YRect& r2) {
    int gapHalf = gap / 2;
    const int W = int(rect.width());
    const int H = int(rect.height());
    if (horizontal) {
        // children side by side
        int w1 = std::max(1, int(W * f));
        int x2 = rect.x() + w1 + gapHalf;
        int w2 = std::max(1, W - w1 - gapHalf);
        // clamp so the right child never leaves the original rect
        r1 = YRect(rect.x(), rect.y(), std::max(1, std::min(w1, W - 1)),
                   H);
        r2 = YRect(std::min(x2, rect.x() + W - 1), rect.y(),
                   std::max(1, std::min(w2, rect.x() + W - x2)), H);
    } else {
        // children stacked vertically
        int h1 = std::max(1, int(H * f));
        int y2 = rect.y() + h1 + gapHalf;
        int h2 = std::max(1, H - h1 - gapHalf);
        r1 = YRect(rect.x(), rect.y(), W, std::max(1, std::min(h1, H - 1)));
        r2 = YRect(rect.x(), std::min(y2, rect.y() + H - 1), W,
                   std::max(1, std::min(h2, rect.y() + H - y2)));
    }
}

/*! Recursively split the rectangle according to the tree, filling
 * outVec in pre-order (i.e. the same order as FrameTree::focusedLeaf
 * traversal and leaf enumeration).
 */
static void computeRec(const Frame* node, YRect rect,
                       int gap, int borderWidth, int titleHeight,
                       vector<TilingGeometry>& out) {
    const FrameSplit* split = node->isSplit() ? node->asSplit() : nullptr;
    const FrameLeaf* leaf = node->isLeaf() ? node->asLeaf() : nullptr;

    if (split) {
        double f = split->fraction();
        bool horizontal = (split->align() == FrameAlign::horizontal);
        YRect r1, r2;
        splitRect(rect, f, horizontal, gap, r1, r2);
        computeRec(split->firstChild(), r1, gap, borderWidth, titleHeight, out);
        computeRec(split->secondChild(), r2, gap, borderWidth, titleHeight, out);
        return;
    }

    if (leaf) {
        TilingGeometry g;
        g.frameRect = rect;
        // inner client rectangle: account for borders and titlebar
        int left = borderWidth;
        int right = borderWidth;
        int top = borderWidth + titleHeight;
        int bottom = borderWidth;
        YRect inner(rect.x() + left, rect.y() + top,
                    std::max(1, int(rect.width()) - left - right),
                    std::max(1, int(rect.height()) - top - bottom));
        g.clientRect = inner;
        out.push_back(g);
    }
}

vector<TilingGeometry>
applyTilingLayout(const FrameTree& tree, const YRect& workArea,
                  int gap, int borderWidth, int titleHeight) {
    vector<TilingGeometry> out;
    const Frame* root = tree.root();
    if (root == nullptr)
        return out;
    computeRec(root, workArea, gap > 0 ? gap : 0,
               borderWidth > 0 ? borderWidth : 0,
               titleHeight > 0 ? titleHeight : 0,
               out);
    return out;
}