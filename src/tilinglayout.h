// tilinglayout.h — computes the geometry of each leaf of a FrameTree
// from a work-area rectangle. Mirror of herbstluftwm's layoutalgoimpl.
//
// Author: tiling port (herbstluftwm model) for IceWM.

#ifndef ICEWM_TILINGLAYOUT_H
#define ICEWM_TILINGLAYOUT_H

#include "frametree.h"
#include "yrect.h"

class YFrameWindow;

/*! Result of applying the layout to one leaf. */
struct TilingGeometry {
    YRect frameRect;    //!< the leaf's rectangle (outer frame)
    YRect clientRect;   //!< the inner client rectangle (minus borders/title)
};

/*! Compute the geometry for every leaf, in traversal (pre-) order.
 * Returns a vector parallel to the leaves in pre-order.
 */
std::vector<TilingGeometry>
applyTilingLayout(const FrameTree& tree, const YRect& workArea,
                  int gap, int borderWidth, int titleHeight);

/*! True if the given YFrameWindow should participate in tiling at all
 * (i.e. is not floating, not minimized, not hidden, not fullscreen,
 * not a dock/desktop, etc.). This is the leaf-membership predicate.
 */
bool isTilingCandidate(const YFrameWindow* frame);

#endif // ICEWM_TILINGLAYOUT_H