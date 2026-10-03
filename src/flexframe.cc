// flexframe.cc — flexible frames for IceWM. See flexframe.h.
#include "flexframe.h"

#include <algorithm>
#include <cstdio>

#include "wmframe.h"
#include "wmmgr.h"
#include "tilinglayout.h"   // isTilingCandidate
#include "tilingmgr.h"      // Tiling::instance().flex()
#include "ypaint.h"         // Graphics
#include "ywindow.h"        // ::desktop, shapes extension
#include "yxapp.h"          // ::xapp

using std::string;
using std::vector;

// ------------------------------------------------------------------
// FlexFrame
// ------------------------------------------------------------------

FlexFrame::FlexFrame() : rect_(0, 0, 1, 1) {}

FlexFrame::FlexFrame(const YRect& r) : rect_(r) {}

bool FlexFrame::attach(YFrameWindow* frame) {
    if (frame == nullptr)
        return false;
    if (hasClient(frame))
        return true;
    clients_.push_back(frame);
    return true;
}

bool FlexFrame::detach(YFrameWindow* frame) {
    if (frame == nullptr)
        return false;
    auto it = std::find(clients_.begin(), clients_.end(), frame);
    if (it == clients_.end())
        return false;
    clients_.erase(it);
    return true;
}

void FlexFrame::clear() {
    clients_.clear();
}

bool FlexFrame::hasClient(const YFrameWindow* frame) const {
    return frame && std::find(clients_.begin(), clients_.end(), frame) !=
                        clients_.end();
}

YFrameWindow* FlexFrame::focusTarget() const {
    for (auto it = clients_.rbegin(); it != clients_.rend(); ++it) {
        YFrameWindow* frame = *it;
        if (frame &&
            (frame->isMinimized() || frame->isHidden() ||
             frame->isRollup() || frame->isFullscreen()))
            continue;
        return frame;
    }
    return nullptr;
}

// ------------------------------------------------------------------
// FlexFrameSet
// ------------------------------------------------------------------

FlexFrame* FlexFrameSet::find(const string& label) {
    auto it = frames_.find(label);
    return it == frames_.end() ? nullptr : &it->second;
}

const FlexFrame* FlexFrameSet::find(const string& label) const {
    auto it = frames_.find(label);
    return it == frames_.end() ? nullptr : &it->second;
}

FlexFrame* FlexFrameSet::add(const string& label, const YRect& r) {
    if (label.empty())
        return nullptr;
    FlexFrame* frame = find(label);
    if (frame == nullptr) {
        frame = &frames_[label];
        frame->setLabel(label);
    }
    frame->setRect(r);
    focusedLabel_ = label;
    return frame;
}

bool FlexFrameSet::remove(const string& label) {
    auto it = frames_.find(label);
    if (it == frames_.end())
        return false;
    // detach the windows first so none keeps a pointer into the
    // FlexFrame object we are about to destroy
    for (YFrameWindow* w : it->second.clients())
        if (w)
            w->setFlexFrame(nullptr);
    frames_.erase(it);
    if (focusedLabel_ == label)
        focusedLabel_.clear();
    // drop the label from every group (and their last-focused pointers)
    using std::find;
    for (auto& g : groups_) {
        auto& members = g.second;
        auto it = find(members.begin(), members.end(), label);
        if (it != members.end())
            members.erase(it);   // the one matching label only
    }
    for (auto& lf : groupLastFocused_)
        if (lf.second == label)
            lf.second.clear();
    return true;
}

bool FlexFrameSet::rename(const string& oldLabel, const string& newLabel) {
    if (oldLabel == newLabel || newLabel.empty())
        return false;
    auto it = frames_.find(oldLabel);
    if (it == frames_.end() || frames_.count(newLabel) != 0)
        return false;
    // take the frame out of the map and re-insert it under the new key
    FlexFrame frame = std::move(it->second);
    frames_.erase(it);
    frame.setLabel(newLabel);
    frames_[newLabel] = std::move(frame);
    if (focusedLabel_ == oldLabel)
        focusedLabel_ = newLabel;
    return true;
}

void FlexFrameSet::clear() {
    // detach the windows before destroying the FlexFrame objects so no
    // window keeps a dangling flexFrame() pointer
    for (auto& kv : frames_)
        for (YFrameWindow* w : kv.second.clients())
            if (w)
                w->setFlexFrame(nullptr);
    frames_.clear();
    focusedLabel_.clear();
    groups_.clear();
    groupLastFocused_.clear();
}

string FlexFrameSet::dump() const {
    string out;
    for (const auto& kv : frames_) {
        const FlexFrame& frame = kv.second;
        const YRect& r = frame.rect();
        char buf[128];
        snprintf(buf, sizeof buf, "%s %d %d %d %d\n", kv.first.c_str(),
                 int(r.x()), int(r.y()), int(r.width()), int(r.height()));
        out += buf;
    }
    return out;
}

// ------------------------------------------------------------------
// groups
// ------------------------------------------------------------------

bool FlexFrameSet::groupAdd(const string& group,
                            const vector<string>& labels) {
    using std::find;
    if (group.empty())
        return false;
    vector<string>& members = groups_[group];
    bool changed = false;
    for (const string& label : labels) {
        if (label.empty() || find(members.begin(), members.end(), label) != members.end())
            continue;
        members.push_back(label);
        changed = true;
    }
    return changed;
}

bool FlexFrameSet::groupRemove(const string& group) {
    auto it = groups_.find(group);
    if (it == groups_.end())
        return false;
    groups_.erase(it);
    groupLastFocused_.erase(group);
    return true;
}

bool FlexFrameSet::groupRename(const string& oldGroup,
                               const string& newGroup) {
    if (oldGroup == newGroup || newGroup.empty())
        return false;
    auto it = groups_.find(oldGroup);
    if (it == groups_.end() || groups_.count(newGroup) != 0)
        return false;
    // move the members and the last-focused pointer under the new name
    groups_[newGroup] = std::move(it->second);
    groups_.erase(it);
    auto lf = groupLastFocused_.find(oldGroup);
    if (lf != groupLastFocused_.end()) {
        groupLastFocused_[newGroup] = lf->second;
        groupLastFocused_.erase(lf);
    }
    return true;
}

vector<string> FlexFrameSet::groupMembers(const string& group) const {
    auto it = groups_.find(group);
    if (it == groups_.end())
        return {};
    return it->second;
}

string FlexFrameSet::groupOfFrame(const string& label) const {
    using std::find;
    for (const auto& kv : groups_)
        if (find(kv.second.begin(), kv.second.end(), label) != kv.second.end())
            return kv.first;
    return "";
}

string FlexFrameSet::focusedGroup(const string& label) const {
    // A frame may belong to several groups; prefer the one that last
    // recorded focus on it (groupNoteFocus), i.e. the group the user is
    // actually working with, over the mere "first group that contains
    // it" fallback.
    for (const auto& lf : groupLastFocused_)
        if (lf.second == label)
            return lf.first;
    return groupOfFrame(label);
}

string FlexFrameSet::groupLastFocused(const string& group) const {
    auto it = groupLastFocused_.find(group);
    if (it == groupLastFocused_.end())
        return "";
    return it->second;
}

void FlexFrameSet::groupNoteFocus(const string& group, const string& label) {
    using std::find;
    auto git = groups_.find(group);
    if (git == groups_.end())
        return;
    // only remember members that are still present
    if (find(git->second.begin(), git->second.end(), label) == git->second.end())
        return;
    // the member order stays as added; the "last focused" pointer is
    // tracked separately (a separate MRU reordering would break the
    // cycling of `flex group focus`)
    groupLastFocused_[group] = label;
}

void FlexFrameSet::noteFocusInGroups(const string& label) {
    for (const auto& kv : groups_)
        groupNoteFocus(kv.first, label);
}

string FlexFrameSet::groupDump() const {
    string out;
    for (const auto& kv : groups_) {
        out += kv.first;
        for (const string& label : kv.second) {
            out += ' ';
            out += label;
        }
        out += '\n';
    }
    return out;
}

// ------------------------------------------------------------------
// FlexApplyGuard
// ------------------------------------------------------------------

static bool g_flexApplying = false;

FlexApplyGuard::FlexApplyGuard() { g_flexApplying = true; }
FlexApplyGuard::~FlexApplyGuard() { g_flexApplying = false; }
bool FlexApplyGuard::active() { return g_flexApplying; }

// ------------------------------------------------------------------
// candidate / helpers
// ------------------------------------------------------------------

bool isFlexCandidate(const YFrameWindow* frame) {
    return isTilingCandidate(frame);
}

int flexGap() {
    return 8;
}

FlexFrameSet* flexFrames(YWindowManager* manager, int workspace) {
    if (manager == nullptr)
        return nullptr;
    return Tiling::instance().flex(workspace);
}

// ------------------------------------------------------------------
// layout
// ------------------------------------------------------------------

void flexApplyLayout(YWindowManager* manager, int workspace) {
    if (manager == nullptr || FlexApplyGuard::active())
        return;
    FlexFrameSet* set = flexFrames(manager, workspace);
    if (set == nullptr || set->empty())
        return;

    FlexApplyGuard guard;
    const int gap = flexGap();
    for (auto& kv : *set) {
        FlexFrame& frame = kv.second;
        const YRect r = frame.rect();
        if (int(r.width()) - 2 * gap < 1 || int(r.height()) - 2 * gap < 1)
            continue; // degenerate frame; do not shrink into oblivion
        for (YFrameWindow* f : frame.clients()) {
            if (f == nullptr)
                continue;
            if (!isFlexCandidate(f) || !f->visibleOn(workspace))
                continue;
            f->applyTilingGeometry(YRect(r.x() + gap, r.y() + gap,
                                         int(r.width()) - 2 * gap,
                                         int(r.height()) - 2 * gap));
        }
    }
}

// ------------------------------------------------------------------
// frame highlight (visual focus feedback)
// ------------------------------------------------------------------

/*! A persistent overlay window that outlines the currently focused
 * flexible frame. The outline is drawn in a dedicated InputOutput
 * window raised above the desktop, so repaints of the windows
 * underneath do not erase it. Unlike the original short-lived flash it
 * stays on screen while the frame keeps focus; it is only resized or
 * hidden when focus moves or the focused window goes fullscreen.
 */
class FlexHighlightWindow : public YWindow {
public:
    FlexHighlightWindow(const YRect& rect);

    void paint(Graphics& g, const YRect& r) override;

    /*! Restrict the window's visible area (bounding shape) to exactly
     * the outline rectangle (the frame rect + pen margin) plus the pen
     * width, so that X does not paint our background over the rest of
     * the window: the margin band and the interior stay see-through.
     * After this, background paints only matter within the shape, and
     * they are covered by paint()'s outline anyway. Called from show()
     * whenever the window is (re)shaped. */
    void layoutShape();

    //! the focused frame's rectangle (this window just adds the margin)
    YRect fFrameRect;
};

FlexHighlightWindow::FlexHighlightWindow(const YRect& rect) : YWindow(nullptr) {
    setStyle(wsOverrideRedirect | wsSaveUnder);
    setTitle("flex-frame-highlight");
    // the overlay only ever paints its outline stroke; everything else
    // must stay see-through (X otherwise fills it with our black
    // background, which would cover the windows underneath with a black
    // rim around the frame). The clear/shape logic then restricts the
    // visible area to the outline itself.
    setParentRelative();
    // start with a size of zero; YWindow skips XMoveResizeWindow for a
    // null geometry, so this merely creates a (through) unmapped window
    setGeometry(YRect(0, 0, 0, 0));
    fFrameRect = rect;
}

void FlexHighlightWindow::paint(Graphics& g, const YRect& /*r*/) {
    const int pen = flexHighlightPen();
    const YColor col = flexHighlightColor();
    // Outline the frame, drawn one pixel inside its edge so a pen
    // larger than 1 is fully on-screen: the line path lies on the pixel
    // grid [1..w-2] and its pen-width both halves stay within the
    // overlay window (which has extra margin, see flexHighlightGeo).
    const int w = int(fFrameRect.width());
    const int h = int(fFrameRect.height());
    g.setColor(col);
    g.setLineWidth(pen);
    g.drawRect(1, 1, w - 2, h - 2);
}

void FlexHighlightWindow::layoutShape() {
#ifdef CONFIG_SHAPE
    if (!shapes.supported)
        return;
    // The visible pixels are the outline stroke only. Build the bounding
    // shape as a RING: the rectangle the stroke is drawn into (frame
    // rect + pen margin), hollowed out in the middle. Without the inner
    // hole the shape would be a filled rect covering the whole frame,
    // and the window's (black, parent-relative) background would still
    // paint the interior over the windows beneath.
    const int pen = flexHighlightPen();
    // the drawn stroke runs a pixel inside the frame edge (paint() uses
    // offset 1) and for pen > 1 its width straddles the path, extending
    // one pixel outward past the origin; the ring therefore starts at
    // -1 so that half a pixel beyond 0 is still inside the shape
    const int L = -1;                                   // stroke inset
    const int w = int(fFrameRect.width());
    const int h = int(fFrameRect.height());
    const int t = pen + 2;                              // stroke thickness (+2 fudge)
    const int R = w - pen / 2 - 1;
    const int B = h - pen / 2 - 1;
    XRectangle ring[4] = {
        { short(L), short(L),   static_cast<unsigned short>(w), static_cast<unsigned short>(t) }, // top
        { short(L), short(B),   static_cast<unsigned short>(w), static_cast<unsigned short>(t) }, // bottom
        { short(L), short(L),   static_cast<unsigned short>(t), static_cast<unsigned short>(h) }, // left
        { short(R), short(L),   static_cast<unsigned short>(t), static_cast<unsigned short>(h) }, // right
    };
    XShapeCombineRectangles(xapp->display(), handle(),
                            ShapeBounding, 0, 0, ring, 4,
                            ShapeSet, Unsorted);
#endif
}

/*! The currently visible highlight overlay (one per process). It stays
 * created for the lifetime of the session; its size is zero while no
 * flexible frame is focused, and it follows the focused frame's
 * rectangle while there is one. The pen thickness and color are
 * runtime-adjustable through IPC.
 */
class FlexHighlight {
public:
    FlexHighlight() = default;
    ~FlexHighlight() = default;

    /*! Resize (and if needed, map, raise, and repaint) the overlay to
     * outline the given frame rectangle. A null rectangle resizes the
     * overlay to zero size so nothing is drawn. 'manager' provides the
     * work area used to clamp the overlay to the visible screen.
     */
    void show(YWindowManager* manager, const YRect& rect);

    /*! Repaint the currently mapped overlay (after the pen or color
     * changed through IPC). */
    void redraw();

private:
    YRect fRect;
    YRect fGeo;
    FlexHighlightWindow* fWin = nullptr;
};

/*! Geometry covering the frame rectangle plus the pen margin (with a
 * little extra headroom so a thick outline is not clipped by the
 * overlay edge). The overlay may extend beyond the screen edges when
 * the frame touches one (its origin goes negative); the drawing inset
 * (paint() uses pen/2) then places the top/left strokes off-screen —
 * see the paint() contract below, which clamps to the work area. */
static YRect flexHighlightGeo(const YRect& rect, int pen) {
    int m = pen;
    // keep a minimum margin so the window decoration (resize handles,
    // border) never overlaps the outline even at pen 1
    if (m < 8)
        m = 8;
    // the outline is drawn at offset 1 in the overlay, one pixel inside
    // the frame edge; add one extra pixel of margin so that a >1px pen
    // straddling the path is not clipped by the window edge
    m += 1;
    return YRect(rect.x() - m, rect.y() - m,
                 rect.width() + 2 * m, rect.height() + 2 * m);
}

/*! Clamp the given overlay rectangle to the screen work area (the
 * desktop minus panels), keeping size. The overlay window may not be
 * larger than the screen's visible area, otherwise its (transparent)
 * margin band would sit beyond the display — the very top/left band is
 * what used to paint black over the desktop. Never enlarges. */
static void flexClampToWorkArea(YWindowManager* manager, YRect& geo) {
    int mx, my, Mx, My;
    if (manager)
        manager->getWorkArea(&mx, &my, &Mx, &My,
                             manager->activeWorkspace());
    else {
        mx = 0; my = 0;
        Mx = xapp->displayWidth();
        My = xapp->displayHeight();
    }
    // Keep the overlay inside the visible area (the screen or the work
    // area, whichever the manager reports): a too-large window may not
    // extend past the display. The origin is left alone: a frame sitting
    // within one margin of the edge will simply have part of its outline
    // clipped at the screen border (the paint stroke still lands on the
    // visible pixels), which is far better than shifting the whole
    // outline away from the frame, as an arbitrary +1 push would.
    // Note: the comment about negative origins painting black applied to
    // the pre-transparent era; the overlay is parent-relative now.
    if (int(geo.right()) > Mx)
        geo.ww = unsigned(std::max(1, Mx - geo.x()));
    if (int(geo.bottom()) > My)
        geo.hh = unsigned(std::max(1, My - geo.y()));
}

void FlexHighlight::show(YWindowManager* manager, const YRect& rect) {
    // fast path: no frame to outline and the overlay does not exist yet.
    // This is the normal case on every focus change while the user has
    // no flexible frames, and it must not touch X (no window creation,
    // no shape/geometry/paint round-trips).
    if (fWin == nullptr && !rect.nonempty())
        return;
    const int pen = flexHighlightPen();
    YRect frameRect = rect;   // mutable copy: the clamp may shift it
    if (fWin == nullptr)
        fWin = new FlexHighlightWindow(frameRect);
    if (!rect.nonempty()) {
        // no frame to outline: shrink to zero, empty the shape (so the
        // window becomes nonexistent on screen) and hide. setGeometry
        // with a null rect skips the X call, so force it via hide;
        // repaint afterwards makes the (possibly) visible outline go
        // away even without an event.
        fRect = YRect();
        fGeo = YRect();
        if (fWin->visible() || fWin->width() != 0 || fWin->height() != 0) {
            fWin->layoutShape();
            fWin->setGeometry(YRect(0, 0, 0, 0));
            fWin->hide();
            fWin->repaint();
        }
        return;
    }
    // the overlay rectangle depends on the pen: when only the pen
    // changed (same focused frame), the geometry still has to be
    // recomputed so a thicker outline is not clipped
    YRect geo = flexHighlightGeo(frameRect, pen);
    // clamp the overlay size to the work area; the origin is untouched
    // (see flexClampToWorkArea) so frames near the screen edge keep
    // their outline aligned with the frame
    flexClampToWorkArea(manager, geo);
    fRect = frameRect;
    if (geo != fGeo || !fWin->visible()) {
        fGeo = geo;
        fWin->fFrameRect = frameRect;
        fWin->setGeometry(geo);
        // map FIRST, then raise: raising an unmapped window is a no-op
        // and a freshly mapped window lands at the bottom of the stack
        fWin->layoutShape();
        fWin->show();
        fWin->raise();
        // Expose events are not delivered for plain YWindows
        // (handleExpose is a no-op), so paint the outline explicitly.
        fWin->paintExpose(0, 0, fWin->width(), fWin->height());
    }
    // else: same geometry, still visible — nothing to do. The outline
    // already shows the right rectangle (fFrameRect is unchanged), so
    // a redundant repaint on every focus change inside one frame is
    // avoided.
}

void FlexHighlight::redraw() {
    if (fWin == nullptr)
        return;
    // paintExpose directly invokes paint(); repaint() would queue an
    // Expose that never arrives for plain YWindows.
    fWin->paintExpose(0, 0, fWin->width(), fWin->height());
}

/*! A single, process-wide highlight instance. */
static FlexHighlight& flexHighlight() {
    static FlexHighlight hl;
    return hl;
}

/*! Defaults and current IPC-adjustable outline settings. The theme
 * breadcrumb (FlexHighlightPen/FlexHighlightColor) is applied at
 * startup; these values are the fallback when no theme sets them. */
static unsigned int sPen = 1;
static int sColorR = 0xFF, sColorG = 0x80, sColorB = 0x00;

int flexHighlightPen() {
    return int(sPen);
}

YColor flexHighlightColor() {
    return YColor(sColorR & 0xFF, sColorG & 0xFF, sColorB & 0xFF);
}

bool flexSetHighlightPen(int pen) {
    if (pen < 0 || pen > 255)
        return false;
    sPen = unsigned(pen);
    return true;
}

bool flexSetHighlightColor(int r, int g, int b) {
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
        return false;
    sColorR = r;
    sColorG = g;
    sColorB = b;
    return true;
}

/*! Update the overlay to outline the flexible frame of a focused
 * window (if it participates in flexible frames and is not fullscreen),
 * or hide it. Called whenever focus changes. The window that ended up
 * focused is passed explicitly because getFocus() is not reliable for
 * windows without an input-focus hint. The outline tracks the FRAME's
 * rectangle (not the window's), since the frame coordinates are what
 * the user configured and remain stable while the window bounces
 * around during activation.
 */
void flexUpdateHighlight(YWindowManager* manager, YFrameWindow* focus) {
    if (manager == nullptr)
        return;
    if (focus == nullptr || !focus->visible() || focus->isFullscreen() ||
        !isFlexCandidate(focus)) {
        flexHighlight().show(manager, YRect());   // empty rect -> hide
        return;
    }
    FlexFrame* frame = focus->flexFrame();
    if (frame == nullptr)
        flexHighlight().show(manager, YRect());
    else
        flexHighlight().show(manager, frame->rect());
}

/*! Explicitly outline the given flexible frame rectangle (used by the
 * IPC focus commands, which know the frame they just focused). */
void flexFrameHighlight(YWindowManager* manager, const YRect& rect) {
    flexHighlight().show(manager, rect);
}

/*! Repaint the visible outline with the current thickness/color. Used
 * by the `flex highlight pen/color` IPC commands when the focused frame
 * has not moved: show() would otherwise take the early-return repaint
 * path, and plain YWindows do not receive Expose events here, so the
 * outline must be repainted explicitly.
 */
void flexHighlightRedraw() {
    flexHighlight().redraw();
}

// ------------------------------------------------------------------
// frame binding (called from manage / workspace switch)
// ------------------------------------------------------------------

void flexUnbindFrame(YFrameWindow* frame) {
    if (frame == nullptr)
        return;
    frame->setFlexFrame(nullptr); // detaches from the frame set's clients
}

/*! Pick the flexible frame a fresh window of 'workspace' should attach
 * to. Priority:
 *  1. the frame of the window focused before this one ('priorFocus',
 *     when it is bound to a frame),
 *  2. the last focused frame of the set (see setFocusedLabel),
 *  3. any single frame (the old "only one frame" behaviour).
 * Returns nullptr when nothing sensible exists.
 */
static FlexFrame* flexTarget(YWindowManager* manager, int workspace,
                             YFrameWindow* priorFocus) {
    FlexFrameSet* set = flexFrames(manager, workspace);
    if (set == nullptr || set->empty())
        return nullptr;

    // 1. follow the previously focused window's frame
    if (priorFocus && priorFocus->flexFrame() != nullptr)
        return priorFocus->flexFrame();

    // 2. follow the last focused frame (flex focus / flex add)
    const string& focused = set->focusedLabel();
    if (!focused.empty()) {
        FlexFrame* f = set->find(focused);
        if (f != nullptr)
            return f;
    }

    // 3. a single frame
    if (set->count() == 1)
        return &set->begin()->second;

    return nullptr;
}

void flexBindFrame(YWindowManager* manager, YFrameWindow* frame,
                   YFrameWindow* priorFocus) {
    if (frame == nullptr || FlexApplyGuard::active())
        return;
    if (frame->flexFrame() != nullptr)
        return; // already bound
    FlexFrame* target = flexTarget(manager, frame->getWorkspace(), priorFocus);
    if (target == nullptr)
        return;
    frame->setFlexFrame(target);
    flexApplyLayout(manager, frame->getWorkspace());
}