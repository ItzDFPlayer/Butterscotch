#ifndef _SW_DRAW_PROFILE_H
#define _SW_DRAW_PROFILE_H

#include "common.h"

// ===[ Draw cost profiler ]===
//
// A frame in a heavy room issues a few hundred draws, and the pixel counters only
// say how many pixels went through - not which draws spent the time. This times
// every sprite/tile/surface draw and keeps the totals per texture-page item, so the
// report names the actual offenders ("tpag 812, 190 draws, 21 ms").
//
// Enabled with BUTTERSCOTCH_PROFILE_DRAWS=1; the report is printed once a second.
// Off by default because the timing itself (two clock reads per draw) is not free.
//
// On this hardware a draw is either pixel-bound (large, translucent layer) or
// overhead-bound (thousands of small tiles), and the two want opposite fixes, so
// knowing which one it is decides what to optimise next.
enum {
    SWR_DRAW_SPRITE = 0,
    SWR_DRAW_SPRITE_PART = 1,
    SWR_DRAW_SPRITE_POS = 2,
    SWR_DRAW_SPRITE_TILED = 3,
    SWR_DRAW_SURFACE = 4,
    SWR_DRAW_RECT = 5,
};

// Starts timing a draw. Returns 0 when profiling is off, which makes the matching
// call to swrDrawProfileEnd() a no-op.
uint64_t swrDrawProfileBegin(void);
void swrDrawProfileEnd(int kind, int key, int pixels, uint64_t started);
bool swrDrawProfileEnabled(void);

// Prints the heaviest draws of the last second and resets the counters.
void SWRenderer_reportDrawProfile(void);

#endif//_SW_DRAW_PROFILE_H
