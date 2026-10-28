#include <stdlib.h>
#include <assert.h>
#include "sw_renderer_private.h"
#include "image/image_decoder.h"

void swrEvictTextureFromCache(SWRenderer* swr, int textureIndex)
{
    SWTexture* texture = swr->textures[textureIndex];
    swr->textures[textureIndex] = NULL;

    if (texture) {
        if (swr->textureBytes >= texture->byteSize) {
            swr->textureBytes -= texture->byteSize;
        } else {
            swr->textureBytes = 0;
        }
        swr->textureEvictions++;

        // Remember this eviction so a quick reload of the same page can grow
        // the budget instead of thrashing one 8 MB page every frame.
        int slot = swr->recentEvictedNext++ & 7;
        swr->recentEvictedPages[slot] = (uint32_t) textureIndex;
        swr->recentEvictedTicks[slot] = swr->textureUseClock;
    }
    
    swrFreeTexture(texture);
}

static bool swrWasRecentlyEvicted(SWRenderer* swr, uint32_t pageId)
{
    for (int i = 0; i < 8; i++) {
        if (swr->recentEvictedPages[i] != pageId)
            continue;
        // Same page requested again within a few frames of its eviction.
        if (swr->textureUseClock - swr->recentEvictedTicks[i] <= 4096) {
            return true;
        }
    }
    return false;
}

// Returns the decoded original TXTR page with the oldest use tick, or -1.
// Only original texture pages are evictable; dynamic sprite/surface pages live
// above textureCount and are owned by their creator.
static int swrLeastRecentlyUsedPage(SWRenderer* swr)
{
    int victim = -1;
    uint64_t oldest = UINT64_MAX;

    for (size_t i = 0; i < swr->textureCount; i++) {
        SWTexture* texture = swr->textures[i];
        if (texture == NULL)
            continue;
        if (texture->lastUsedTick < oldest) {
            oldest = texture->lastUsedTick;
            victim = (int) i;
        }
    }

    return victim;
}

// Evict least-recently-used pages until `needed` more bytes fit in the cache budget.
// The budget is advisory: a single page larger than the budget is still allowed.
void swrReserveTextureBytes(SWRenderer* swr, size_t needed)
{
    while (swr->textureBytes > 0 && swr->textureBytes + needed > swr->textureBudgetBytes) {
        int victim = swrLeastRecentlyUsedPage(swr);
        if (victim < 0)
            break;

        swrEvictTextureFromCache(swr, victim);
    }
}

// Creates the texture object for a page, decoding it immediately unless the page
// is stored with strip checkpoints (then it is allocated empty and filled per
// strip on demand).
static bool swrCreatePageTextureIfNeeded(SWRenderer* swr, uint32_t pageId)
{
    if (pageId < swr->totalTextureCount && swr->textures[pageId] != NULL) {
        return true;
    }

    // If this page was just evicted, the working set is larger than the budget.
    // Grow it by one page (up to the device-dependent maximum) instead of
    // paying a full bz2+QOI decode every frame.
    if (swr->textureBudgetBytes < swr->textureBudgetMax && swrWasRecentlyEvicted(swr, pageId)) {
        size_t newBudget = swr->textureBudgetBytes + 8u * 1024u * 1024u;
        if (newBudget > swr->textureBudgetMax) {
            newBudget = swr->textureBudgetMax;
        }
        if (newBudget > swr->textureBudgetBytes) {
            swr->textureBudgetBytes = newBudget;
            fprintf(stderr, "SWR: texture thrash on page %u; raising cache budget to %zu MB\n",
                    pageId, swr->textureBudgetBytes / (1024 * 1024));
        }
    }

    DataWin* dw = swr->base.dataWin;

#if PIXEL_SIZE == 16
    // Sub-page residency: when the sidecar carries strip checkpoints, allocate an
    // empty page and decode only the strips that get drawn. This keeps both the
    // resident bytes and the decode cost proportional to what is on screen.
    {
        static int stripsEnabled = -1;
        if (stripsEnabled < 0) {
            const char* noStrips = getenv("BUTTERSCOTCH_NO_STRIPS");
            stripsEnabled = (noStrips != nullptr && noStrips[0] != '\0' && noStrips[0] != '0') ? 0 : 1;
        }

        BakedPageView view;
        if (stripsEnabled && DataWin_getBakedPage(dw, pageId, &view) && view.checkpoints != NULL &&
            view.width > 0 && view.height > 0 && view.stripCount > 0 &&
            view.stripRows > 0 && view.stripCount <= 32) {
            SWTexture* sparse = swrCreateSparseTexture(view.width, view.height, view.stripRows, view.stripCount);
            if (sparse != NULL) {
                sparse->lastUsedTick = ++swr->textureUseClock;
                swr->textures[pageId] = sparse;
                swr->textureLoads++;
                return true;
            }
        }
    }
#endif

    Texture* txtr = &dw->txtr.textures[pageId];

    // If a pre-baked sidecar is present, use its uncompressed QOI blob and skip
    // the expensive BZip2 step entirely.
    size_t bakedSize = 0;
    const uint8_t* bakedBlob = DataWin_getBakedTexture(dw, pageId, &bakedSize);
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);

    const uint8_t* decodeBlob = bakedBlob;
    size_t decodeBlobSize = bakedSize;
    bool decodeBlobIsGm2022_5 = false; // sidecar blobs are plain "fioq"
    uint8_t* extractedQoi = NULL;

    if (decodeBlob == NULL) {
        if (!txtr->blobData) {
            DataWin_loadTxtrIfNeeded(dw, pageId);
        }
        decodeBlob = txtr->blobData;
        decodeBlobSize = (size_t) txtr->blobSize;
        decodeBlobIsGm2022_5 = gm2022_5;

        if (decodeBlob != NULL) {
            // Unwrap BZip2 exactly once; the QOI result is also appended to the
            // on-device sidecar so the next launch can skip this step.
            size_t qoiSize = 0;
            extractedQoi = ImageDecoder_extractQoi(decodeBlob, decodeBlobSize, gm2022_5, &qoiSize);
            if (extractedQoi != NULL) {
                DataWin_autoBakeTexture(dw, pageId, extractedQoi, qoiSize);
                decodeBlob = extractedQoi;
                decodeBlobSize = qoiSize;
                decodeBlobIsGm2022_5 = false;
            }
        }
    }

    int w, h;
    uint8_t* pixels = NULL;

    if (decodeBlob != NULL) {
        // Reserve the decoded size before decoding so under memory pressure we
        // evict first rather than getting OOM-killed mid-decode.
        int probeW = 0, probeH = 0;
        if (ImageDecoder_probeDimensions(decodeBlob, decodeBlobSize, decodeBlobIsGm2022_5, &probeW, &probeH)) {
            swrReserveTextureBytes(swr, (size_t) probeW * (size_t) probeH * sizeof(uintpixel_t));
        }

        do
        {
#if PIXEL_SIZE == 16
            // Decode straight to RGB1555: no full-size RGBA intermediate buffer.
            pixels = (uint8_t*) ImageDecoder_decodeToRgb565(decodeBlob, decodeBlobSize, decodeBlobIsGm2022_5, &w, &h);
#else
            pixels = ImageDecoder_decodeToRgba(decodeBlob, decodeBlobSize, decodeBlobIsGm2022_5, &w, &h);
#endif
            if (pixels != NULL) {
                break;
            }

            fprintf(stderr, "swr: Failed to decode TXTR page %u.  This is likely because we're out of memory, so evicting a texture.\n", pageId);

            int victim = swrLeastRecentlyUsedPage(swr);
            if (victim < 0) {
                fprintf(stderr, "swr: Looks like we can't fit this texture in memory at all. Bummer.\n");
                break;
            }

            swrEvictTextureFromCache(swr, victim);
            fprintf(stderr, "swr: Evicted texture %d, trying again.\n", victim);
        }
        while (true);

        if (pixels != NULL && bakedBlob == NULL && !txtr->mapped) {
            free(txtr->blobData);
            txtr->blobData = NULL;
        }
    } else {
        fprintf(stderr, "swr: Failed to load TXTR page %u.  This is likely because we're out of memory, so evicting a texture.\n", pageId);
    }

    free(extractedQoi);

    if (pixels == nullptr) {
        fprintf(stderr, "swr: Failed to decode TXTR page %u.\n", pageId);
        return false;
    }

    // Use the actual decoded size in case the probe disagreed with the decoder.
    swrReserveTextureBytes(swr, (size_t) w * (size_t) h * sizeof(uintpixel_t));
#if PIXEL_SIZE == 16
    // The 16bpp decoder already produced pixels in the renderer's native
    // format, so take ownership instead of allocating and copying again.
    swr->textures[pageId] = swrAdoptTexture((uintpixel_t*) pixels, w, h);
#else
    swr->textures[pageId] = swrCreateTexture(pixels, w, h);
    free(pixels);
#endif
    swr->textures[pageId]->lastUsedTick = ++swr->textureUseClock;
    swr->textureBytes += swr->textures[pageId]->byteSize;
    swr->textureLoads++;
    
    fprintf(stderr, "SWR: Loaded TXTR page %u (%dx%d, cache %zu/%zu MB)\n",
            pageId, w, h, swr->textureBytes / (1024 * 1024), swr->textureBudgetBytes / (1024 * 1024));
    
    return true;
}

// Decodes one strip of a sparse page into its buffer.
static bool swrLoadStrip(SWRenderer* swr, uint32_t pageId, int stripIndex)
{
    SWTexture* tex = swr->textures[pageId];
    if (tex == NULL) return false;
    if (tex->stripCount == 0) return true; // not sparse: fully resident
    if (stripIndex < 0 || stripIndex >= (int) tex->stripCount) return true;
    if (tex->stripsLoaded & (1u << stripIndex)) return true;

#if PIXEL_SIZE != 16
    // Only the 16bpp path stores sparse pages; this cannot happen.
    return false;
#else
    BakedPageView view;
    if (!DataWin_getBakedPage(swr->base.dataWin, pageId, &view) || view.checkpoints == NULL) {
        return false;
    }

    int firstRow = stripIndex * (int) tex->stripRows;
    int rows = (int) tex->height - firstRow;
    if (rows > (int) tex->stripRows) rows = (int) tex->stripRows;
    if (rows <= 0) return true;

    size_t pixels = (size_t) tex->width * (size_t) rows;
    size_t stripBytes = pixels * sizeof(uintpixel_t);

    // Reserve before decoding so a full cache evicts another page instead of
    // growing past the budget.
    swrReserveTextureBytes(swr, stripBytes);

    uintpixel_t* dst = tex->buffer + (size_t) firstRow * (size_t) tex->width;
    if (!ImageDecoder_decodeStrip(view.qoi, view.qoiSize, &view.checkpoints[stripIndex],
                                  (uint16_t*) dst, pixels)) {
        fprintf(stderr, "swr: failed to decode strip %d of TXTR page %u\n", stripIndex, pageId);
        return false;
    }

    tex->stripsLoaded |= (1u << stripIndex);
    tex->byteSize += stripBytes;
    swr->textureBytes += stripBytes;
    return true;
#endif
}

// Makes sure the pixels under [sx,sy,sw,sh] are resident. Only the strips that
// rectangle covers are decoded, so a sprite costs a fraction of a full page.
bool swrEnsureTextureRegion(SWRenderer* swr, uint32_t pageId, int sx, int sy, int sw, int sh)
{
    if (!swrCreatePageTextureIfNeeded(swr, pageId)) return false;

    SWTexture* tex = swr->textures[pageId];
    tex->lastUsedTick = ++swr->textureUseClock;

    if (tex->stripCount == 0) return true;

    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sw <= 0 || sh <= 0) return true;
    if (sx >= (int) tex->width || sy >= (int) tex->height) return true;
    if (sx + sw > (int) tex->width) sw = (int) tex->width - sx;
    if (sy + sh > (int) tex->height) sh = (int) tex->height - sy;

    int firstStrip = sy / (int) tex->stripRows;
    int lastStrip = (sy + sh - 1) / (int) tex->stripRows;
    if (lastStrip >= (int) tex->stripCount) lastStrip = (int) tex->stripCount - 1;

    for (int strip = firstStrip; strip <= lastStrip; strip++) {
        if (!swrLoadStrip(swr, pageId, strip)) return false;
    }

    return true;
}

// Whole-page residency (text, tiled backgrounds, prefetch).
bool swrEnsureTextureIsLoaded(SWRenderer* swr, uint32_t pageId)
{
    if (!swrCreatePageTextureIfNeeded(swr, pageId)) return false;

    SWTexture* tex = swr->textures[pageId];
    tex->lastUsedTick = ++swr->textureUseClock;

    if (tex->stripCount == 0) return true;

    for (int strip = 0; strip < (int) tex->stripCount; strip++) {
        if (!swrLoadStrip(swr, pageId, strip)) return false;
    }

    return true;
}
