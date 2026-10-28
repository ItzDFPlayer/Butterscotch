#ifndef _SW_RENDERER_H
#define _SW_RENDERER_H

#include "renderer.h"

Renderer* SWRenderer_create(void);

void SWRenderer_clearFrameBuffer(Renderer* renderer, uint32_t color);

// Draws a loading bar straight into the framebuffer (used before the game
// starts, e.g. while pre-baking textures).
void SWRenderer_showLoadingProgress(Renderer* renderer, int32_t width, int32_t height, float progress);

// Live decoded-texture cache figures, used by --perf-log to correlate hitches
// with texture churn. `loads` and `evictions` are cumulative for the session.
typedef struct {
    uint32_t textureBytes;
    uint32_t budgetBytes;
    uint32_t loads;
    uint32_t evictions;
    uint64_t drawsAttempted;
    uint64_t drawsCulled;
} SWRendererTextureStats;

void SWRenderer_getTextureStats(Renderer* renderer, SWRendererTextureStats* out);

// Decodes and uploads one texture page ahead of the frame that draws it.
// Returns true when the page is resident afterwards. Evicting is left to the
// normal cache accounting, so this never grows past the budget.
bool SWRenderer_prefetchTexturePage(Renderer* renderer, int32_t pageId);

// Best-effort decoded size of a page, without decoding it (0 when unknown).
uint32_t SWRenderer_estimateTexturePageBytes(Renderer* renderer, int32_t pageId);

// True when this page is stored with strip checkpoints, i.e. decoded per strip on
// demand. Prefetching such a page would defeat the memory saving.
bool SWRenderer_pageIsSubPage(Renderer* renderer, int32_t pageId);

#endif//_SW_RENDERER_H
