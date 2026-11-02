#ifndef _SW_RENDERER_H
#define _SW_RENDERER_H

#include "renderer.h"
// For uintpixel_t, which the overlay helpers take as a colour type.
#include "pixel_convert.h"

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

// True when BUTTERSCOTCH_LOG_BLEND is set. Gates the draw-state log (blend mode and
// factors, colour write mask, pixel mode) and the surface pipeline log (create,
// target, clear, composite, plus every draw made into a surface), which together
// describe an effect that renders as a flat colour or a missing layer.
int swrDiagLogEnabled(void);

// Draws a short debug string (fps, memory) straight into the framebuffer using the
// built-in 8x8 debug font, with a one-pixel shadow so it stays readable over any
// scene. `scale` is 0..1 of the font's natural size; 0.25 gives readable 11px text
// at 640x480. Only the software renderer has this, which is where it is needed.
void SWRenderer_drawOverlayText(Renderer* renderer, int x, int y, float scale, const char* text);

// Same text, but laid out so that every line ends `marginRight` pixels from the right
// edge of the framebuffer. Used for the short-lived "FRAME SKIP ON/OFF" banner.
void SWRenderer_drawOverlayTextRight(Renderer* renderer, int marginRight, int y, float scale,
                                     const char* text);

// Same text in an explicit colour (RGB555, no alpha bit). The shadow stays black, so
// a bright colour reads on both light and dark scenes; a dark colour would not, which
// is why the menu marks its selection with colour instead of an inverted row.
void SWRenderer_drawOverlayTextColor(Renderer* renderer, int x, int y, float scale,
                                     const char* text, uintpixel_t color);

// Width and line height of overlay text at the given scale, so a caller can size a
// panel around it instead of guessing.
int SWRenderer_measureOverlayText(Renderer* renderer, float scale, const char* text);
int SWRenderer_overlayLineHeight(float scale);

// Fills a rectangle straight into the framebuffer, blended towards `alpha` (0..256).
// `color` is RGB555 without the alpha bit. Used to put a readable panel behind overlay
// text; the game's own drawRectangle goes through the viewport transform, which is not
// what screen-space overlay widgets want.
void SWRenderer_fillOverlayRect(Renderer* renderer, int x, int y, int width, int height,
                                uintpixel_t color, int alpha);

// Writes the finished framebuffer to `path` as an RGBA PNG. Used by the debug
// capture key so a bug can be reported without a screen grabber: the software
// renderer has no GL readback path, so this is the only way to get the exact
// pixels the game produced. Returns false when there is nothing to write.
bool SWRenderer_captureFramebuffer(Renderer* renderer, const char* path);

#endif//_SW_RENDERER_H
