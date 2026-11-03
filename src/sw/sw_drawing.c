#include <stdlib.h>
#include <limits.h>
#include <float.h>
#include <string.h>
#include "text_utils.h"
#include "sw_renderer_private.h"

// ==== Internal structures ====

typedef struct
{
    Font* font;
    TexturePageItem* fontTpag; // single TPAG for regular fonts (NULL for sprite fonts)
    int fontTpagIndex;
    int fontPageId;
    Sprite* spriteFontSprite; // source sprite for sprite fonts (NULL for regular fonts)
}
SwrFontState;

// ==== Internal functions ====

FORCE_INLINE void swrPlotPixel(Renderer* renderer, int x, int y, uintpixel_t color, int srcalpha, int dstalpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (x < swr->portX || y < swr->portY) return;
    if (x >= swr->maxX || y >= swr->maxY) return;
    
    alphaBlend(&swr->fb[y * swr->fbPitch + x], color, srcalpha, dstalpha);
}

// Draws `count` pixels of one colour through the extended blend state. This is the
// path the Chapter 3 mask scripts take: they fill the whole camera rectangle while
// only the alpha channel is writable, to clear or restore the stencil.
static void swrDrawSpanEx(SWRenderer* swr, int x, int y, int count, uintpixel_t color, int alpha, uint16_t maskBits)
{
    uintpixel_t* dstline = &swr->fb[y * swr->fbPitch + x];
    int srcAlpha = alpha < 0 ? 0 : (alpha > 256 ? 256 : alpha);
    uintpixel_t srcPixel = (uintpixel_t) ((color & 0x7FFF) | 0x8000);

    if (swr->alphaTestEnabled && srcAlpha * 255 < swr->alphaTestRef * 256)
        return;

    if (swr->pixelMode == SWR_PIXEL_REPLACE)
    {
        // Same result for every pixel, so this reduces to a masked memset.
        uintpixel_t value = (uintpixel_t) ((srcPixel & SWR_COLOR_BITS) | (srcAlpha > 127 ? SWR_ALPHA_BIT : 0));
        if (maskBits == 0xFFFF)
        {
            for (int i = 0; i < count; i++) dstline[i] = value;
        }
        else
        {
            for (int i = 0; i < count; i++)
            {
                uintpixel_t dst = dstline[i];
                dstline[i] = (uintpixel_t) ((dst & ~maskBits) | (value & maskBits));
            }
        }
        return;
    }

    for (int i = 0; i < count; i++)
        swrBlitPixelEx(swr, &dstline[i], srcPixel, srcAlpha, maskBits);
}

// ===[ Draw trace ]===
//
// One line per draw, so a frame can be compared against the instances that are on
// stage: an instance whose sprite never appears here was not drawn at all, which is
// a very different bug from one that was drawn with the wrong colour or blend.
// Every draw is logged when the frame trace is on, and when BUTTERSCOTCH_LOG_BLEND is
// set while drawing into a surface: surface-offscreen passes are where the effect
// state lives, and logging all of them costs a handful of lines while logging every
// draw of a busy room would bury the log.
static void swrTraceDraw(SWRenderer* swr, const char* kind, const SWTexture* texture,
                         int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh,
                         int alpha, uint32_t tint)
{
    if (!swr->traceDraws && !(swrIsOnSurface(swr) && swrDiagLogEnabled())) return;

    int page = -1;
    if (texture != nullptr) {
        for (size_t i = 0; i < swr->textureCount; i++) {
            if (swr->textures[i] == texture) { page = (int) i; break; }
        }
    }

    const char* shaderName = "-";
    DataWin* dataWin = swr->base.dataWin;
    if (swr->shaderIndex >= 0 && dataWin != nullptr && (uint32_t) swr->shaderIndex < dataWin->shdr.count)
        shaderName = dataWin->shdr.shaders[swr->shaderIndex].name;

    // Report both ends: the GameMaker colour the game asked for and the RGB555 value
    // the blitter will actually multiply the sprite by. The old line printed a masked
    // truncation of the raw colour, which cannot be turned back into either one.
    const unsigned rawColor = (unsigned) (tint & 0xFFFFFFu);
    const unsigned blitColor = (unsigned) (swrConvertPixel(tint) & 0x7FFFu);
    logInfo("DRAW %-8s page=%2d src=%4d,%4d,%4d,%4d dst=%5d,%5d,%4d,%4d alpha=%3d tint=%04X raw=%06X blend=%d mask=%04X mode=%d surf=%d shd=%s swr=%p\n",
            kind, page, sx, sy, sw, sh, dx, dy, dw, dh, alpha, blitColor, rawColor,
            swr->blendMode, (unsigned) swr->writeMask, swr->pixelMode,
            swrIsOnSurface(swr) ? swr->currentSurfaceIndex : -1, shaderName, (void*) swr);
}

// ==== Vectorised fills ====
//
// Solid-colour spans are the cheapest thing in the renderer to vectorise and they
// dominate some scenes: Deltarune Chapter 3's iris/spotlight object repaints a
// full-screen veil plus three discs into a 2040x480 surface every single frame,
// which is two full-screen fills per frame on top of the scene. As 8-lane vectors
// each span costs about a quarter of the scalar alphaBlend() loop on a Cortex-A7.
#if PIXEL_SIZE == 16 && defined(__GNUC__)
typedef uint16_t swrVec8 __attribute__((vector_size(16)));
typedef int16_t swrVec8s __attribute__((vector_size(16)));

FORCE_INLINE swrVec8 swrSplat8(uint16_t value)
{
    swrVec8 out = { value, value, value, value, value, value, value, value };
    return out;
}

// True when all eight pixels are opaque, i.e. the group can be stored whole.
FORCE_INLINE bool swrVecAllOpaque(swrVec8 src)
{
    const swrVec8 bits = src & swrSplat8(0x8000);
    uint64_t halves[2];
    memcpy(halves, &bits, sizeof(halves));
    return (halves[0] & halves[1]) == 0x8000800080008000ull;
}

FORCE_INLINE swrVec8 swrMinVec(swrVec8 a, swrVec8 b)
{
    swrVec8 lower = (a < b);
    return (a & lower) | (b & ~lower);
}

// 0 = normal, 1 = additive (clamped per channel), 2 = subtract.
#define SWR_FILL_NORMAL   0
#define SWR_FILL_ADD      1
#define SWR_FILL_SUBTRACT 2

// Fills `count` pixels from `dst` with one colour. `srcalpha`/`dstalpha` follow the
// same convention as alphaBlend().
static void swrFillSpanVector(uintpixel_t* dst, int count, uintpixel_t color,
                              int srcalpha, int dstalpha, int mode)
{
    const swrVec8 mask5 = swrSplat8(0x1F);
    const swrVec8 opaqueBit = swrSplat8(0x8000);

    // bm_subtract arrives with a negated weight (swrCalcSrcAlpha does that); the
    // magnitude is what the per-channel maths needs, while the tail must keep the
    // sign so alphaBlend() reaches the same conclusion.
    const int mag = (mode == SWR_FILL_SUBTRACT) ? -srcalpha : srcalpha;

    // alphaBlend() short-circuits these two cases before any arithmetic; if the
    // vector path did not do the same the results would visibly disagree (a fully
    // transparent write would flip the destination alpha bit on, and an opaque one
    // would gain an alpha bit it must not have).
    if (srcalpha == 0) return;
    if (dstalpha < 3 && srcalpha > 253)
    {
        // Write whole vectors while a whole vector fits, then the remainder one
        // pixel at a time. The previous version stored eight pixels per iteration
        // over `count` iterations, so it ran up to seven pixels past the end of the
        // span; when the span ended on the last row of the framebuffer that wrote
        // past the allocation and corrupted the heap (it is what made the chapter
        // hand-off die with "malloc(): corrupted top size").
        swrVec8 src = swrSplat8(color);
        int i = 0;
        for (; i + 8 <= count; i += 8)
            memcpy(dst + i, &src, sizeof(src));
        for (; i < count; i++)
            dst[i] = color;
        return;
    }

    const int sr = (color >> 10) & 0x1F;
    const int sg = (color >> 5) & 0x1F;
    const int sb = color & 0x1F;

    // For add/subtract the source term is a constant, so it is folded once. The
    // subtract term rounds up because the scalar path computes the difference before
    // shifting, which floors the negated sum.
    const int round = (mode == SWR_FILL_SUBTRACT) ? 255 : 0;
    const int addR = (sr * mag + round) >> 8;
    const int addG = (sg * mag + round) >> 8;
    const int addB = (sb * mag + round) >> 8;
    const swrVec8 kR = swrSplat8((uint16_t) addR);
    const swrVec8 kG = swrSplat8((uint16_t) addG);
    const swrVec8 kB = swrSplat8((uint16_t) addB);
    const swrVec8 sR = swrSplat8((uint16_t) sr);
    const swrVec8 sG = swrSplat8((uint16_t) sg);
    const swrVec8 sB = swrSplat8((uint16_t) sb);
    const swrVec8 srcAlpha = swrSplat8((uint16_t) mag);
    const swrVec8 dstAlpha = swrSplat8((uint16_t) dstalpha);

    // A subtract removes the destination alpha bit as well; see alphaBlend().
    const swrVec8 alphaBits = (mode == SWR_FILL_SUBTRACT) ? swrSplat8(0) : opaqueBit;

    int i = 0;
    for (; i + 8 <= count; i += 8)
    {
        swrVec8 d;
        memcpy(&d, dst + i, sizeof(d));

        swrVec8 dr = (d >> 10) & mask5;
        swrVec8 dg = (d >> 5) & mask5;
        swrVec8 db = d & mask5;

        swrVec8 r, g, b;
        if (mode == SWR_FILL_ADD)
        {
            r = swrMinVec(dr + kR, mask5);
            g = swrMinVec(dg + kG, mask5);
            b = swrMinVec(db + kB, mask5);
        }
        else if (mode == SWR_FILL_SUBTRACT)
        {
            // dst - min(dst, delta) saturates at zero without a signed compare.
            r = dr - swrMinVec(dr, kR);
            g = dg - swrMinVec(dg, kG);
            b = db - swrMinVec(db, kB);
        }
        else
        {
            r = (dr * dstAlpha + sR * srcAlpha) >> 8;
            g = (dg * dstAlpha + sG * srcAlpha) >> 8;
            b = (db * dstAlpha + sB * srcAlpha) >> 8;
        }

        swrVec8 out = alphaBits | (r << 10) | (g << 5) | b;
        memcpy(dst + i, &out, sizeof(out));
    }

    for (; i < count; i++)
        alphaBlend(&dst[i], color, srcalpha, dstalpha);
}

// The vector filler reproduces alphaBlend() for the modes it accepts.
FORCE_INLINE bool swrCanFillSpanVector(int srcalpha)
{
    return srcalpha >= -256 && srcalpha <= 256;
}

FORCE_INLINE int swrFillSpanMode(const SWRenderer* swr)
{
    if (swr->blendMode == bm_add) return SWR_FILL_ADD;
    if (swr->blendMode == bm_subtract) return SWR_FILL_SUBTRACT;
    return SWR_FILL_NORMAL;
}
#endif

static void swrDrawHLineInt(Renderer* renderer, int dx, int dy, int dw, uintpixel_t color, UNUSED uintpixel_t color2, int alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    if (dy < swr->portY) return;
    if (dy >= swr->maxY) return;
    if (dx < swr->portX) { dw += dx - swr->portX; dx = swr->portX; }
    if (dx + dw >= swr->maxX) dw = swr->maxX - dx;
    if (dw <= 0) return;

    if (swr->pixelMode != SWR_PIXEL_LEGACY)
    {
        swrDrawSpanEx(swr, dx, dy, dw, color, alpha, swrWriteMaskBits(swr->writeMask));
        return;
    }
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    
#if PIXEL_SIZE == 32
    if (color == color2)
#endif
    {
        uintpixel_t *line = &swr->fb[dy * swr->fbPitch + dx];

#if PIXEL_SIZE == 16 && defined(__GNUC__)
        if (swrCanFillSpanVector(srcalpha))
        {
            swrFillSpanVector(line, dw, color, srcalpha, invalpha, swrFillSpanMode(swr));
            return;
        }
#endif

        for (int i = 0; i < dw; i++)
            alphaBlend(&line[i], color, srcalpha, invalpha);
    }
#if PIXEL_SIZE == 32
    else
    {
        Pixel32ARGB clr1, clr2;
        clr1.l = color;
        clr2.l = color2;
        
        uint32_t rinit = clr1.p.r << 20;
        uint32_t ginit = clr1.p.g << 20;
        uint32_t binit = clr1.p.b << 20;
        int32_t rstep = ((int)clr2.p.r - clr1.p.r) << 20;
        int32_t gstep = ((int)clr2.p.g - clr1.p.g) << 20;
        int32_t bstep = ((int)clr2.p.b - clr1.p.b) << 20;
        rstep /= dw;
        gstep /= dw;
        bstep /= dw;
        
        uintpixel_t *line = &swr->fb[dy * swr->fbPitch + dx];
        for (int i = 0; i < dw; i++)
        {
            Pixel32ARGB resultPixel;
            resultPixel.p.r = rinit >> 20;
            resultPixel.p.g = ginit >> 20;
            resultPixel.p.b = binit >> 20;
            rinit += rstep;
            ginit += gstep;
            binit += bstep;
            alphaBlend(&line[i], resultPixel.l, srcalpha, invalpha);
        }
    }
#endif
}

static void swrDrawVLineInt(Renderer* renderer, int dx, int dy, int dh, uintpixel_t color, UNUSED uintpixel_t color2, int alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    if (dx < swr->portX) return;
    if (dx >= swr->maxX) return;
    if (dy < swr->portY) { dh += dy - swr->portY; dy = swr->portY; }
    if (dy + dh >= swr->maxY) dh = swr->maxY - dy;
    if (dh <= 0) return;

    if (swr->pixelMode != SWR_PIXEL_LEGACY)
    {
        uint16_t maskBits = swrWriteMaskBits(swr->writeMask);
        for (int i = 0; i < dh; i++)
            swrDrawSpanEx(swr, dx, dy + i, 1, color, alpha, maskBits);
        return;
    }
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    
#if PIXEL_SIZE == 32
    if (color == color2)
#endif
    {
        for (int i = 0; i < dh; i++)
        {
            uintpixel_t *line = &swr->fb[(dy + i) * swr->fbPitch + dx];
            alphaBlend(&line[0], color, srcalpha, invalpha);
        }
    }
#if PIXEL_SIZE == 32
    else
    {
        Pixel32ARGB clr1, clr2;
        clr1.l = color;
        clr2.l = color2;
        
        uint32_t rinit = clr1.p.r << 20;
        uint32_t ginit = clr1.p.g << 20;
        uint32_t binit = clr1.p.b << 20;
        int32_t rstep = ((int)clr2.p.r - clr1.p.r) << 20;
        int32_t gstep = ((int)clr2.p.g - clr1.p.g) << 20;
        int32_t bstep = ((int)clr2.p.b - clr1.p.b) << 20;
        rstep /= dh;
        gstep /= dh;
        bstep /= dh;
        
        for (int i = 0; i < dh; i++)
        {
            uintpixel_t *line = &swr->fb[(dy + i) * swr->fbPitch + dx];
            Pixel32ARGB resultPixel;
            resultPixel.p.r = rinit >> 20;
            resultPixel.p.g = ginit >> 20;
            resultPixel.p.b = binit >> 20;
            rinit += rstep;
            ginit += gstep;
            binit += bstep;
            alphaBlend(&line[0], resultPixel.l, srcalpha, invalpha);
        }
    }
#endif
}

static void swrDrawLineInt(Renderer* renderer, int x1, int y1, int x2, int y2, MAYBE_UNUSED int width, uintpixel_t color1, uintpixel_t color2, int alpha)
{
    if (x1 == x2)
    {
        swrDrawVLineInt(renderer, x1, swrMin(y1, y2), swrAbs(y1 - y2), color1, color2, alpha);
        return;
    }
    if (y1 == y2)
    {
        swrDrawHLineInt(renderer, swrMin(x1, x2), y1, swrAbs(x1 - x2), color1, color2, alpha);
        return;
    }
    
    int dx = x2 - x1, dy = y2 - y1;
    int dx1 = swrAbs(dx), dy1 = swrAbs(dy), xe, ye, x, y;
    int px = 2 * dy1 - dx1, py = 2 * dx1 - dy1;
    int srcalpha = swrCalcSrcAlpha((SWRenderer*) renderer, alpha);
    int invalpha = swrCalcDstAlpha((SWRenderer*) renderer, alpha);
    
    uintpixel_t color = color1;
#if PIXEL_SIZE == 32
    Pixel32ARGB clr1, clr2;
    clr1.l = color1;
    clr2.l = color2;
    
    uint32_t rinit = clr1.p.r << 20;
    uint32_t ginit = clr1.p.g << 20;
    uint32_t binit = clr1.p.b << 20;
    int32_t rstep = ((int)clr2.p.r - clr1.p.r) << 20;
    int32_t gstep = ((int)clr2.p.g - clr1.p.g) << 20;
    int32_t bstep = ((int)clr2.p.b - clr1.p.b) << 20;
#endif
    
    if (dy1 <= dx1)
    {
        if (dx >= 0)
        {
            x = x1, y = y1, xe = x2;
        }
        else
        {
            x = x2, y = y2, xe = x1;
        }
        
#if PIXEL_SIZE == 32
        if (dx1 > 0) {
            rstep /= dx1;
            gstep /= dx1;
            bstep /= dx1;
        } else {
            rstep = gstep = bstep = 0;
        }
#endif
        
        swrPlotPixel(renderer, x, y, color, srcalpha, invalpha);
        
        while (x < xe)
        {
            x++;
            if (px < 0)
            {
                px += 2 * dy1;
            }
            else
            {
                if ((dx < 0 && dy < 0) || (dx > 0 && dy > 0)) y++; else y--;
                px += 2 * (dy1 - dx1);
            }
            
#if PIXEL_SIZE == 32
            Pixel32ARGB resultPixel;
            resultPixel.p.r = rinit >> 20;
            resultPixel.p.g = ginit >> 20;
            resultPixel.p.b = binit >> 20;
            rinit += rstep;
            ginit += gstep;
            binit += bstep;
            color = resultPixel.l;
#endif
            
            swrPlotPixel(renderer, x, y, color, srcalpha, invalpha);
        }
    }
    else
    {
        if (dy >= 0)
        {
            x = x1, y = y1, ye = y2;
        }
        else
        {
            x = x2, y = y2, ye = y1;
        }
        
#if PIXEL_SIZE == 32
        if (dy1 > 0) {
            rstep /= dy1;
            gstep /= dy1;
            bstep /= dy1;
        } else {
            rstep = gstep = bstep = 0;
        }
#endif
        
        swrPlotPixel(renderer, x, y, color, srcalpha, invalpha);
        
        while (y < ye)
        {
            y++;
            if (py <= 0)
            {
                py += 2 * dx1;
            }
            else
            {
                if ((dx < 0 && dy < 0) || (dx > 0 && dy > 0)) x++; else x--;
                py += 2 * (dx1 - dy1);
            }
            
#if PIXEL_SIZE == 32
            Pixel32ARGB resultPixel;
            resultPixel.p.r = rinit >> 20;
            resultPixel.p.g = ginit >> 20;
            resultPixel.p.b = binit >> 20;
            rinit += rstep;
            ginit += gstep;
            binit += bstep;
            color = resultPixel.l;
#endif
            
            swrPlotPixel(renderer, x, y, color, srcalpha, invalpha);
        }
    }
}

// Copies one run of destination pixels from a 1:1 source, skipping transparent
// source pixels. Eight pixels are tested at a time so a completely opaque group turns
// into one 16-byte load and one 16-byte store instead of eight per-pixel branches.
//
// Tile maps and background layers are mostly opaque runs, and they are the bulk of
// what a software renderer spends its time on, so this is worth the special case.
//
// The store goes through a may_alias word pointer rather than memcpy: GCC 15 cannot
// bound `dst + x` here and emits a spurious -Wstringop-overflow for the memcpy.
typedef uint32_t swrAliasWord __attribute__((may_alias));

FORCE_INLINE void swrCopyRunOpaque(uintpixel_t* dst, const uintpixel_t* src, int count)
{
#if PIXEL_SIZE == 16
    int x = 0;

    for (; x + 8 <= count; x += 8)
    {
        swrVec8 group;
        memcpy(&group, src + x, sizeof(group));

        if (swrVecAllOpaque(group))
        {
            memcpy(dst + x, &group, sizeof(group));
        }
        else
        {
            for (int i = 0; i < 8; i++)
            {
                uintpixel_t pixel = src[x + i];
                if (opaque(pixel)) dst[x + i] = pixel;
            }
        }
    }

    for (; x < count; x++)
    {
        uintpixel_t pixel = src[x];
        if (opaque(pixel)) dst[x] = pixel;
    }
#else
    for (int x = 0; x < count; x++)
    {
        uintpixel_t pixel = src[x];
        if (opaque(pixel)) dst[x] = pixel;
    }
#endif
}

// ==== Vectorised blend ====
//
// The dominant cost in a busy scene is alpha blending: Deltarune's Cyber World
// frames blend ~2 million pixels (7 screens of overdraw), and the scalar path costs
// ~40 cycles per pixel on a Cortex-A7, which is where the entire frame budget goes.
//
// This kernels eight pixels at a time for the most common non-trivial blit:
// 16bpp, unscaled, unflipped, untinted, normal blend mode. It is written with
// GCC/Clang vector extensions rather than intrinsics so it compiles to NEON on the
// handheld *and* to SSE on the host, which is what makes it testable for bit
// exactness off-device.
//
// The math is identical to alphaBlend() for this case: with a normal blend the
// weights sum to 256, so each channel stays inside its 5-bit range and the scalar
// clamps never fire.
#if PIXEL_SIZE == 16 && defined(__GNUC__)
FORCE_INLINE swrVec8 swrBlendVec(swrVec8 src, swrVec8 dst, swrVec8 tintR, swrVec8 tintG, swrVec8 tintB,
                                 bool applyTint, bool additive, int srcalpha, int dstalpha)
{
    const swrVec8 mask5 = swrSplat8(0x1F);
    const swrVec8 srcAlpha = swrSplat8((uint16_t) srcalpha);
    const swrVec8 dstAlpha = swrSplat8((uint16_t) dstalpha);
    const swrVec8 opaqueBit = swrSplat8(0x8000);

    swrVec8 sr = (src >> 10) & mask5;
    swrVec8 sg = (src >> 5) & mask5;
    swrVec8 sb = src & mask5;

    // Same as tint(): a multiply by the tint channel divided by 32. Only applied
    // when the tint really is non-white, because tint() short-circuits that case.
    if (applyTint)
    {
        sr = (sr * tintR) >> 5;
        sg = (sg * tintG) >> 5;
        sb = (sb * tintB) >> 5;
    }

    swrVec8 dr = (dst >> 10) & mask5;
    swrVec8 dg = (dst >> 5) & mask5;
    swrVec8 db = dst & mask5;

    swrVec8 r, g, b;
    if (additive)
    {
        // bm_add: destination plus the (alpha-weighted) source, saturated per
        // channel. The scalar path lets the 5-bit channels overflow into their
        // neighbours instead of clamping; clamping is what the effect is supposed
        // to look like and it is free here.
        r = swrMinVec(dr + ((sr * srcAlpha) >> 8), mask5);
        g = swrMinVec(dg + ((sg * srcAlpha) >> 8), mask5);
        b = swrMinVec(db + ((sb * srcAlpha) >> 8), mask5);
    }
    else
    {
        r = (dr * dstAlpha + sr * srcAlpha) >> 8;
        g = (dg * dstAlpha + sg * srcAlpha) >> 8;
        b = (db * dstAlpha + sb * srcAlpha) >> 8;
    }

    swrVec8 blended = opaqueBit | (r << 10) | (g << 5) | b;

    // lanes whose source pixel is transparent (bit 15 clear) keep the destination
    swrVec8 keepMask = (swrVec8) (((swrVec8s) src) >> 15);

    return (blended & keepMask) | (dst & ~keepMask);
}

// True when the vector kernel handles this blend. bm_normal reproduces the scalar
// output exactly; bm_add clamps instead of letting 5-bit channels overflow. That is
// the only intentional difference in output, and it fixes a channel-bleed artefact
// rather than introducing one.
FORCE_INLINE bool swrCanBlendVector(const SWRenderer* swr, int srcalpha, int dstalpha)
{
    if (srcalpha <= 0 || srcalpha > 256) return false;
    if (swr->blendMode == bm_normal) return dstalpha == 256 - srcalpha;
    if (swr->blendMode == bm_add) return dstalpha == 256;
    return false;
}

FORCE_INLINE bool swrBlendIsAdditive(const SWRenderer* swr)
{
    return swr->blendMode == bm_add;
}

// True when any of the eight source pixels carries the alpha bit. Sprite draws are
// silhouettes inside a rectangle, so a lot of eight-pixel groups are pure padding.
// swrBlendVec() computes every lane and only merges the destination back afterwards,
// so blending a transparent group is a no-op that still costs the whole blend; skip
// it instead. The output is identical, not merely similar.
FORCE_INLINE bool swrVecAnyOpaque(swrVec8 src)
{
    const swrVec8 bits = src & swrSplat8(0x8000);
    // Reduce through the two halves: the GCC/clang vector extensions this file is
    // written against have no movemask.
    uint64_t halves[2];
    memcpy(halves, &bits, sizeof(halves));
    return (halves[0] | halves[1]) != 0;
}

// True when the row holds at least one opaque pixel. The middle of a sprite answers
// on the first group, so this costs a few cycles; the fully transparent margin rows
// pay for the scan and then skip a whole destination row of blending.
FORCE_INLINE bool swrRowAnyOpaque(const uintpixel_t* row, int count)
{
    int x = 0;
    for (; x + 8 <= count; x += 8)
    {
        swrVec8 s;
        memcpy(&s, row + x, sizeof(s));
        if (swrVecAnyOpaque(s)) return true;
    }
    for (; x < count; x++)
    {
        if (opaque(row[x])) return true;
    }
    return false;
}

// Blends a scaled (nearest-neighbour) sprite rectangle eight pixels at a time.
// The gather is still scalar - source columns are stepped by a fixed-point delta -
// but it is a handful of adds and loads per eight pixels, while the blend maths that
// follows is the expensive part and runs on vectors.
static void swrBlendSpriteScaledVector(SWRenderer* swr, int dx, int dy, int dw, int dh,
                                       const SWTexture* texture, int sx, int sy, int sh_val,
                                       int iys, int oys, int iys2, int oys2,
                                       int ixs2, int oxs2, int fp_prec,
                                       uintpixel_t tintColor, int srcalpha, int dstalpha)
{
    const bool applyTint = !swrTintIsWhite(tintColor);
    const bool additive = swrBlendIsAdditive(swr);
    const swrVec8 tintR = swrSplat8((uint16_t) ((tintColor >> 10) & 0x1F));
    const swrVec8 tintG = swrSplat8((uint16_t) ((tintColor >> 5) & 0x1F));
    const swrVec8 tintB = swrSplat8((uint16_t) (tintColor & 0x1F));

    // Columns this draw actually samples, so the padding test below only looks at
    // the strip it reads rather than the whole texture page.
    const int32_t firstColumn = ixs2 >> fp_prec;
    const int32_t lastColumn = (ixs2 + (int32_t) (dw - 1) * oxs2) >> fp_prec;
    const int scanStart = sx + (firstColumn < lastColumn ? firstColumn : lastColumn);
    const int scanCount = (firstColumn < lastColumn ? lastColumn - firstColumn
                                                   : firstColumn - lastColumn) + 1;
    const bool scanInBounds = scanStart >= 0 && scanCount > 0
                              && scanStart + scanCount <= (int) texture->width;

    int32_t ys2 = iys2;
    // An upscaled draw samples the same source row many times over - a 600px strip
    // stretched to 9600 tests each row sixteen times - so remember the answer.
    int checkedRow = INT_MIN;
    bool checkedRowHasPixels = true;

    for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
    {
        uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
        const int srcRow = (dh == sh_val) ? sy + ys : sy + (int) (ys2 >> fp_prec);
        if (scanInBounds && srcRow != checkedRow)
        {
            checkedRow = srcRow;
            checkedRowHasPixels = swrRowAnyOpaque(&texture->buffer[srcRow * texture->width + scanStart],
                                                  scanCount);
        }
        if (!checkedRowHasPixels) continue;
        const uintpixel_t* srcline = &texture->buffer[srcRow * texture->width + sx];

        int32_t xs2 = ixs2;
        int x = 0;

        for (; x + 8 <= dw; x += 8)
        {
            swrVec8 s, d;
            const int32_t groupFirst = xs2 >> fp_prec;
            if ((xs2 + 7 * oxs2) >> fp_prec == groupFirst)
            {
                // A wide upscale: all eight destination pixels sample one texel (the
                // final battle's background bands stretch a 600px strip to 9000px, so
                // every group is a single source pixel). Splat it rather than running
                // the eight-step gather, which is half of this loop's cost.
                s = swrSplat8(srcline[groupFirst]);
                xs2 += oxs2 * 8;
            }
            else
            {
                uint16_t gathered[8];
                for (int k = 0; k < 8; k++)
                {
                    gathered[k] = srcline[(int)(xs2 >> fp_prec)];
                    xs2 += oxs2;
                }
                memcpy(&s, gathered, sizeof(s));
            }
            if (!swrVecAnyOpaque(s)) continue;
            memcpy(&d, dstline + x, sizeof(d));

            swrVec8 out = swrBlendVec(s, d, tintR, tintG, tintB, applyTint, additive, srcalpha, dstalpha);
            memcpy(dstline + x, &out, sizeof(out));
        }

        for (; x < dw; x++, xs2 += oxs2)
        {
            uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
            if (opaque(pixel))
                alphaBlend(&dstline[x], tint(tintColor, pixel), srcalpha, dstalpha);
        }
    }
}

// Blends a whole unscaled, unflipped sprite rectangle: one call per draw, so small
// tiles (8x8 UI pieces, for instance) do not pay a call per row.
static void swrBlendSpriteVector(SWRenderer* swr, int dx, int dy, int dw, int dh,
                                 const SWTexture* texture, int sx, int sy,
                                 uintpixel_t tintColor, int srcalpha, int dstalpha)
{
    const bool applyTint = !swrTintIsWhite(tintColor);
    const bool additive = swrBlendIsAdditive(swr);
    const swrVec8 tintR = swrSplat8((uint16_t) ((tintColor >> 10) & 0x1F));
    const swrVec8 tintG = swrSplat8((uint16_t) ((tintColor >> 5) & 0x1F));
    const swrVec8 tintB = swrSplat8((uint16_t) (tintColor & 0x1F));

    for (int y = 0; y < dh; y++)
    {
        const uintpixel_t* srcline = &texture->buffer[(sy + y) * texture->width + sx];
        uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
        if (!swrRowAnyOpaque(srcline, dw)) continue;

        int x = 0;
        for (; x + 8 <= dw; x += 8)
        {
            swrVec8 s, d;
            memcpy(&s, srcline + x, sizeof(s));
            if (!swrVecAnyOpaque(s)) continue;
            memcpy(&d, dstline + x, sizeof(d));

            swrVec8 out = swrBlendVec(s, d, tintR, tintG, tintB, applyTint, additive, srcalpha, dstalpha);
            memcpy(dstline + x, &out, sizeof(out));
        }
        for (; x < dw; x++)
        {
            uintpixel_t pixel = srcline[x];
            if (opaque(pixel))
                alphaBlend(&dstline[x], tint(tintColor, pixel), srcalpha, dstalpha);
        }
    }
}
#endif

static void swrDrawSpriteInternal(
    Renderer* renderer, int dx, int dy, int dw, int dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uintpixel_t tintColor, int alpha
){
    SWRenderer *swr = (SWRenderer*) renderer;
    
    bool flipX = false, flipY = false;
    if (dw < 0) { dx += dw; dw = -dw; flipX = true; }
    if (dh < 0) { dy += dh; dh = -dh; flipY = true; }
    
    //basic out of bound checks
    if (dw == 0 || dh == 0) return;
    if (sw == 0) sw = 1;
    if (sh == 0) sh = 1;
    if (dx + dw <= swr->portX) return;
    if (dy + dh <= swr->portY) return;
    if (dx >= swr->maxX) return;
    if (dy >= swr->maxY) return;
    
    int odw = dw, odh = dh;
    int osw = sw, osh = sh;
    
    int minx = swr->portX, miny = swr->portY, maxx = swr->portX + swr->portW, maxy = swr->portY + swr->portH;
    
    //out of bounds adjustment checks
    int diffxl = 0, diffyl = 0, diffxu = 0, diffyu = 0;
    if (dx < minx) { diffxl = minx - dx; dx = minx; dw -= diffxl; }
    if (dy < miny) { diffyl = miny - dy; dy = miny; dh -= diffyl; }
    if (dx + dw > maxx) { diffxu = dx + dw - maxx; dw -= diffxu; }
    if (dy + dh > maxy) { diffyu = dy + dh - maxy; dh -= diffyu; }
    
    if (diffxl != 0 || diffyl != 0 || diffxu != 0 || diffyu != 0)
    {
        //adjust source coordinates too
        diffxl = (int)((long)diffxl * osw / odw);
        diffyl = (int)((long)diffyl * osh / odh);
        diffxu = (int)((long)(diffxu + 1) * osw / odw);
        diffyu = (int)((long)(diffyu + 1) * osh / odh);
        sx += flipX ? diffxu : diffxl;
        sy += flipY ? diffyu : diffyl;
        sw -= diffxl + diffxu;
        sh -= diffyl + diffyu;
        if (sw <= 0 || sh <= 0) return;
    }
    
    //clip the source coords into bounds too
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw >= texture->width)  { sw = texture->width  - sx; }
    if (sy + sh >= texture->height) { sh = texture->height - sy; }
    if (sw <= 0 || sh <= 0) return;
    
    //okay, now we can finally get on with rendering
    
    int ixs = 0, oxs = 1, iys = 0, oys = 1;
    if (flipX) ixs = dw - 1, oxs = -1;
    if (flipY) iys = dh - 1, oys = -1;
    
    // tweak these if stuff doesn't look right
    typedef int32_t fixedp_t;
    const int fp_prec = 14;

    fixedp_t ystep = (sh == dh) ? (1 << fp_prec) : ((fixedp_t) osh << fp_prec) / odh;
    fixedp_t xstep = (sw == dw) ? (1 << fp_prec) : ((fixedp_t) osw << fp_prec) / odw;
    fixedp_t oxs2 = oxs * xstep;
    fixedp_t oys2 = oys * ystep;
    fixedp_t ixs2 = ixs * xstep;
    fixedp_t iys2 = iys * ystep;
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);

    // Extended GPU state (Chapter 3+'s stencil masks). Handled by a plain scalar
    // loop that consults the blend unit per pixel, because none of the fast paths
    // below can express "write the alpha channel only" or blended destinations.
    if (swr->pixelMode != SWR_PIXEL_LEGACY)
    {
        const uint16_t maskBits = swrWriteMaskBits(swr->writeMask);
        const int extAlpha = alpha < 0 ? 0 : (alpha > 256 ? 256 : alpha);
        const bool extTint = !swrTintIsWhite(tintColor);

        swr->pxWritten += (uint64_t) dw * (uint64_t) dh;
        swr->pxScalarBlend += (uint64_t) dw * (uint64_t) dh;

        fixedp_t ys2 = iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            const uintpixel_t* srcline;
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];

            fixedp_t xs2 = ixs2;
            for (int x = 0; x < dw; x++, xs2 += oxs2)
            {
                uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                if (!opaque(pixel)) continue;
                swrBlitPixelEx(swr, &dstline[x], extTint ? tint(tintColor, pixel) : pixel,
                               extAlpha, maskBits);
            }
        }
        return;
    }

    // Shader draws: the fragment colour comes from the emulated shader at the
    // destination pixel, so none of the fast paths below apply - they all copy the
    // sprite's own colour. The sprite still supplies the coverage and alpha.
    SWRShaderContext shaderCtx;
    if (swrShaderEmuBegin(swr, &shaderCtx))
    {
        const bool shadedTint = !swrTintIsWhite(tintColor);

        swr->pxWritten += (uint64_t) dw * (uint64_t) dh;
        swr->pxScalarBlend += (uint64_t) dw * (uint64_t) dh;

        fixedp_t ys2 = iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            const uintpixel_t* srcline;
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];

            fixedp_t xs2 = ixs2;
            for (int x = 0; x < dw; x++, xs2 += oxs2)
            {
                if (!opaque(srcline[(int)(xs2 >> fp_prec)])) continue;
                uint16_t shaded;
                swrShaderEmuPixel(&shaderCtx, dx + x, dy + y, &shaded);
                alphaBlend(&dstline[x], shadedTint ? tint(tintColor, shaded) : shaded,
                           srcalpha, invalpha);
            }
        }
        return;
    }

    const bool unscaled = (sw == dw && sh == dh && !flipX && !flipY);
    const bool plainCopy = swrTintIsWhite(tintColor) && srcalpha > 253 && invalpha < 3;

#if PIXEL_SIZE == 16 && defined(__GNUC__)
    // Rasterise the whole sprite with the vector kernel when the blend maths allows
    // it. Copies are left to the scalar path below: they are already dominated by the
    // memory traffic, and the SIMD gather would cost more than it saves.
    if (!plainCopy && swrCanBlendVector(swr, srcalpha, invalpha))
    {
        swr->pxWritten += (uint64_t) dw * (uint64_t) dh;
        swr->pxVectorBlend += (uint64_t) dw * (uint64_t) dh;

        if (unscaled)
            swrBlendSpriteVector(swr, dx, dy, dw, dh, texture, sx, sy, tintColor, srcalpha, invalpha);
        else
            swrBlendSpriteScaledVector(swr, dx, dy, dw, dh, texture, sx, sy, sh,
                                       iys, oys, iys2, oys2, ixs2, oxs2, fp_prec,
                                       tintColor, srcalpha, invalpha);
        return;
    }
#endif

    if (unscaled)
    {
        // Unscaled, unflipped blit: what tiles, background layers and most UI end
        // up going through. With no tint and full opacity the source pixel simply
        // replaces the destination, so runs can be copied whole (see
        // swrCopyRunOpaque); the remaining cases blend per pixel.
        swr->pxWritten += (uint64_t) dw * (uint64_t) dh;

        if (plainCopy)
        {
            swr->pxFastCopy += (uint64_t) dw * (uint64_t) dh;

            for (int y = 0; y < dh; y++)
            {
                const uintpixel_t* srcline = &texture->buffer[(sy + y) * texture->width + sx];

                // A layer that reaches across the screen is mostly empty in the rows
                // above and below what it actually covers. Scanning eight pixels at a
                // time costs a fraction of the copy, and a row with no opaque pixel
                // in it would otherwise be walked pixel by pixel for nothing.
                if (!swrRowAnyOpaque(srcline, dw)) continue;

                uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
                swrCopyRunOpaque(dstline, srcline, dw);
            }
        }
        else
        {
            swr->pxScalarBlend += (uint64_t) dw * (uint64_t) dh;

            for (int y = 0; y < dh; y++)
            {
                const uintpixel_t* srcline = &texture->buffer[(sy + y) * texture->width + sx];
                uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];

                for (int x = 0; x < dw; x++)
                {
                    uintpixel_t pixel = srcline[x];
                    if (opaque(pixel))
                        alphaBlend(&dstline[x], tint(tintColor, pixel), srcalpha, invalpha);
                }
            }
        }
    }
    else
    {
        swr->pxWritten += (uint64_t) dw * (uint64_t) dh;

        // A scaled draw of an untinted, fully opaque sprite only has to move the
        // source pixel across. This used to fall through to alphaBlend() per pixel,
        // which is why the Cyber World parallax layers - roughly 40% of that room's
        // pixels, drawn as scaled sprite parts - cost about 15 ns each.
        if (plainCopy)
        {
            swr->pxFastCopy += (uint64_t) dw * (uint64_t) dh;

            const bool unitXStep = (oxs2 == (fixedp_t) (1 << fp_prec));
            // Exact 2x across: the whole room's grass and cliff bands are sprite
            // parts blown up by two, and stepping a fixed-point counter per output
            // pixel is most of what those rows cost. Writing each source pixel twice
            // walks the source once, in order, with no per-pixel index maths.
            const bool doubleXStep = (oxs2 * 2 == (fixedp_t) (1 << fp_prec));
            fixedp_t ys2 = iys2;
            for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
            {
                uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
                const uintpixel_t* srcline;
                if (dh == sh)
                    srcline = &texture->buffer[(sy + ys) * texture->width + sx];
                else
                    srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];

                if (unitXStep)
                {
                    // Whole pixels across: the row is a run, transparent holes included.
                    const int xoff = (int) (ixs2 >> fp_prec);
                    if (xoff >= 0 && xoff + dw <= (int) texture->width)
                    {
                        swrCopyRunOpaque(dstline, srcline + xoff, dw);
                        continue;
                    }
                }
                else if (doubleXStep)
                {
                    const int xoff = (int) (ixs2 >> fp_prec);
                    const int pairs = dw >> 1;
                    if (xoff >= 0 && xoff + pairs <= (int) texture->width)
                    {
                        int x = 0;
                        for (; x + 8 <= pairs; x += 8)
                        {
                            swrVec8 group;
                            memcpy(&group, srcline + xoff + x, sizeof(group));
                            if (swrVecAllOpaque(group))
                            {
                                // Nothing transparent in the group, so the pixels can be
                                // duplicated without testing each one.
                                for (int i = 0; i < 8; i++)
                                {
                                    const uintpixel_t pixel = srcline[xoff + x + i];
                                    dstline[2 * (x + i)] = pixel;
                                    dstline[2 * (x + i) + 1] = pixel;
                                }
                            }
                            else
                            {
                                for (int i = 0; i < 8; i++)
                                {
                                    const uintpixel_t pixel = srcline[xoff + x + i];
                                    if (opaque(pixel))
                                    {
                                        dstline[2 * (x + i)] = pixel;
                                        dstline[2 * (x + i) + 1] = pixel;
                                    }
                                }
                            }
                        }
                        for (; x < pairs; x++)
                        {
                            const uintpixel_t pixel = srcline[xoff + x];
                            if (opaque(pixel))
                            {
                                dstline[2 * x] = pixel;
                                dstline[2 * x + 1] = pixel;
                            }
                        }
                        if (dw & 1)
                        {
                            const uintpixel_t pixel = srcline[xoff + pairs];
                            if (opaque(pixel)) dstline[dw - 1] = pixel;
                        }
                        continue;
                    }
                }

                fixedp_t xs2 = ixs2;
                for (int x = 0; x < dw; x++, xs2 += oxs2)
                {
                    uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                    if (opaque(pixel)) dstline[x] = pixel;
                }
            }
            return;
        }

        swr->pxScalarBlend += (uint64_t) dw * (uint64_t) dh;

        fixedp_t ys2 = iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline;
            const uintpixel_t* srcline;
            dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];

            fixedp_t xs2 = ixs2;
            for (int x = 0; x < dw; x++, xs2 += oxs2)
            {
                uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                if (opaque(pixel))
                    alphaBlend(&dstline[x], tint(tintColor, pixel), srcalpha, invalpha);
            }
        }
    }
}

static void swrDrawSpriteRotatedInternal(
    Renderer* renderer, int dx, int dy, int dw, int dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uintpixel_t tintColor, int alpha,
    float angleDeg,
    float pivotX,
    float pivotY
)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    float angleRad = -angleDeg * M_PI / 180.0f;
    
    bool flipX = false, flipY = false;
    if (dw < 0) { dw = -dw; dx -= dw; pivotX = dw - pivotX; flipX = true; }
    if (dh < 0) { dh = -dh; dy -= dh; pivotY = dh - pivotY; flipY = true; }
    
    float cosA = cosf(angleRad);
    float sinA = sinf(angleRad);
    
    float cnrx[4], cnry[4];
    cnrx[0] = cnrx[3] = dx;
    cnry[0] = cnry[1] = dy;
    cnrx[1] = cnrx[2] = dx + dw;
    cnry[2] = cnry[3] = dy + dh;
    
    float pxa = pivotX + dx;
    float pya = pivotY + dy;
    
    float minXf = FLT_MAX, minYf = FLT_MAX, maxXf = -FLT_MAX, maxYf = -FLT_MAX;
    for (int i = 0; i < 4; i++)
    {
        float cxi = cnrx[i] - pxa;
        float cyi = cnry[i] - pya;
        float rx = cosA * cxi - sinA * cyi + pxa;
        float ry = sinA * cxi + cosA * cyi + pya;
        if (minXf > rx) minXf = rx;
        if (maxXf < rx) maxXf = rx;
        if (minYf > ry) minYf = ry;
        if (maxYf < ry) maxYf = ry;
    }

    // minX, minY, maxX, maxY now represent an AABB of pixels we should loop over
    int minX = swrFloor(minXf);
    int minY = swrFloor(minYf);
    int maxX = swrCeiling(maxXf);
    int maxY = swrCeiling(maxYf);
    
    // basic out-of-bound checks
    if (maxX < swr->portX) return;
    if (maxY < swr->portY) return;
    if (minX >= swr->maxX) return;
    if (minY >= swr->maxY) return;
    
    // however, we'll need to clip it against out of bounds first
    int minXc = minX, minYc = minY, maxXc = maxX, maxYc = maxY;
    int minx = swr->portX, miny = swr->portY, maxx = swr->portX + swr->portW, maxy = swr->portY + swr->portH;
    
    if (minXc < minx) minXc = minx;
    if (minYc < miny) minYc = miny;
    if (maxXc >= maxx) maxXc = maxx;
    if (maxYc >= maxy) maxYc = maxy;
    
    // some final clip checks
    if (minXc >= maxXc || minYc >= maxYc) return;
    
    swr->pxRotated += (uint64_t) (maxXc - minXc) * (uint64_t) (maxYc - minYc);

    int sox = flipX ? sw - 1 : 0;
    int soy = flipY ? sh - 1 : 0;
    int six = flipX ? -1 : 1;
    int siy = flipY ? -1 : 1;
    
    float sw_dw = (float) sw / dw;
    float sh_dh = (float) sh / dh;
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);

    const bool extState = (swr->pixelMode != SWR_PIXEL_LEGACY);
    const uint16_t extMaskBits = swrWriteMaskBits(swr->writeMask);
    const int extAlpha = alpha < 0 ? 0 : (alpha > 256 ? 256 : alpha);
    const bool extTint = !swrTintIsWhite(tintColor);
    
    for (int cy = minYc; cy < maxYc; cy++)
    {
        uintpixel_t *dstline = &swr->fb[cy * swr->fbPitch];
        for (int cx = minXc; cx < maxXc; cx++)
        {
            // we need to determine the texture-space coordinate of cx/cy
            float ox = (float) cx + 0.5f - pxa;
            float oy = (float) cy + 0.5f - pya;
            
            // "undo" the rotation
            float lx =  cosA * ox + sinA * oy;
            float ly = -sinA * ox + cosA * oy;
            
            // turn it into a texture-local coordinate
            lx += pxa - dx;
            ly += pya - dy;
            
            if (lx < 0 || ly < 0 || lx >= (float) dw || ly >= (float) dh) continue;
            
            lx = lx * sw_dw;
            ly = ly * sh_dh;
            
            int tx = (int)(sox + lx * six);
            int ty = (int)(soy + ly * siy);
            
            if (tx < 0) tx = 0;
            if (ty < 0) ty = 0;
            if (tx >= sw) tx = sw - 1;
            if (ty >= sh) ty = sh - 1;
            
            tx += sx;
            ty += sy;
            
            uintpixel_t src = texture->buffer[ty * texture->width + tx];
            
            if (extState)
            {
                if (opaque(src))
                    swrBlitPixelEx(swr, &dstline[cx], extTint ? tint(tintColor, src) : src,
                                   extAlpha, extMaskBits);
            }
            else if (opaque(src))
                alphaBlend(&dstline[cx], tint(tintColor, src), srcalpha, invalpha);
        }
    }
}

static void swrDrawTriangleInternal(SWRenderer* swr, int xup, int yup, int xleft, int yleft, int xright, int yright, uint32_t color1, uint32_t color2, uint32_t color3, int alpha)
{
    // TODO: update this
    (void) color2;
    (void) color3;
    
    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    
    // Figure out the maximum Y extent of the triangle.
    // (Note that we know yup is the minimum.)
    int xmid, ymid, xmid2 = xup, xmax, ymax;
    if (yleft < yright) {
        xmax = xright, ymax = yright;
        xmid = xleft, ymid = yleft;
        if (yright != yup)
            xmid2 = xup + (xright - xup) * (ymid - yup) / (yright - yup);
    } else {
        xmax = xleft, ymax = yleft;
        xmid = xright, ymid = yright;
        if (yleft != yup)
            xmid2 = xup + (xleft - xup) * (ymid - yup) / (yleft - yup);
    }
    
    for (int y = yup; y < ymax; y++)
    {
        if (y < 0) continue;
        if (y >= swr->height) break;
        
        int x1 = xup, x2 = xup;
        if (y <= ymid)
        {
            // Lines: between up and mid, and between up and max
            if (ymid != yup)
                x1 = xup + (xmid - xup) * (y - yup) / (ymid - yup);
            
            if (ymid != yup)
                x2 = xup + (xmid2 - xup) * (y - yup) / (ymid - yup);
        }
        else
        {
            // Lines: between mid and max, and between up and max
            if (ymax != yup)
                x1 = xup + (xmax - xup) * (y - yup) / (ymax - yup);
            
            if (ymax != ymid)
                x2 = xmid + (xmax - xmid) * (y - ymid) / (ymax - ymid);
        }
        
        if (x1 >= x2) {
            int tmp = x1;
            x1 = x2;
            x2 = tmp;
        }
        
        if (x1 < swr->portX) x1 = swr->portX;
        if (x1 >= swr->maxX) continue;
        if (x2 < swr->portX) continue;
        if (x2 >= swr->maxX) x2 = swr->maxX - 1;
        if (x1 > x2) continue;
        
        uintpixel_t* line = &swr->fb[y * swr->width];

#if PIXEL_SIZE == 16 && defined(__GNUC__)
        // Circles and ellipses become triangle fans, so this is where Chapter 3's
        // spotlight discs are rasterised.
        if (swrCanFillSpanVector(srcalpha))
        {
            swrFillSpanVector(line + x1, x2 - x1, color1, srcalpha, invalpha, swrFillSpanMode(swr));
            continue;
        }
#endif

        for (int x = x1; x < x2; x++) {
            alphaBlend(&line[x], color1, srcalpha, invalpha);
        }
    }
}

static bool swrResolveFontState(SWRenderer* swr, DataWin* dw, Font* font, SwrFontState* state)
{
    state->font = font;
    state->fontTpag = NULL;
    state->fontTpagIndex = 0;
    state->spriteFontSprite = NULL;
    
    if (font->isSpriteFont)
    {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    else
    {
        state->fontTpagIndex = font->tpagIndex;
        if (state->fontTpagIndex < 0) return false;
        
        state->fontTpag = &dw->tpag.items[state->fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (0 > pageId || (uint32_t) pageId >= swr->totalTextureCount) return false;
        if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pageId)) return false;
        
        state->fontPageId = pageId;
    }
    
    return true;
}

static bool swrResolveGlyph(
    SWRenderer* swr, DataWin* dw, SwrFontState* state, FontGlyph* glyph, float cursorX, float cursorY,
    int* tpagIndex, int* pageId, int* sx, int* sy, int* sw, int* sh, float* dx, float* dy
)
{
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != NULL)
    {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex ||  glyphIndex >= (int32_t) sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (0 > tpagIdx) return false;

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (0 > pid || (uint32_t) pid >= swr->totalTextureCount) return false;
        if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pid)) return false;

        *tpagIndex = tpagIdx;
        *pageId = glyphTpag->texturePageId;
        
        *sx = glyphTpag->sourceX;
        *sy = glyphTpag->sourceY;
        *sw = glyphTpag->sourceWidth;
        *sh = glyphTpag->sourceHeight;
        
        *dx = cursorX + glyph->offset;
        *dy = cursorY + glyphTpag->targetY - sprite->originY;
    }
    else
    {
        *tpagIndex = state->fontTpagIndex;
        *pageId = state->fontPageId;
        
        *sx = state->fontTpag->sourceX + glyph->sourceX;
        *sy = state->fontTpag->sourceY + glyph->sourceY;
        *sw = glyph->sourceWidth;
        *sh = glyph->sourceHeight;
        
        *dx = cursorX + glyph->offset;
        *dy = cursorY;
    }
    
    return true;
}

// ==== Exposed interface ====

bool swrSwitchToSurface(Renderer* renderer, int32_t targetSurfaceId, bool restoreOldView)
{
    SWRenderer* swr = (SWRenderer*) renderer;

    if (swrDiagLogEnabled())
        logInfo("Surf switch   swr=%p target=%d from=%d drawing=%d\n", (void*) swr,
                (int) targetSurfaceId, (int) swr->currentSurfaceIndex,
                swrIsOnSurface(swr) ? 1 : 0);
    
    if (swrIsOnSurface(swr)) {
        // The game can free the surface it is drawing into (Deltarune's menu does),
        // and a torn-down pass can leave `drawingToSurface` set with no valid
        // currentSurfaceIndex. Reading swr->surfaces[-1] there is what segfaulted on
        // the GUI pass right after a chapter hand-off, so check before indexing.
        int32_t current = swr->currentSurfaceIndex;
        if (current >= 0 && (size_t) current < swr->surfaceCount)
            swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[current]);
        else {
            if (swrDiagLogEnabled())
                logWarn("Surf target   drawing into an invalid surface (%d), dropping it\n",
                        (int) current);
            swr->drawingToSurface = false;
        }
    }
    
    if (targetSurfaceId == RENDER_TARGET_HOST_FRAMEBUFFER)
    {
        if (!swrIsOnSurface(swr))
            return true;
        
        if (swrDiagLogEnabled())
            logInfo("Surf target   screen (from %d)\n", (int) swr->currentSurfaceIndex);
        
        swr->fb = swr->mainFb;
        swr->width = swr->mainWidth;
        swr->height = swr->mainHeight;
        swr->fbPitch = swr->mainPitch;
        swr->blendMode = bm_normal;
        swr->blendSrcFactor = 0;
        swr->blendDstFactor = 0;
        swr->blendEnabled = true;
        swr->alphaTestEnabled = false;
        swr->currentSurfaceIndex = -1;
        swr->writeMask = WRITE_MASK_ALL;
        swrSetupBlendState(swr);
        
        if (restoreOldView) {
            // restore the old transform, if needed
            swr->viewX = swr->lastViewX;
            swr->viewY = swr->lastViewY;
            swr->viewW = swr->lastViewW;
            swr->viewH = swr->lastViewH;
            swr->portX = swr->lastPortX;
            swr->portY = swr->lastPortY;
            swr->portW = swr->lastPortW;
            swr->portH = swr->lastPortH;
            swr->gameW = swr->lastGameW;
            swr->gameH = swr->lastGameH;
            swr->maxX = swr->lastMaxX;
            swr->maxY = swr->lastMaxY;
            swr->scaleX = swr->lastScaleX;
            swr->scaleY = swr->lastScaleY;
        }
        return true;
    }
    
    if (targetSurfaceId < 0 || (size_t) targetSurfaceId >= swr->surfaceCount || swr->surfaces[targetSurfaceId] == NULL) {
        fprintf(stderr, "swr: Invalid surface id %d\n", targetSurfaceId);
        return false;
    }

    if (swrDiagLogEnabled()) {
        SWTexture* t = swr->surfaces[targetSurfaceId]->texture;
        logInfo("Surf target   id=%d %dx%d view=%.0f,%.0f %.0fx%.0f port=%d,%d %dx%d scale=%.2f,%.2f\n",
                (int) targetSurfaceId, (int) t->width, (int) t->height,
                swr->viewX, swr->viewY, swr->viewW, swr->viewH,
                swr->portX, swr->portY, swr->portW, swr->portH, swr->scaleX, swr->scaleY);
    }
    
    if (!swrIsOnSurface(swr))
    {
        // back up the original framebuffer
        swr->drawingToSurface = true;
        swr->mainFb = swr->fb;
        swr->mainWidth = swr->width;
        swr->mainHeight = swr->height;
        swr->mainPitch = swr->fbPitch;
        swr->blendMode = bm_normal;
        
        // and the old transform
        swr->lastViewX = swr->viewX;
        swr->lastViewY = swr->viewY;
        swr->lastViewW = swr->viewW;
        swr->lastViewH = swr->viewH;
        swr->lastPortX = swr->portX;
        swr->lastPortY = swr->portY;
        swr->lastPortW = swr->portW;
        swr->lastPortH = swr->portH;
        swr->lastGameW = swr->gameW;
        swr->lastGameH = swr->gameH;
        swr->lastMaxX = swr->maxX;
        swr->lastMaxY = swr->maxY;
        swr->lastScaleX = swr->scaleX;
        swr->lastScaleY = swr->scaleY;
    }
    
    SWTexture* surface = swr->surfaces[targetSurfaceId]->texture;
    swr->fb = surface->buffer;
    swr->width = surface->width;
    swr->height = surface->height;
    swr->fbPitch = surface->width;
    swr->drawingToSurface = true;
    swr->blendMode = bm_normal;
    swr->blendSrcFactor = 0;
    swr->blendDstFactor = 0;
    swr->blendEnabled = true;
    swr->alphaTestEnabled = false;
    swr->currentSurfaceIndex = targetSurfaceId;
    swr->writeMask = WRITE_MASK_ALL;
    swrSetupBlendState(swr);
    
    swr->viewX = swr->portX = 0;
    swr->viewY = swr->portY = 0;
    swr->maxX = swr->viewW = swr->portW = surface->width;
    swr->maxY = swr->viewH = swr->portH = surface->height;
    swr->scaleX = swr->scaleY = 1.0f;
    
    return true;
}

void swrDrawHLine(Renderer* renderer, float dx, float dy, float dw, uintpixel_t color, uintpixel_t color2, float alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    float thickness = 1;

    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &dw, &thickness);

    // TODO: use thickness
    swrDrawHLineInt(renderer, swrFloor(dx), swrFloor(dy), swrCeiling(dw), color, color2, swrIntAlpha(alpha));
}

void swrDrawVLine(Renderer* renderer, float dx, float dy, float dh, uintpixel_t color, uintpixel_t color2, float alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    float thickness = 1;

    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &thickness, &dh);
    
    // TODO: use thickness
    swrDrawVLineInt(renderer, swrFloor(dx), swrFloor(dy), swrCeiling(dh), color, color2, swrIntAlpha(alpha));
}

void swrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uintpixel_t color, uintpixel_t color2, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    swrTransformSizeIfNeeded(swr, &width, NULL);
    swrDrawLineInt(renderer, swrFloor(x1), swrFloor(y1), swrCeiling(x2), swrCeiling(y2), swrCeiling(width), color, color2, swrIntAlpha(alpha));
}

void swrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color, float alpha)
{
    swrDrawHLine(renderer, x1, y1, (x2 - x1) + 1, color, color, alpha);
    swrDrawHLine(renderer, x1, y2, (x2 - x1) + 1, color, color, alpha);
    swrDrawVLine(renderer, x1, y1, (y2 - y1) + 1, color, color, alpha);
    swrDrawVLine(renderer, x2, y1, (y2 - y1) + 1, color, color, alpha);
}

void swrDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uintpixel_t color4, float alpha)
{
    swrDrawHLine(renderer, x1, y1, (x2 - x1) + 1, color1, color2, alpha);
    swrDrawHLine(renderer, x1, y2, (x2 - x1) + 1, color3, color4, alpha);
    swrDrawVLine(renderer, x1, y1, (y2 - y1) + 1, color1, color3, alpha);
    swrDrawVLine(renderer, x2, y1, (y2 - y1) + 1, color2, color4, alpha);
}

void swrFillRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t pxcolor, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);

    int alphaInt = swrIntAlpha(alpha);
    int x1i = swrFloor(x1), x2i = swrCeiling(x2), y1i = swrFloor(y1), y2i = swrCeiling(y2);
    int xd = x2i - x1i;
    int yd = y2i - y1i;
    if (xd < 0) { x1i = x2i; xd = -xd; }
    if (yd < 0) { y1i = y2i; yd = -yd; }
    if (xd <= 0 || yd <= 0) return;
    
    swrTraceDraw(swr, "fillrect", nullptr, 0, 0, 0, 0, x1i, y1i, xd, yd, alphaInt, pxcolor);

    // shd_ripple: the game draws each water ripple as a filled rectangle and lets the
    // shader turn it into a ring, taking the alpha from the distance to the ripple
    // centre instead of from the draw call. Drawing the rectangle as-is leaves a solid
    // block over the scene, so the ring has to be reproduced here.
    SWRippleCtx ripple;
    if (UNLIKELY(swrShaderEmuRippleBegin(swr, &ripple)))
    {
        const uint16_t maskBits = swrWriteMaskBits(swr->writeMask);
        const uintpixel_t srcPixel = (uintpixel_t) ((pxcolor & 0x7FFF) | 0x8000);
        const int lastX = swr->maxX - 1;
        const int minX = x1i;
        const int maxRectX = x1i + xd;
        for (int y = 0; y <= yd; y++) {
            const int dstY = y1i + y;
            if (dstY < swr->portY || dstY > swr->maxY - 1) continue;

            int outerX0, outerX1, holeX0, holeX1;
            if (!swrShaderEmuRippleRow(&ripple, dstY, &outerX0, &outerX1, &holeX0, &holeX1)) continue;
            if (outerX0 < minX) outerX0 = minX;
            if (outerX1 > maxRectX) outerX1 = maxRectX;
            if (outerX0 < swr->portX) outerX0 = swr->portX;
            if (outerX1 > lastX) outerX1 = lastX;

            uintpixel_t* row = &swr->fb[(size_t) dstY * swr->fbPitch];
            for (int x = outerX0; x <= outerX1; x++) {
                if (x >= holeX0 && x <= holeX1) { x = holeX1; continue; }
                const int ringAlpha = swrShaderEmuRippleAlpha(&ripple, x, dstY);
                if (ringAlpha <= 0) continue;
                swrBlitPixelEx(swr, &row[x], srcPixel, ringAlpha, maskBits);
            }
        }
        return;
    }

    for (int y = 0; y <= yd; y++) {
        swrDrawHLineInt(renderer, x1i, y1i + y, xd, pxcolor, pxcolor, alphaInt);
    }
}

void swrFillRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t pxcolor1, uintpixel_t pxcolor2, uintpixel_t pxcolor3, uintpixel_t pxcolor4, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);

    int alphaInt = swrIntAlpha(alpha);
    int x1i = swrFloor(x1), x2i = swrCeiling(x2), y1i = swrFloor(y1), y2i = swrCeiling(y2);
    int xd = x2i - x1i;
    int yd = y2i - y1i;
    if (xd < 0) { x1i = x2i; xd = -xd; }
    if (yd < 0) { y1i = y2i; yd = -yd; }
    if (xd <= 0 || yd <= 0) return;
    
    // TODO: blending vertically
    (void) pxcolor3;
    (void) pxcolor4;
    
    for (int y = 0; y <= yd; y++) {
        swrDrawHLineInt(renderer, x1i, y1i + y, xd, pxcolor1, pxcolor2, alphaInt);
    }
}

// Diagnostic switch: with BUTTERSCOTCH_DIAG_NODRAW=1 every sprite blit returns
// immediately, so a frame's cost can be split into "the game's own code" and "the
// pixels". Nothing else changes, so the draw list and the VM work stay identical.
static bool swrDiagNoDraw(void)
{
    static int cached = -1;
    if (cached < 0)
    {
        const char* env = getenv("BUTTERSCOTCH_DIAG_NODRAW");
        cached = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return cached != 0;
}

void swrDrawSprite(
    Renderer* renderer, float dx, float dy, float dw, float dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uint32_t tintColor, float alpha
)
{
    SWRenderer *swr = (SWRenderer*) renderer;

    if (UNLIKELY(swrDiagNoDraw())) return;

    // A fully faded-out sprite writes nothing under the normal blend, but the blit
    // below would still walk its whole rectangle to find that out. Deltarune fades
    // whole full-screen parallax layers out this way, so the walk is not free.
    if (UNLIKELY(alpha <= 0.0f) && swr->blendMode == 0 && swr->pixelMode == SWR_PIXEL_LEGACY) return;

    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &dw, &dh);

    swrTraceDraw(swr, "sprite", texture, sx, sy, sw, sh,
                 swrFloor(dx), swrFloor(dy), swrCeiling(dw), swrCeiling(dh),
                 swrIntAlpha(alpha), tintColor);

    swrDrawSpriteInternal(
        renderer,
        swrFloor(dx),
        swrFloor(dy),
        swrCeiling(dw),
        swrCeiling(dh),
        texture,
        sx, sy,
        sw, sh,
        swrConvertPixel(tintColor),
        swrIntAlpha(alpha)
    );
}

void swrDrawSpriteRotated(
    Renderer* renderer, float dx, float dy, float dw, float dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uint32_t tintColor, float alpha,
    float angleDeg,
    float pivotX,
    float pivotY
)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &pivotX, &pivotY);
    swrTransformSizeIfNeeded(swr, &dw, &dh);
    
    swrTraceDraw(swr, "rotate", texture, sx, sy, sw, sh,
                 swrFloor(dx), swrFloor(dy), swrCeiling(dw), swrCeiling(dh),
                 swrIntAlpha(alpha), tintColor);

    swrDrawSpriteRotatedInternal(
        renderer,
        swrFloor(dx),
        swrFloor(dy),
        swrCeiling(dw),
        swrCeiling(dh),
        texture,
        sx, sy,
        sw, sh,
        swrConvertPixel(tintColor),
        swrIntAlpha(alpha),
        angleDeg,
        pivotX,
        pivotY
    );
}

void swrDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha)
{
    float xup, yup, xleft, yleft, xright, yright;
    uint32_t colorup, colorleft, colorright;
    
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    swrTransformPosIfNeeded(swr, &x3, &y3);
    
    //which vertex is higher?
    xup = x1, yup = y1; colorup = color1;
    xleft = x2, yleft = y2; colorleft = color2;
    xright = x3, yright = y3; colorright = color3;
    if (yup > y2) {
        xup = x2, yup = y2, colorup = color2;
        xleft = x1, yleft = y1, colorleft = color1;
        //xright = x3, yright = y3;
    }
    if (yup > y3) {
        xup = x3, yup = y3, colorup = color3;
        xleft = x1, yleft = y1, colorleft = color1;
        xright = x2, yright = y2, colorright = color2;
    }
    
    if (xleft > xright) {
        float tmp = xleft;
        xleft = xright;
        xright = tmp;
        tmp = yleft;
        yleft = yright;
        yright = tmp;
        uint32_t tmp2 = colorleft;
        colorleft = colorright;
        colorright = tmp2;
    }
    
    swrDrawTriangleInternal(
        swr,
        swrFloor(xup), swrFloor(yup),
        swrFloor(xleft), swrCeiling(yleft),
        swrFloor(xright), swrCeiling(yright),
        swrConvertPixel(colorup),
        swrConvertPixel(colorleft),
        swrConvertPixel(colorright),
        swrIntAlpha(alpha)
    );
}

// ===[ Textured triangles ]===
//
// draw_sprite_pos() maps a sprite onto four arbitrary corners (skewed, rotated and
// scaled at once), which is how Deltarune Chapter 3 draws the actors' bodies: each
// part is placed independently so the characters can lean and squash. Upstream
// engines send this to the GPU as two triangles; here it is rasterised with the same
// nearest-neighbour sampling the rest of the software renderer uses.
//
// Quality note: the interpolation is affine per triangle, not perspective correct.
// For the flat, front-facing quads these games draw the difference is invisible.
static void swrRasterTexturedTriangle(SWRenderer* swr, const SWTexture* texture,
                                      float x0, float y0, float u0, float v0,
                                      float x1, float y1, float u1, float v1,
                                      float x2, float y2, float u2, float v2,
                                      int srcalpha, int invalpha)
{
    // Screen-space bounding box, clipped to the render port.
    float minXf = swrMin(x0, swrMin(x1, x2));
    float maxXf = swrMax(x0, swrMax(x1, x2));
    float minYf = swrMin(y0, swrMin(y1, y2));
    float maxYf = swrMax(y0, swrMax(y1, y2));

    int minX = swrMax(swr->portX, (int) floorf(minXf));
    int maxX = swrMin(swr->maxX - 1, (int) ceilf(maxXf));
    int minY = swrMax(swr->portY, (int) floorf(minYf));
    int maxY = swrMin(swr->maxY - 1, (int) ceilf(maxYf));
    if (minX > maxX || minY > maxY) return;

    // Edge function for a clockwise triangle; flip the winding if it came in the
    // other way round so both cases rasterise.
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0.0f) return;
    if (area < 0.0f) {
        float tx = x1, ty = y1, tu = u1, tv = v1;
        x1 = x2; y1 = y2; u1 = u2; v1 = v2;
        x2 = tx; y2 = ty; u2 = tu; v2 = tv;
        area = -area;
    }
    const float invArea = 1.0f / area;

    const int texW = (int) texture->width;
    const int texH = (int) texture->height;

    for (int py = minY; py <= maxY; py++)
    {
        const float fy = (float) py + 0.5f;
        uintpixel_t* line = &swr->fb[(size_t) py * swr->fbPitch];

        for (int px = minX; px <= maxX; px++)
        {
            const float fx = (float) px + 0.5f;

            float w0 = (x2 - x1) * (fy - y1) - (y2 - y1) * (fx - x1);
            float w1 = (x0 - x2) * (fy - y2) - (y0 - y2) * (fx - x2);
            float w2 = (x1 - x0) * (fy - y0) - (y1 - y0) * (fx - x0);
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            w0 *= invArea; w1 *= invArea; w2 *= invArea;
            float u = u0 * w0 + u1 * w1 + u2 * w2;
            float v = v0 * w0 + v1 * w1 + v2 * w2;

            int tx = (int) u;
            int ty = (int) v;
            if (tx < 0) tx = 0; else if (tx >= texW) tx = texW - 1;
            if (ty < 0) ty = 0; else if (ty >= texH) ty = texH - 1;

            uintpixel_t texel = texture->buffer[(size_t) ty * texture->width + tx];
            if (!opaque(texel)) continue;
            alphaBlend(&line[px], texel, srcalpha, invalpha);
        }
    }
}

void swrDrawTexturedTriangle(Renderer* renderer, const SWTexture* texture,
                             float x0, float y0, float u0, float v0,
                             float x1, float y1, float u1, float v1,
                             float x2, float y2, float u2, float v2,
                             int alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (texture == nullptr || texture->buffer == nullptr) return;

    int srcalpha = swrCalcSrcAlpha(swr, alpha);
    int invalpha = swrCalcDstAlpha(swr, alpha);
    if (srcalpha == 0) return;

    swrTraceDraw(swr, "texquad", texture, (int) u0, (int) v0, 0, 0,
                 (int) x0, (int) y0, (int) x1, (int) y1, alpha, 0xFFFFFF);

    swrRasterTexturedTriangle(swr, texture, x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2,
                              srcalpha, invalpha);
}

// ====[ Glyph colour gradients ]====
//
// draw_text_*_color takes four corner colours and GameMaker interpolates them across
// the drawn string, so a glyph is not one flat colour: Deltarune's dialogue draws the
// coloured word of a line as white at the top of the letters fading to green at the
// bottom. Pick a single representative colour per glyph and the whole word turns a
// pale wash of that average, which is left looking lighter than the solid-colour
// shadow underneath it -- the shadow has to be a real gradient too, not a mixture.
//
// The blending is per pixel over the glyph box: bilinear between the four corners,
// which matches what the GPU renderer and the original engine do.
FORCE_INLINE uintpixel_t swrLerpRgb555(uintpixel_t a, uintpixel_t b, int t)
{
    const int it = 255 - t;
    const int blue  = (((a      ) & 0x1F) * it + ((b      ) & 0x1F) * t) / 255;
    const int green = (((a >>  5) & 0x1F) * it + ((b >>  5) & 0x1F) * t) / 255;
    const int red   = (((a >> 10) & 0x1F) * it + ((b >> 10) & 0x1F) * t) / 255;
    return (uintpixel_t) (blue | (green << 5) | (red << 10));
}

// True when a glyph can use the gradient path: no scaling (the source rect and the
// destination rect are the same size), no rotation, and none of the extended pixel
// modes, which have their own scalar loops in swrDrawSpriteInternal.
static bool swrGlyphCanGradient(const SWRenderer* swr, float dw, float dh, int sw, int sh, bool mustRotate)
{
    if (mustRotate || swr->pixelMode != SWR_PIXEL_LEGACY || swr->writeMask != 0xF) return false;
    if (sw == 0 || sh == 0) return false;
    return swrCeiling(dw) == sw && swrCeiling(dh) == sh;
}

static void swrBlendGlyphGradient(SWRenderer* swr, int dx, int dy,
                                  const SWTexture* texture, int sx, int sy, int sw, int sh,
                                  int32_t c1, int32_t c2, int32_t c3, int32_t c4, int alpha)
{
    // GameMaker orders the corners top-left, top-right, bottom-right, bottom-left.
    const uintpixel_t t1 = swrConvertPixel((uint32_t) c1);
    const uintpixel_t t2 = swrConvertPixel((uint32_t) c2);
    const uintpixel_t t3 = swrConvertPixel((uint32_t) c3);
    const uintpixel_t t4 = swrConvertPixel((uint32_t) c4);

    const int minx = swr->portX, miny = swr->portY;
    const int maxx = swr->portX + swr->portW, maxy = swr->portY + swr->portH;
    if (dx >= maxx || dy >= maxy) return;
    if (dx + sw <= minx || dy + sh <= miny) return;

    int startX = 0, endX = sw, startY = 0, endY = sh;
    if (dx < minx) startX = minx - dx;
    if (dy < miny) startY = miny - dy;
    if (dx + sw > maxx) endX = maxx - dx;
    if (dy + sh > maxy) endY = maxy - dy;
    if (startX >= endX || startY >= endY) return;

    const int srcalpha = swrCalcSrcAlpha(swr, alpha);
    const int dstalpha = swrCalcDstAlpha(swr, alpha);
    if (srcalpha == 0) return;

    swr->pxWritten += (uint64_t) (endX - startX) * (uint64_t) (endY - startY);
    swr->pxScalarBlend += (uint64_t) (endX - startX) * (uint64_t) (endY - startY);

    for (int y = startY; y < endY; y++)
    {
        const uintpixel_t* srcline = &texture->buffer[(size_t) (sy + y) * texture->width + sx];
        uintpixel_t* dstline = &swr->fb[(size_t) (dy + y) * swr->fbPitch + dx];
        const int v = (sh > 1) ? ((y * 255) / (sh - 1)) : 0;
        for (int x = startX; x < endX; x++)
        {
            const uintpixel_t pixel = srcline[x];
            if (!opaque(pixel)) continue;
            const int u = (sw > 1) ? ((x * 255) / (sw - 1)) : 0;
            const uintpixel_t top = swrLerpRgb555(t1, t2, u);
            const uintpixel_t bottom = swrLerpRgb555(t4, t3, u);
            const uintpixel_t colour = swrLerpRgb555(top, bottom, v);
            alphaBlend(&dstline[x], tint(colour, pixel), srcalpha, dstalpha);
        }
    }
}

void swrDrawTextGradient(SWRenderer* swr, const char* text, float x, float y,
                         float xscale, float yscale, float angleDeg,
                         int32_t c1, int32_t c2, int32_t c3, int32_t c4,
                         float alpha, float lineSeparation)
{
    Renderer* renderer = &swr->base;
    DataWin* dwin = renderer->dataWin;
    
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dwin->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dwin->font.fonts[fontIndex];
    
    SwrFontState fontState;
    memset(&fontState, 0, sizeof fontState); // silence warning treated as error
    
    if (!swrResolveFontState(swr, dwin, font, &fontState)) return;
    
    // TODO: do we need to mirror the way the text scrolls too?!
    float cosA = 1.0f, sinA = 0.0f, angleRad = 0.0f;
    bool mustRotate = swrMustRotateSensitive(angleDeg);
    if (UNLIKELY(mustRotate))
    {
        angleRad = -angleDeg * M_PI / 180.0f;
        cosA = cosf(angleRad);
        sinA = sinf(angleRad);
    }
    
    int textLen = (int) strlen(text);
    int lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    // Vertical alignment offset
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;
    
    xscale *= font->scaleX;
    yscale *= font->scaleY;

    // Iterate through lines. HTML5 subtracts ascenderOffset from the per-line y offset
    // (see yyFont.GR_Text_Draw), shifting glyphs up so the baseline aligns with the drawn y.
    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        // Where this glyph starts along the line, as a fraction: GameMaker's
        // draw_text_*_color takes four corner colours for the whole string, so the
        // colour of a glyph depends on where it sits in it.
        float gradientX = 0.0f;
        const bool gradient = c1 != c2 || c2 != c3 || c3 != c4;

        // Render each glyph in the line - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            // The gradient runs across the whole string, so a glyph keeps only the
            // slice of it that its own column covers. Averaging those slices into one
            // colour is the fallback for glyphs that cannot be drawn per pixel.
            int32_t glyphColor = c1;
            int32_t g1 = c1, g2 = c2, g3 = c3, g4 = c4;
            float advance = (glyph != nullptr) ? (float) glyph->shift : 0.0f;
            if (gradient)
            {
                const float leftFrac = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                const float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                g1 = Color_lerp(c1, c2, leftFrac);
                g2 = Color_lerp(c1, c2, rightFrac);
                g3 = Color_lerp(c4, c3, rightFrac);
                g4 = Color_lerp(c4, c3, leftFrac);
                glyphColor = Color_lerp(Color_lerp(g1, g2, 0.5f),
                                        Color_lerp(g4, g3, 0.5f), 0.5f);
            }

            if (glyph != nullptr) {
                bool drewSuccessfully = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    int fontTpagIndex = 0, pageId = 0;
                    int sx, sy, sw, sh, dw, dh;
                    float dx, dy;
                    if (swrResolveGlyph(swr, dwin, &fontState, glyph, cursorX, cursorY,
                            &fontTpagIndex, &pageId, &sx, &sy, &sw, &sh, &dx, &dy))
                    {
                        dx *= xscale; dx += x;
                        dy *= yscale; dy += y;
                        dw = swrCeiling(xscale * glyph->sourceWidth);
                        dh = swrCeiling(yscale * glyph->sourceHeight);
                        
                        // TODO: at 640x480, for some reason, without this fixup the
                        // letters in the "Name the fallen human." screen don't shake
                        dx = roundf(dx * 2) / 2;
                        dy = roundf(dy * 2) / 2;
                        
                        SWTexture* texture = swr->textures[pageId];
                        
                        if (UNLIKELY(mustRotate))
                        {
                            dx -= x;
                            dy -= y;
                            float ndx = cosA * dx - sinA * dy;
                            float ndy = sinA * dx + cosA * dy;
                            ndx += x;
                            ndy += y;
                            swrDrawSpriteRotated(renderer, ndx, ndy, dw, dh, texture, sx, sy, sw, sh, glyphColor, alpha, angleDeg, 0.0f, 0.0f);
                        }
                        else
                        {
                            // Same view transform swrDrawSprite applies: HUD text is drawn
                            // in game coordinates, and skipping this put the glyph outside
                            // the window while its shadow stayed on screen.
                            float gdxF = dx, gdyF = dy, gdwF = dw, gdhF = dh;
                            swrTransformPosIfNeeded(swr, &gdxF, &gdyF);
                            swrTransformSizeIfNeeded(swr, &gdwF, &gdhF);

                            if (gradient && swrGlyphCanGradient(swr, gdwF, gdhF, sw, sh, mustRotate))
                            {
                                const int gdx = swrFloor(gdxF), gdy = swrFloor(gdyF);
                                swrTraceDraw(swr, "glyph", texture, sx, sy, sw, sh,
                                             gdx, gdy, sw, sh, swrIntAlpha(alpha), (uint32_t) g1);
                                swrBlendGlyphGradient(swr, gdx, gdy, texture, sx, sy, sw, sh,
                                                      g1, g2, g3, g4, swrIntAlpha(alpha));
                            }
                            else
                            {
                                swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh,
                                              glyphColor, alpha);
                            }
                        }
                        
                        drewSuccessfully = true;
                    }
                }

                cursorX += glyph->shift;
                gradientX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    float kerning = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX += kerning;
                    gradientX += kerning;
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        // Skip past the newline, treating \r\n and \n\r as single breaks
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

void swrDrawText(SWRenderer* swr, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t color, float alpha, float lineSeparation)
{
    swrDrawTextGradient(swr, text, x, y, xscale, yscale, angleDeg, color, color, color, color, alpha, lineSeparation);
}
