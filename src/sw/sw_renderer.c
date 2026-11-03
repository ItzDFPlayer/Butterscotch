#include <stdio.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include "text_utils.h"
#include "image/image_decoder.h"
#include "stb_image_write.h"
#include "debug_font/debug_font.h"

#include "sw_renderer_private.h"
#include "sw_draw_profile.h"

// Set by the draw entries once they know the destination size, read by the timing
// wrappers below: the alternative is threading an out-parameter through every blit.
static int sProfileLastPixels;

void platformSetNextFramebuffer(uintpixel_t* framebuffer, int width, int height);
void platformSwapBuffers(void);

// Pick a decoded-texture budget that fits the device. Deltarune rooms commonly
// touch five or six 2048x2048 pages (40-48 MB), so smaller budgets evict/reload a
// page every cycle. 48 MB fits that working set on a 128 MB Miyoo while leaving
// room for the heap and decode scratch buffers; larger machines get extra room.
static unsigned long swrReadMemTotalKb(void) {
#if defined(__linux__)
    FILE* meminfo = fopen("/proc/meminfo", "r");
    if (meminfo != NULL) {
        char line[128];
        unsigned long kb = 0;
        while (fgets(line, sizeof(line), meminfo) != NULL) {
            if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
                break;
            }
        }
        fclose(meminfo);
        return kb;
    }
#endif
    return 0;
}

static size_t swrDefaultTextureBudget(unsigned long memTotalKb) {
    // 48 MB fits the six 2048x2048 pages that a Deltarune room commonly touches.
    if (memTotalKb > 0 && memTotalKb <= 192ul * 1024ul) {
        return 48u * 1024u * 1024u;
    }
    return 64u * 1024u * 1024u;
}

static size_t swrMaxTextureBudget(unsigned long memTotalKb) {
    // Allow the cache to grow one/two pages when a scene's working set exceeds
    // the initial budget, while keeping headroom for the heap and OS.
    if (memTotalKb > 0 && memTotalKb <= 192ul * 1024ul) {
        return 64u * 1024u * 1024u;
    }
    return 256u * 1024u * 1024u;
}

static void SWRenderer_init(Renderer* renderer, DataWin* dataWin)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    renderer->dataWin = dataWin;
    
    //allocate texture buffer
    swr->textureCount = dataWin->txtr.count;
    swr->surfaceCount = SURFACE_MAX_COUNT;
    swr->totalTextureCount = swr->textureCount + swr->surfaceCount;
    swr->textures = (SWTexture**) safeCalloc(swr->totalTextureCount, sizeof(SWTexture*));
    swr->surfaces = (SWSurface**) safeCalloc(swr->surfaceCount, sizeof(SWSurface*));
    
    swr->textureUseClock = 0;
    swr->textureBytes = 0;
    swr->recentEvictedNext = 0;
    for (int i = 0; i < 8; i++) {
        swr->recentEvictedPages[i] = UINT32_MAX;
        swr->recentEvictedTicks[i] = 0;
    }

    unsigned long memTotalKb = swrReadMemTotalKb();
    swr->textureBudgetBytes = swrDefaultTextureBudget(memTotalKb);
    swr->textureBudgetMax = swrMaxTextureBudget(memTotalKb);
    const char* textureBudgetEnv = getenv("BUTTERSCOTCH_TEXTURE_MB");
    if (textureBudgetEnv != NULL && textureBudgetEnv[0] != '\0') {
        long mb = strtol(textureBudgetEnv, NULL, 10);
        if (mb > 0) {
            swr->textureBudgetBytes = (size_t) mb * 1024 * 1024;
        }
    }
    const char* textureMaxEnv = getenv("BUTTERSCOTCH_TEXTURE_MAX_MB");
    if (textureMaxEnv != NULL && textureMaxEnv[0] != '\0') {
        long mb = strtol(textureMaxEnv, NULL, 10);
        if (mb > 0) {
            // Explicit maximum disables (or bounds) adaptive growth.
            swr->textureBudgetMax = (size_t) mb * 1024 * 1024;
        }
    }
    if (swr->textureBudgetMax < swr->textureBudgetBytes) {
        swr->textureBudgetMax = swr->textureBudgetBytes;
    }
    fprintf(stderr, "SWR: texture cache budget: %zu MB (max %zu MB)\n",
            swr->textureBudgetBytes / (1024 * 1024), swr->textureBudgetMax / (1024 * 1024));
    
    //HACK: this isn't good, really.  This should seriously be refactored.
    //expand datawin's tpag items list to include our surface count.
    swr->originalTPagCount = dataWin->tpag.count;
    dataWin->tpag.items = (TexturePageItem*) safeRealloc(dataWin->tpag.items, sizeof(TexturePageItem) * (dataWin->tpag.count + swr->surfaceCount));
    dataWin->tpag.count += swr->surfaceCount;
    
    swr->originalSpriteCount = dataWin->sprt.count;

    // Default GPU state: normal blending, everything written, no alpha test.
    swr->blendMode = bm_normal;
    swr->blendEnabled = true;
    swr->blendSrcFactor = 0;
    swr->blendDstFactor = 0;
    swr->alphaTestEnabled = false;
    swr->alphaTestRef = 0;
    swr->writeMask = WRITE_MASK_ALL;
    // No shader is bound until the game binds one; the emulation reads this.
    swr->shaderIndex = -1;
    swrSetupBlendState(swr);
    
    for (size_t i = swr->originalTPagCount; i < dataWin->tpag.count; i++)
    {
        memset(&dataWin->tpag.items[i], 0, sizeof(TexturePageItem));
        dataWin->tpag.items[i].texturePageId = -1;
    }
    
    fprintf(stderr, "SWRenderer initialized.\n");
}

static void SWRenderer_destroy(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    // TODO: why didn't I implement this.
    (void) swr;
    
    fprintf(stderr, "SWRenderer destroyed.\n");
}

static void SWRenderer_beginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->gameW = gameW;
    swr->gameH = gameH;
    swr->drawingToSurface = false;
    // The frame always starts on the host framebuffer, so the remembered target must
    // not survive into it: a stale id would be indexed on the next surface switch.
    swr->currentSurfaceIndex = -1;
    // A shader bound in one frame must not leak into the next: the games always
    // bind and reset around the draw that needs it.
    swr->shaderIndex = -1;
    swr->blendMode = bm_normal;
    // GameMaker resets the blend unit at the start of every frame; the mask
    // scripts depend on that, otherwise their alpha-only writes leak into the
    // next frame's normal blending.
    swr->blendEnabled = true;
    swr->blendSrcFactor = 0;
    swr->blendDstFactor = 0;
    swr->alphaTestEnabled = false;
    swr->alphaTestRef = 0;
    swr->writeMask = WRITE_MASK_ALL;
    swrSetupBlendState(swr);

    if (swr->width != windowW || swr->height != windowH)
    {
        //allocate frame buffer
        free(swr->fb);
        swr->fb = (uintpixel_t*) safeMalloc(windowW * windowH * sizeof(uintpixel_t));
        swr->fbPitch = windowW;
        swr->width = windowW;
        swr->height = windowH;
    }
}

// This used to be just one, "endFrame". Not sure what the difference is.
static void SWRenderer_endFrameInit(Renderer* renderer)
{
    (void) renderer;
    
    //this is kinda useless to do twice isn't it?
}

static void SWRenderer_endFrameEnd(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    assert(!swrIsOnSurface(swr));
    platformSetNextFramebuffer(swr->fb, swr->width, swr->height);
}

static uintpixel_t swrLoadingColor(uint8_t r, uint8_t g, uint8_t b)
{
    return swrConvertPixel((uint32_t) r | ((uint32_t) g << 8) | ((uint32_t) b << 16));
}

void SWRenderer_showLoadingProgress(Renderer* renderer, int32_t width, int32_t height, float progress)
{
    if (renderer == NULL || renderer->vtable == NULL || width <= 0 || height <= 0) return;

    if (renderer->vtable->beginFrame != NULL) {
        renderer->vtable->beginFrame(renderer, width, height, width, height);
    }

    SWRenderer* swr = (SWRenderer*) renderer;
    if (swr->fb == NULL || swr->width == 0 || swr->height == 0 || swr->fbPitch == 0) return;

    uintpixel_t background = swrLoadingColor(12, 12, 18);
    uintpixel_t border = swrLoadingColor(90, 90, 110);
    uintpixel_t track = swrLoadingColor(30, 40, 60);
    uintpixel_t fill = swrLoadingColor(110, 190, 255);

    int w = swr->width;
    int h = swr->height;
    size_t pitch = swr->fbPitch;
    uintpixel_t* fb = swr->fb;

    for (int y = 0; y < h; y++) {
        uintpixel_t* row = fb + (size_t) y * pitch;
        for (int x = 0; x < w; x++) row[x] = background;
    }

    int barW = w * 3 / 4;
    int barH = h / 20;
    if (barH < 6) barH = 6;
    if (barW < 16) barW = w - 8;
    int x0 = (w - barW) / 2;
    int y0 = (h - barH) / 2;

    for (int y = y0 - 2; y < y0 + barH + 2; y++) {
        if (y < 0 || y >= h) continue;
        uintpixel_t* row = fb + (size_t) y * pitch;
        for (int x = x0 - 2; x < x0 + barW + 2; x++) {
            if (x >= 0 && x < w) row[x] = border;
        }
    }
    for (int y = y0; y < y0 + barH; y++) {
        uintpixel_t* row = fb + (size_t) y * pitch;
        for (int x = x0; x < x0 + barW; x++) row[x] = track;
    }

    float p = progress;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    int filled = (int) (barW * p + 0.5f);
    for (int y = y0; y < y0 + barH; y++) {
        uintpixel_t* row = fb + (size_t) y * pitch;
        for (int x = x0; x < x0 + filled; x++) row[x] = fill;
    }

    platformSetNextFramebuffer(fb, w, h);
    platformSwapBuffers();
}

void SWRenderer_getTextureStats(Renderer* renderer, SWRendererTextureStats* out)
{
    if (out == NULL) return;
    out->textureBytes = 0;
    out->budgetBytes = 0;
    out->loads = 0;
    out->evictions = 0;
    if (renderer == NULL) return;

    SWRenderer* swr = (SWRenderer*) renderer;
    out->textureBytes = (uint32_t) swr->textureBytes;
    out->budgetBytes = (uint32_t) swr->textureBudgetBytes;
    out->loads = swr->textureLoads;
    out->evictions = swr->textureEvictions;
    out->drawsAttempted = swr->drawsAttempted;
    out->drawsCulled = swr->drawsCulled;
    out->pxWritten = swr->pxWritten;
    out->pxFastCopy = swr->pxFastCopy;
    out->pxVectorBlend = swr->pxVectorBlend;
    out->pxScalarBlend = swr->pxScalarBlend;
    out->pxRotated = swr->pxRotated;
    out->pxWritten = swr->pxWritten;
    out->pxFastCopy = swr->pxFastCopy;
}

bool SWRenderer_prefetchTexturePage(Renderer* renderer, int32_t pageId)
{
    if (renderer == NULL) return false;

    SWRenderer* swr = (SWRenderer*) renderer;
    if (pageId < 0 || (uint32_t) pageId >= swr->textureCount) return false;
    if (swr->textures[pageId] != NULL) return true; // already resident

    // Sub-page pages decode per strip when drawn, which costs a fraction of a
    // whole page; materialising all of it here would undo the memory saving.
    if (SWRenderer_pageIsSubPage(renderer, pageId)) return true;

    return swrEnsureTextureIsLoaded(swr, (uint32_t) pageId);
}

void SWRenderer_setDrawTrace(Renderer* renderer, bool enabled)
{
    if (renderer == nullptr) return;
    ((SWRenderer*) renderer)->traceDraws = enabled ? 1 : 0;
}

// Writes one surface to a PNG, with the 1-bit transparent flag turned into a real
// alpha channel so an empty pixel is distinguishable from a black one. Debug aid:
// surfaces are where the game builds its masks, and a mask that is white where it
// should be black looks identical to "the blend is wrong" until you see the mask.
static bool swrDumpSurface(SWRenderer* swr, int index, const char* path)
{
    if (index < 0 || (size_t) index >= swr->surfaceCount) return false;
    SWSurface* surface = swr->surfaces[index];
    if (surface == nullptr || surface->texture == nullptr || surface->texture->buffer == nullptr)
        return false;

    const int32_t w = (int32_t) surface->texture->width;
    const int32_t h = (int32_t) surface->texture->height;
    uint8_t* rgba = (uint8_t*) safeMalloc((size_t) w * (size_t) h * 4);
    if (rgba == nullptr) return false;

    for (int32_t y = 0; y < h; y++)
    {
        const uintpixel_t* row = &surface->texture->buffer[(size_t) y * (size_t) w];
        uint8_t* out = rgba + (size_t) y * (size_t) w * 4;
        for (int32_t x = 0; x < w; x++)
        {
            const uintpixel_t px = row[x];
            out[x * 4 + 0] = (uint8_t) (((px >> 10) & 0x1F) * 255 / 31);
            out[x * 4 + 1] = (uint8_t) (((px >> 5) & 0x1F) * 255 / 31);
            out[x * 4 + 2] = (uint8_t) ((px & 0x1F) * 255 / 31);
            // Surfaces carry real coverage; fall back to the transparent bit for
            // anything that does not.
            out[x * 4 + 3] = (surface->texture->alphaPlane != NULL)
                ? surface->texture->alphaPlane[(size_t) y * (size_t) w + x]
                : ((px & SWR_ALPHA_BIT) ? 255 : 0);
        }
    }

    const int ok = stbi_write_png(path, w, h, 4, rgba, w * 4);
    free(rgba);
    return ok != 0;
}

bool SWRenderer_captureFramebuffer(Renderer* renderer, const char* path)
{
    if (renderer == nullptr || path == nullptr) return false;

    SWRenderer* swr = (SWRenderer*) renderer;
    if (swr->fb == nullptr || swr->width <= 0 || swr->height <= 0) return false;

    int32_t w = swr->width, h = swr->height;
    uint8_t* rgba = (uint8_t*) safeMalloc((size_t) w * (size_t) h * 4);
    if (rgba == nullptr) return false;

    for (int32_t y = 0; y < h; y++)
    {
        const uintpixel_t* row = &swr->fb[(size_t) y * swr->fbPitch];
        uint8_t* out = rgba + (size_t) y * (size_t) w * 4;
        for (int32_t x = 0; x < w; x++)
        {
            uintpixel_t px = row[x];
            out[x * 4 + 0] = (uint8_t) (((px >> 10) & 0x1F) * 255 / 31);
            out[x * 4 + 1] = (uint8_t) (((px >> 5) & 0x1F) * 255 / 31);
            out[x * 4 + 2] = (uint8_t) ((px & 0x1F) * 255 / 31);
            out[x * 4 + 3] = 255;
        }
    }

    int ok = stbi_write_png(path, w, h, 4, rgba, w * 4);
    free(rgba);

    {
        const char* dump = getenv("BUTTERSCOTCH_DUMP_SURFACES");
        if (dump != nullptr && dump[0] != '\0' && dump[0] != '0')
        {
            for (int i = 0; i < (int) swr->surfaceCount; i++)
            {
                char surfacePath[512];
                snprintf(surfacePath, sizeof(surfacePath), "swsurf_%d.png", i);
                if (swrDumpSurface(swr, i, surfacePath))
                    logInfo("Surface dump: %s (%dx%d)\n", surfacePath,
                            (int) swr->surfaces[i]->texture->width,
                            (int) swr->surfaces[i]->texture->height);
            }
        }
    }

    return ok != 0;
}

bool SWRenderer_pageIsSubPage(Renderer* renderer, int32_t pageId)
{
    if (renderer == NULL || renderer->dataWin == NULL) return false;
    BakedPageView view;
    return DataWin_getBakedPage(renderer->dataWin, pageId, &view) && view.checkpoints != NULL;
}

uint32_t SWRenderer_estimateTexturePageBytes(Renderer* renderer, int32_t pageId)
{
    if (renderer == NULL || renderer->dataWin == NULL) return 0;

    DataWin* dw = renderer->dataWin;
    if (pageId < 0 || (uint32_t) pageId >= dw->txtr.count) return 0;

    Texture* txtr = &dw->txtr.textures[pageId];
    if (txtr->textureWidth > 0 && txtr->textureHeight > 0) {
        return (uint32_t) txtr->textureWidth * (uint32_t) txtr->textureHeight * (uint32_t) sizeof(uintpixel_t);
    }

    // Older WAD versions do not store the page size in TXTR; read it from the
    // baked QOI header instead, which needs no decoding.
    size_t bakedSize = 0;
    const uint8_t* baked = DataWin_getBakedTexture(dw, pageId, &bakedSize);
    if (baked != NULL && bakedSize >= 8) {
        uint32_t w = (uint32_t) baked[4] | ((uint32_t) baked[5] << 8);
        uint32_t h = (uint32_t) baked[6] | ((uint32_t) baked[7] << 8);
        if (w > 0 && h > 0) {
            return w * h * (uint32_t) sizeof(uintpixel_t);
        }
    }

    // Unknown: assume a full-size page so the fit check stays conservative.
    return 2048u * 2048u * (uint32_t) sizeof(uintpixel_t);
}


static void SWRenderer_beginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                                 int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle)
{
    (void)renderer; (void)viewX; (void)viewY; (void)viewW; (void)viewH;
    (void)portX; (void)portY; (void)portW; (void)portH; (void)viewAngle;
    UNIMP2();
    
    SWRenderer* swr = (SWRenderer*) renderer;
    
    float xratio, yratio;
    
    float portviewX = (float) portW / viewW;
    float portviewY = (float) portH / viewH;
    
    int offsetX = 0, offsetY = 0;
    
    if (swrIsOnSurface(swr)) {
        UNIMP();
        xratio = 1.0f;
        yratio = 1.0f;
        portX = (int)(portX * xratio);
        portY = (int)(portY * yratio);
    }
    else {
        float scaleX = (float) swr->width  / swr->gameW;
        float scaleY = (float) swr->height / swr->gameH;
        float scale  = (scaleX < scaleY) ? scaleX : scaleY;

        int32_t scaledW = (int32_t)(swr->gameW * scale);
        int32_t scaledH = (int32_t)(swr->gameH * scale);
        
        offsetX = (swr->width  - scaledW) / 2;
        offsetY = (swr->height - scaledH) / 2;

        xratio = scale;
        yratio = scale;

        portX = (int)(portX * xratio) + offsetX;
        portY = (int)(portY * yratio) + offsetY;
    }
    
    swr->scaleX = xratio * portviewX;
    swr->scaleY = yratio * portviewY;
    swr->offsetX = offsetX;
    swr->offsetY = offsetY;
    swr->defaultScaleX = xratio;
    swr->defaultScaleY = yratio;
    
    portW = (int)(portW * xratio);
    portH = (int)(portH * yratio);
    
    swr->viewActive = true;
    swr->viewX = viewX;
    swr->viewY = viewY;
    swr->viewW = viewW;
    swr->viewH = viewH;
    swr->portX = portX;
    swr->portY = portY;
    swr->portW = portW;
    swr->portH = portH;
    swr->maxX = portX + portW;
    swr->maxY = portY + portH;
}

static void SWRenderer_endView(Renderer* renderer)
{
    (void)renderer;
    UNIMP2();
    
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->viewActive = false;
    
    swr->viewX = 0;
    swr->viewY = 0;
    swr->portX = swr->offsetX;
    swr->portY = swr->offsetY;
    swr->portW = swr->viewW = swr->width;
    swr->portH = swr->viewH = swr->height;
    swr->maxX = swr->portX + swr->portW;
    swr->maxY = swr->portY + swr->portH;
    swr->scaleX = swr->defaultScaleX;
    swr->scaleY = swr->defaultScaleY;
}

static void SWRenderer_beginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
                                int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId)
{
    swrSwitchToSurface(renderer, targetSurfaceId, false);
    
    (void)guiW; (void)guiH;
    (void)portX; (void)portY; (void)portW; (void)portH;
    (void)targetSurfaceId;
    UNIMP2();
}

static void SWRenderer_setGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW, int32_t portH, bool renderingToUserSurface)
{
    (void) renderer;
    (void) guiW; (void) guiH;
    (void) portW; (void) portH;
    (void) renderingToUserSurface;
    UNIMP();
}

static void SWRenderer_endGUI(Renderer* renderer)
{
    (void)renderer;
    UNIMP2();
}

// Returns true when a destination rectangle cannot contribute a single pixel to
// the area currently being drawn into. The test mirrors the early-out inside
// swrDrawSpriteInternal()/swrDrawSpriteRotatedInternal(), so culling here never
// changes what ends up on screen - it only avoids decoding a texture page (up to
// 8 MB and a full QOI pass) for content that is nowhere near the viewport.
//
// This matters on low-memory devices: a large room can reference far more
// texture pages than fit in RAM, so paging in pages for off-screen instances
// evicts pages that are still visible and the cache thrashes every frame.
static bool swrDestIsOffscreen(SWRenderer* swr, float dx, float dy, float dw, float dh, bool mayRotate)
{
    if (swr == NULL || swr->fb == NULL) return false;
    if (swr->portW <= 0 || swr->portH <= 0) return false;
    if (swr->maxX <= swr->portX || swr->maxY <= swr->portY) return false;

    swr->drawsAttempted++;

    // Escape hatch in case a game relies on drawing being attempted off-screen.
    static int cullingEnabled = -1;
    if (cullingEnabled < 0) {
        const char* noCull = getenv("BUTTERSCOTCH_NO_CULL");
        cullingEnabled = (noCull != NULL && noCull[0] != '\0' && noCull[0] != '0') ? 0 : 1;
    }
    if (!cullingEnabled) return false;

    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &dw, &dh);

    if (dw < 0.0f) { dx += dw; dw = -dw; }
    if (dh < 0.0f) { dy += dh; dh = -dh; }
    if (dw <= 0.0f || dh <= 0.0f) return true;

    if (mayRotate) {
        // A rotated sprite can reach outside its axis-aligned destination box, so
        // test a square large enough to contain every rotation of it.
        float cx = dx + dw * 0.5f;
        float cy = dy + dh * 0.5f;
        float radius = 0.5f * (dw + dh) + 2.0f;
        dx = cx - radius;
        dy = cy - radius;
        dw = dh = radius * 2.0f;
    }

    // One pixel of slack for rounding differences with the draw routines.
    int x0 = swrFloor(dx) - 1;
    int y0 = swrFloor(dy) - 1;
    int x1 = swrCeiling(dx + dw) + 1;
    int y1 = swrCeiling(dy + dh) + 1;

    bool offscreen = false;
    if (x1 <= swr->portX || y1 <= swr->portY) offscreen = true;
    if (x0 >= swr->maxX || y0 >= swr->maxY) offscreen = true;
    if (offscreen) swr->drawsCulled++;

    return offscreen;
}
static void swrDrawSpriteEntry(Renderer* renderer, int32_t tpagIndex, float x, float y,
                                  float originX, float originY, float xscale, float yscale,
                                  float angleDeg, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) {
        fprintf(stderr, "%s: tpagIndex of %d is invalid\n", __func__, tpagIndex);
        return;
    }

    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) {
        fprintf(stderr, "%s: tpagIndex of %d is invalid, as pageId of %d is invalid\n", __func__, tpagIndex, pageId);
        return;
    }

    // Reject draws that cannot reach the screen BEFORE touching the texture page.
    // Large rooms reference far more pages than fit in RAM on a handheld, and a
    // decoded 2048x2048 page costs ~8 MB and a full QOI decode; loading pages for
    // instances that are off-screen is what makes the cache thrash.
    {
        float cullDx = (float) (tpag->targetX - originX) * xscale + x;
        float cullDy = (float) (tpag->targetY - originY) * yscale + y;
        float cullDw = xscale * (float) tpag->targetWidth;
        float cullDh = yscale * (float) tpag->targetHeight;
        if (swrDestIsOffscreen(swr, cullDx, cullDy, cullDw, cullDh, swrMustRotate(angleDeg))) {
            return;
        }
    }

    if (!swrEnsureTextureRegion(swr, (uint32_t) pageId, tpag->sourceX, tpag->sourceY,
                                tpag->sourceWidth, tpag->sourceHeight)) {
        fprintf(stderr, "%s: could not ensure texture is loaded, tpagIndex: %d, pageId: %d\n", __func__, tpagIndex, pageId);
        return;
    }
    
    int sx = tpag->sourceX;
    int sy = tpag->sourceY;
    int sw = tpag->sourceWidth;
    int sh = tpag->sourceHeight;
    
    float dx = (float)(tpag->targetX - originX);
    float dy = (float)(tpag->targetY - originY);
    int dw = (int)(xscale * tpag->targetWidth);
    int dh = (int)(yscale * tpag->targetHeight);
    dx *= xscale;
    dy *= yscale;
    dx += x;
    dy += y;

    SWTexture* texture = swr->textures[pageId];
    sProfileLastPixels = dw * dh;

    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        float pivotX = (x - dx) * swrSgn(xscale);
        float pivotY = (y - dy) * swrSgn(yscale);
        
        if (tpag->targetWidth != tpag->sourceWidth)
            pivotX *= (float)tpag->targetWidth / tpag->sourceWidth;
        if (tpag->targetHeight != tpag->sourceHeight)
            pivotY *= (float)tpag->targetHeight/ tpag->sourceHeight;
        
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha, angleDeg, pivotX, pivotY);
    }
    else
    {
        swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
    }
}

static void swrDrawSpritePartEntry(Renderer* renderer, int32_t tpagIndex,
                                      float srcOffX, float srcOffY, float srcW, float srcH,
                                      float x, float y, float xscale, float yscale, float angleDeg,
                                      float pivotX, float pivotY, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;
    
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) return;
    
    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) return;

    // Tiles and sprite parts (including font glyphs) come through here, so culling
    // off-screen draws keeps far-away room tiles from decoding texture pages.
    if (swrDestIsOffscreen(swr, x, y, xscale * (float) srcW, yscale * (float) srcH,
                           swrMustRotate(angleDeg))) {
        return;
    }

    // Only the strips covering this source rectangle are decoded.
    if (!swrEnsureTextureRegion(swr, (uint32_t) pageId, tpag->sourceX + srcOffX, tpag->sourceY + srcOffY,
                                srcW, srcH)) {
        return;
    }
    
    int sx = tpag->sourceX + (int) srcOffX;
    int sy = tpag->sourceY + (int) srcOffY;
    int sw = (int) srcW;
    int sh = (int) srcH;
    
    float dx = x;
    float dy = y;
    int dw = swrCeiling(xscale * sw);
    int dh = swrCeiling(yscale * sh);
    
    if (tpag->sourceWidth != tpag->targetWidth) {
        sx = sx * tpag->sourceWidth / tpag->targetWidth;
        sw = sw * tpag->sourceWidth / tpag->targetWidth;
    }
    if (tpag->sourceHeight != tpag->targetHeight) {
        sy = sy * tpag->sourceHeight / tpag->targetHeight;
        sh = sh * tpag->sourceHeight / tpag->targetHeight;
    }
    
    SWTexture* texture = swr->textures[pageId];
    sProfileLastPixels = dw * dh;

    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha, angleDeg, pivotX * dw, pivotY * dh);
    }
    else
    {
        swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
    }
}

// draw_sprite_pos(): the sprite's source rectangle mapped onto four arbitrary
// corners. Chapter 3 draws its actors like this (one call per body part), so leaving
// it unimplemented made characters vanish piece by piece.
static void SWRenderer_drawSpritePos(Renderer* renderer, int32_t tpagIndex,
                                     float x1, float y1, float x2, float y2,
                                     float x3, float y3, float x4, float y4, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) return;

    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) return;

    // Same source rectangle remapping the other blits use.
    int sx = tpag->sourceX;
    int sy = tpag->sourceY;
    int sw = tpag->targetWidth;
    int sh = tpag->targetHeight;
    if (tpag->sourceWidth != tpag->targetWidth) {
        sx = sx * tpag->sourceWidth / tpag->targetWidth;
        sw = sw * tpag->sourceWidth / tpag->targetWidth;
    }
    if (tpag->sourceHeight != tpag->targetHeight) {
        sy = sy * tpag->sourceHeight / tpag->targetHeight;
        sh = sh * tpag->sourceHeight / tpag->targetHeight;
    }
    if (sw <= 0 || sh <= 0) return;

    if (!swrEnsureTextureRegion(swr, (uint32_t) pageId, sx, sy, sw, sh)) return;

    SWTexture* texture = swr->textures[pageId];
    if (texture == nullptr) return;

    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    swrTransformPosIfNeeded(swr, &x3, &y3);
    swrTransformPosIfNeeded(swr, &x4, &y4);

    int alphaInt = swrIntAlpha(alpha);
    const float u0 = (float) sx, v0 = (float) sy;
    const float u1 = (float) (sx + sw), v1 = (float) sy;
    const float u2 = (float) (sx + sw), v2 = (float) (sy + sh);
    const float u3 = (float) sx, v3 = (float) (sy + sh);

    uint64_t profStart = swrDrawProfileBegin();
    swrDrawTexturedTriangle(renderer, texture, x1, y1, u0, v0, x2, y2, u1, v1, x3, y3, u2, v2, alphaInt);
    swrDrawTexturedTriangle(renderer, texture, x1, y1, u0, v0, x3, y3, u2, v2, x4, y4, u3, v3, alphaInt);
    swrDrawProfileEnd(SWR_DRAW_SPRITE_POS, tpagIndex, 0, profStart);
}

static void SWRenderer_drawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                                     uint32_t color, float alpha, bool outline)
{
    uintpixel_t pxcolor = swrConvertPixel(color);
    
    if (outline)
        swrDrawRectangle(renderer, x1, y1, x2, y2, pxcolor, alpha);
    else
        swrFillRectangle(renderer, x1, y1, x2, y2, pxcolor, alpha);
}

static void SWRenderer_drawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                          uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
                                          float alpha, bool outline)
{
    uintpixel_t pxcolor1 = swrConvertPixel(color1);
    uintpixel_t pxcolor2 = swrConvertPixel(color2);
    uintpixel_t pxcolor3 = swrConvertPixel(color3);
    uintpixel_t pxcolor4 = swrConvertPixel(color4);
    
    if (outline)
        swrDrawRectangleColor(renderer, x1, y1, x2, y2, pxcolor1, pxcolor2, pxcolor3, pxcolor4, alpha);
    else
        swrFillRectangleColor(renderer, x1, y1, x2, y2, pxcolor1, pxcolor2, pxcolor3, pxcolor4, alpha);
}

static void SWRenderer_drawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                                float width, uint32_t color, float alpha)
{
    (void)renderer; (void)x1; (void)y1; (void)x2; (void)y2;
    (void)width; (void)color; (void)alpha;
    
    uintpixel_t colorCvt = swrConvertPixel(color);
#ifdef TRANSPARENT_MASK
    colorCvt |= TRANSPARENT_MASK;
#endif
    swrDrawLine(renderer, x1, y1, x2, y2, width, colorCvt, colorCvt, alpha);
}

static void SWRenderer_drawTriangle(Renderer* renderer,
                                    float x1, float y1, float x2, float y2, float x3, float y3,
                                    uint32_t color1, uint32_t color2, uint32_t color3,
                                    float alpha, bool outline)
{
    if (outline)
    {
        uintpixel_t color1cvt = swrConvertPixel(color1);
        uintpixel_t color2cvt = swrConvertPixel(color2);
        uintpixel_t color3cvt = swrConvertPixel(color3);
        swrDrawLine(renderer, x1, y1, x2, y2, 1, color1cvt, color2cvt, renderer->drawAlpha);
        swrDrawLine(renderer, x1, y1, x3, y3, 1, color1cvt, color3cvt, renderer->drawAlpha);
        swrDrawLine(renderer, x2, y2, x3, y3, 1, color3cvt, color3cvt, renderer->drawAlpha);
    }
    else
    {
        swrDrawTriangle(renderer, x1, y1, x2, y2, x3, y3, color1, color2, color3, alpha);
    }
}

static void SWRenderer_drawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                     float width, uint32_t color1, uint32_t color2, float alpha)
{
    swrDrawLine(renderer, x1, y1, x2, y2, width, swrConvertPixel(color1), swrConvertPixel(color2), alpha);
}

static void SWRenderer_drawText(Renderer* renderer, const char* text, float x, float y,
                                float xscale, float yscale, float angleDeg, float lineSeparation)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrDrawText(swr, text, x, y, xscale, yscale, angleDeg, renderer->drawColor, renderer->drawAlpha, lineSeparation);
}

static void SWRenderer_drawTextColor(Renderer* renderer, const char* text, float x, float y,
                                     float xscale, float yscale, float angleDeg,
                                     int32_t c1, int32_t c2, int32_t c3, int32_t c4, MAYBE_UNUSED float alpha,
                                     float lineSeparation)
{
    SWRenderer* swr = (SWRenderer*) renderer;

    if (swr->traceDraws) {
        logInfo("TEXTCOLOR '%s' at %g,%g c1=%06X c2=%06X c3=%06X c4=%06X\n",
                text != nullptr ? text : "(null)", (double) x, (double) y,
                (unsigned) c1, (unsigned) c2, (unsigned) c3, (unsigned) c4);
    }

    swrDrawTextGradient(swr, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4,
                        renderer->drawAlpha, lineSeparation);
}

// Timed wrappers: the profiler has to see the whole call, off-screen rejection
// included, because a draw that is culled is still a draw the game paid for.
static void SWRenderer_drawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                                  float originX, float originY, float xscale, float yscale,
                                  float angleDeg, uint32_t color, float alpha) {
    uint64_t profStart = swrDrawProfileBegin();
    sProfileLastPixels = 0;
    swrDrawSpriteEntry(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
    swrDrawProfileEnd(SWR_DRAW_SPRITE, tpagIndex, sProfileLastPixels, profStart);
}

static void SWRenderer_drawSpritePart(Renderer* renderer, int32_t tpagIndex,
                                      float srcOffX, float srcOffY, float srcW, float srcH,
                                      float x, float y, float xscale, float yscale, float angleDeg,
                                      float pivotX, float pivotY, uint32_t color, float alpha) {
    uint64_t profStart = swrDrawProfileBegin();
    sProfileLastPixels = 0;
    swrDrawSpritePartEntry(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y,
                           xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
    swrDrawProfileEnd(SWR_DRAW_SPRITE_PART, tpagIndex, sProfileLastPixels, profStart);
}

static void SWRenderer_drawSpriteTiled(Renderer* renderer, int32_t tpagIndex,
                                       float originX, float originY, float x, float y,
                                       float xscale, float yscale, bool tileX, bool tileY,
                                       float roomW, float roomH, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;

    if (0 > tpagIndex || dwin->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) return;

    // Cull the whole tiled area before decoding the page for it.
    {
        float cullAx = fabsf(xscale);
        float cullAy = fabsf(yscale);
        float cullTileW = (float) tpag->boundingWidth * cullAx;
        float cullTileH = (float) tpag->boundingHeight * cullAy;
        float cullStartX = x - originX * cullAx;
        float cullStartY = y - originY * cullAy;
        float cullEndX = tileX ? roomW : cullStartX + cullTileW;
        float cullEndY = tileY ? roomH : cullStartY + cullTileH;
        // The wrap-around start can be one tile further left/up than the origin.
        if (tileX) cullStartX -= cullTileW;
        if (tileY) cullStartY -= cullTileH;
        if (swrDestIsOffscreen(swr, cullStartX, cullStartY, cullEndX - cullStartX, cullEndY - cullStartY, false)) {
            return;
        }
    }

    if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pageId)) return;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) tpag->boundingWidth * axScale;
    float tileH = (float) tpag->boundingHeight * ayScale;
    if (0 >= tileW || 0 >= tileH) return;

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(x - originX * axScale, tileW);
        if (startX > 0) startX -= tileW;
        endX = roomW;
    } else {
        startX = x - originX * axScale;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(y - originY * ayScale, tileH);
        if (startY > 0) startY -= tileH;
        endY = roomH;
    } else {
        startY = y - originY * ayScale;
        endY = startY + tileH;
    }
    
    int sx = tpag->sourceX;
    int sy = tpag->sourceY;
    int sw = tpag->sourceWidth;
    int sh = tpag->sourceHeight;

    int localX0 = tpag->targetX - originX;
    int localY0 = tpag->targetY - originY;
    int localX1 = localX0 + tpag->sourceWidth;
    int localY1 = localY0 + tpag->sourceHeight;
    int sx0 = xscale * localX0;
    int sy0 = yscale * localY0;
    int sx1 = xscale * localX1;
    int sy1 = yscale * localY1;

    uint64_t profStart = swrDrawProfileBegin();
    for (int dy = startY; endY > dy; dy += tileH) {
        int cy = dy + (int)(originY * ayScale);
        int vy0 = cy + sy0;
        int vy1 = cy + sy1;
        int dh = vy1 - vy0;

        for (int dx = startX; endX > dx; dx += tileW) {
            int cx = dx + (int)(originX * axScale);
            int vx0 = cx + sx0;
            int vx1 = cx + sx1;
            int dw = vx1 - vx0;

            swrDrawSprite(renderer, vx0, vy0, dw, dh, swr->textures[pageId], sx, sy, sw, sh, color, alpha);
        }
    }
    swrDrawProfileEnd(SWR_DRAW_SPRITE_TILED, tpagIndex, 0, profStart);
}

static void SWRenderer_drawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;

    if (0 > surfaceID || swr->surfaceCount <= (size_t) surfaceID) return;

    SWSurface* surfaceP = swr->surfaces[surfaceID];
    if (!surfaceP) return;

    swrCommitShadowWritesToSurfaceIfNeeded(swr, surfaceP);
    SWTexture* surface = surfaceP->texture;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) surface->width * axScale;
    float tileH = (float) surface->height * ayScale;
    if (0 >= tileW || 0 >= tileH) return;
    
    float originX = 0, originY = 0;

    float startX, endX, startY, endY;
    startX = fmodf(x - originX * axScale, tileW);
    if (startX > 0) startX -= tileW;
    endX = roomW;
    startY = fmodf(y - originY * ayScale, tileH);
    if (startY > 0) startY -= tileH;
    endY = roomH;
    
    int sx = 0, sy = 0;
    int sw = surface->width;
    int sh = surface->height;

    int localX0 = -originX;
    int localY0 = -originY;
    int localX1 = localX0 + surface->width;
    int localY1 = localY0 + surface->height;
    int sx0 = xscale * localX0;
    int sy0 = yscale * localY0;
    int sx1 = xscale * localX1;
    int sy1 = yscale * localY1;

    for (int dy = startY; endY > dy; dy += tileH) {
        int cy = dy + (int)(originY * ayScale);
        int vy0 = cy + sy0;
        int vy1 = cy + sy1;
        int dh = vy1 - vy0;

        for (int dx = startX; endX > dx; dx += tileW) {
            int cx = dx + (int)(originX * axScale);
            int vx0 = cx + sx0;
            int vx1 = cx + sx1;
            int dw = vx1 - vx0;

            swrDrawSprite(renderer, vx0, vy0, dw, dh, surface, sx, sy, sw, sh, color, alpha);
        }
    }
}

static void SWRenderer_flush(Renderer* renderer)
{
    (void)renderer;
    UNIMP();
}

// ====[ Draw state logging ]====
//
// BUTTERSCOTCH_LOG_BLEND=1 prints the GPU state the moment it changes, plus every
// draw that lands in a surface and every surface composite. Deltarune builds its
// overworld effects (damage veil, layer highlight, spotlight masks) out of
// gpu_set_blendmode[_ext], gpu_set_colorwriteenable and surfaces, and none of that
// is visible in a draw list: the same sprite draws white or darkens the room purely
// by the state it is drawn with. Only transitions and surface work are logged, so a
// normal room is a handful of lines.
int swrDiagLogEnabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("BUTTERSCOTCH_LOG_BLEND");
        enabled = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
        if (enabled) logInfo("Blend/surface log enabled\n");
    }
    return enabled;
}

static void SWRenderer_clearScreen(Renderer* renderer, uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    color = swrConvertPixel(color);
#ifdef TRANSPARENT_MASK
    if (alpha >= 0.95f) {
        color |= TRANSPARENT_MASK;
    }
    else if (alpha < 0.01f) {
        color &= ~TRANSPARENT_MASK;
    }
    else {
    #if PIXEL_SIZE == 32
        int alphai = (int)(255.0f * alpha);
        if (alphai < 0) alphai = 0;
        if (alphai > 255) alphai = 255;
        color |= (alphai << 24);
    #elif PIXEL_SIZE == 16
        color |= (alpha > 0.5f);
    #endif
    }
#endif

    if (swrIsOnSurface(swr) && swrDiagLogEnabled())
        logInfo("Surf clear    target=%d color=%06X alpha=%.3f -> %04X\n",
                swr->currentSurfaceIndex, (unsigned) (color & 0xFFFFFF), alpha, (unsigned) color);
    
    // The colour carries a one-bit transparency flag; a surface's alpha plane is
    // the real coverage the game blends against, so it has to follow the clear.
    uint8_t coverage = 255;
    if (alpha < 0.002f) coverage = 0;
    else if (alpha < 0.998f) coverage = (uint8_t) (alpha * 255.0f + 0.5f);

    for (int y = 0; y < swr->height; y++) {
        uintpixel_t* line = &swr->fb[y * swr->fbPitch];
        uint8_t* coverageLine = swrTargetCoverage(swr);
        if (coverageLine != NULL)
            memset(&coverageLine[y * swr->fbPitch], coverage, (size_t) swr->width);
        for (int x = 0; x < swr->width; x++) {
            line[x] = color;
        }
    }
}

// ====[ Blend state logging ]====
static void swrLogBlendState(SWRenderer* swr, const char* source)
{
    static unsigned lastKey = 0;
    static bool haveLast = false;

    if (!swrDiagLogEnabled()) return;

    unsigned key = ((unsigned) (swr->blendEnabled ? 1u : 0u) << 24)
                 | ((unsigned) (swr->blendMode & 0xF) << 20)
                 | ((unsigned) (swr->blendSrcFactor & 0xF) << 16)
                 | ((unsigned) (swr->blendDstFactor & 0xF) << 12)
                 | ((unsigned) (swr->writeMask & 0xF) << 8)
                 | ((unsigned) (swr->pixelMode & 0xF) << 4);
    if (haveLast && key == lastKey) return;
    haveLast = true;
    lastKey = key;

    logInfo("Blend %-14s enabled=%d mode=%d src=%d dst=%d mask=%X -> pixelMode=%d %s swr=%p\n",
            source, swr->blendEnabled ? 1 : 0, swr->blendMode,
            swr->blendSrcFactor, swr->blendDstFactor, swr->writeMask, swr->pixelMode,
            swrIsOnSurface(swr) ? "surface" : "screen", (void*) swr);
}

static void SWRenderer_gpuSetBlendMode(Renderer* renderer, int32_t mode)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->blendMode = mode;
    // gpu_set_blendmode() also cancels a gpu_set_blendmode_ext() pair, which is how
    // the mask scripts hand control back to normal sprite blending.
    swr->blendSrcFactor = 0;
    swr->blendDstFactor = 0;
    swrSetupBlendState(swr);
    swrLogBlendState(swr, "set_blendmode");
}

static void SWRenderer_gpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    // Only the RGB factors are used; GameMaker's _ext form sets the alpha factors to
    // the same values, and the sel form (which we get through _ext here) is not used
    // by anything we run.
    (void) sfactor_alpha;
    (void) dfactor_alpha;
    swr->blendSrcFactor = sfactor;
    swr->blendDstFactor = dfactor;
    swrSetupBlendState(swr);
    swrLogBlendState(swr, "set_blendmode_ext");
}

static void SWRenderer_gpuSetBlendEnable(Renderer* renderer, bool enable)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->blendEnabled = enable;
    swrSetupBlendState(swr);
    swrLogBlendState(swr, "set_blendenable");
}

// Every sprite and surface blit in this renderer samples the texture directly, so
// interpolation is simply not something it can turn on: accept the call and keep the
// nearest-pixel look, which is what GameMaker's default is anyway.
static void SWRenderer_gpuSetTexFilter(Renderer* renderer, bool enable)
{
    (void)renderer; (void)enable;
}

static void SWRenderer_gpuSetAlphaTestEnable(Renderer* renderer, bool enable)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->alphaTestEnabled = enable;
}

static void SWRenderer_gpuSetAlphaTestRef(Renderer* renderer, uint8_t ref)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->alphaTestRef = ref;
}

static void SWRenderer_gpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;

    swr->writeMask =
        (red ? WRITE_MASK_RED : 0) |
        (green ? WRITE_MASK_GREEN : 0) |
        (blue ? WRITE_MASK_BLUE : 0) |
        (alpha ? WRITE_MASK_ALPHA : 0);
    // The mask alone puts us on the general blitter; see swrSetupBlendState.
    swrSetupBlendState(swr);
    swrLogBlendState(swr, "set_colorwrite");

    if (!swrIsOnSurface(swr)) {
        // Nothing else to do: the draw routines apply the mask per pixel.
        return;
    }
    
    SWSurface* currSurf = swr->surfaces[swr->currentSurfaceIndex];
    
    swrCommitShadowWritesToSurfaceIfNeeded(swr, currSurf);
    
    // no need to change other properties, because the width and height are the same.
    // but we ALWAYS need to re-fetch the writable surface texture since the old one
    // may have been freed.
    SWTexture* writable = swrWritableSurfaceTexture(swr, swr->currentSurfaceIndex);
    swr->fb = writable->buffer;
    
    if (currSurf->shadowTexture) {
        assert(currSurf->texture->width == currSurf->shadowTexture->width);
        assert(currSurf->texture->height == currSurf->shadowTexture->height);
        assert(currSurf->texture->buffer != currSurf->shadowTexture->buffer);
    }
}

static void SWRenderer_gpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    *red = (swr->writeMask & WRITE_MASK_RED) != 0;
    *green = (swr->writeMask & WRITE_MASK_GREEN) != 0;
    *blue = (swr->writeMask & WRITE_MASK_BLUE) != 0;
    *alpha = (swr->writeMask & WRITE_MASK_ALPHA) != 0;
}

static bool SWRenderer_gpuGetBlendEnable(Renderer* renderer)
{
    UNIMP();
    (void)renderer;
    return false;
}

static void SWRenderer_gpuSetFog(Renderer* renderer, bool enable, uint32_t color, float start, float end)
{
    UNIMP();
    (void)renderer; (void)enable; (void)color; (void)start; (void)end;
}

static int32_t SWRenderer_createSurface(Renderer* renderer, int32_t width, int32_t height)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    int32_t slot = -1;
    for (size_t i = 0; i < swr->surfaceCount; i++)
    {
        if (swr->surfaces[i] == NULL) {
            slot = (int32_t) i;
            break;
        }
    }
    
    if (slot < 0) {
        fprintf(stderr, "swr: Could not create surface, too many exist at once.\n");
        return slot;
    }
    
    swr->surfaces[slot] = swrCreateSurface(width, height);
    if (swrDiagLogEnabled())
        logInfo("Surf create   id=%d %dx%d\n", (int) slot, width, height);
    return slot;
}

static bool SWRenderer_surfaceExists(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount)
        return false;
    
    return swr->surfaces[surfaceID] != NULL;
}

static bool SWRenderer_setRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface)
{
    return swrSwitchToSurface(renderer, surfaceID, implicitApplicationSurface);
}

static float SWRenderer_getSurfaceWidth(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID)
        return (float)(int)((swrIsOnSurface(swr) ? swr->mainWidth : swr->width) / swr->scaleX);
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL)
        return 0.0f;
    
    return (float) swr->surfaces[surfaceID]->texture->width;
}

static float SWRenderer_getSurfaceHeight(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) 
        return (float)(int)((swrIsOnSurface(swr) ? swr->mainHeight : swr->height) / swr->scaleY);
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL)
        return 0.0f;
    
    return (float) swr->surfaces[surfaceID]->texture->height;
}

static void SWRenderer_drawSurface(Renderer* renderer, int32_t surfaceID,
                                   int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                                   float x, float y, float xscale, float yscale, float angleDeg,
                                   uint32_t color, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    SWTexture* surface, localSurface;
    if (surfaceID == APPLICATION_SURFACE_ID) {
        localSurface.buffer = swrIsOnSurface(swr) ? swr->mainFb : swr->fb;
        localSurface.width = swrIsOnSurface(swr) ? swr->mainWidth : swr->width;
        localSurface.height = swrIsOnSurface(swr) ? swr->mainHeight : swr->height;
        // The screen has no coverage plane; leaving this unset would hand the blit a
        // stack-garbage pointer to blend against.
        localSurface.alphaPlane = NULL;
        surface = &localSurface;
    } else {
        if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
            fprintf(stderr, "swr: Invalid surface id %d for drawSurface\n", surfaceID);
            return;
        }

        swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[surfaceID]);
        surface = swr->surfaces[surfaceID]->texture;
    }

    if (swrDiagLogEnabled()) {
        logInfo("Surf draw     src=%d srcRect=%d,%d,%dx%d dst=%.1f,%.1f scale=%.2f,%.2f angle=%.1f "
                "tint=%06X alpha=%.3f blend=%d mode=%d to=%d\n",
                (int) surfaceID, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale,
                angleDeg, (unsigned) (color & 0xFFFFFF), alpha, swr->blendMode, swr->pixelMode,
                swrIsOnSurface(swr) ? swr->currentSurfaceIndex : -1);
    }
    
    if (srcWidth < 0) {
        srcWidth = surface->width;
        swrReverseTransformSizeIfNeeded(swr, &xscale, NULL);
    }
    if (srcHeight < 0) {
        srcHeight = surface->height;
        swrReverseTransformSizeIfNeeded(swr, NULL, &yscale);
    }
    
    int sx = srcLeft;
    int sy = srcTop;
    int sw = srcWidth;
    int sh = srcHeight;
    
    int tw = (int)(srcWidth * swr->scaleX);
    int th = (int)(srcHeight * swr->scaleY);
    
    float dx = x;
    float dy = y;
    int dw = (int)(xscale * tw);
    int dh = (int)(yscale * th);

    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        float pivotX = (x - dx) * swrSgn(xscale);
        float pivotY = (y - dy) * swrSgn(yscale);
        
        if (tw != sw)
            pivotX *= (float)tw / sw;
        if (th != sh)
            pivotY *= (float)th / sh;
        
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, surface, sx, sy, sw, sh, color, alpha, angleDeg, pivotX, pivotY);
    }
    else
    {
        swrDrawSprite(renderer, dx, dy, dw, dh, surface, sx, sy, sw, sh, color, alpha);
    }
}

static void SWRenderer_surfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (surfaceID == APPLICATION_SURFACE_ID) {
        fprintf(stderr, "swr: Don't support resizing the application window with this.  There must be another way! (need to set to %dx%d)\n", width, height);
        return;
    }
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
        fprintf(stderr, "swr: Cannot resize surface id %d, it's invalid\n", surfaceID);
        return;
    }
    
    swrFreeSurface(swr->surfaces[surfaceID]);
    swr->surfaces[surfaceID] = swrCreateSurface(width, height);
}

static void SWRenderer_surfaceFree(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (surfaceID == APPLICATION_SURFACE_ID) {
        fprintf(stderr, "swr: Don't support SWRenderer_surfaceCopy the application window with this.  There must be another way!\n");
        return;
    }
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
        fprintf(stderr, "swr: Cannot resize surface id %d, it's invalid\n", surfaceID);
        return;
    }
    
    swrFreeSurface(swr->surfaces[surfaceID]);
    swr->surfaces[surfaceID] = NULL;
}

static void SWRenderer_surfaceCopy(Renderer* renderer,
                                   int32_t DestSurfaceID, int32_t DestX, int32_t DestY,
                                   int32_t SrcSurfaceID, int32_t SrcX, int32_t SrcY,
                                   int32_t SrcW, int32_t SrcH, bool part)
{
    (void) part; // TODO: figure out the meaning of this parameter

    SWRenderer* swr = (SWRenderer*) renderer;
    
    SWTexture temp1, temp2;
    SWTexture *dstSurf, *srcSurf;
    
    if (DestSurfaceID == APPLICATION_SURFACE_ID) {
        dstSurf = &temp1;
        temp1.width = swr->mainWidth;
        temp1.height = swr->mainHeight;
        temp1.buffer = swr->mainFb;
        temp1.alphaPlane = NULL;
    }
    else if (DestSurfaceID < 0 || (size_t) DestSurfaceID >= swr->surfaceCount || swr->surfaces[DestSurfaceID] == NULL) {
        fprintf(stderr, "swr: Cannot resize surface id %d, it's invalid (dest in surfaceCopy)\n", DestSurfaceID);
        return;
    }
    else {
        dstSurf = swrWritableSurfaceTexture(swr, DestSurfaceID);
    }
    
    if (SrcSurfaceID == APPLICATION_SURFACE_ID) {
        srcSurf = &temp2;
        temp2.width = swr->mainWidth;
        temp2.height = swr->mainHeight;
        temp2.buffer = swr->mainFb;
        temp2.alphaPlane = NULL;
    }
    else if (SrcSurfaceID < 0 || (size_t) SrcSurfaceID >= swr->surfaceCount || swr->surfaces[SrcSurfaceID] == NULL) {
        fprintf(stderr, "swr: Cannot resize surface id %d, it's invalid (src in surfaceCopy)\n", SrcSurfaceID);
        return;
    }
    else {
        swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[SrcSurfaceID]);
        srcSurf = swr->surfaces[SrcSurfaceID]->texture;
    }
    
    if (SrcX + SrcW < 0) return;
    if (SrcY + SrcH < 0) return;
    if (SrcX >= srcSurf->width) return;
    if (SrcY >= srcSurf->height) return;
    if (DestX + SrcW < 0) return;
    if (DestY + SrcH < 0) return;
    if (DestX >= dstSurf->width) return;
    if (DestY >= dstSurf->height) return;
    
    if (SrcY < 0) {
        SrcH += SrcY;
        DestY -= SrcY;
        SrcY = 0;
    }
    if (SrcX < 0) {
        SrcW += SrcX;
        DestX -= SrcX;
        SrcX = 0;
    }
    if (SrcX + SrcW >= srcSurf->width)
        SrcW = srcSurf->width - SrcX;
    if (SrcY + SrcH >= srcSurf->height)
        SrcH = srcSurf->height - SrcY;
    
    if (DestX + SrcW >= dstSurf->width)
        SrcW = dstSurf->width - DestX;
    if (DestY + SrcH >= dstSurf->height)
        SrcH = dstSurf->height - DestY;
    
    for (int dy = 0; dy < SrcH; dy++) {
        /***/ uintpixel_t* dstLine = &dstSurf->buffer[(dy + DestY) * dstSurf->width];
        const uintpixel_t* srcLine = &srcSurf->buffer[(dy + SrcY)  * srcSurf->width];
        for (int dx = 0; dx < SrcW; dx++) {
            dstLine[dx + DestX] = srcLine[dx + SrcX];
        }
        if (dstSurf->alphaPlane != NULL)
        {
            uint8_t* dstAlpha = &dstSurf->alphaPlane[(dy + DestY) * dstSurf->width + DestX];
            if (srcSurf->alphaPlane != NULL)
                memcpy(dstAlpha, &srcSurf->alphaPlane[(dy + SrcY) * srcSurf->width + SrcX], (size_t) SrcW);
            else
                memset(dstAlpha, 255, (size_t) SrcW);
        }
    }
    
    UNIMP();
}

static bool SWRenderer_surfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA)
{
    UNIMP();
    (void)renderer; (void)surfaceID; (void)outRGBA;
    return false;
}

// Overwrites a surface with a fully opaque RGB555 frame. Video playback is the only
// caller: a decoded frame has to become something the game can draw_surface().
static bool SWRenderer_surfaceUploadRgb555(Renderer* renderer, int32_t surfaceID, const uint16_t* pixels,
                                           int32_t width, int32_t height, int32_t pitch)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL)
        return false;

    SWSurface* surf = swr->surfaces[surfaceID];
    swrCommitShadowWritesToSurfaceIfNeeded(swr, surf);

    SWTexture* tex = surf->texture;
    int32_t w = swrMin(width, (int32_t) tex->width);
    int32_t h = swrMin(height, (int32_t) tex->height);

    // Four pixels per step: a 720p frame is 1.8 MB of this, and a per-pixel loop made
    // the upload a visible share of a cutscene frame.
    for (int32_t y = 0; y < h; y++)
    {
        const uint16_t* src = pixels + (size_t) y * (size_t) pitch;
        uintpixel_t* dst = &tex->buffer[(size_t) y * tex->width];
        int32_t x = 0;
        for (; x + 4 <= w; x += 4)
        {
            uint64_t four;
            memcpy(&four, src + x, sizeof(four));
            four |= 0x8000800080008000ull;
            memcpy(dst + x, &four, sizeof(four));
        }
        for (; x < w; x++)
            dst[x] = (uintpixel_t) (src[x] | 0x8000);
    }

    return true;
}

static int32_t SWRenderer_gpuGetBlendMode(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    return swr->blendMode;
}

static int32_t SWRenderer_createSpriteFromSurface(Renderer* renderer, int32_t surfaceID,
                                                   int32_t x, int32_t y, int32_t w, int32_t h,
                                                   bool removeback, bool smooth,
                                                   int32_t xorig, int32_t yorig)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    swrTransformPosIntIfNeeded(swr, &x, &y);
    swrTransformSizeIntIfNeeded(swr, &w, &h);
    swrTransformSizeIntIfNeeded(swr, &xorig, &yorig);
    
    (void) removeback;
    (void) smooth;
    
    SWTexture *srcTex, temp1;
    
    if (surfaceID == APPLICATION_SURFACE_ID) {
        srcTex = &temp1;
        temp1.width = swr->width;
        temp1.height = swr->height;
        temp1.buffer = swr->fb;
    } else {
        if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
            fprintf(stderr, "%s: Invalid surface ID %d\n", __func__, surfaceID);
            return 0;
        }
        SWSurface* surf = swr->surfaces[surfaceID];
        swrCommitShadowWritesToSurfaceIfNeeded(swr, surf);
        srcTex = surf->texture;
    }

    int32_t texturePageId = swrFindSurfaceTextureSlot(swr);
    int32_t tpagIndex = swrFindSurfaceTPagSlot(swr);
    if (texturePageId == -1 || tpagIndex == -1) {
        fprintf(stderr, "%s: Sprite overflow!!\n", __func__);
        return 0;
    }
    
    SWTexture* tex = swrCreateTexture(NULL, w, h);
    
    // grab the pixels.
    for (int iy = 0; iy < h; iy++)
    {
        uintpixel_t* dstline = &tex->buffer[iy * tex->width];
        if ((iy + y) < 0 || (iy + y) >= srcTex->height)
        {
            for (int ix = 0; ix < w; ix++)
                dstline[ix] = 0;
            
            continue;
        }
        
        uintpixel_t* srcline = &srcTex->buffer[(iy + y) * srcTex->width + x];
        
        int ix = 0, sx = x;
        // left edge
        for (; sx < 0 && ix < w; sx++, ix++)
            dstline[ix] = 0;
        
        // in-bounds
        for (; sx < srcTex->width && ix < w; sx++, ix++)
            // Copy the pixel as it is, transparency bit included. Forcing the
            // transparent bit on (which is what this used to do) turns every
            // sprite_create_from_surface() into an opaque rectangle: Deltarune's
            // obj_battleLayerHighlight renders one tile layer into a surface cleared
            // with draw_clear_alpha(c_white, 0) and draws the result over the room, so
            // the whole screen came out white instead of the layer's magenta border
            // tiles sitting on the scene.
            dstline[ix] = srcline[ix];

        // right edge
        for (; ix < w; ix++)
            dstline[ix] = 0;
    }
    
    int32_t spriteW = w;
    int32_t spriteH = h;
    
    int32_t targetW = spriteW;
    int32_t targetH = spriteH;
    swrReverseTransformSizeIntIfNeeded(swr, &targetW, &targetH);

    swr->textures[texturePageId] = tex;

    // TODO[MrPowerGamerBR]: This is supposed to be refactored, not to modify data.win structs directly.
    DataWin* dw = swr->base.dataWin;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) spriteW;
    tpag->sourceHeight = (uint16_t) spriteH;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) (spriteW / swr->scaleX);
    tpag->targetHeight = (uint16_t) (spriteH / swr->scaleY);
    tpag->boundingWidth = (uint16_t) spriteW;
    tpag->boundingHeight = (uint16_t) spriteH;
    tpag->texturePageId = texturePageId;
    
    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, swr->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    // name was set by DataWin_allocSpriteSlot ("__newsprite<N>"); don't overwrite it here
    sprite->width = (uint32_t) tpag->targetWidth;
    sprite->height = (uint32_t) tpag->targetHeight;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t*) safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    fprintf(stderr, "%s: Allocated surface sprite with ID %d\n", __func__, spriteIndex);
    return spriteIndex;
}

static void SWRenderer_deleteSprite(Renderer* renderer, int32_t spriteIndex)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    DataWin* dw = renderer->dataWin;
    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;

    // Refuse to delete original data.win sprites
    if (swr->originalSpriteCount > (uint32_t) spriteIndex) {
        fprintf(stderr, "%s: Cannot delete sprite with index %d, it's invalid.\n", __func__, spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    for (uint32_t i = 0; i < sprite->textureCount; i++)
    {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t) tpagIdx >= swr->originalTPagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && swr->totalTextureCount > (uint32_t) pageId) {
                swrFreeTexture(swr->textures[pageId]);
                swr->textures[pageId] = NULL;
            }
            // Mark TPAG slot as free for reuse
            tpag->texturePageId = -1;
        }
    }
    
    free(sprite->tpagIndices);
    sprite->tpagIndices = NULL;
    
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    fprintf(stderr, "SWR: Deleted sprite %d\n", spriteIndex);
}

static void SWRenderer_drawTiledPart(Renderer* renderer, int32_t tpagIndex,
                                     int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                     float dstX, float dstY, float dstW, float dstH,
                                     uint32_t color, float alpha)
{
    UNIMP();
    (void)renderer; (void)tpagIndex;
    (void)srcX; (void)srcY; (void)srcW; (void)srcH;
    (void)dstX; (void)dstY; (void)dstW; (void)dstH;
    (void)color; (void)alpha;
}

static int32_t SWRenderer_ensureApplicationSurface(Renderer* renderer, int32_t width, int32_t height)
{
    // We don't evict surfaces, and especially not the primary framebuffer,
    // but if we did, this is where we would restore it.
    (void) renderer;
    (void) width;
    (void) height;
    
    return APPLICATION_SURFACE_ID;
}

static RendererVtable swrVtable;

void SWRenderer_clearFrameBuffer(Renderer* renderer, uint32_t color)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    uintpixel_t pxcolor = swrConvertPixel(color);
    
    size_t fbSize = swr->fbPitch;
    fbSize *= swr->height;
    for (size_t i = 0; i < fbSize; i++)
    {
        swr->fb[i] = pxcolor;
    }
}

static uint32_t SWRenderer_spriteGetTexture(Renderer* renderer, int32_t tpagIndex)
{
    (void) renderer;

    return (uint32_t) tpagIndex + 1;
}

static uint32_t SWRenderer_surfaceGetTexture(Renderer* renderer, int32_t surfaceID)
{
    (void) renderer;
    (void) surfaceID;

    return (uint32_t) -1;
}

static float SWRenderer_textureGetTexelWidth(Renderer* renderer, uint32_t texID)
{
    (void) renderer;
    (void) texID;
    
    return 1.0f;
}

static float SWRenderer_textureGetTexelHeight(Renderer* renderer, uint32_t texID)
{
    (void) renderer;
    (void) texID;
    
    return 1.0f;
}

static bool SWRenderer_textureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs)
{
    (void) renderer;
    (void) texID;
    (void) outUVs;
    
    return false;
}

static void SWRenderer_textureSetStage(Renderer* renderer, int32_t slot, uint32_t texID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (slot < 0 || slot >= SWR_SHADER_STAGES) return;
    swr->shaderStages[slot] = texID;
}

static bool SWRenderer_shaderIsCompiled(Renderer* renderer, int32_t shader)
{
    (void) renderer;
    (void) shader;
    
    return false;
}

static bool SWRenderer_shadersSupported(void)
{
    return false;
}

static void SWRenderer_gpuSetShader(Renderer* renderer, int32_t shaderIndex)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (swr->shaderIndex == shaderIndex) return;
    swr->shaderIndex = shaderIndex;
    // Stages belong to the program that was bound when they were set, so a new
    // bind starts from nothing; the game re-binds them right after.
    memset(swr->shaderStages, 0, sizeof(swr->shaderStages));
}

static void SWRenderer_gpuResetShader(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->shaderIndex = -1;
}

// Uniform handles are indices into a per-renderer name table, which is how the
// value set later by shader_set_uniform_f() finds its way to the emulated
// fragment. The names are stable for the lifetime of the renderer.
static int32_t swrFindUniform(SWRenderer* swr, const char* name)
{
    for (int32_t i = 0; i < swr->shaderUniformCount; i++)
    {
        if (strcmp(swr->shaderUniforms[i].name, name) == 0) return i;
    }
    if (swr->shaderUniformCount >= SWR_SHADER_UNIFORMS) return -1;
    int32_t index = swr->shaderUniformCount++;
    snprintf(swr->shaderUniforms[index].name, sizeof(swr->shaderUniforms[index].name),
             "%s", name);
    return index;
}

static int32_t SWRenderer_shaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform)
{
    (void) shaderIndex;
    if (uniform == NULL) return -1;
    return swrFindUniform((SWRenderer*) renderer, uniform);
}

static int32_t SWRenderer_shaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform)
{
    (void) renderer;
    (void) shaderIndex;
    if (uniform == NULL) return -1;
    // Fixed slots, so that the emulation reads the texture the game bound here.
    if (strcmp(uniform, "texGradient") == 0) return SWR_SHADER_SLOT_GRADIENT;
    if (strcmp(uniform, "texBubble")   == 0) return SWR_SHADER_SLOT_BUBBLE;
    if (strcmp(uniform, "texStars")    == 0) return SWR_SHADER_SLOT_STARS;
    return -1;
}

static void SWRenderer_shaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (handle < 0 || handle >= swr->shaderUniformCount) return;
    float values[4] = { value1, value2, value3, value4 };
    if (count < 1) count = 1;
    if (count > 4) count = 4;
    for (int32_t i = 0; i < count; i++)
        swr->shaderUniforms[handle].value[i] = values[i];
}

static void SWRenderer_shaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4)
{
    // Integer uniforms (shd_ripple's rippleBanding, rippleFading) feed the same table
    // as the float ones; the emulation reads them back as numbers.
    SWRenderer* swr = (SWRenderer*) renderer;
    if (handle < 0 || handle >= swr->shaderUniformCount) return;
    int32_t values[4] = { value1, value2, value3, value4 };
    if (count < 1) count = 1;
    if (count > 4) count = 4;
    for (int32_t i = 0; i < count; i++)
        swr->shaderUniforms[handle].value[i] = (float) values[i];
}

static void SWRenderer_applyProjection(Renderer* renderer, const Matrix4f* worldToClip, const Matrix4f* idk)
{
    (void) renderer;
    (void) worldToClip;
    (void) idk;
    UNIMP();
}

Renderer* SWRenderer_create(void)
{
    SWRenderer* swr = (SWRenderer*) safeCalloc(1, sizeof(SWRenderer));
    swr->base.vtable = &swrVtable;
    swrVtable.init                     = SWRenderer_init;
    swrVtable.destroy                  = SWRenderer_destroy;
    swrVtable.beginFrame               = SWRenderer_beginFrame;
    swrVtable.endFrameInit             = SWRenderer_endFrameInit;
    swrVtable.endFrameEnd              = SWRenderer_endFrameEnd;
    swrVtable.beginView                = SWRenderer_beginView;
    swrVtable.endView                  = SWRenderer_endView;
    swrVtable.beginGUI                 = SWRenderer_beginGUI;
    swrVtable.setGuiProjection         = SWRenderer_setGuiProjection;
    swrVtable.endGUI                   = SWRenderer_endGUI;
    swrVtable.drawSprite               = SWRenderer_drawSprite;
    swrVtable.drawSpritePart           = SWRenderer_drawSpritePart;
    swrVtable.drawSpritePos            = SWRenderer_drawSpritePos;
    swrVtable.drawRectangle            = SWRenderer_drawRectangle;
    swrVtable.drawRectangleColor       = SWRenderer_drawRectangleColor;
    swrVtable.drawLine                 = SWRenderer_drawLine;
    swrVtable.drawTriangle             = SWRenderer_drawTriangle;
    swrVtable.drawLineColor            = SWRenderer_drawLineColor;
    swrVtable.drawText                 = SWRenderer_drawText;
    swrVtable.drawTextColor            = SWRenderer_drawTextColor;
    swrVtable.flush                    = SWRenderer_flush;
    swrVtable.clearScreen              = SWRenderer_clearScreen;
    swrVtable.createSpriteFromSurface  = SWRenderer_createSpriteFromSurface;
    swrVtable.deleteSprite             = SWRenderer_deleteSprite;
    swrVtable.gpuSetBlendMode          = SWRenderer_gpuSetBlendMode;
    swrVtable.gpuSetBlendModeExt       = SWRenderer_gpuSetBlendModeExt;
    swrVtable.gpuSetBlendEnable        = SWRenderer_gpuSetBlendEnable;
    swrVtable.gpuSetTexFilter          = SWRenderer_gpuSetTexFilter;
    swrVtable.gpuSetAlphaTestEnable    = SWRenderer_gpuSetAlphaTestEnable;
    swrVtable.gpuSetAlphaTestRef       = SWRenderer_gpuSetAlphaTestRef;
    swrVtable.gpuSetColorWriteEnable   = SWRenderer_gpuSetColorWriteEnable;
    swrVtable.gpuGetColorWriteEnable   = SWRenderer_gpuGetColorWriteEnable;
    swrVtable.gpuGetBlendEnable        = SWRenderer_gpuGetBlendEnable;
    swrVtable.gpuGetBlendMode          = SWRenderer_gpuGetBlendMode;
    swrVtable.gpuSetFog                = SWRenderer_gpuSetFog;
    swrVtable.drawSpriteTiled          = SWRenderer_drawSpriteTiled;
    swrVtable.drawSurfaceTiled         = SWRenderer_drawSurfaceTiled;
    swrVtable.createSurface            = SWRenderer_createSurface;
    swrVtable.surfaceExists            = SWRenderer_surfaceExists;
    swrVtable.setRenderTarget          = SWRenderer_setRenderTarget;
    swrVtable.ensureApplicationSurface = SWRenderer_ensureApplicationSurface;
    swrVtable.getSurfaceWidth          = SWRenderer_getSurfaceWidth;
    swrVtable.getSurfaceHeight         = SWRenderer_getSurfaceHeight;
    swrVtable.drawSurface              = SWRenderer_drawSurface;
    swrVtable.surfaceResize            = SWRenderer_surfaceResize;
    swrVtable.surfaceFree              = SWRenderer_surfaceFree;
    swrVtable.surfaceCopy              = SWRenderer_surfaceCopy;
    swrVtable.surfaceGetPixels         = SWRenderer_surfaceGetPixels;
    swrVtable.surfaceUploadRgb555      = SWRenderer_surfaceUploadRgb555;
    swrVtable.drawTiledPart            = SWRenderer_drawTiledPart;
    swrVtable.spriteGetTexture         = SWRenderer_spriteGetTexture;
    swrVtable.surfaceGetTexture        = SWRenderer_surfaceGetTexture;
    swrVtable.textureGetTexelWidth     = SWRenderer_textureGetTexelWidth;
    swrVtable.textureGetTexelHeight    = SWRenderer_textureGetTexelHeight;
    swrVtable.textureGetUVs            = SWRenderer_textureGetUVs;
    swrVtable.textureSetStage          = SWRenderer_textureSetStage;
    swrVtable.gpuSetShader             = SWRenderer_gpuSetShader;
    swrVtable.gpuResetShader           = SWRenderer_gpuResetShader;
    swrVtable.shaderGetUniform         = SWRenderer_shaderGetUniform;
    swrVtable.shaderGetSamplerIndex    = SWRenderer_shaderGetSamplerIndex;
    swrVtable.shaderSetUniformF        = SWRenderer_shaderSetUniformF;
    swrVtable.shaderSetUniformI        = SWRenderer_shaderSetUniformI;
    swrVtable.shaderIsCompiled         = SWRenderer_shaderIsCompiled;
    swrVtable.shadersSupported         = SWRenderer_shadersSupported;
    swrVtable.applyProjection          = SWRenderer_applyProjection;
    
    swrVtable.drawTile                 = NULL;
    
    swr->base.drawColor = 0xFFFFFF;
    swr->base.drawAlpha = 1.0f;
    swr->base.drawFont = -1;
    swr->base.drawHalign = 0;
    swr->base.drawValign = 0;
    swr->base.circlePrecision = 24;

    return (Renderer*) swr;
}
