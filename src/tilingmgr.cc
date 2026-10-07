// tilingmgr.cc — glue between IceWM and the herbstluftwm-style
// FrameTree. See tilingmgr.h.
#include "tilingmgr.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "wmframe.h"
#include "wmmgr.h"
#include "yxapp.h"
#include "tilinglayout.h"
#include "flexframe.h"

using std::string;
using std::vector;

// ------------------------------------------------------------------
// TilingWorkspace
// ------------------------------------------------------------------

TilingWorkspace::TilingWorkspace() = default;
TilingWorkspace::~TilingWorkspace() = default;

bool TilingWorkspace::bind(YFrameWindow* frame) {
    if (frame == nullptr)
        return false;
    if (frame->frameLeaf() != nullptr)
        return true; // already bound
    if (!isTilingCandidate(frame))
        return false;
    FrameLeaf* leaf = tree.ensureRootLeaf();
    frame->setFrameLeaf(leaf);
    frame->setTilingApplied(false);
    leaf->insertClient(frame, true);
    return true;
}

bool TilingWorkspace::unbind(YFrameWindow* frame) {
    if (frame == nullptr)
        return false;
    FrameLeaf* leaf = frame->frameLeaf();
    if (leaf == nullptr)
        return false;
    leaf->removeClient(frame);
    frame->setFrameLeaf(nullptr);
    frame->setTilingApplied(false);
    return true;
}

bool TilingWorkspace::contains(const YFrameWindow* frame) const {
    return frame && frame->frameLeaf() != nullptr;
}

// ------------------------------------------------------------------
// Tiling
// ------------------------------------------------------------------

Tiling& Tiling::instance() {
    static Tiling inst;
    return inst;
}

TilingWorkspace* Tiling::ensure(int index) {
    return &byWorkspace_[index];
}

TilingWorkspace* Tiling::workspace(int index) {
    auto it = byWorkspace_.find(index);
    return it == byWorkspace_.end() ? nullptr : &it->second;
}

const TilingWorkspace* Tiling::workspace(int index) const {
    auto it = byWorkspace_.find(index);
    return it == byWorkspace_.end() ? nullptr : &it->second;
}

void Tiling::forget(int index) {
    byWorkspace_.erase(index);
    // detach the windows bound to this workspace's flexible frames
    // before the FlexFrame objects go away
    auto it = flexByWorkspace_.find(index);
    if (it != flexByWorkspace_.end()) {
        for (auto& kv : it->second)
            for (YFrameWindow* w : kv.second.clients())
                if (w)
                    w->setFlexFrame(nullptr);
        flexByWorkspace_.erase(it);
    }
}

FlexFrameSet* Tiling::flex(int index) {
    auto it = flexByWorkspace_.find(index);
    return it == flexByWorkspace_.end() ? nullptr : &it->second;
}

const FlexFrameSet* Tiling::flex(int index) const {
    auto it = flexByWorkspace_.find(index);
    return it == flexByWorkspace_.end() ? nullptr : &it->second;
}

FlexFrameSet* Tiling::ensureFlex(int index) {
    return &flexByWorkspace_[index];
}

// ------------------------------------------------------------------
// TilingApplyGuard
// ------------------------------------------------------------------

static bool g_tilingApplying = false;

TilingApplyGuard::TilingApplyGuard() { g_tilingApplying = true; }
TilingApplyGuard::~TilingApplyGuard() { g_tilingApplying = false; }
bool TilingApplyGuard::active() { return g_tilingApplying; }

// ------------------------------------------------------------------
// layout application
// ------------------------------------------------------------------

static int tilingGap() {
    // default gap; can be driven by a pref later
    return 8;
}

static int tilingBorder() {
    // tiled windows draw a slim border, no title (or a small one)
    return 1;
}

static int tilingTitle() {
    return 0; // tiled: no title bar
}

/*! The work area (screen minus panels) for the given workspace is
 * fetched from the manager's work-area table.
 */
static YRect tilingWorkArea(YWindowManager* manager, int workspace) {
    int mx, my, Mx, My;
    manager->getWorkArea(&mx, &my, &Mx, &My, workspace);
    return YRect(mx, my, std::max(1, Mx - mx), std::max(1, My - my));
}

/*! Collect all frames that should be bound to the given workspace's
 * tree (those that are tiling candidates and not already floating).
 */
static vector<YFrameWindow*> tilingFrames(YWindowManager* manager,
                                          int workspace, bool withSticky) {
    vector<YFrameWindow*> frames;
    for (YFrameIter it = manager->focusedIterator(); ++it; ) {
        YFrameWindow* f = it;
        if (f == nullptr)
            continue;
        if (f->getWorkspace() != workspace && !(withSticky && f->isSticky()))
            continue;
        if (!isTilingCandidate(f))
            continue;
        frames.push_back(f);
    }
    return frames;
}

void tilingEnableWorkspace(YWindowManager* manager, int workspace) {
    TilingWorkspace* tw = Tiling::instance().ensure(workspace);
    if (tw->enabled)
        return;
    tw->enabled = true;
    // bind all candidate frames
    for (YFrameWindow* f : tilingFrames(manager, workspace, false))
        tw->bind(f);
    tilingApplyLayout(manager, workspace);
}

void tilingDisableWorkspace(YWindowManager* manager, int workspace) {
    TilingWorkspace* tw = Tiling::instance().workspace(workspace);
    if (tw == nullptr || !tw->enabled)
        return;
    tw->enabled = false;
    // unbind all (they go back to free placement)
    for (YFrameWindow* f : tilingFrames(manager, workspace, false))
        tw->unbind(f);
    // restore the frame configuration (title/border) of the unbound
    // frames so they look normal again
    for (YFrameWindow* f : tilingFrames(manager, workspace, false)) {
        f->performLayout();
        f->setTilingApplied(false);
    }
}

void tilingApplyLayout(YWindowManager* manager, int workspace) {
    TilingWorkspace* tw = Tiling::instance().workspace(workspace);
    if (tw == nullptr || !tw->enabled)
        return;
    if (TilingApplyGuard::active())
        return; // re-entrancy guard

    TilingApplyGuard guard;
    YRect area = tilingWorkArea(manager, workspace);
    vector<TilingGeometry> geos =
        applyTilingLayout(tw->tree, area, tilingGap(), tilingBorder(),
                          tilingTitle());

    // collect leaves in pre-order to match geos order
    vector<FrameLeaf*> leaves;
    tw->tree.root()->fmap(
        [](FrameSplit*) {},
        [&](FrameLeaf* leaf) { leaves.push_back(leaf); },
        0);

    size_t n = std::min(leaves.size(), geos.size());
    for (size_t i = 0; i < n; ++i) {
        FrameLeaf* leaf = leaves[i];
        const TilingGeometry& g = geos[i];
        // Position each client inside the leaf.
        for (YFrameWindow* frame : leaf->clients()) {
            if (!isTilingCandidate(frame))
                continue;
            // keep leaf==frame->frameLeaf() invariant
            if (frame->frameLeaf() != leaf)
                frame->setFrameLeaf(leaf);
            // apply the leaf's client rectangle
            frame->setTilingApplied(true);
            if (frame->isMinimized() || frame->isHidden() ||
                frame->isRollup() || frame->isFullscreen())
                continue; // handled separately
            // Set the frame outer geometry and let layoutClient size
            // the client inside (see YFrameWindow::applyTilingGeometry).
            frame->applyTilingGeometry(g.frameRect);
        }
    }
    // (clients of leaves beyond 'n' — should not happen — are left;
    //  they would be boundary artifacts if tree and geometry diverge.)
}

// ------------------------------------------------------------------
// single-frame bind/unbind (called from manage/unmanage/fullscreen)
// ------------------------------------------------------------------

void tilingBindFrame(YWindowManager* manager, YFrameWindow* frame) {
    if (frame == nullptr || TilingApplyGuard::active())
        return;
    TilingWorkspace* tw =
        Tiling::instance().workspace(frame->getWorkspace());
    if (tw == nullptr || !tw->enabled)
        return;
    if (tw->bind(frame))
        tilingApplyLayout(manager, frame->getWorkspace());
}

void tilingToggleActiveWorkspace(YWindowManager* manager) {
    if (manager == nullptr)
        return;
    const int ws = manager->activeWorkspace();
    TilingWorkspace* tw = Tiling::instance().ensure(ws);
    if (tw->enabled)
        tilingDisableWorkspace(manager, ws);
    else
        tilingEnableWorkspace(manager, ws);
}

void tilingUnbindFrame(YWindowManager* manager, YFrameWindow* frame) {
    if (frame == nullptr || TilingApplyGuard::active())
        return;
    TilingWorkspace* tw =
        Tiling::instance().workspace(frame->getWorkspace());
    if (tw == nullptr || !tw->enabled)
        return;
    if (tw->unbind(frame))
        tilingApplyLayout(manager, frame->getWorkspace());
}

// ------------------------------------------------------------------
// commands
// ------------------------------------------------------------------

static TilingWorkspace* tilingWorkspaceOf(YWindowManager* manager,
                                          int* workspaceOut = nullptr) {
    int ws = manager->activeWorkspace();
    TilingWorkspace* tw = Tiling::instance().workspace(ws);
    if (tw && workspaceOut)
        *workspaceOut = ws;
    return tw;
}

bool tilingSplit(YWindowManager* manager, const string& dir, double fraction) {
    int ws;
    TilingWorkspace* tw = tilingWorkspaceOf(manager, &ws);
    if (tw == nullptr || !tw->enabled)
        return false;
    FrameAlign align = (dir == "h" || dir == "horizontal")
                           ? FrameAlign::horizontal
                           : FrameAlign::vertical;
    FrameLeaf* leaf = tw->tree.focusedLeaf();
    if (leaf == nullptr)
        return false;
    FrameLeaf* newLeaf = tw->tree.splitLeaf(leaf, align, fraction);
    if (newLeaf == nullptr)
        return false;
    // re-bind the clients that were moved into the new leaf
    for (YFrameWindow* f : newLeaf->clients())
        f->setFrameLeaf(newLeaf);
    tilingApplyLayout(manager, ws);
    return true;
}

bool tilingRemove(YWindowManager* manager) {
    int ws;
    TilingWorkspace* tw = tilingWorkspaceOf(manager, &ws);
    if (tw == nullptr || !tw->enabled)
        return false;
    // unbind all clients of the focused leaf so they go to the target
    FrameLeaf* leaf = tw->tree.focusedLeaf();
    if (leaf == nullptr)
        return false;
    // collect clients of this leaf and mark them as floating until the
    // tree re-binds them during the merge
    vector<YFrameWindow*> clients(leaf->clients().begin(),
                                  leaf->clients().end());
    for (YFrameWindow* f : clients)
        f->setFrameLeaf(nullptr);
    tw->tree.removeFocusedLeaf();
    // the merged target leaf gets the clients; re-bind them
    FrameLeaf* target = tw->tree.focusedLeaf();
    if (target) {
        for (YFrameWindow* f : clients)
            f->setFrameLeaf(target);
    }
    tilingApplyLayout(manager, ws);
    return true;
}

bool tilingFocusIndex(YWindowManager* manager, const string& index) {
    int ws;
    TilingWorkspace* tw = tilingWorkspaceOf(manager, &ws);
    if (tw == nullptr || !tw->enabled)
        return false;
    Frame* f = tw->tree.lookup(index);
    if (f == nullptr || f->isSplit())
        return false;
    tw->tree.focusFrame(f);
    // reflect the tree focus in the WM's focus
    FrameLeaf* leaf = f->asLeaf();
    if (leaf && leaf->selectedClient()) {
        manager->setFocus(leaf->selectedClient(), true);
    }
    return true;
}

/*! Find a leaf by its label in the given tree (or all workspaces). */
static FrameLeaf* findLeafByLabel(FrameTree& tree, const string& label) {
    FrameLeaf* found = nullptr;
    tree.root()->fmap(
        [](FrameSplit*) {},
        [&](FrameLeaf* leaf) {
            if (found == nullptr && leaf->label() == label)
                found = leaf;
        });
    return found;
}

bool tilingFocusLabel(YWindowManager* manager, const string& label) {
    int ws;
    TilingWorkspace* tw = tilingWorkspaceOf(manager, &ws);
    if (tw == nullptr || !tw->enabled)
        return false;
    FrameLeaf* leaf = findLeafByLabel(tw->tree, label);
    if (leaf == nullptr)
        return false;
    tw->tree.focusFrame(leaf);
    if (leaf->selectedClient())
        manager->setFocus(leaf->selectedClient(), true);
    return true;
}

bool tilingSetLabel(YWindowManager* manager, const string& label) {
    int ws;
    TilingWorkspace* tw = tilingWorkspaceOf(manager, &ws);
    if (tw == nullptr || !tw->enabled)
        return false;
    FrameLeaf* leaf = tw->tree.focusedLeaf();
    if (leaf == nullptr)
        return false;
    // uniqueness check across this workspace's tree
    if (!label.empty() && findLeafByLabel(tw->tree, label) != nullptr &&
        findLeafByLabel(tw->tree, label) != leaf)
        return false;
    leaf->setLabel(label);
    return true;
}

string tilingDump(YWindowManager* manager) {
    TilingWorkspace* tw = tilingWorkspaceOf(manager);
    if (tw == nullptr)
        return "";
    return tw->tree.dump();
}

void tilingAction(YWindowManager* manager) {
    if (manager == nullptr)
        return;
    // Is there a pending request property from icesh?
    YProperty prop(xapp->root(), _XA_ICEWM_TILING, F8, 8192,
               _XA_ICEWM_TILING, True);
    if (!prop) {
        tilingToggleActiveWorkspace(manager);
        return;
    }
    const char* text = prop.string();
    if (text == nullptr || text[0] == 0) {
        tilingToggleActiveWorkspace(manager);
        return;
    }
    // pass the raw buffer and length: the request is NUL-separated
    // tokens, so we must not truncate at the first NUL
    tilingHandleRequest(manager, text, prop.size());
}

/*! Parse 'request' (NUL-separated tokens) and dispatch. The tokens
 * are the same fields icesh puts in _ICEWM_TILING. Empty command is
 * ignored. Returns true if the subcommand was recognized.
 */
void tilingHandleRequest(YWindowManager* manager, const char* request,
                         size_t len) {
    if (manager == nullptr || request == nullptr)
        return;
    vector<string> tokens;
    const char* p = request;
    const char* end = request + len;
    while (p < end) {
        const char* start = p;
        while (p < end && *p)
            ++p;
        tokens.push_back(string(start, p));
        if (p >= end)
            break;
        ++p; // skip NUL separator
    }
    if (tokens.empty())
        return;

    const string& cmd = tokens[0];
    if (cmd == "toggle") {
        tilingToggleActiveWorkspace(manager);
    }
    else if (cmd == "split") {
        // split [dir [fraction]]
        string dir = tokens.size() > 1 ? tokens[1] : "v";
        double fraction = 0.5;
        if (tokens.size() > 2) {
            char* end = nullptr;
            double f = strtod(tokens[2].c_str(), &end);
            if (end && *end == 0)
                fraction = f;
        }
        tilingSplit(manager, dir, fraction);
    }
    else if (cmd == "remove") {
        tilingRemove(manager);
    }
    else if (cmd == "focus") {
        // focus <path|label>
        if (tokens.size() > 1) {
            if (!tilingFocusIndex(manager, tokens[1]))
                tilingFocusLabel(manager, tokens[1]);
        }
    }
    else if (cmd == "setlabel") {
        // setlabel [label]
        string label = tokens.size() > 1 ? tokens[1] : "";
        tilingSetLabel(manager, label);
    }
    else if (cmd == "dump") {
        // write the serialized tree back to the reply property, so
        // icesh can read it (see _ICEWM_TILING_REPLY in icesh)
        string dump = tilingDump(manager);
        XChangeProperty(xapp->display(), xapp->root(),
                        _XA_ICEWM_TILING_REPLY, _XA_ICEWM_TILING_REPLY,
                        8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(dump.c_str()),
                        dump.size());
    }
    else if (cmd == "load") {
        if (tokens.size() > 1)
            tilingLoad(manager, tokens[1]);
    }
    else if (cmd == "flex") {
        // flex <add|remove|focus|setlabel|dump|clear> ...
        if (tokens.size() > 1) {
            const string& sub = tokens[1];
            if (sub == "add") {
                flexAdd(manager, tokens, 2);
            }
            else if (sub == "remove") {
                if (tokens.size() > 2)
                    flexRemove(manager, tokens[2]);
            }
            else if (sub == "focus") {
                if (tokens.size() > 2)
                    flexFocus(manager, tokens[2]);
            }
            else if (sub == "setlabel") {
                if (tokens.size() > 3)
                    flexSetLabel(manager, tokens[2], tokens[3]);
                else if (tokens.size() == 3)
                    flexSetLabel(manager, tokens[2], "");
            }
            else if (sub == "dump") {
                string dumpv = flexDump(manager);
                XChangeProperty(xapp->display(), xapp->root(),
                                _XA_ICEWM_TILING_REPLY, _XA_ICEWM_TILING_REPLY,
                                8, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(dumpv.c_str()),
                                dumpv.size());
            }
            else if (sub == "clear") {
                flexClear(manager);
            }
            else if (sub == "resize") {
                // flex resize <label> <dw> <dh> [dx [dy]]
                // Resize (and optionally shift) a frame by relative
                // deltas; label "." means the focused frame.
                if (tokens.size() > 4) {
                    int dw = atoi(tokens[3].c_str());
                    int dh = atoi(tokens[4].c_str());
                    int dx = tokens.size() > 5 ? atoi(tokens[5].c_str()) : 0;
                    int dy = tokens.size() > 6 ? atoi(tokens[6].c_str()) : 0;
                    flexResize(manager, tokens[2], dw, dh, dx, dy);
                }
            }
            else if (sub == "move") {
                // flex move <label> <dx> <dy>
                // Shift a frame by the given relative deltas; label
                // "." means the focused frame.
                if (tokens.size() > 4) {
                    int dx = atoi(tokens[3].c_str());
                    int dy = atoi(tokens[4].c_str());
                    flexMove(manager, tokens[2], dx, dy);
                }
            }
            else if (sub == "bind") {
                // flex bind <label>
                // Bind the focused window to the frame, moving it
                // there even if it already lives elsewhere.
                if (tokens.size() > 2)
                    flexBind(manager, tokens[2]);
            }
            else if (sub == "highlight") {
                // flex highlight pen <n>
                // flex highlight color <r> <g> <b>
                // Adjust the thickness and color of the persistent
                // focus outline. No reply is written (the change is
                // visible immediately on the next repaint).
                if (tokens.size() > 2) {
                    const string& hsub = tokens[2];
                    bool ok = false;
                    auto parseByte = [](const string& s, int* v) {
                        if (s.empty())
                            return false;
                        char* end = nullptr;
                        const long n = strtol(s.c_str(), &end, 10);
                        if (end == nullptr || *end != 0 || n < 0 || n > 255)
                            return false;
                        *v = int(n);
                        return true;
                    };
                    if (hsub == "pen" && tokens.size() > 3) {
                        int v = 0;
                        ok = parseByte(tokens[3], &v) &&
                             flexSetHighlightPen(v);
                    }
                    else if (hsub == "color" && tokens.size() > 5) {
                        int r = 0, g = 0, b = 0;
                        ok = parseByte(tokens[3], &r) &&
                             parseByte(tokens[4], &g) &&
                             parseByte(tokens[5], &b) &&
                             flexSetHighlightColor(r, g, b);
                    }
                    if (ok) {
                        // redraw with the new thickness/color even
                        // though the focused frame itself did not move
                        flexHighlightRedraw();
                    }
                }
            }
            else if (sub == "group") {
                // flex group <add|open|focus|remove|dump> ...
                if (tokens.size() > 2) {
                    const string& gsub = tokens[2];
                    if (gsub == "add") {
                        // group add <name> <labels...>
                        if (tokens.size() > 3) {
                            vector<string> labels(tokens.begin() + 4, tokens.end());
                            // "." means the focused window's frame
                            if (labels.size() == 1 && labels[0] == ".") {
                                YFrameWindow* focus = manager->getFocus();
                                if (focus == nullptr || focus->flexFrame() == nullptr)
                                    return;
                                labels[0] = focus->flexFrame()->label();
                            }
                            flexGroupAdd(manager, tokens[3], labels);
                        }
                    }
                    else if (gsub == "open") {
                        // group open <name>
                        if (tokens.size() > 3)
                            flexGroupOpen(manager, tokens[3]);
                    }
                    else if (gsub == "focus") {
                        // flex group focus [next|prev] [name]:
                        // cycle the focus among all frames of the
                        // workspace, or within one group. Direction
                        // defaults to "next" (after the focused frame);
                        // "prev" goes the other way.
                        bool forward = true;
                        size_t arg = 3;
                        if (tokens.size() > arg &&
                            (tokens[arg] == "next" || tokens[arg] == "prev")) {
                            forward = (tokens[arg] == "next");
                            ++arg;
                        }
                        if (tokens.size() > arg)
                            flexGroupFocus(manager, tokens[arg], forward);
                        else
                            flexGroupFocusAll(manager, forward);
                    }
                    else if (gsub == "remove") {
                        // group remove <name>
                        if (tokens.size() > 3)
                            flexGroupRemove(manager, tokens[3]);
                    }
                    else if (gsub == "close") {
                        // group close <name>; "<name>" may be "." for the
                        // focused frame's group. Closes every window of
                        // the group and drops the group itself.
                        if (tokens.size() > 3)
                            flexGroupClose(manager, tokens[3]);
                    }
                    else if (gsub == "rename") {
                        // group rename <old> <new>; "<old>" may be "."
                        // to rename the group of the focused frame.
                        if (tokens.size() > 4)
                            flexGroupRename(manager, tokens[3], tokens[4]);
                        else if (tokens.size() == 4)
                            flexGroupRename(manager, tokens[3], "");
                    }
                    else if (gsub == "dump") {
                        string dumpv = flexGroupDump(manager);
                        XChangeProperty(xapp->display(), xapp->root(),
                                        _XA_ICEWM_TILING_REPLY, _XA_ICEWM_TILING_REPLY,
                                        8, PropModeReplace,
                                        reinterpret_cast<const unsigned char*>(dumpv.c_str()),
                                        dumpv.size());
                    }
                }
            }
            else if (sub == "window") {
                // flex window [next|prev] [label]: cycle the focus among
                // the windows bound to one flexible frame. Direction
                // defaults to "next"; tokens[2] is "1"/"0" from icesh.
                if (tokens.size() > 2) {
                    bool forward = (tokens[2] == "1");
                    string label = tokens.size() > 3 ? tokens[3] : "";
                    flexWindowFocus(manager, label, forward);
                }
            }
            else if (sub == "mode") {
                // flex mode <resize|move> <on|off>
                // Enter or leave a modal mode in which the arrow keys
                // resize or move the flexible frame of the focused
                // window (Escape leaves the mode).
                if (tokens.size() > 3) {
                    const bool resize = (tokens[2] == "resize");
                    const bool on = (tokens[3] == "on");
                    manager->setFlexMode(resize, on);
                }
            }
        }
    }
    else if (cmd == "workspace-set-group") {
        // workspace-set-group <group> <count>
        // Generate (or shrink) the sub-workspaces `group|1..count` on
        // the server side.
        if (tokens.size() > 2) {
            long n = atol(tokens[2].c_str());
            if (n >= 1 && n <= NewMaxWorkspaces)
                workspaceSetGroup(manager, tokens[1].c_str(), int(n));
        }
    }
    else if (cmd == "workspace-group-step") {
        // workspace-group-step <next|prev> [group]
        // Cycle to the next/previous sub-workspace of a group, wrapping
        // around at both ends. Group defaults to the group of the
        // currently active workspace.
        if (tokens.size() > 1) {
            bool forward = (tokens[1] != "prev");
            string group = tokens.size() > 2 ? tokens[2] : "";
            workspaceGroupStep(manager, group, forward);
        }
    }
    else if (cmd == "workspace-move-frame") {
        // workspace-move-frame [label] <group> <target>
        // Move the flexible frame (or the focused window's frame) of
        // the active workspace to sub-workspace `group|target`.
        if (tokens.size() < 3)
            return;
        if (tokens.size() >= 4) {
            // label <group> <target>
            long t = atol(tokens[3].c_str());
            if (t >= 1 && t <= NewMaxWorkspaces)
                workspaceMoveFrame(manager, tokens[1], tokens[2], int(t));
        } else {
            // <group> <target> (no label)
            long t = atol(tokens[2].c_str());
            if (t >= 1 && t <= NewMaxWorkspaces)
                workspaceMoveFrame(manager, "", tokens[1], int(t));
        }
    }
    else if (cmd == "workspace-move-frame-step") {
        // workspace-move-frame-step <next|prev> [label]
        // Cycle the flexible frame (or the focused window's frame) to
        // the next/previous sub-workspace of its group, wrapping around.
        if (tokens.size() > 1) {
            bool forward = (tokens[1] != "prev");
            string label = tokens.size() > 2 ? tokens[2] : "";
            workspaceMoveFrameStep(manager, label, forward);
        }
    }
}

bool tilingLoad(YWindowManager* manager, const string& layout) {
    int ws;
    TilingWorkspace* tw = tilingWorkspaceOf(manager, &ws);
    if (tw == nullptr)
        return false;
    string error;
    if (!tw->tree.load(layout, error))
        return false;
    // re-bind all clients to the (possibly new) focused leaf
    for (YFrameWindow* f : tilingFrames(manager, ws, false))
        f->setFrameLeaf(nullptr); // will re-bind below
    FrameLeaf* leaf = tw->tree.focusedLeaf();
    if (leaf) {
        for (YFrameWindow* f : tilingFrames(manager, ws, false))
            f->setFrameLeaf(leaf);
    }
    if (tw->enabled)
        tilingApplyLayout(manager, ws);
    return true;
}

// ------------------------------------------------------------------
// dynamic sub-workspaces (group|N)
// ------------------------------------------------------------------

/*! True when 'name' is "group|N" (N positive), i.e. a member of the
 * sub-workspace group 'group'.
 */
static bool subWorkspaceOf(const std::string& name, const std::string& group) {
    if (name.size() <= group.size() + 1)
        return false;
    if (name.compare(0, group.size(), group) != 0)
        return false;
    if (name[group.size()] != '|')
        return false;
    for (size_t i = group.size() + 1; i < name.size(); ++i) {
        if (!isdigit((unsigned char)name[i]))
            return false;
    }
    return true;
}

/*! Parse the N from a "group|N" workspace name; returns -1 if it does
 * not look like one.
 */
static int subWorkspaceNumber(const std::string& name,
                              const std::string& group) {
    if (!subWorkspaceOf(name, group))
        return -1;
    return atoi(name.c_str() + group.size() + 1);
}

/*! Expand (or shrink) the sub-workspaces of one group to `count`
 * members, keeping the flat order of all other workspaces. Existing
 * windows on a removed sub-workspace are moved to its predecessor
 * (through lessenWorkspaces which handles that for the tail), and the
 * tail is dropped to match the shorter list.
 */
bool workspaceSetGroup(YWindowManager* manager, const std::string& group,
                       int count) {
    if (manager == nullptr || group.empty() || count < 1)
        return false;

    // Collect the currently-live workspace names.
    std::vector<std::string> names;
    for (int i = 0; i < workspaceCount; ++i)
        names.push_back(workspaceNames[i]);

    // The group keeps its position: the index of its first member.
    size_t anchor = names.size();
    for (size_t i = 0; i < names.size(); ++i) {
        if (subWorkspaceOf(names[i], group)) { anchor = i; break; }
    }

    // Build the new flat list, preserving the order of every foreign
    // workspace and dropping the group's old members.
    std::vector<std::string> newNames;
    newNames.reserve(names.size() + count);
    for (size_t i = 0; i < anchor && i < names.size(); ++i)
        newNames.push_back(names[i]);
    for (int k = 1; k <= count; ++k) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", k);
        newNames.push_back(group + "|" + buf);
    }
    for (size_t i = anchor; i < names.size(); ++i) {
        if (!subWorkspaceOf(names[i], group))
            newNames.push_back(names[i]);
    }

    const size_t target = newNames.size();
    if (target > size_t(workspaceCount) &&
        target <= size_t(NewMaxWorkspaces))
    {
        manager->extendWorkspaces(int(target));
    }

    // Apply the new names to the existing (at most `target`) entries.
    const int have = workspaceCount;
    const int to = min<int>(int(target), have);
    for (int i = 0; i < to; ++i) {
        if (strcmp(workspaceNames[i], newNames[i].c_str())) {
            char* name = newstr(newNames[i].c_str());
            swap(name, *workspaces[i]);
            delete[] name;
        }
    }
    // Shrink from the tail; the workspaces that moved to lower indices
    // already carry their new names, so the tail holds only duplicates
    // of the removed group members.
    if (int(target) < have)
        manager->lessenWorkspaces(int(target));

    // keep the per-workspace tiling registry in sync
    for (int i = 0; i < workspaceCount; ++i)
        Tiling::instance().ensure(i);

    // publish the new names as _NET_DESKTOP_NAMES so icesh and other
    // clients see them; updateTaskBarNames relabels the task bar.
    manager->setDesktopNames(workspaceCount);
    manager->updateTaskBarNames();
    return true;
}

/*! Determine the group ("prefix" before '|') of a workspace name, or
 * empty if the name has no '|'.
 */
static std::string groupOf(const std::string& name) {
    size_t p = name.find('|');
    if (p == std::string::npos || p == 0)
        return "";
    return name.substr(0, p);
}

/*! Cycle the focus to the next (`forward`) or previous sub-workspace of
 * the group `group` (or, when empty, the group of the currently active
 * workspace). Wraps around at both ends. Returns false if the current
 * workspace has no group or the group has no members.
 */
bool workspaceGroupStep(YWindowManager* manager, const std::string& group,
                        bool forward) {
    if (manager == nullptr)
        return false;

    // Determine the group to cycle in.
    std::string grp = group;
    int cur = manager->activeWorkspace();
    if (grp.empty()) {
        if (cur < 0 || cur >= workspaceCount)
            return false;
        grp = groupOf(workspaceNames[cur]);
        if (grp.empty())
            return false;
    }
    if (grp.empty())
        return false;

    // Enumerate the members of the group (in current flat order).
    std::vector<int> members;
    for (int i = 0; i < workspaceCount; ++i) {
        int n = subWorkspaceNumber(workspaceNames[i], grp);
        if (n >= 1)
            members.push_back(i);
    }
    if (members.empty())
        return false;

    const int curNum = (grp == groupOf(cur < workspaceCount ? workspaceNames[cur] : ""))
                     ? subWorkspaceNumber(workspaceNames[cur], grp) : -1;
    int idx = -1;
    if (curNum >= 1) {
        for (size_t i = 0; i < members.size(); ++i) {
            int n = subWorkspaceNumber(workspaceNames[members[i]], grp);
            if (n == curNum) { idx = int(i); break; }
        }
    }
    int next;
    if (idx < 0) {
        next = forward ? members.front() : members.back();
    }
    else {
        size_t ni = forward
                  ? (size_t(idx) + 1) % members.size()
                  : (size_t(idx) + members.size() - 1) % members.size();
        next = members[ni];
    }
    manager->activateWorkspace(next);
    return true;
}

/*! Move the active workspace's flexible frame `label` (or the focused
 * window's frame when `label` is empty) to the sub-workspace
 * `group|target` of `group`. The frame's windows move along; the
 * frame's rectangle is recreated at the target (or the windows join the
 * existing frame of the same label there). Activates the target
 * workspace at the end.
 */
bool workspaceMoveFrame(YWindowManager* manager, const std::string& label,
                        const std::string& group, int target) {
    if (manager == nullptr || group.empty() || target < 1)
        return false;

    // Resolve the frame to move. With an explicit label, look on every
    // workspace (the frame may live on an inactive sub-workspace, e.g.
    // right after a middle-click activate switched the manager's active
    // workspace to where the focused window lives). Otherwise use the
    // frame of the focused window.
    FlexFrame* frame = nullptr;
    int cur = -1;
    if (!label.empty() && label != ".") {
        for (int i = 0; i < workspaceCount; ++i) {
            FlexFrameSet* set = Tiling::instance().flex(i);
            if (set == nullptr)
                continue;
            frame = set->find(label);
            if (frame != nullptr) {
                cur = i;
                break;
            }
        }
    } else {
        YFrameWindow* focus = manager->getFocus();
        if (focus != nullptr)
            frame = focus->flexFrame();
        if (frame != nullptr)
            cur = manager->activeWorkspace();
    }
    if (frame == nullptr)
        return false;

    const string labelName = frame->label();
    const YRect rect = frame->rect();
    FlexFrameSet* set = Tiling::instance().flex(cur);

    // Find the flat index of the target sub-workspace. When the target
    // does not exist yet, create it with the same "group|N" naming as
    // WorkspaceGroups/setWorkspaceGroup (appended at the tail).
    int targetWs = -1;
    for (int i = 0; i < workspaceCount; ++i) {
        if (subWorkspaceOf(workspaceNames[i], group) &&
            subWorkspaceNumber(workspaceNames[i], group) == target)
        {
            targetWs = i;
            break;
        }
    }
    if (targetWs < 0) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", target);
        manager->extendWorkspaces(workspaceCount + 1);
        const int idx = workspaceCount - 1;
        char* name = newstr((group + "|" + buf).c_str());
        swap(name, *workspaces[idx]);   // the last slot is named `name`
        delete[] name;
        Tiling::instance().ensure(idx);
        manager->setDesktopNames(workspaceCount);
        manager->updateTaskBarNames();
        targetWs = idx;
    }

    // A flexible frame without windows is meaningless: moving it would
    // only leave an empty rectangle on the target while the user's
    // windows stay put. In that case just drop the empty source frame.
    vector<YFrameWindow*> moving(frame->clients());
    if (moving.empty()) {
        set->remove(labelName);
        return true;
    }

    // Detach the frame's clients from the source workspace's set.
    for (YFrameWindow* w : moving)
        if (w)
            w->setFlexFrame(nullptr);
    // The entire frame moves; drop the source copy (the windows get
    // re-attached below). Removing also detaches any remaining clients
    // (there should be none left) and updates the set bookkeeping.
    set->remove(labelName);

    // Move the windows to the target workspace and re-attach them to
    // the frame there (creating it with the carried-over rect when the
    // target has no frame of that label yet).
    FlexFrameSet* tset = Tiling::instance().ensureFlex(targetWs);
    FlexFrame* tf = tset->find(labelName);
    if (tf == nullptr)
        tf = tset->add(labelName, rect);
    else
        tf->setRect(rect);   // update in case it was resized meanwhile
    for (YFrameWindow* w : moving) {
        if (w == nullptr)
            continue;
        w->setFlexFrame(tf);
        if (w->getWorkspace() != targetWs)
            w->setWorkspace(targetWs);
    }
    tset->setFocusedLabel(labelName);
    flexApplyLayout(manager, targetWs);

    manager->activateWorkspace(targetWs);
    return true;
}

/*! Move the flexible frame `label` of the active workspace (or the frame
 * of the focused window when `label` is empty) to the next (`forward`)
 * sub-workspace of its group, wrapping around at both ends — like
 * workspaceGroupStep cycles the focus, but the frame moves along. The
 * group is derived from the workspace the frame currently lives on (which
 * may be an inactive sub-workspace when an explicit label was given).
 */
bool workspaceMoveFrameStep(YWindowManager* manager,
                            const std::string& label, bool forward) {
    if (manager == nullptr)
        return false;

    // Resolve the frame the same way workspaceMoveFrame does, then derive
    // the group from the workspace it lives on.
    FlexFrame* frame = nullptr;
    int cur = -1;
    if (!label.empty() && label != ".") {
        for (int i = 0; i < workspaceCount; ++i) {
            FlexFrameSet* set = Tiling::instance().flex(i);
            if (set == nullptr)
                continue;
            frame = set->find(label);
            if (frame != nullptr) {
                cur = i;
                break;
            }
        }
    } else {
        YFrameWindow* focus = manager->getFocus();
        if (focus != nullptr)
            frame = focus->flexFrame();
        if (frame != nullptr)
            cur = manager->activeWorkspace();
    }
    if (frame == nullptr)
        return false;

    // The group of the frame's current (flat) workspace.
    if (cur < 0 || cur >= workspaceCount)
        return false;
    const string grp = groupOf(workspaceNames[cur]);
    if (grp.empty())
        return false;

    // Enumerate the members of the group, in current flat order.
    vector<int> members;
    for (int i = 0; i < workspaceCount; ++i) {
        if (subWorkspaceNumber(workspaceNames[i], grp) >= 1)
            members.push_back(i);
    }
    if (members.empty())
        return false;

    // The position of the frame's workspace inside the group's list.
    size_t idx = members.size();
    for (size_t i = 0; i < members.size(); ++i) {
        if (members[i] == cur) { idx = i; break; }
    }
    if (idx >= members.size())
        return false;   // workspace not part of the group after all

    const int targetFlat = members[forward
            ? (idx + 1) % members.size()
            : (idx + members.size() - 1) % members.size()];
    const int target = subWorkspaceNumber(workspaceNames[targetFlat], grp);
    if (target < 1)
        return false;

    return workspaceMoveFrame(manager, label, grp, target);
}

// ------------------------------------------------------------------
// flexible frames (defined in flexframe.cc; command drivers here)
// ------------------------------------------------------------------

/*! Parse tail tokens of a flex request into label + rect. The label may
 * contain spaces, so the coordinates are taken from the END of the
 * token list: the last 4 tokens are x y w h (in order), or the last 2
 * are x y (in order) with the size defaulting to 80% of the work area,
 * centered on (x, y). Everything before them is the label.
 */
static bool parseFlexByInts(YWindowManager* manager,
                            const vector<string>& tokens, size_t start,
                            string& label, YRect& rect) {
    const size_t n = tokens.size();
    if (n <= start)
        return false;
    const auto isInt = [](const string& s) {
        if (s.empty())
            return false;
        char* end = nullptr;
        strtol(s.c_str(), &end, 10);
        return end != nullptr && *end == 0;
    };

    size_t labelEnd = n;
    int x = 0, y = 0, w = 0, h = 0;
    if (n - start >= 4 && isInt(tokens[n - 4]) && isInt(tokens[n - 3]) &&
        isInt(tokens[n - 2]) && isInt(tokens[n - 1])) {
        x = atoi(tokens[n - 4].c_str());
        y = atoi(tokens[n - 3].c_str());
        w = atoi(tokens[n - 2].c_str());
        h = atoi(tokens[n - 1].c_str());
        labelEnd = n - 4;
        rect = YRect(x, y, std::max(1, w), std::max(1, h));
    }
    else if (n - start >= 2 && isInt(tokens[n - 2]) && isInt(tokens[n - 1])) {
        x = atoi(tokens[n - 2].c_str());
        y = atoi(tokens[n - 1].c_str());
        labelEnd = n - 2;
        // only position given: default size = 80% of work area, centered
        YRect area = tilingWorkArea(manager, manager->activeWorkspace());
        w = int(area.width()) * 4 / 5;
        h = int(area.height()) * 4 / 5;
        int gap = flexGap();
        rect = YRect(std::max(0, x - w / 2), std::max(0, y - h / 2),
                     std::max(1, w - 2 * gap), std::max(1, h - 2 * gap));
    }
    else {
        return false;
    }

    if (labelEnd <= start)
        return false;
    label.clear();
    for (size_t i = start; i < labelEnd; ++i) {
        if (i > start)
            label += ' ';
        label += tokens[i];
    }
    return true;
}

bool flexAdd(YWindowManager* manager, const vector<string>& tokens,
             size_t start) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    string label;
    YRect rect;
    if (!parseFlexByInts(manager, tokens, start, label, rect))
        return false;
    FlexFrameSet* set = Tiling::instance().ensureFlex(ws);
    set->add(label, rect); // create or override this frame's rect
    set->setFocusedLabel(label);
    set->noteFocusInGroups(label);
    // bind the focused window into this frame (overriding its rect);
    // accept fullscreen windows too so their outline follows them
    YFrameWindow* focus = manager->getFocus();
    if (focus && isFlexBindCandidate(focus))
        focus->setFlexFrame(set->find(label));
    flexApplyLayout(manager, ws);
    flexFrameHighlight(manager, rect);
    return true;
}

bool flexRemove(YWindowManager* manager, const string& label) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    bool ok = set->remove(label); // detaches the frame's windows
    flexApplyLayout(manager, ws);
    return ok;
}

static bool flexWindowFocusStep(YWindowManager* manager, FlexFrameSet* set,
                                const string& label, bool forward);

/*! Pre-declare flexWindowFocusStep so flexFocus can jump straight to
 * window cycling while the full definition follows below. */
static bool flexWindowFocusStep(YWindowManager* manager, FlexFrameSet* set,
                                const string& label, bool forward);

bool flexFocus(YWindowManager* manager, const string& label) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    FlexFrame* f = set->find(label);
    // The frame may live on another (sub-)workspace: windows are bound
    // per-workspace, so `flex focus` from a different tag must first
    // switch to the workspace that owns the frame, otherwise nothing
    // becomes visible (the frame's windows are hidden there). Look it
    // up across all workspaces like workspaceMoveFrame() does, then
    // activate that workspace and continue on its own set.
    if (f == nullptr) {
        for (int i = 0; f == nullptr && i < workspaceCount; ++i) {
            if (i == ws)
                continue;
            FlexFrameSet* other = Tiling::instance().flex(i);
            if (other == nullptr)
                continue;
            f = other->find(label);
            if (f != nullptr) {
                manager->activateWorkspace(i);
                ws = i;
                set = other;
                break;
            }
        }
        if (f == nullptr)
            return false;
    }
    // A frame can hold several windows (e.g. a stack of fullscreen
    // windows). Cycling `flex focus <label>` should step to the next
    // window like `flex window next` does, not re-focus whatever
    // focusTarget() returns (which is the same window every call).
    if (f->clientCount() > 1) {
        return flexWindowFocusStep(manager, set, label, true);
    }
    YFrameWindow* target = f->focusTarget();
    set->setFocusedLabel(label);
    // keep the group "last focused" pointers in sync with direct focus
    set->noteFocusInGroups(label);
    if (target != nullptr) {
        if (target->getWorkspace() != ws)
            target->setWorkspace(ws);
        // Focus AND raise: a plain setFocus() would not restack in the
        // X server, so a (flex) fullscreen window whose layer just
        // dropped back to normal could still cover the target. focus()
        // does setFocus() and then wmRaise() (RaiseOnFocus), which
        // physically raises the window within its layer.
        target->focus(true);
        // remember the focused window of this frame explicitly: focus()
        // routes through setFocus() which also updates it via
        // flexNoteFrameFocus(), but when a multi-window frame is
        // cycled through flexWindowFocusStep (or activate()) make sure
        // the per-frame pointer follows the actual focus too.
        f->noteFocus(target);
    }
    // visual feedback: flash the frame rectangle
    if (target != nullptr)
        flexFrameHighlight(manager, f->rect());
    return true;
}

bool flexSetLabel(YWindowManager* manager, const string& oldLabel,
                  const string& newLabel) {
    if (manager == nullptr || oldLabel == newLabel || newLabel.empty())
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    return set->rename(oldLabel, newLabel);
}

string flexDump(YWindowManager* manager) {
    if (manager == nullptr)
        return "";
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return "";
    return set->dump();
}

bool flexClear(YWindowManager* manager) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    set->clear();
    flexApplyLayout(manager, ws);
    return true;
}

bool flexResize(YWindowManager* manager, const string& label,
                int dw, int dh, int dx, int dy) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    FlexFrame* f = nullptr;
    if (label.empty() || label == "." || label == "focused")
        f = set->find(set->focusedLabel());
    else
        f = set->find(label);
    if (f == nullptr)
        return false;
    // apply the deltas to the current rectangle; width/height keep a
    // minimum of 1 pixel, position may move freely (negative dx/dy
    // shift towards the top-left)
    const YRect r = f->rect();
    const int x = int(r.x()) + dx;
    const int y = int(r.y()) + dy;
    const int w = std::max(1, int(r.width()) + dw);
    const int h = std::max(1, int(r.height()) + dh);
    f->setRect(YRect(x, y, w, h));
    // unlike `flex add`, this does not touch the focused-label or the
    // group bookkeeping: only the rectangle changes, the frame keeps
    // its identity, clients and group membership
    flexApplyLayout(manager, ws);
    flexFrameHighlight(manager, f->rect());
    return true;
}

bool flexMove(YWindowManager* manager, const string& label,
              int dx, int dy) {
    // a pure shift: same geometry logic as `flex resize` with zero
    // size deltas (label resolution, focused-frame fallback, layout
    // and outline refresh all live there)
    return flexResize(manager, label, 0, 0, dx, dy);
}

bool flexBind(YWindowManager* manager, const string& label) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    FlexFrame* f = set->find(label);
    if (f == nullptr)
        return false;
    YFrameWindow* focus = manager->getFocus();
    if (focus == nullptr || !isFlexBindCandidate(focus))
        return false;
    focus->setFlexFrame(f); // rebind; group membership is not touched
    flexApplyLayout(manager, ws);
    flexFrameHighlight(manager, f->rect());
    return true;
}

// ------------------------------------------------------------------
// flex group
// ------------------------------------------------------------------

bool flexGroupAdd(YWindowManager* manager, const string& group,
                  const std::vector<string>& labels) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    // only members that actually exist as frames
    vector<string> valid;
    for (const string& label : labels)
        if (set->find(label) != nullptr)
            valid.push_back(label);
    if (valid.empty())
        return false;
    bool changed = set->groupAdd(group, valid);
    // if the focused frame is a member, record it as last-focused now
    YFrameWindow* focus = manager->getFocus();
    if (focus && focus->flexFrame() != nullptr) {
        const string flabel = focus->flexFrame()->label();
        for (const string& label : valid)
            if (label == flabel) {
                set->groupNoteFocus(group, flabel);
                break;
            }
    }
    return changed;
}

bool flexGroupOpen(YWindowManager* manager, const string& group) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    // 1. the most recently focused member
    FlexFrame* target = nullptr;
    string last = set->groupLastFocused(group);
    if (!last.empty())
        target = set->find(last);
    // 2. any focusable member in list (MRU) order
    if (target == nullptr) {
        for (const string& label : set->groupMembers(group)) {
            FlexFrame* f = set->find(label);
            if (f != nullptr && f->focusTarget() != nullptr) {
                target = f;
                break;
            }
        }
    }
    if (target == nullptr)
        return false;
    // activate the frame's target window
    YFrameWindow* w = target->focusTarget();
    if (w == nullptr)
        return false;
    set->setFocusedLabel(target->label());
    set->groupNoteFocus(group, target->label());
    w->activate(true, true);   // may switch workspace to reach the window
    flexFrameHighlight(manager, target->rect());
    return true;
}

//! helper for `flex group focus`: move the focus to the next frame of
//! 'members' after (or, when 'forward' is false, to the frame before)
//! the one the focused window belongs to, wrapping around the list.
static bool flexGroupFocusStep(YWindowManager* manager, FlexFrameSet* set,
                               const string& notingGroup,
                               const vector<string>& members, bool forward) {
    if (manager == nullptr || set == nullptr)
        return false;
    // current focus: which member does it belong to?
    YFrameWindow* focus = manager->getFocus();
    string cur;
    if (focus && focus->flexFrame() != nullptr)
        cur = focus->flexFrame()->label();
    // member after (or before) the current one
    string next;
    if (!members.empty()) {
        auto it = find(members.begin(), members.end(), cur);
        if (it != members.end()) {
            if (forward) {
                ++it;
                if (it != members.end())
                    next = *it;
            }
            else {
                if (it != members.begin())
                    next = *prev(it);
            }
        }
        if (next.empty())  // not found, at the edge -> wrap around
            next = forward ? members.front() : members.back();
    }
    if (next.empty())
        return false;
    FlexFrame* f = set->find(next);
    YFrameWindow* w = f ? f->focusTarget() : nullptr;
    if (w == nullptr)
        return false;
    set->setFocusedLabel(next);
    if (!notingGroup.empty())
        set->groupNoteFocus(notingGroup, next);
    w->activate(true, true);
    flexFrameHighlight(manager, f->rect());
    return true;
}

bool flexGroupFocus(YWindowManager* manager, const string& group,
                    bool forward) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    return flexGroupFocusStep(manager, set, group, set->groupMembers(group),
                              forward);
}

bool flexGroupFocusAll(YWindowManager* manager, bool forward) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    // all frames in map (label) order
    vector<string> labels;
    for (auto& kv : *set)
        labels.push_back(kv.first);
    return flexGroupFocusStep(manager, set, "", labels, forward);
}

//! helper for `flex window`: move the focus to the next (or, when
//! 'forward' is false, the previous) window bound to a single flexible
//! frame, wrapping around its client list. Works on the frame of the
//! currently focused window when 'label' is empty.
static bool flexWindowFocusStep(YWindowManager* manager, FlexFrameSet* set,
                                const string& label, bool forward) {
    if (manager == nullptr || set == nullptr)
        return false;
    // resolve the frame: explicit label, else the focused window's frame,
    // else the set's last focused frame.
    FlexFrame* f = nullptr;
    if (!label.empty())
        f = set->find(label);
    else {
        YFrameWindow* focus = manager->getFocus();
        if (focus && focus->flexFrame() != nullptr)
            f = focus->flexFrame();
        else {
            const string& fl = set->focusedLabel();
            if (!fl.empty())
                f = set->find(fl);
        }
    }
    if (f == nullptr || f->isEmpty())
        return false;

    const vector<YFrameWindow*>& clients = f->clients();
    YFrameWindow* cur = manager->getFocus();
    // index of the currently focused window in this frame, if any
    auto it = std::find(clients.begin(), clients.end(), cur);
    if (it == clients.end() && clients.size() > 1) {
        // The focus is not in this frame (e.g. `flex focus` arrived from
        // another frame): restore the window that was focused here last,
        // not the frame's preferred (last-attached) window. Without this,
        // returning to a multi-window frame always jumps to the first
        // window of the frame, losing the remembered focus.
        YFrameWindow* remembered = f->focusTarget();
        if (remembered != nullptr) {
            set->setFocusedLabel(f->label());
            set->noteFocusInGroups(f->label());
            f->noteFocus(remembered);
            remembered->activate(true, true);
            flexApplyLayout(manager, manager->activeWorkspace());
            flexFrameHighlight(manager, f->rect());
            return true;
        }
    }
    // step from the focused window; when the focus is not in this frame,
    // start from the frame's preferred (last visible) window. Wraps.
    int start = (it != clients.end()) ? static_cast<int>(it - clients.begin())
                                      : static_cast<int>(clients.size());
    if (start == static_cast<int>(clients.size()))
        start = static_cast<int>(clients.size()) - 1;   // last = focusTarget end
    YFrameWindow* next = nullptr;
    for (size_t k = 1; k <= clients.size(); ++k) {
        size_t idx = forward
                   ? (static_cast<size_t>(start) + k) % clients.size()
                   : (start + clients.size() - k) % clients.size();
        YFrameWindow* w = clients[idx];
        if (w == nullptr || w == cur)
            continue;
        next = w;
        break;
    }
    if (next == nullptr)
        return false;

    set->setFocusedLabel(f->label());
    set->noteFocusInGroups(f->label());
    // raise the window and give it focus; a subsequent flexApplyLayout
    // (below) also re-applies the frame's current rectangle, so the
    // window matches a frame that was resized/moved since it was bound.
    f->noteFocus(next);   // remember the window currently focused here
    next->activate(true, true);
    flexApplyLayout(manager, manager->activeWorkspace());
    flexFrameHighlight(manager, f->rect());
    return true;
}

bool flexWindowFocus(YWindowManager* manager, const string& label,
                     bool forward) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    return flexWindowFocusStep(manager, set, label, forward);
}

bool flexGroupRemove(YWindowManager* manager, const string& group) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    return set->groupRemove(group);
}

bool flexGroupClose(YWindowManager* manager, const string& group) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    // "." resolves to the group of the focused window's frame, exactly
    // as in flexGroupRename.
    string name = group;
    if (name == ".") {
        YFrameWindow* focus = manager->getFocus();
        if (focus == nullptr || focus->flexFrame() == nullptr)
            return false;
        name = set->focusedGroup(focus->flexFrame()->label());
        if (name.empty())
            return false;
    }
    const vector<string> labels = set->groupMembers(name);
    if (labels.empty())
        return false;
    // Close every window in every frame of the group. wmClose() sends
    // WM_DELETE_WINDOW (or force-closes when the client accepts no
    // protocol); frames orphaned by the close are collected on the
    // server side, the group entry is dropped last.
    bool any = false;
    for (const string& label : labels) {
        FlexFrame* f = set->find(label);
        if (f == nullptr)
            continue;
        for (YFrameWindow* w : f->clients())
            if (w != nullptr) {
                w->wmClose();
                any = true;
            }
    }
    set->groupRemove(name);
    return any;
}

bool flexGroupRename(YWindowManager* manager, const string& oldGroup,
                     const string& newGroup) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    // "." means the group the focused window's frame belongs to (the
    // one that last recorded focus on it, if any).
    string old = oldGroup;
    if (old == ".") {
        YFrameWindow* focus = manager->getFocus();
        if (focus == nullptr || focus->flexFrame() == nullptr)
            return false;
        old = set->focusedGroup(focus->flexFrame()->label());
        if (old.empty())
            return false;
    }
    return set->groupRename(old, newGroup);
}

string flexGroupDump(YWindowManager* manager) {
    if (manager == nullptr)
        return "";
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return "";
    return set->groupDump();
}