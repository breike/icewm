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
    // bind the focused window into this frame (overriding its rect)
    YFrameWindow* focus = manager->getFocus();
    if (focus && isFlexCandidate(focus))
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

bool flexFocus(YWindowManager* manager, const string& label) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    FlexFrame* f = set->find(label);
    if (f == nullptr)
        return false;
    YFrameWindow* target = f->focusTarget();
    set->setFocusedLabel(label);
    // keep the group "last focused" pointers in sync with direct focus
    set->noteFocusInGroups(label);
    if (target != nullptr) {
        if (target->getWorkspace() != ws)
            target->setWorkspace(ws);
        manager->setFocus(target, true, true);
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

bool flexGroupRemove(YWindowManager* manager, const string& group) {
    if (manager == nullptr)
        return false;
    int ws = manager->activeWorkspace();
    FlexFrameSet* set = Tiling::instance().flex(ws);
    if (set == nullptr)
        return false;
    return set->groupRemove(group);
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