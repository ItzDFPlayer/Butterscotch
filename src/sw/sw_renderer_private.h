#ifndef _SW_RENDERER_PRIVATE_H
#define _SW_RENDERER_PRIVATE_H

#include "sw_renderer.h"
#include "defines.h"
#include "pixel_convert.h"

// Unimplemented Functions
//#define UNIMP() do { fprintf(stderr, "NYI %s\n", __func__); } while (0)
#define UNIMP() do { } while (0)
#define UNIMP2() do { } while (0)

// Capacity of the shader emulation state kept in SWRenderer.
#define SWR_SHADER_STAGES   8
#define SWR_SHADER_UNIFORMS 16

// Provide PI if not specified
#ifndef M_PI
#define M_PI 3.1415926535897932384626
#endif

// Configurable Properties
#define SURFACE_MAX_COUNT 64
// Decoded texture pages are held in RAM (16bpp). Deltarune uses 2048x2048 pages
// (8 MB each), so limiting by entry count (64) could pin ~512 MB and OOM a 128 MB
// device. The cache is capped by bytes instead; the budget is chosen at runtime
// (48 MB on 128 MB devices, 64 MB otherwise) and can be overridden with
// BUTTERSCOTCH_TEXTURE_MB.
// (NOTE: See PIXEL_SIZE in defines.h)


// Struct Definitions
typedef struct
{
    uintpixel_t* buffer;
    uint16_t width, height;
    size_t byteSize;
    // Monotonic use counter; the lowest value is evicted first. Updated every
    // time a page is touched by a draw, not just when it is decoded, so pages
    // used every frame (fonts/UI) are not evicted ahead of background pages.
    uint64_t lastUsedTick;
    // Sub-page residency: when stripCount > 0 the buffer was calloc'd and only
    // the strips in `stripsLoaded` have been decoded into it. byteSize then
    // tracks the resident (decoded) bytes, not the full page. Non-sparse
    // textures have stripCount == 0 and are always fully resident.
    uint32_t stripsLoaded;
    uint16_t stripRows;
    uint16_t stripCount;
}
SWTexture;

typedef struct
{
    // used for almost all intents and purposes.
    SWTexture* texture;
    // upon a gpuSetColorWriteEnable change, the shadow texture is used for writing instead.
    SWTexture* shadowTexture;
}
SWSurface;

#define WRITE_MASK_ALL   (15)
#define WRITE_MASK_RED   (1)
#define WRITE_MASK_GREEN (2)
#define WRITE_MASK_BLUE  (4)
#define WRITE_MASK_ALPHA (8)

typedef struct
{
    Renderer base;
    
    // Window Properties
    uint16_t width;
    uint16_t height;
    // Framebuffer
    uintpixel_t* fb;
    uint16_t fbPitch; // in sizeof(uintpixel_t) units, NOT in bytes!
    
    bool drawingToSurface;
    uintpixel_t* mainFb;
    uint16_t mainWidth;
    uint16_t mainHeight;
    uint16_t mainPitch;
    int lastViewX, lastViewY, lastViewW, lastViewH;
    int lastPortX, lastPortY, lastPortW, lastPortH;
    int lastGameW, lastGameH, lastMaxX, lastMaxY;
    float lastScaleX, lastScaleY;
    
    SWTexture** textures;
    SWSurface** surfaces;
    uint64_t textureUseClock;
    size_t textureCount;
    size_t surfaceCount;
    size_t totalTextureCount;
    size_t textureBytes;       // decoded bytes currently resident in the cache
    size_t textureBudgetBytes; // current cap for decoded texture bytes
    size_t textureBudgetMax;   // hard cap; raised when a page is evicted and
                               // immediately needed again (working set > budget)
    // Cumulative cache churn counters, reported by --perf-log.
    uint32_t textureLoads;
    uint32_t textureEvictions;
    // Draw culling counters: how many sprite/tile draws were skipped because
    // they were entirely outside the current viewport.
    uint64_t drawsAttempted;
    uint64_t drawsCulled;
    // Rasterisation counters for --perf-log. pxWritten counts destination pixels
    // covered by sprite blits (transparent source pixels are skipped, so it is an
    // upper bound), and pxFastCopy counts the part that took the opaque copy path.
    uint64_t pxWritten;
    uint64_t pxFastCopy;
    uint64_t pxVectorBlend;
    uint64_t pxScalarBlend; // blended pixels that still ran through the scalar loop
    uint64_t pxRotated;     // pixels covered by rotated sprites (area of their bounds)
    // Recently evicted pages, used to detect thrashing.
    uint32_t recentEvictedPages[8];
    uint64_t recentEvictedTicks[8];
    int recentEvictedNext;
    size_t originalTPagCount;
    size_t originalSpriteCount;
    
    bool viewActive;
    int viewX, viewY, viewW, viewH;
    int portX, portY, portW, portH;
    int gameW, gameH, maxX, maxY;

    int offsetX, offsetY;
    float scaleX, scaleY;
    float defaultScaleX, defaultScaleY;
    
    int blendMode;

    // Full GPU blend state, as set through gpu_set_blendenable,
    // gpu_set_blendmode_ext, gpu_set_alphatestenable/ref and
    // gpu_set_colorwriteenable. Deltarune Chapter 3+ builds its spotlight/stencil
    // masks out of these instead of plain sprite blending, so they cannot be
    // stubbed out. `blendSrcFactor == 0` means "the blendMode enum above is in
    // charge", which is the state almost every draw is in.
    bool blendEnabled;
    int blendSrcFactor;
    int blendDstFactor;
    bool alphaTestEnabled;
    int alphaTestRef;     // 0..255, compared against the fragment alpha
    // Derived from the three fields above plus writeMask; see swrSetupBlendState.
    // 0 = legacy path (alphaBlend), and anything else selects the general blitter.
    int pixelMode;

    // only used for surfaces.  The application surface doesn't support these at the moment.
    int currentSurfaceIndex;
    int writeMask;

    // When non-zero every draw is written to the log, which is how a scene that
    // "renders wrong" gets compared against the instances that are on stage.
    int traceDraws;

    // Minimal shader state: which shader is bound, the textures bound to its
    // samplers with texture_set_stage, and the float uniforms set with
    // shader_set_uniform_f. Nothing is compiled here - sw_shader_emu.c uses this to
    // reproduce the handful of screen-space fragment shaders the games use.
    int32_t shaderIndex;                    // -1 when nothing is bound
    uint32_t shaderStages[SWR_SHADER_STAGES];
    int32_t shaderUniformCount;
    struct SwrShaderUniform
    {
        char name[24];
        float value[4];
    } shaderUniforms[SWR_SHADER_UNIFORMS];
}
SWRenderer;

// True when the renderer is drawing into a surface. Every read of the draw target
// goes through this instead of looking at `drawingToSurface` directly: the flag and
// `currentSurfaceIndex` can disagree (a pass torn down by a room or chapter change
// leaves the flag set with no index, which is exactly how swr->surfaces[-1] got read
// and crashed the GUI pass), and a target is only real when both agree.
FORCE_INLINE bool swrIsOnSurface(const SWRenderer* swr)
{
    return swr->drawingToSurface &&
           swr->currentSurfaceIndex >= 0 &&
           (size_t) swr->currentSurfaceIndex < swr->surfaceCount;
}

// Inlined function definitions included below
#include "sw_pixel_calc.h"
#include "sw_inlined.h"
#include "sw_transform.h"

#include "sw_texture.h"
#include "sw_surface.h"
#include "sw_drawing.h"
#include "sw_texture_lru.h"
#include "sw_shader_emu.h"

#endif//_SW_RENDERER_PRIVATE_H
