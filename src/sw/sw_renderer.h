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
    uint64_t pxWritten;
    uint64_t pxFastCopy;
    uint64_t pxVectorBlend;
    uint64_t pxScalarBlend;
    uint64_t pxRotated;
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

// Turns the per-draw log on or off. Used with --trace-frame: the frame's draws are
// logged so they can be compared against the instances that were on stage.
void SWRenderer_setDrawTrace(Renderer* renderer, bool enabled);

// Draws a short debug string (fps, memory) straight into the framebuffer using the
// built-in 8x8 debug font, with a one-pixel shadow so it stays readable over any
// scene. `scale` is 0..1 of the font's natural size; 0.25 gives readable 11px text
// at 640x480. Only the software renderer has this, which is where it is needed.
void SWRenderer_drawOverlayText(Renderer* renderer, int x, int y, float scale, const char* text);

// Same text, but laid out so that every line ends `marginRight` pixels from the right
// edge of the framebuffer. Used for the short-lived "FRAME SKIP ON/OFF" banner.
void SWRenderer_drawOverlayTextRight(Renderer* renderer, int marginRight, int y, float scale,
                                     const char* text);

// Writes the finished framebuffer to `path` as an RGBA PNG. Used by the debug
// capture key so a bug can be reported without a screen grabber: the software
// renderer has no GL readback path, so this is the only way to get the exact
// pixels the game produced. Returns false when there is nothing to write.
bool SWRenderer_captureFramebuffer(Renderer* renderer, const char* path);

#endif//_SW_RENDERER_H
