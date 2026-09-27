// flexframe.h — "flexible frames" for IceWM: named rectangles that can
// be created directly through IPC and to which windows are bound. Unlike
// the FrameTree (binary tiling), flexible frames are defined by explicit
// size and position only; they may overlap and intersect arbitrarily.
//
// Each workspace owns a FlexFrameSet: a map of label -> FlexFrame. A
// FlexFrame holds a rectangle plus the windows currently bound to it.
// Windows bound to a flexible frame are resized to the frame rectangle.
//
// Author: flexible-frame port for IceWM.

#ifndef ICEWM_FLEXFRAME_H
#define ICEWM_FLEXFRAME_H

#include <map>
#include <string>
#include <vector>

#include "base.h"      // PRECONDITION
#include "yfull.h"     // X11 types used by yrect.h
#include "ypaint.h"    // YColor
#include "yrect.h"
#include "ytimer.h"    // YTimerListener

class YFrameWindow;
class YWindowManager;
class Tiling;           // for Tiling::instance() (flex storage)

/*! One flexible frame: an explicit rectangle plus an optional label
 * (the map key, used to address it via IPC) and the bound windows.
 * Windows may belong to at most one flexible frame.
 */
class FlexFrame {
public:
    FlexFrame();
    explicit FlexFrame(const YRect& r);

    const YRect& rect() const { return rect_; }
    void setRect(const YRect& r) { rect_ = r; }

    const std::string& label() const { return label_; }
    void setLabel(const std::string& l) { label_ = l; }

    // --- bound windows ---------------------------------------------
    //! attach a window; no-op if already attached. Returns true if the
    //! frame now contains it.
    bool attach(YFrameWindow* frame);
    //! detach a window; no-op if not attached. Returns true if removed.
    bool detach(YFrameWindow* frame);
    //! detach every window (used on frame removal / workspace teardown).
    void clear();

    const std::vector<YFrameWindow*>& clients() const { return clients_; }
    size_t clientCount() const { return clients_.size(); }
    bool isEmpty() const { return clients_.empty(); }
    bool hasClient(const YFrameWindow* frame) const;

    //! the preferred window to focus: the last attached, non-minimized
    //! one that is visible on the workspace.
    YFrameWindow* focusTarget() const;

private:
    YRect rect_;
    std::string label_;
    std::vector<YFrameWindow*> clients_;
};

/*! The set of flexible frames of one workspace, ordered by label. */
class FlexFrameSet {
public:
    using Map = std::map<std::string, FlexFrame>;
    using iterator = Map::iterator;
    using const_iterator = Map::const_iterator;

    //! number of frames
    size_t count() const { return frames_.size(); }
    bool empty() const { return frames_.empty(); }

    iterator begin() { return frames_.begin(); }
    iterator end() { return frames_.end(); }
    const_iterator begin() const { return frames_.begin(); }
    const_iterator end() const { return frames_.end(); }

    //! find a frame by label; nullptr if none.
    FlexFrame* find(const std::string& label);
    const FlexFrame* find(const std::string& label) const;

    //! add (or update the rect of) the frame with this label.
    FlexFrame* add(const std::string& label, const YRect& r);

    //! remove the frame (detaching its windows). Returns true if it existed.
    bool remove(const std::string& label);

    //! rename a frame, keeping its windows. Returns true if 'oldLabel'
    //! existed and 'newLabel' is a fresh non-empty name.
    bool rename(const std::string& oldLabel, const std::string& newLabel);

    //! remove every frame (avoids a policy on which window to keep).
    void clear();

    //! the label focused most recently (empty if none).
    const std::string& focusedLabel() const { return focusedLabel_; }
    void setFocusedLabel(const std::string& l) { focusedLabel_ = l; }

    //! serialize on one line per frame: "label x y w h".
    std::string dump() const;

    // --- groups -----------------------------------------------------
    //! A group is a named list of frame labels. Groups exist purely as
    //! IPC-level organization on top of the frames; the frames continue
    //! to live independently, and a group may reference frames that were
    //! created or removed after the group. Opening a group brings up the
    //! frame of the group that was focused most recently.

    //! add 'labels' to the group 'group' (creating it if needed).
    //! Returns true if anything changed.
    bool groupAdd(const std::string& group, const std::vector<std::string>& labels);
    //! remove the group (the frames themselves are kept). Returns true if
    //! it existed.
    bool groupRemove(const std::string& group);
    //! the group's member labels in list order.
    std::vector<std::string> groupMembers(const std::string& group) const;
    //! the label of the group's member that was focused most recently,
    //! or empty if the group has no members / none was focused.
    std::string groupLastFocused(const std::string& group) const;
    //! record that the frame 'label' (in group 'group') received focus.
    void groupNoteFocus(const std::string& group, const std::string& label);
    //! record that the frame 'label' received focus, updating the
    //! last-focused pointer of every group that contains it.
    void noteFocusInGroups(const std::string& label);
    //! serialize on one line per group: "group label1 label2 ...".
    std::string groupDump() const;

private:
    Map frames_;
    std::string focusedLabel_;
    std::map<std::string, std::vector<std::string>> groups_;   // group -> member labels
    std::map<std::string, std::string> groupLastFocused_;      // group -> last focused label
};

/*! RAII helper used to suspend re-entrant layout while flexible frames
 * are being applied (mirrors TilingApplyGuard).
 */
class FlexApplyGuard {
public:
    FlexApplyGuard();
    ~FlexApplyGuard();
    static bool active();
};

/*! The gap between a flexible frame's rectangle and the windows inside
 * it. Mirrors the tiling gap so both systems place windows identically.
 */
int flexGap();

/*! The FlexFrameSet of the given workspace, or null if that workspace
 * has no flexible frames yet.
 */
FlexFrameSet* flexFrames(class YWindowManager* manager, int workspace);

/*! Set the outline the focused flexible frame is drawn with: thickness
 * in pixels (default 3) and RGB color (default (0x30, 0xFF, 0x60)).
 * The outline stays visible until focus moves to another frame or the
 * focused window goes fullscreen. Returns false if the string was not
 * a valid unsigned integer in 0..255.
 */
bool flexSetHighlightPen(int pen);
bool flexSetHighlightColor(int r, int g, int b);

/*! The current outline thickness (pixels) and color used to draw the
 * focused-frame highlight. Read back by the overlay's paint().
 */
int flexHighlightPen();
YColor flexHighlightColor();

/*! A persistent overlay outline around the rectangle of the currently
 * focused flexible frame. It is redrawn on focus changes and while a
 * flexible frame is focused, and removed while the focused window is
 * fullscreen. To avoid circular dependencies this is driven from
 * YWindowManager (which observes all focus changes); the low-level
 * rectangle updater is exposed here. The window that ended up focused
 * is passed explicitly because YWindowManager::getFocus() is only
 * updated for some windows (e.g. those with an input-focus hint).
 */
void flexUpdateHighlight(class YWindowManager* manager,
                         class YFrameWindow* frame);
void flexFrameHighlight(class YWindowManager* manager, const YRect& rect);

/*! Repaint the visible outline after the pen/color changed through IPC
 * (the focused frame has not moved, so no show() is triggered). */
void flexHighlightRedraw();

/*! True if the window should be subject to flexible frames at all.
 * Reuses the tiling candidate predicate, so a window never participates
 * in both systems at once (we do not bind it anywhere else, though).
 */
bool isFlexCandidate(const YFrameWindow* frame);

/*! Apply the rectangles of all flexible frames of the given workspace:
 * each bound, still-valid window is resized to its frame's rectangle.
 * No-op when there are no frames. Soft-errored only.
 */
void flexApplyLayout(class YWindowManager* manager, int workspace);

/*! (Re)attach 'frame' to a flexible frame of its workspace. 'frame'
 * binds to the frame of the window 'priorFocus' (the window focused
 * before this one was managed), falling back to the last focused
 * frame of the set, then to any single frame. Called at the end of
 * manageClient once the window has been placed.
 */
void flexBindFrame(class YWindowManager* manager, class YFrameWindow* frame,
                   class YFrameWindow* priorFocus);

/*! Detach 'frame' from its flexible frame (if any). Called when a
 * window is unmanaged; no layout is applied.
 */
void flexUnbindFrame(class YFrameWindow* frame);

#endif // ICEWM_FLEXFRAME_H