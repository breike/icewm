// frametree.cc — herbstluftwm-style tiling frame tree for IceWM.
#include "frametree.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <sstream>

#include "wmframe.h"

using std::function;
using std::make_shared;
using std::string;
using std::vector;

FrameAlgorithm frameAlignToAlgorithm(FrameAlign align) {
    return (align == FrameAlign::horizontal)
               ? FrameAlgorithm::horizontal
               : FrameAlgorithm::vertical;
}

// ------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------

string Frame::edgeToParent() const {
    if (parent_ == nullptr)
        return "";
    return (parent_->isSplit()) ? (parent_->asSplit()->firstChild() == this ? "0" : "1")
                                : "";
}

string Frame::frameIndex() const {
    if (parent_ == nullptr)
        return "";
    return parent_->frameIndex() + edgeToParent();
}

Frame* Frame::root() {
    Frame* node = this;
    while (node->parent_)
        node = node->parent_;
    return node;
}

int Frame::splitsToRoot(FrameAlign align) const {
    if (parent_ == nullptr)
        return 0;
    FrameSplit* split = parent_->asSplit();
    return split->align() == align
               ? 1 + split->splitsToRoot(align)
               : split->splitsToRoot(align);
}

// ------------------------------------------------------------------
// FrameSplit
// ------------------------------------------------------------------

FrameSplit::FrameSplit(Frame* parent, FrameAlign align, double fraction,
                       Frame* a, Frame* b)
    : Frame(parent)
    , align_(align)
    , fraction_(clampFraction(fraction))
    , selection_(false)
    , a_(a)
    , b_(b)
{
    a_->setParent(this);
    b_->setParent(this);
}

FrameSplit::~FrameSplit() {
    delete a_;
    delete b_;
}

void FrameSplit::fmap(function<void(FrameSplit*)> onSplit,
                      function<void(FrameLeaf*)> onLeaf, int order) {
    if (order <= 0) onSplit(this);
    a_->fmap(onSplit, onLeaf, order);
    if (order == 1) onSplit(this);
    b_->fmap(onSplit, onLeaf, order);
    if (order >= 1) onSplit(this);
}

void FrameSplit::replaceChild(Frame* old, Frame* child) {
    if (a_ == old) {
        a_ = child;
        a_->setParent(this);
    } else if (b_ == old) {
        b_ = child;
        b_->setParent(this);
    }
}

void FrameSplit::swapChildren() {
    std::swap(a_, b_);
    a_->setParent(this);
    b_->setParent(this);
}

// ------------------------------------------------------------------
// FrameLeaf
// ------------------------------------------------------------------

FrameLeaf::FrameLeaf(Frame* parent)
    : Frame(parent)
{}

FrameLeaf::~FrameLeaf() = default;

void FrameLeaf::fmap(function<void(FrameSplit*)> onSplit,
                     function<void(FrameLeaf*)> onLeaf, int order) {
    (void) onSplit;
    (void) order;
    onLeaf(this);
}

void FrameLeaf::insertClient(YFrameWindow* frame, bool focus) {
    if (frame == nullptr)
        return;
    int index = std::min(selection_ + (focus ? 1 : 0),
                         int(clients_.size()));
    clients_.insert(clients_.begin() + index, frame);
    if (focus)
        selection_ = index;
}

bool FrameLeaf::removeClient(YFrameWindow* frame) {
    int i = clientIndex(frame);
    if (i < 0)
        return false;
    clients_.erase(clients_.begin() + i);
    if (selection_ > i && !clients_.empty())
        selection_--;
    else if (clients_.empty())
        selection_ = 0;
    else if (selection_ > int(clients_.size()) - 1)
        selection_ = int(clients_.size()) - 1;
    return true;
}

void FrameLeaf::removeClients(size_t first, size_t count) {
    if (first >= clients_.size())
        return;
    size_t end = std::min(first + count, clients_.size());
    clients_.erase(clients_.begin() + first, clients_.begin() + end);
    if (!clients_.empty() && selection_ >= int(clients_.size()))
        selection_ = int(clients_.size()) - 1;
}

void FrameLeaf::removeAllClients(vector<YFrameWindow*>& out) {
    out.insert(out.end(), clients_.begin(), clients_.end());
    clients_.clear();
    selection_ = 0;
}

void FrameLeaf::setSelection(int index) {
    if (index < 0)
        index = 0;
    if (int(clients_.size()) == 0)
        index = 0;
    else if (index >= int(clients_.size()))
        index = int(clients_.size()) - 1;
    selection_ = index;
}

YFrameWindow* FrameLeaf::selectedClient() const {
    if (clients_.empty())
        return nullptr;
    return clients_[std::min(selection_, int(clients_.size()) - 1)];
}

int FrameLeaf::clientIndex(YFrameWindow* frame) const {
    for (size_t i = 0; i < clients_.size(); ++i)
        if (clients_[i] == frame)
            return int(i);
    return -1;
}

// ------------------------------------------------------------------
// FrameTree
// ------------------------------------------------------------------

FrameTree::FrameTree() {
    root_ = new FrameLeaf(nullptr);
}

FrameTree::~FrameTree() {
    delete root_;
}

FrameLeaf* FrameTree::focusedLeaf() const {
    if (root_ == nullptr)
        return nullptr;
    Frame* node = root_;
    while (node->isSplit()) {
        FrameSplit* split = node->asSplit();
        node = split->selectedChild();
    }
    return node->asLeaf();
}

FrameLeaf* FrameTree::ensureRootLeaf() {
    if (root_ == nullptr)
        root_ = new FrameLeaf(nullptr);
    return root_->asLeaf();
}

bool FrameTree::focusFrame(Frame* frame) {
    // only leaves may be focused (like hlwm)
    if (frame == nullptr || !frame->isLeaf())
        return false;
    Frame* node = frame;
    while (node->parent()) {
        FrameSplit* split = node->parent()->asSplit();
        if (split->firstChild() == node)
            split->setSelection(0);
        else if (split->secondChild() == node)
            split->setSelection(1);
        node = node->parent();
    }
    return true;
}

Frame* FrameTree::lookup(const string& path) const {
    Frame* node = root_;
    for (char c : path) {
        if (node->isLeaf())
            return nullptr; // dead-end
        FrameSplit* split = node->asSplit();
        switch (c) {
            case '0': node = split->firstChild(); break;
            case '1': node = split->secondChild(); break;
            case '.': node = split->selection() ? split->firstChild()
                                                : split->secondChild();
                      break;
            case '/': node = split->selection() ? split->secondChild()
                                                : split->firstChild();
                      break;
            default: return nullptr;
        }
    }
    return node;
}

string FrameTree::pathTo(Frame* target) const {
    if (target == nullptr)
        return "";
    // walk from target to root collecting edges, then reverse
    string rev;
    Frame* node = target;
    while (node && node->parent()) {
        FrameSplit* parent = node->parent()->asSplit();
        rev.push_back((parent->firstChild() == node) ? '0' : '1');
        node = node->parent();
    }
    if (node != root_)
        return ""; // not in this tree
    string path(rev.rbegin(), rev.rend());
    return path;
}

FrameLeaf* FrameTree::splitLeaf(FrameLeaf* leaf, FrameAlign align,
                                double fraction) {
    if (leaf == nullptr)
        return nullptr;
    // guard against exceeding a sane tree depth like hlwm's MAX_TREE_HEIGHT
    if (leaf->splitsToRoot(align) >= 32)
        return nullptr;

    FrameSplit* parent = leaf->parent() ? leaf->parent()->asSplit()
                                        : nullptr;
    auto* newLeaf = new FrameLeaf(parent);
    newLeaf->setAlgorithm(leaf->algorithm());

    // Move the trailing half of the clients to the new leaf,
    // preserving order and adjusting the selection index.
    size_t count = leaf->clientCount();
    size_t leaving = count / 2;
    for (size_t i = count - leaving; i < count; ++i)
        newLeaf->insertClient(leaf->client(i), false);
    leaf->removeClients(count - leaving, leaving);
    if (leaf->selection() >= int(count - leaving))
        leaf->setSelection(int(count - leaving) - 1);

    auto* split = new FrameSplit(parent, align, fraction, leaf, newLeaf);

    if (parent) {
        parent->replaceChild(leaf, split);
    } else {
        root_ = split;
    }
    leaf->setParent(split);
    newLeaf->setParent(split);

    // focus the leaf with the most clients, or the original on a tie
    if (newLeaf->clientCount() > leaf->clientCount())
        focusFrame(newLeaf);
    else
        focusFrame(leaf);
    return newLeaf;
}

void FrameTree::removeLeaf(FrameLeaf* leaf) {
    if (leaf == nullptr)
        return;
    if (leaf == root_) {
        // root leaf: just clear it
        vector<YFrameWindow*> clients;
        leaf->removeAllClients(clients);
        // clients are re-laid out by the caller
        return;
    }
    FrameSplit* parent = leaf->parent()->asSplit();
    Frame* sibling = (parent->firstChild() == leaf)
                         ? parent->secondChild()
                         : parent->firstChild();

    // move leaf's clients to the sibling leaf (nearest: follow focus)
    // If the sibling is a split, go to its focused leaf.
    Frame* target = sibling;
    while (target->isSplit()) {
        target = target->asSplit()->selectedChild();
    }
    FrameLeaf* targetLeaf = target->asLeaf();

    vector<YFrameWindow*> moved;
    leaf->removeAllClients(moved);
    for (YFrameWindow* w : moved)
        targetLeaf->insertClient(w, true);

    // replace parent in grandparent with sibling
    Frame* grandparent = parent->parent();
    if (grandparent) {
        FrameSplit* gp = grandparent->asSplit();
        gp->replaceChild(parent, sibling);
        sibling->setParent(gp);
    } else {
        root_ = sibling;
        sibling->setParent(nullptr);
    }
    // detach both children from the parent so its destructor (which
    // deletes both children) cannot free the surviving sibling
    parent->detachChildren();
    delete parent; // frees the removed leaf only
    focusFrame(targetLeaf);
}

bool FrameTree::removeFocusedLeaf() {
    FrameLeaf* leaf = focusedLeaf();
    if (leaf == nullptr)
        return false;
    removeLeaf(leaf);
    return true;
}

size_t FrameTree::leafCount() const {
    size_t n = 0;
    root_->fmap([] (FrameSplit*) {},
                [&] (FrameLeaf*) { ++n; });
    return n;
}

string FrameTree::dump() const {
    std::ostringstream out;
    function<void(Frame*)> rec = [&] (Frame* f) {
        FrameSplit* split = f->isSplit() ? f->asSplit() : nullptr;
        FrameLeaf* leaf = f->isLeaf() ? f->asLeaf() : nullptr;
        if (split) {
            out << "(split "
                << (split->align() == FrameAlign::vertical ? "v" : "h")
                << " "
                << static_cast<int>(split->fraction() * 1000 + 0.5)
                << " "
                << split->selection()
                << " ";
            rec(split->firstChild());
            out << " ";
            rec(split->secondChild());
            out << ")";
        } else if (leaf) {
            out << "(clients "
                << int(leaf->algorithm());
            if (!leaf->label().empty())
                out << " " << leaf->label();
            out << ")";
        }
    };
    rec(root_);
    return out.str();
}

// A tiny S-expression parser for the dump format used by
// tiling load. Grammar (whitespace-separated):
//   frame := '(' 'split' dir fraction selection frame frame ')'
//         | '(' 'clients' algorithm [ label ] ')'
// Clients themselves are NOT encoded (IceWM session restores which client
// goes where separately); this only encodes the tree shape and labels.
bool FrameTree::load(const string& text, string& error) {
    // tokenize into tokens, ignoring whitespace and parentheses as
    // separate tokens
    vector<string> tokens;
    string cur;
    for (char c : text) {
        if (c == '(' || c == ')') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            tokens.push_back(string(1, c));
        } else if (isspace((unsigned char)c)) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);

    size_t pos = 0;
    function<Frame*()> parseFrame = [&] () -> Frame* {
        if (pos >= tokens.size() || tokens[pos] != "(") {
            error = "expected '('";
            return nullptr;
        }
        ++pos; // consume '('
        if (pos >= tokens.size()) { error = "unexpected end"; return nullptr; }
        if (tokens[pos] == "split") {
            ++pos;
            if (pos + 2 >= tokens.size()) { error = "split spec incomplete"; return nullptr; }
            const string& dirStr = tokens[pos];
            const string& fracStr = tokens[pos + 1];
            const string& selStr = tokens[pos + 2];
            pos += 3;
            FrameAlign align = (dirStr == "h") ? FrameAlign::horizontal
                                               : FrameAlign::vertical;
            int frac1000 = atoi(fracStr.c_str());
            double fraction = clampFraction(frac1000 / 1000.0);
            int selection = atoi(selStr.c_str());
            Frame* a = parseFrame();
            Frame* b = parseFrame();
            if (a == nullptr || b == nullptr) { error = "invalid children"; return nullptr; }
            if (pos >= tokens.size() || tokens[pos] != ")") { error = "expected ')'"; return nullptr; }
            ++pos;
            auto* split = new FrameSplit(nullptr, align, fraction, a, b);
            split->setSelection(selection);
            return split;
        } else if (tokens[pos] == "clients") {
            ++pos;
            if (pos >= tokens.size()) { error = "clients spec incomplete"; return nullptr; }
            string alg = tokens[pos];
            ++pos;
            string label;
            if (pos < tokens.size() && tokens[pos] != ")") {
                label = tokens[pos];
                ++pos;
            }
            if (pos >= tokens.size() || tokens[pos] != ")") { error = "expected ')'"; return nullptr; }
            ++pos;
            auto* leaf = new FrameLeaf(nullptr);
            leaf->setAlgorithm(static_cast<FrameAlgorithm>(atoi(alg.c_str())));
            if (!label.empty())
                leaf->setLabel(label);
            return leaf;
        }
        error = "unknown token '" + tokens[pos] + "'";
        return nullptr;
    };

    Frame* newRoot = parseFrame();
    if (newRoot == nullptr || pos != tokens.size()) {
        error = error.empty() ? "unexpected trailing tokens" : error;
        delete newRoot;
        return false;
    }
    delete root_;
    root_ = newRoot;
    return true;
}