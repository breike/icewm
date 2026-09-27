// frametree.h — herbstluftwm-style tiling frame tree for IceWM.
//
// A FrameTree is a binary tree of FrameSplit (internal) / FrameLeaf
// (window container) nodes. Each workspace owns one FrameTree. Every
// non-floating client (YFrameWindow*) is bound to exactly one FrameLeaf.
// Focus is encoded in the tree: each FrameSplit has a selection_ bit;
// walking the bits from the root yields the focused leaf, exactly like
// herbstluftwm.
//
// Author: tiling port (herbstluftwm model) for IceWM.

#ifndef ICEWM_FRAMETREE_H
#define ICEWM_FRAMETREE_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base.h"      // PRECONDITION
#include "yfull.h"     // X11 types used by yrect.h
#include "yrect.h"

class YFrameWindow;

class Frame;        // base of the binary frame tree
class FrameSplit;   // internal (split) node
class FrameLeaf;    // leaf (window container) node

enum class FrameAlign {
    vertical = 0,   // split horizontally (children stacked vertically)
    horizontal,     // split vertically (children side by side)
};

enum class FrameAlgorithm {
    vertical = 0,
    horizontal,
    max,
    grid,
};

// Convert a FrameAlign to the algorithm a leaf would use for "
// split explode" (kept for symmetry with hlwm).
FrameAlgorithm frameAlignToAlgorithm(FrameAlign align);

/*! The 0..1 split fraction of a FrameSplit, clamped to a sane range.
 * Simple double-based counter part of hlwm's FixPrecDec.
 */
inline double clampFraction(double f) {
    const double minFrac = 0.05;
    if (f < minFrac) return minFrac;
    if (f > 1.0 - minFrac) return 1.0 - minFrac;
    return f;
}

/*! Base class of the binary frame tree. */
class Frame {
public:
    Frame(Frame* parent) : parent_(parent) {}
    virtual ~Frame() = default;

    Frame* parent() const { return parent_; }
    void setParent(Frame* parent) { parent_ = parent; }

    virtual bool isSplit() const = 0;
    virtual bool isLeaf() const = 0;
    virtual FrameSplit* asSplit() { return nullptr; }
    virtual const FrameSplit* asSplit() const { return nullptr; }
    virtual FrameLeaf* asLeaf() { return nullptr; }
    virtual const FrameLeaf* asLeaf() const { return nullptr; }

    /*! Recursively apply onSplit to splits and onLeaf to leaves.
     * order: 0 = pre-order, 1 = in-order, 2 = post-order.
     */
    virtual void fmap(std::function<void(FrameSplit*)> onSplit,
                      std::function<void(FrameLeaf*)> onLeaf,
                      int order = 0) = 0;

    /*! If this frame is the first child of its parent return '0',
     * if the second child return '1', if root return "".
     */
    std::string edgeToParent() const;

    /*! The absolute index string ("" for root, ""+children "0"/"1"...). */
    std::string frameIndex() const;

    /*! The root of this tree. */
    Frame* root();

    /*! Count of splits of the given alignment up to the root. */
    int splitsToRoot(FrameAlign align) const;

protected:
    Frame* parent_;
};

class FrameSplit : public Frame {
public:
    FrameSplit(Frame* parent, FrameAlign align, double fraction,
               Frame* a, Frame* b);
    ~FrameSplit() override;

    bool isSplit() const override { return true; }
    bool isLeaf() const override { return false; }
    FrameSplit* asSplit() override { return this; }
    const FrameSplit* asSplit() const override { return this; }
    FrameLeaf* asLeaf() override { return nullptr; }
    const FrameLeaf* asLeaf() const override { return nullptr; }

    void fmap(std::function<void(FrameSplit*)> onSplit,
              std::function<void(FrameLeaf*)> onLeaf,
              int order = 0) override;

    FrameAlign align() const { return align_; }
    void setAlign(FrameAlign a) { align_ = a; }

    double fraction() const { return fraction_; }
    void setFraction(double f) { fraction_ = clampFraction(f); }

    int selection() const { return selection_ ? 1 : 0; }
    void setSelection(int s) { selection_ = (s != 0); }
    void swapSelection() { selection_ = !selection_; }

    Frame* firstChild() const { return a_; }
    Frame* secondChild() const { return b_; }
    Frame* selectedChild() const { return selection_ ? b_ : a_; }
    Frame* unselectedChild() const { return selection_ ? a_ : b_; }
    void replaceChild(Frame* old, Frame* child);
    void swapChildren();

    //! Detach both children so the destructor does not delete them.
    void detachChildren() { a_ = nullptr; b_ = nullptr; }

private:
    FrameAlign align_;
    double fraction_;
    bool selection_;        // false = first child, true = second child
    Frame* a_;
    Frame* b_;
};

class FrameLeaf : public Frame {
public:
    explicit FrameLeaf(Frame* parent);
    ~FrameLeaf() override;

    bool isSplit() const override { return false; }
    bool isLeaf() const override { return true; }
    FrameSplit* asSplit() override { return nullptr; }
    const FrameSplit* asSplit() const override { return nullptr; }
    FrameLeaf* asLeaf() override { return this; }
    const FrameLeaf* asLeaf() const override { return this; }

    void fmap(std::function<void(FrameSplit*)> onSplit,
              std::function<void(FrameLeaf*)> onLeaf,
              int order = 0) override;

    // --- client management -----------------------------------------
    void insertClient(YFrameWindow* frame, bool focus = true);
    bool removeClient(YFrameWindow* frame);
    void removeAllClients(std::vector<YFrameWindow*>& out);
    //! remove clients [first, first+count) from the vector
    void removeClients(size_t first, size_t count);
    YFrameWindow* client(size_t i) const { return i < clients_.size() ? clients_[i] : nullptr; }
    const std::vector<YFrameWindow*>& clients() const { return clients_; }
    size_t clientCount() const { return clients_.size(); }
    bool isEmpty() const { return clients_.empty(); }

    int selection() const { return selection_; }
    void setSelection(int index);
    YFrameWindow* selectedClient() const;
    int clientIndex(YFrameWindow* frame) const;

    FrameAlgorithm algorithm() const { return algorithm_; }
    void setAlgorithm(FrameAlgorithm a) { algorithm_ = a; }

    //! optional user-visible name; empty if unset
    const std::string& label() const { return label_; }
    void setLabel(const std::string& s) { label_ = s; }

private:
    std::vector<YFrameWindow*> clients_;
    int selection_ = 0;
    FrameAlgorithm algorithm_ = FrameAlgorithm::vertical;
    std::string label_;
};

/*! The tree itself: owns the root and provides tree commands. */
class FrameTree {
public:
    FrameTree();
    ~FrameTree();

    Frame* root() const { return root_; }
    void setRoot(Frame* root) { root_ = root; }

    FrameLeaf* focusedLeaf() const;

    //! Ensure the tree has a root leaf (creating an empty one if it is
    //! empty). Returns the root leaf.
    FrameLeaf* ensureRootLeaf();

    bool focusFrame(Frame* frame);
    bool focusByIndex(const std::string& index);
    Frame* lookup(const std::string& path) const;
    std::string pathTo(Frame* frame) const;

    //! split the focused leaf (or the leaf found in 'path') into two
    //! leaves; returns the new leaf number-2 (the second child) or
    //! nullptr on failure.
    FrameLeaf* splitLeaf(FrameLeaf* leaf, FrameAlign align,
                         double fraction);

    //! remove the given leaf: reparent its clients into the sibling and
    //! collapse the parent split. If the leaf is the root, clear it.
    void removeLeaf(FrameLeaf* leaf);
    //! remove the focused leaf, returning whether removed.
    bool removeFocusedLeaf();

    //! number of leaves
    size_t leafCount() const;

    //! serialize/deserialize (hlwm-style S-expression without clients:
    //! "(split v:fraction:selection ...)" / "(clients alg:sel ...labels? )")
    std::string dump() const;
    bool load(const std::string& layoutString, std::string& error);

private:
    Frame* root_ = nullptr;
};

#endif // ICEWM_FRAMETREE_H