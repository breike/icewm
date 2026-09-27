// flexframe.cc — flexible frames for IceWM. See flexframe.h.
#include "flexframe.h"

#include <algorithm>
#include <cstdio>

#include "wmframe.h"
#include "wmmgr.h"
#include "tilinglayout.h"   // isTilingCandidate
#include "tilingmgr.h"      // Tiling::instance().flex()
#include "ypaint.h"         // Graphics
#include "ywindow.h"        // ::desktop

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

vector<string> FlexFrameSet::groupMembers(const string& group) const {
    auto it = groups_.find(group);
    if (it == groups_.end())
        return {};
    return it->second;
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

    //! the focused frame's rectangle (this window just adds the margin)
    YRect fFrameRect;
};

FlexHighlightWindow::FlexHighlightWindow(const YRect& rect) : YWindow(nullptr) {
    setStyle(wsOverrideRedirect | wsSaveUnder);
    setTitle("flex-frame-highlight");
    // start with a size of zero; YWindow skips XMoveResizeWindow for a
    // null geometry, so this merely creates a (through) unmapped window
    setGeometry(YRect(0, 0, 0, 0));
    fFrameRect = rect;
}

void FlexHighlightWindow::paint(Graphics& g, const YRect& /*r*/) {
    const int pen = flexHighlightPen();
    const YColor col = flexHighlightColor();
    // outline the frame, drawn slightly inside the overlay so a thick
    // pen stays inside and lines are not clipped
    g.setColor(col);
    g.setLineWidth(pen);
    g.drawRect(pen / 2, pen / 2, fFrameRect.width() - pen,
               fFrameRect.height() - pen);
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
     * overlay to zero size so nothing is drawn.
     */
    void show(const YRect& rect);

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
 * overlay edge). */
static YRect flexHighlightGeo(const YRect& rect, int pen) {
    int m = pen;
    // keep a minimum margin so the window decoration (resize handles,
    // border) never overlaps the outline even at pen 1
    if (m < 8)
        m = 8;
    return YRect(rect.x() - m, rect.y() - m,
                 rect.width() + 2 * m, rect.height() + 2 * m);
}

void FlexHighlight::show(const YRect& rect) {
    const int pen = flexHighlightPen();
    if (fWin == nullptr)
        fWin = new FlexHighlightWindow(rect);
    if (!rect.nonempty()) {
        // no frame to outline: shrink to zero and hide. setGeometry
        // with a null rect skips the X call, so force it via hide;
        // repaint afterwards makes the (possibly) visible outline go
        // away even without an event.
        fRect = YRect();
        fGeo = YRect();
        fWin->setGeometry(YRect(0, 0, 0, 0));
        fWin->hide();
        fWin->repaint();
        return;
    }
    // the overlay rectangle depends on the pen: when only the pen
    // changed (same focused frame), the geometry still has to be
    // recomputed so a thicker outline is not clipped
    const YRect geo = flexHighlightGeo(rect, pen);
    fRect = rect;
    if (geo != fGeo || !fWin->visible()) {
        fGeo = geo;
        fWin->fFrameRect = rect;
        fWin->setGeometry(geo);
        // map FIRST, then raise: raising an unmapped window is a no-op
        // and a freshly mapped window lands at the bottom of the stack
        fWin->show();
        fWin->raise();
    } else {
        fWin->fFrameRect = rect;
    }
    // Expose events are not delivered for plain YWindows (handleExpose
    // is a no-op), so paint the outline explicitly.
    fWin->paintExpose(0, 0, fWin->width(), fWin->height());
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

/*! Defaults and current IPC-adjustable outline settings. */
static unsigned int sPen = 3;
static int sColorR = 0x30, sColorG = 0xFF, sColorB = 0x60;

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
        flexHighlight().show(YRect());   // empty rect -> hide
        return;
    }
    FlexFrame* frame = focus->flexFrame();
    if (frame == nullptr)
        flexHighlight().show(YRect());
    else
        flexHighlight().show(frame->rect());
}

/*! Explicitly outline the given flexible frame rectangle (used by the
 * IPC focus commands, which know the frame they just focused). */
void flexFrameHighlight(YWindowManager* manager, const YRect& rect) {
    (void)manager;
    flexHighlight().show(rect);
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