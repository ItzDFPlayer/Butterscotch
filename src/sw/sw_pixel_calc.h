#ifndef _SW_PIXEL_CALC_H
#define _SW_PIXEL_CALC_H

#include "defines.h"
#include "pixel_convert.h"

// Check if a pixel is opaque.
//
// Later, this should be changed to perform full alpha-blending
// (at least in 32-bit pixel mode)
FORCE_INLINE bool opaque(uintpixel_t color)
{
#if PIXEL_SIZE == 8
    return (color != PXL_TRANSPARENT);
#else
    return (color & TRANSPARENT_MASK) != 0;
#endif
}

// Multiplies a color value (`color`) by another color value (`tintColor`).
FORCE_INLINE uintpixel_t tint(uintpixel_t tintColor, uintpixel_t color)
{
#if PIXEL_SIZE == 32
    Pixel32ARGB x, y;
    
    if ((tintColor & 0xFFFFFF) == 0xFFFFFF)
        return color;
    
    x.l = color;
    y.l = tintColor;
    
    x.p.b = (int)x.p.b * y.p.b / 255;
    x.p.g = (int)x.p.g * y.p.g / 255;
    x.p.r = (int)x.p.r * y.p.r / 255;
    return x.l;
#elif PIXEL_SIZE == 16
    if ((tintColor & 0x7FFF) == 0x7FFF)
        return color;
    
    int tcb = tintColor & 0x1F;
    int tcg = (tintColor >> 5) & 0x1F;
    int tcr = (tintColor >> 10) & 0x1F;
    
    int cb = color & 0x1F;
    int cg = (color >> 5) & 0x1F;
    int cr = (color >> 10) & 0x1F;
    int ca = color & 0x8000;
    
    cb = (cb * tcb) / 32;
    cg = (cg * tcg) / 32;
    cr = (cr * tcr) / 32;
    return ca | cb | (cg << 5) | (cr << 10);
#elif PIXEL_SIZE == 8
    // fast but hacky
    if (tintColor == 0xFF || tintColor == PXL_TRANSPARENT)
        return color;
    
    return color & tintColor;
#endif
}

// True when `tintColor` is the identity tint, i.e. tint() returns the pixel
// unchanged. The blit fast paths use this to skip the per-pixel tint test.
FORCE_INLINE bool swrTintIsWhite(uintpixel_t tintColor)
{
#if PIXEL_SIZE == 32
    return (tintColor & 0xFFFFFF) == 0xFFFFFF;
#elif PIXEL_SIZE == 16
    return (tintColor & 0x7FFF) == 0x7FFF;
#else
    return tintColor == 0xFF || tintColor == PXL_TRANSPARENT;
#endif
}

// Performs alpha blending on a pixel, with another pixel.
//
// NOTE: alpha is between 0 and 256, NOT between 0 and 255!
//
// TODO: This routine could use some optimization.  Obviously I tried my best, but clearly
// it's still true that too many calculations are being performed.
//
// NOTE: Obviously I could use SIMD here, but old computers didn't have SIMD, and the code
// runs fast enough on modern computers to not need to do SIMD.
FORCE_INLINE void alphaBlend(uintpixel_t* dcolor, uintpixel_t scolor, int srcalpha, int dstalpha)
{
#if PIXEL_SIZE == 32 || PIXEL_SIZE == 16
    // it's so significant here we might as well fill in the whole color
    if (LIKELY(dstalpha < 3 && srcalpha > 253)) {
        *dcolor = scolor;
        return;
    }
    
    // it's so insignificant here nobody will notice if we just don't...
    if (UNLIKELY(srcalpha == 0))
        return;
#endif

#if PIXEL_SIZE == 32
    Pixel32ARGB dc, sc;
    dc.l = *dcolor;
    sc.l = scolor;
    
    int dcr = (dc.p.r * dstalpha + sc.p.r * srcalpha) >> 8;
    int dcg = (dc.p.g * dstalpha + sc.p.g * srcalpha) >> 8;
    int dcb = (dc.p.b * dstalpha + sc.p.b * srcalpha) >> 8;
    
    //clamp to 0
    dcr &= ((-dcr) >> 31);
    dcg &= ((-dcg) >> 31);
    dcb &= ((-dcb) >> 31);
    //clamp to 255
    dcr |= ((signed char)(dcr >> 1) >> 7);
    dcg |= ((signed char)(dcg >> 1) >> 7);
    dcb |= ((signed char)(dcb >> 1) >> 7);
    
    dc.p.r = dcr;
    dc.p.g = dcg;
    dc.p.b = dcb;
    dc.p.a = 0xFF;
    
    *dcolor = dc.l;
#elif PIXEL_SIZE == 16
    int scb = scolor & 0x1F;
    int scg = (scolor >> 5) & 0x1F;
    int scr = (scolor >> 10) & 0x1F;

    uintpixel_t _dcolor = *dcolor;
    int dcb = _dcolor & 0x1F;
    int dcg = (_dcolor >> 5) & 0x1F;
    int dcr = (_dcolor >> 10) & 0x1F;
    
    dcr = (dcr * dstalpha + scr * srcalpha) >> 8;
    dcg = (dcg * dstalpha + scg * srcalpha) >> 8;
    dcb = (dcb * dstalpha + scb * srcalpha) >> 8;
    
    //clamp to 0
    dcr &= ((-dcr) >> 31);
    dcg &= ((-dcg) >> 31);
    dcb &= ((-dcb) >> 31);
    //clamp to 255
    dcr |= ((signed char)(dcr >> 1) >> 7);
    dcg |= ((signed char)(dcg >> 1) >> 7);
    dcb |= ((signed char)(dcb >> 1) >> 7);

    // The result alpha. bm_subtract is the only mode that arrives here with a
    // negative source weight (swrCalcSrcAlpha negates the alpha for it), and it is
    // how the spotlight/iris effects cut their hole: a partially transparent black
    // veil is painted first, then the same draw alpha is subtracted back out of the
    // destination alpha, leaving it at zero inside the circle. Five bits of colour
    // and one bit of alpha cannot represent "0.4 - 0.4", so any subtract is treated
    // as removing the destination alpha outright. Without this the hole never opens
    // and the overlay stays solid black.
    uintpixel_t alphaBits = 0x8000;
    if (UNLIKELY(srcalpha < 0)) alphaBits = 0;

    *dcolor = alphaBits | dcb | (dcg << 5) | (dcr << 10);
#else
    if (srcalpha < 240) {
        static int alphaApproximationThingy = 0;
        alphaApproximationThingy += 1339;
        if (alphaApproximationThingy > 601000)
            alphaApproximationThingy = 0;
        
        //gotta love that RNG
        if ((alphaApproximationThingy & 0xFF) >= srcalpha)
            return;
    }
    
    *dcolor = scolor;
#endif
}

// ==== Extended blend state ====
//
// alphaBlend() above covers the handful of blend modes the older engines used.
// Deltarune Chapter 3+ instead builds spotlight and stencil masks out of the full
// blend unit: it clears the destination *alpha* channel, draws a shape into it, and
// then blends the scene with (bm_inv_dest_alpha, bm_dest_alpha) so only the pixels
// outside the shape are painted. Without that the whole screen just stays covered by
// the mask, which is what makes the Chapter 3 intro look like a black screen.
//
// The state lives on the renderer and is reduced to a `pixelMode` by
// swrSetupBlendState(), so the draw loops only pay for one predictable branch:
//
//   0  legacy          - alphaBlend(), the fast paths everyone else uses
//   1  replace         - gpu_set_blendenable(false)
//   2  stencil select  - (bm_inv_dest_alpha, bm_dest_alpha), the mask pass
//   3  generic         - any other gpu_set_blendmode_ext pair

#define SWR_PIXEL_LEGACY  0
#define SWR_PIXEL_REPLACE 1
#define SWR_PIXEL_SELECT  2
#define SWR_PIXEL_GENERIC 3

#define SWR_ALPHA_BIT 0x8000
#define SWR_COLOR_BITS 0x7FFF

// Maps a 5-bit channel value to the 0..256 fixed point range the blend factors use.
FORCE_INLINE int swrFactor5(int value)
{
    return (value * 256 + 15) / 31;
}

// One factor of the GL blend equation, in 0..256 fixed point.
FORCE_INLINE int swrBlendFactor(int factor, int srcChan, int dstChan, int srcAlpha, int dstAlpha)
{
    switch (factor)
    {
        case bm_zero:           return 0;
        case bm_one:            return 256;
        case bm_src_color:      return swrFactor5(srcChan);
        case bm_inv_src_color:  return 256 - swrFactor5(srcChan);
        case bm_src_alpha:      return srcAlpha;
        case bm_inv_src_alpha:  return 256 - srcAlpha;
        case bm_dest_alpha:     return dstAlpha;
        case bm_inv_dest_alpha: return 256 - dstAlpha;
        case bm_dest_color:     return swrFactor5(dstChan);
        case bm_inv_dest_color: return 256 - swrFactor5(dstChan);
        case bm_src_alpha_sat:
        {
            int remaining = 256 - dstAlpha;
            return srcAlpha < remaining ? srcAlpha : remaining;
        }
        default:                return 256;
    }
}

// Turns the raw GPU state into the pixel mode the draw loops switch on.
FORCE_INLINE void swrSetupBlendState(SWRenderer* swr)
{
    if (!swr->blendEnabled)
    {
        swr->pixelMode = SWR_PIXEL_REPLACE;
    }
    else if (swr->blendSrcFactor == 0)
    {
        // The blendMode enum is in charge. It is expressible as a factor pair for
        // the two modes that matter, but only the general path can apply a colour
        // write mask, so drop out of the legacy path when one is set.
        bool maskActive = swr->writeMask != WRITE_MASK_ALL;
        if (maskActive && (swr->blendMode == bm_normal || swr->blendMode == bm_add))
            swr->pixelMode = SWR_PIXEL_GENERIC;
        else
            swr->pixelMode = SWR_PIXEL_LEGACY;
    }
    else if (swr->blendSrcFactor == bm_inv_dest_alpha && swr->blendDstFactor == bm_dest_alpha)
        swr->pixelMode = SWR_PIXEL_SELECT;
    else
        swr->pixelMode = SWR_PIXEL_GENERIC;
}

// Channel bits a gpu_set_colorwriteenable mask writes.
FORCE_INLINE uint16_t swrWriteMaskBits(int writeMask)
{
#if PIXEL_SIZE == 16
    uint16_t bits = 0;
    if (writeMask & WRITE_MASK_RED)   bits |= 0x7C00;
    if (writeMask & WRITE_MASK_GREEN) bits |= 0x03E0;
    if (writeMask & WRITE_MASK_BLUE)  bits |= 0x001F;
    if (writeMask & WRITE_MASK_ALPHA) bits |= SWR_ALPHA_BIT;
    return bits;
#else
    (void) writeMask;
    return 0xFFFF;
#endif
}

// Blends one covered source pixel into the destination using the extended state.
// `srcAlpha` is the fragment alpha (draw alpha once the source pixel is known to be
// opaque), between 0 and 256.
FORCE_INLINE void swrBlitPixelEx(SWRenderer* swr, uintpixel_t* dstPtr, uintpixel_t srcPixel,
                                 int srcAlpha, uint16_t maskBits)
{
#if PIXEL_SIZE == 16
    if (swr->alphaTestEnabled && srcAlpha * 255 < swr->alphaTestRef * 256)
        return;

    uintpixel_t dst = *dstPtr;
    uintpixel_t result;

    switch (swr->pixelMode)
    {
        case SWR_PIXEL_REPLACE:
            // GL with blending disabled: the fragment replaces the destination.
            result = (uintpixel_t) ((srcPixel & SWR_COLOR_BITS) | (srcAlpha > 127 ? SWR_ALPHA_BIT : 0));
            break;

        case SWR_PIXEL_SELECT:
            // (bm_inv_dest_alpha, bm_dest_alpha): paint where the destination is
            // transparent, leave it alone where it is opaque. This is the mask pass.
            if (dst & SWR_ALPHA_BIT)
                return;
            result = (uintpixel_t) ((srcPixel & SWR_COLOR_BITS) | (srcAlpha > 127 ? SWR_ALPHA_BIT : 0));
            break;

        default:
        {
            const int sr = (srcPixel >> 10) & 0x1F;
            const int sg = (srcPixel >> 5) & 0x1F;
            const int sb = srcPixel & 0x1F;
            const int dr = (dst >> 10) & 0x1F;
            const int dg = (dst >> 5) & 0x1F;
            const int db = dst & 0x1F;
            const int dstAlpha = (dst & SWR_ALPHA_BIT) ? 256 : 0;

            int sf = swr->blendSrcFactor, df = swr->blendDstFactor;
            if (sf == 0)
            {
                // A colour write mask with the plain blend modes: bm_normal and
                // bm_add are both expressible as a factor pair.
                if (swr->blendMode == bm_add) { sf = bm_src_alpha; df = bm_one; }
                else { sf = bm_src_alpha; df = bm_inv_src_alpha; }
            }

            int r = (sr * swrBlendFactor(sf, sr, dr, srcAlpha, dstAlpha) +
                     dr * swrBlendFactor(df, sr, dr, srcAlpha, dstAlpha)) >> 8;
            int g = (sg * swrBlendFactor(sf, sg, dg, srcAlpha, dstAlpha) +
                     dg * swrBlendFactor(df, sg, dg, srcAlpha, dstAlpha)) >> 8;
            int b = (sb * swrBlendFactor(sf, sb, db, srcAlpha, dstAlpha) +
                     db * swrBlendFactor(df, sb, db, srcAlpha, dstAlpha)) >> 8;

            int sa5 = srcAlpha >> 3, da5 = dstAlpha >> 3;
            int a = (sa5 * swrBlendFactor(sf, sa5, da5, srcAlpha, dstAlpha) +
                     da5 * swrBlendFactor(df, sa5, da5, srcAlpha, dstAlpha)) >> 8;

            if (r < 0) r = 0; else if (r > 31) r = 31;
            if (g < 0) g = 0; else if (g > 31) g = 31;
            if (b < 0) b = 0; else if (b > 31) b = 31;

            result = (uintpixel_t) ((a >= 16 ? SWR_ALPHA_BIT : 0) | (r << 10) | (g << 5) | b);
            break;
        }
    }

    *dstPtr = (uintpixel_t) ((dst & ~maskBits) | (result & maskBits));
#else
    (void) swr; (void) dstPtr; (void) srcPixel; (void) srcAlpha; (void) maskBits;
#endif
}

// Calculates an internal "alpha" value from GML-provided "alpha" values.
FORCE_INLINE int swrIntAlpha(float alphaf)
{
    return (int)(alphaf * 256);
}

// Calculates the source alpha for a pixel based on the current blend mode.
FORCE_INLINE int swrCalcSrcAlpha(SWRenderer* swr, int alpha)
{
    switch (swr->blendMode)
    {
        default:
            return alpha;
        case bm_add:
            return alpha;
        case bm_subtract:
            return -alpha;
    }
}

// Calculates the destination alpha for a pixel based on the current blend mode.
FORCE_INLINE int swrCalcDstAlpha(SWRenderer* swr, int alpha)
{
    switch (swr->blendMode)
    {
        default:
            return 256 - alpha;
        case bm_add:
            return 256;
        case bm_subtract:
            return 256;
    }
}

#endif//_SW_PIXEL_CALC_H
