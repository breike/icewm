// tilingmgr.h — glue between IceWM and the herbstluftwm-style
// FrameTree: binds YFrameWindows to leaves, applies layout, exposes
// workspace-local operations used by actions and icesh.
//
// Author: tiling port (herbstluftwm model) for IceWM.

#ifndef ICEWM_TILINGMGR_H
#define ICEWM_TILINGMGR_H

#include <map>
#include <string>

#include "frametree.h"
#include "yrect.h"

class YFrameWindow;
class YWindowManager;
class FlexFrameSet;

/*! Per-workspace tiling state: the frame tree plus a flag whether
 * tiling is enabled for this workspace.
 */
class TilingWorkspace {
public:
    TilingWorkspace();
    ~TilingWorkspace();

    bool enabled = false;
    FrameTree tree;

    //! Bind the given frame into the tree (inserts into focused leaf
    //! if it isn't already bound). Returns true if it was bound.
    bool bind(YFrameWindow* frame);

    //! Detach the frame from the tree (e.g. floating/fullscreen or
    //! unmanage). Returns true if it was bound before.
    bool unbind(YFrameWindow* frame);

    //! True if 'frame' is a member of this workspace's tree.
    bool contains(const YFrameWindow* frame) const;

    //! The leaf that currently has focus in the tree.
    FrameLeaf* focusedLeaf() { return tree.focusedLeaf(); }

private:
    TilingWorkspace(const TilingWorkspace&) = delete;
    TilingWorkspace& operator=(const TilingWorkspace&) = delete;
};

/*! Tiny RAII helper used to suspend re-entrant layout during apply. */
class TilingApplyGuard {
public:
    TilingApplyGuard();
    ~TilingApplyGuard();
    static bool active();
};

/*! The central registry: keeps one TilingWorkspace per workspace
 * index, lazily created and destroyed with workspaces.
 */
class Tiling {
public:
    static Tiling& instance();

    TilingWorkspace* workspace(int index);
    const TilingWorkspace* workspace(int index) const;

    //! ensure a workspace entry exists
    TilingWorkspace* ensure(int index);

    //! forget the entry for a removed workspace
    void forget(int index);

    //! the flexible frames of a workspace (const overload for the
    //! tree-tiling side). Null if the workspace has none yet.
    FlexFrameSet* flex(int index);
    const FlexFrameSet* flex(int index) const;

    //! ensure the flexible-frame set of a workspace exists
    FlexFrameSet* ensureFlex(int index);

private:
    Tiling() = default;
    std::map<int, TilingWorkspace> byWorkspace_;
    std::map<int, FlexFrameSet> flexByWorkspace_;
};

/*! Apply the tiling layout for the given workspace using the frame
 * tree of the active workspace (the frame count / titles etc.).
 * Recomputes all leaf geometries and moves the bound windows.
 */
void tilingApplyLayout(YWindowManager* manager, int workspace);

/*! (Re)bind all manageable windows of a workspace into the tree and
 * apply the layout. Called when tiling is enabled for a workspace.
 */
void tilingEnableWorkspace(YWindowManager* manager, int workspace);

/*! Unbind all windows of a workspace (they return to free placement).
 */
void tilingDisableWorkspace(YWindowManager* manager, int workspace);

/*! Bind a single frame into the tiling tree of the workspace it
 * belongs to, if that workspace has tiling enabled. Called when a
 * window is managed.
 */
void tilingBindFrame(YWindowManager* manager, YFrameWindow* frame);

/*! Toggle tiling for the active workspace (enables and rebinds all
 * current windows, or disables and restores free placement).
 */
void tilingToggleActiveWorkspace(YWindowManager* manager);

/*! Detach a single frame from the tiling tree of the workspace it
 * belongs to. Called when a window is unmanaged.
 */
void tilingUnbindFrame(YWindowManager* manager, YFrameWindow* frame);

// --- commands (called from actions / icesh) -----------------------

//! split the focused leaf of the active workspace; 'dir' is "v"/"h".
//! Returns false on failure.
bool tilingSplit(YWindowManager* manager, const std::string& dir,
                 double fraction);

//! remove the focused leaf of the active workspace.
bool tilingRemove(YWindowManager* manager);

//! focus the leaf addressed by an index string ("" = root, "0","1",...).
//! Returns false if path invalid.
bool tilingFocusIndex(YWindowManager* manager, const std::string& index);

//! move focus to a leaf by its optional label.
bool tilingFocusLabel(YWindowManager* manager, const std::string& label);

//! set/clear the label of the focused leaf ("newLabel" empty = clear).
bool tilingSetLabel(YWindowManager* manager, const std::string& label);

//! serialize the active workspace's tree (S-expression, no clients).
std::string tilingDump(YWindowManager* manager);

//! deserialize into the active workspace's tree and re-apply.
bool tilingLoad(YWindowManager* manager, const std::string& layout);

/*! Handle an IPC request received via the _ICEWM_TILING property.
 * 'request' contains NUL-separated tokens, first the subcommand
 * ("split", "remove", "focus", "focuslabel", "setlabel", "dump",
 * "load", "toggle"). Executes against the active workspace.
 */
void tilingHandleRequest(YWindowManager* manager, const char* request,
                         size_t len);

/*! Process any pending _ICEWM_TILING request property; if there is
 * none (icesh only sent the _ICEWM_ACTION_TILING signal), toggle
 * tiling on the active workspace. Called from the action handler.
 */
void tilingAction(YWindowManager* manager);

// --- flexible-frame commands (driven through the same IPC channel) --

//! `flex add`: replace the active workspace's flexible frames with a
//! single frame at the given rect (label = tokens[start..]), bind the
//! focused window to it and apply.
bool flexAdd(YWindowManager* manager, const std::vector<std::string>& tokens,
             size_t start);

//! `flex remove <label>`: detach and delete the frame.
bool flexRemove(YWindowManager* manager, const std::string& label);

//! `flex focus <label>`: focus the frame's focusable window.
bool flexFocus(YWindowManager* manager, const std::string& label);

//! `flex setlabel <old> <new>`: rename a frame.
bool flexSetLabel(YWindowManager* manager, const std::string& oldLabel,
                  const std::string& newLabel);

//! `flex dump`: serialize the active workspace's frames.
std::string flexDump(YWindowManager* manager);

//! `flex clear`: remove all flexible frames of the active workspace.
bool flexClear(YWindowManager* manager);

//! `flex resize <label> <dw> <dh> [dx [dy]]`: resize (and optionally
//! shift) a frame by the given relative deltas. A label of "." or an
//! empty label means the focused frame.
bool flexResize(YWindowManager* manager, const std::string& label,
                int dw, int dh, int dx, int dy);

//! `flex move <label> <dx> <dy>`: shift a frame by the given relative
//! deltas without changing its size. A label of "." or an empty label
//! means the focused frame.
bool flexMove(YWindowManager* manager, const std::string& label,
              int dx, int dy);

//! `flex bind <label>`: bind the focused window to the frame, moving
//! it there even if it already lives in another frame (its group
//! membership is not touched). A label of "." or an empty label means
//! the focused frame (a no-op).
bool flexBind(YWindowManager* manager, const std::string& label);

// --- flexible-frame groups (multiple frames arranged into named
// --- groups; opening a group activates its most recently focused frame)

//! `flex group add <name> <labels...>`: add the frames to the group.
bool flexGroupAdd(YWindowManager* manager, const std::string& group,
                  const std::vector<std::string>& labels);

//! `flex group open <name>`: activate the group's last focused frame
//! (or its first focusable member when none was focused yet).
bool flexGroupOpen(YWindowManager* manager, const std::string& group);

//! `flex group focus [next|prev] <name>`: move focus to the group's
//! next (or previous) member, wrapping around.
bool flexGroupFocus(YWindowManager* manager, const std::string& group,
                    bool forward = true);

//! `flex group focus [next|prev]` (no name): cycle among all frames of
//! the workspace.
bool flexGroupFocusAll(YWindowManager* manager, bool forward = true);

//! `flex window [next|prev] <label>`: activate the next (or previous)
//! window bound to one flexible frame, wrapping around. Without a label
//! the frame of the currently focused window is used. The activated
//! window is raised, focused, and sized to the frame's current rect.
bool flexWindowFocus(YWindowManager* manager, const std::string& label,
                     bool forward = true);

//! `flex group remove <name>`: delete the group (frames are kept).
bool flexGroupRemove(YWindowManager* manager, const std::string& group);

//! `flex group dump`: serialize the workspace's groups.
std::string flexGroupDump(YWindowManager* manager);

#endif // ICEWM_TILINGMGR_H