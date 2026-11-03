#include "sw_renderer_private.h"

#include <math.h>
#include <string.h>

// ===[ Fragment shader emulation ]===
//
// Only the shaders that a scene cannot be understood without are implemented, and
// each one is matched by name. The GML that sets them up is:
//
//   u_time        = shader_get_uniform("time", shd_shoujo)
//   u_width       = shader_get_uniform("appSurfWidth", shd_shoujo)
//   u_texgradient = shader_get_sampler_index("texGradient", shd_shoujo)
//   ...
//   shader_set(shd_shoujo)
//   shader_set_uniform_f(u_time, t)
//   shader_set_uniform_f(u_width, surface_get_width(application_surface))
//   texture_set_stage(u_texgradient, sprite_get_texture(0, 6087))
//   draw_sprite(spr_logo, 0, x, y)
//
// so the state we need is exactly what SWRenderer records. The fragment maths is
// copied from the GLSL shown by `butterscotch --print-shaders`, with gl_FragCoord
// taken to be the pixel centre as OpenGL defines it.

static bool swrShaderNameIs(SWRenderer* swr, const char* name)
{
    DataWin* dw = swr->base.dataWin;
    if (dw == NULL) return false;
    if (swr->shaderIndex < 0 || (uint32_t) swr->shaderIndex >= dw->shdr.count) return false;
    const char* bound = dw->shdr.shaders[swr->shaderIndex].name;
    if (bound == NULL) return false;
    return strcmp(bound, name) == 0;
}

bool swrShaderEmuActive(SWRenderer* swr)
{
    return swrShaderNameIs(swr, "shd_shoujo");
}

static float swrUniform(const SWRenderer* swr, const char* name, float fallback)
{
    for (int32_t i = 0; i < swr->shaderUniformCount; i++)
    {
        if (strcmp(swr->shaderUniforms[i].name, name) == 0)
            return swr->shaderUniforms[i].value[0];
    }
    return fallback;
}

// Reads up to `count` components of a vector uniform, filling the rest from
// `fallback`. Returns false when the uniform was never set.
static bool swrUniformVec(const SWRenderer* swr, const char* name, float* out, int count, float fallback)
{
    for (int32_t i = 0; i < swr->shaderUniformCount; i++)
    {
        if (strcmp(swr->shaderUniforms[i].name, name) != 0) continue;
        for (int c = 0; c < count; c++)
            out[c] = c < 4 ? swr->shaderUniforms[i].value[c] : fallback;
        return true;
    }
    for (int c = 0; c < count; c++) out[c] = fallback;
    return false;
}

// shd_alpha: gl_FragColor.a = min(gl_FragColor.a, alpha). The game animates the
// `alpha` uniform from a layer begin script, so the whole layer fades together.
bool swrShaderEmuAlphaClampBegin(SWRenderer* swr, int* outAlpha)
{
    if (!swrShaderNameIs(swr, "shd_alpha")) return false;

    float a = swrUniform(swr, "alpha", 1.0f);
    if (!(a > 0.0f)) a = 0.0f;   // also catches NaN
    if (a > 1.0f) a = 1.0f;
    *outAlpha = (int) (a * 256.0f + 0.5f);
    return true;
}

// ===[ shd_shadowblend / shd_forcecolour ]===
//
//   gl_FragColor = v_vColour * texture2D(gm_BaseTexture, v_vTexcoord);
//   gl_FragColor.rgb = shadowCol.rgb;
//   if (gl_FragColor.a > 0.0) gl_FragColor.a = min(shadowCol.a, gl_FragColor.a);
//
// and, for the highlight surface,
//
//   vec4 col = texture2D(gm_BaseTexture, v_vTexcoord);
//   gl_FragColor = v_vColour * col;
//   gl_FragColor.rgb = mix(vec3(0.0), forceColour, col.a);
//
// Both read the mask's alpha rather than its colour, so the surfaces only ever
// need to say "something is here": the colour comes from the uniform.
static uint16_t swrColourToRgb555(const float* rgb)
{
    int r = (int) (rgb[0] * 31.0f + 0.5f);
    int g = (int) (rgb[1] * 31.0f + 0.5f);
    int b = (int) (rgb[2] * 31.0f + 0.5f);
    r = r < 0 ? 0 : (r > 31 ? 31 : r);
    g = g < 0 ? 0 : (g > 31 ? 31 : g);
    b = b < 0 ? 0 : (b > 31 ? 31 : b);
    return (uint16_t) (b | (g << 5) | (r << 10));
}

bool swrShaderEmuFlatBegin(SWRenderer* swr, SWRFlatShaderCtx* ctx)
{
    if (swrShaderNameIs(swr, "shd_shadowblend"))
    {
        float shadowCol[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        // A uniform the game never set reads as zero, exactly as it would on a GPU:
        // shd_shadowblend then has alpha 0 and the pass is invisible. Falling through
        // to the normal sprite path instead would paint the raw white mask, which is
        // how this scene used to look.
        swrUniformVec(swr, "shadowCol", shadowCol, 4, 0.0f);
        ctx->forceColour = false;
        ctx->colour = swrColourToRgb555(shadowCol);
        ctx->alpha = (int) (shadowCol[3] * 256.0f + 0.5f);
        if (ctx->alpha > 256) ctx->alpha = 256;
        if (ctx->alpha < 0) ctx->alpha = 0;
        return true;
    }

    if (swrShaderNameIs(swr, "shd_forcecolour"))
    {
        float forceColour[3] = { 0.0f, 0.0f, 0.0f };
        swrUniformVec(swr, "forceColour", forceColour, 3, 0.0f);
        ctx->forceColour = true;
        ctx->colour = swrColourToRgb555(forceColour);
        ctx->alpha = 256;
        return true;
    }

    return false;
}

void swrShaderEmuFlatPixel(const SWRFlatShaderCtx* ctx, int maskAlpha, int drawAlpha,
                           uint16_t* outRgb, int* outAlpha)
{
    const int a = (drawAlpha * maskAlpha + 127) / 255;

    if (!ctx->forceColour)
    {
        // The colour is constant, so nothing here depends on the mask beyond its
        // coverage; the alpha is the darker of the draw's and the shadow's.
        *outRgb = ctx->colour;
        *outAlpha = (a < ctx->alpha) ? a : ctx->alpha;
        return;
    }

    // mix(black, forceColour, col.a): straight multiplication, since mixing from
    // black is a scale.
    const int b = ((ctx->colour & 0x1F) * maskAlpha + 127) / 255;
    const int g = (((ctx->colour >> 5) & 0x1F) * maskAlpha + 127) / 255;
    const int r = (((ctx->colour >> 10) & 0x1F) * maskAlpha + 127) / 255;
    *outRgb = (uint16_t) (b | (g << 5) | (r << 10));
    *outAlpha = a;
}

// ===[ shd_ripple ]===
//
// Deltarune draws its water ripples as plain filled rectangles with this shader
// bound: the fragment keeps the draw colour and takes its alpha from the distance to
// the ripple centre, so the rectangle comes out as a soft ring. Without the shader
// the whole rectangle lands solid, which reads as a big grey block over the scene -
// the reason the chapter 5 splash looked so wrong.
//
// The ring only covers a thin band and dozens of ripples can be live at once, so the
// uniforms are read once per draw and the caller iterates per-row spans instead of
// testing every pixel of the rectangle.
bool swrShaderEmuRippleBegin(SWRenderer* swr, SWRippleCtx* ctx)
{
    if (!swrShaderNameIs(swr, "shd_ripple")) return false;

    float center[2] = { 0.0f, 0.0f };
    if (!swrUniformVec(swr, "rippleCenter", center, 2, 0.0f)) return false;

    // x: current radius, y: max radius, z: ring thickness, w: vertical squash.
    float rad[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!swrUniformVec(swr, "rippleRad", rad, 4, 0.0f)) return false;
    if (rad[0] <= 0.0f) return false;
    if (rad[3] <= 0.0f) rad[3] = 1.0f;

    ctx->centerX = center[0];
    ctx->centerY = center[1];
    ctx->radius = rad[0];
    ctx->maxRadius = rad[1];
    ctx->thickness = rad[2] > 0.0f ? rad[2] : 0.0f;
    ctx->yRatio = rad[3];
    ctx->banding = (int) (swrUniform(swr, "rippleBanding", 0.0f) + 0.5f);
    ctx->fading = swrUniform(swr, "rippleFading", 0.0f) > 0.5f;
    return true;
}

// Vertical offset from the centre for destination row y. The fragment squashes y by
// the y ratio, so the ring reads as a circle seen at an angle.
static float swrRippleRowOffset(const SWRippleCtx* ctx, int y)
{
    return ((float) y + 0.5f - ctx->centerY) * ctx->yRatio;
}

// Horizontal extent of the ring on one row: out to the outer edge, minus the hole in
// the middle (reported as holeX0 > holeX1 when there is none). False when the row
// misses the ring entirely.
bool swrShaderEmuRippleRow(const SWRippleCtx* ctx, int y, int* outerX0, int* outerX1, int* holeX0, int* holeX1)
{
    const float dy = swrRippleRowOffset(ctx, y);
    const float ady = fabsf(dy);
    if (ady >= ctx->radius) return false;

    // One pixel of slack, so the rounding of the edge test stays on the safe side.
    const float outer = sqrtf(ctx->radius * ctx->radius - ady * ady) + 1.0f;
    *outerX0 = (int) floorf(ctx->centerX - outer);
    *outerX1 = (int) ceilf(ctx->centerX + outer);

    const float innerRadius = ctx->radius - ctx->thickness;
    if (innerRadius > 0.0f && ady < innerRadius)
    {
        const float inner = sqrtf(innerRadius * innerRadius - ady * ady) - 1.0f;
        *holeX0 = (int) ceilf(ctx->centerX - inner);
        *holeX1 = (int) floorf(ctx->centerX + inner);
    }
    else
    {
        *holeX0 = 1;
        *holeX1 = 0;
    }

    return *outerX1 >= *outerX0;
}

// Fragment alpha (0..256) the shader would produce at destination pixel (x, y).
int swrShaderEmuRippleAlpha(const SWRippleCtx* ctx, int x, int y)
{
    const float dx = (float) x + 0.5f - ctx->centerX;
    const float dy = swrRippleRowOffset(ctx, y);
    const float dist = sqrtf(dx * dx + dy * dy);

    const float currentRadius = ctx->radius;
    const float thickness = ctx->thickness;

    float edge = dist - (currentRadius - thickness);
    if (edge < 0.0f) edge = 0.0f;

    const float perc = dist / currentRadius;
    float alph = 1.0f;
    const float rimThickness = thickness / 3.0f;

    if (ctx->banding == 1)
    {
        alph = perc;
    }
    else if (ctx->banding == 0)
    {
        if (edge > 0.0f) alph = 0.6f;
        if (edge > rimThickness) alph = 0.8f;
        if (edge > rimThickness * 2.0f) alph = 1.0f;
    }
    else if (ctx->banding == 2)
    {
        if (edge > thickness) alph = 0.0f;
    }

    // The shader clamps the rim ramp to one pixel, so a ripple is a banded annulus
    // rather than a gradient spread over its whole thickness.
    if (edge > 1.0f) edge = 1.0f;
    if (perc > 1.0f) alph = 0.0f;

    float alpha = edge * alph;
    if (ctx->fading)
    {
        const float fadeStart = ctx->maxRadius / 3.0f;
        if (ctx->maxRadius > fadeStart)
        {
            float fade = 1.0f - (currentRadius - fadeStart) / (ctx->maxRadius - fadeStart);
            if (fade > 1.0f) fade = 1.0f;
            if (fade < 0.0f) fade = 0.0f;
            alpha *= fade;
        }
    }

    if (alpha <= 0.0f) return 0;
    if (alpha > 1.0f) alpha = 1.0f;
    return (int) (alpha * 256.0f + 0.5f);
}

// Resolves a texture id from texture_set_stage() to the decoded page plus the
// region of it the sprite covers. The id is sprite_get_texture()'s, which the
// software renderer encodes as tpagIndex + 1.
static void swrResolveStage(SWRenderer* swr, int slot, SWRShaderSampler* out)
{
    out->texture = NULL;
    out->x = out->y = out->width = out->height = 0;
    if (slot < 0 || slot >= SWR_SHADER_STAGES) return;

    uint32_t texID = swr->shaderStages[slot];
    if (texID == 0) return;
    uint32_t tpagIndex = texID - 1;

    DataWin* dw = swr->base.dataWin;
    if (dw == NULL || tpagIndex >= dw->tpag.count) return;

    TexturePageItem* item = &dw->tpag.items[tpagIndex];
    int pageId = item->texturePageId;
    if (pageId < 0 || (uint32_t) pageId >= swr->textureCount) return;

    // A stage texture is sampled with wrap-around, so the whole sprite rect has to
    // be resident, not just whatever strip the current draw touches.
    swrEnsureTextureRegion(swr, (uint32_t) pageId, item->sourceX, item->sourceY,
                           item->sourceWidth, item->sourceHeight);
    SWTexture* texture = swr->textures[pageId];
    if (texture == NULL) return;

    out->texture = texture;
    out->x = item->sourceX;
    out->y = item->sourceY;
    out->width = item->sourceWidth;
    out->height = item->sourceHeight;
    if (out->x + out->width > texture->width) out->width = texture->width - out->x;
    if (out->y + out->height > texture->height) out->height = texture->height - out->y;
    if (out->width < 1 || out->height < 1) out->texture = NULL;
}

bool swrShaderEmuBegin(SWRenderer* swr, SWRShaderContext* ctx)
{
    if (!swrShaderEmuActive(swr)) return false;

    memset(ctx, 0, sizeof(*ctx));
    for (int slot = 0; slot < SWR_SHADER_STAGES; slot++)
        swrResolveStage(swr, slot, &ctx->stages[slot]);

    // The gradient is what the effect is; without it there is nothing to draw.
    if (ctx->stages[SWR_SHADER_SLOT_GRADIENT].texture == NULL) return false;

    float width = swrUniform(swr, "appSurfWidth", (float) swr->gameW);
    if (width <= 0.0f) width = (float) swr->gameW;
    if (width <= 0.0f) width = 640.0f;
    ctx->surfaceWidth = width;
    ctx->time = swrUniform(swr, "time", 0.0f);
    return true;
}

static float swrFract(float v)
{
    return v - floorf(v);
}

// Nearest-neighbour sample with GL_REPEAT wrapping, in the page's RGB555 space.
static void swrSampleStage(const SWRShaderSampler* sampler, float u, float v, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (sampler->texture == NULL) return;

    int sx = (int) (swrFract(u) * (float) sampler->width);
    int sy = (int) (swrFract(v) * (float) sampler->height);
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= sampler->width) sx = sampler->width - 1;
    if (sy >= sampler->height) sy = sampler->height - 1;

    uint16_t pixel = sampler->texture->buffer[(sampler->y + sy) * sampler->texture->width
                                            + (sampler->x + sx)];
    out[0] = (float) ((pixel >> 10) & 31) / 31.0f;
    out[1] = (float) ((pixel >> 5) & 31) / 31.0f;
    out[2] = (float) (pixel & 31) / 31.0f;
}

static uint16_t swrPackRgb555(float r, float g, float b)
{
    if (r < 0.0f) r = 0.0f;
    if (g < 0.0f) g = 0.0f;
    if (b < 0.0f) b = 0.0f;
    if (r > 1.0f) r = 1.0f;
    if (g > 1.0f) g = 1.0f;
    if (b > 1.0f) b = 1.0f;

    uint16_t ir = (uint16_t) (r * 31.0f + 0.5f);
    uint16_t ig = (uint16_t) (g * 31.0f + 0.5f);
    uint16_t ib = (uint16_t) (b * 31.0f + 0.5f);
    return (uint16_t) ((ir << 10) | (ig << 5) | ib);
}

// shd_shoujo: a horizontally scrolling gradient, followed by bubbles and sparkles
// that drift diagonally, all of it replacing the sprite's colour while the
// sprite's alpha still masks the result.
void swrShaderEmuPixel(const SWRShaderContext* ctx, int x, int y, uint16_t* outRgb555)
{
    const float width = ctx->surfaceWidth;
    const float time = ctx->time;
    const float fx = (float) x + 0.5f; // gl_FragCoord
    const float fy = (float) y + 0.5f;

    const float gradientWidth = 128.0f;

    // color.rgb = texture(texGradient, vec2(mod(gl_FragCoord.x/appSurfWidth + time, 1), 0))
    float gradPos = swrFract(fx / width + time);
    float rgb[3];
    swrSampleStage(&ctx->stages[SWR_SHADER_SLOT_GRADIENT], gradPos, 0.0f, rgb);

    // The gradient is 128 texels wide but stretched over the whole surface, so the
    // shader cross-fades each texel into its neighbour to hide the interpolation.
    float mixPercent = swrFract(fx / (width / gradientWidth));
    float next[3];
    swrSampleStage(&ctx->stages[SWR_SHADER_SLOT_GRADIENT],
                   gradPos + 0.5f / gradientWidth, 0.0f, next);
    for (int c = 0; c < 3; c++)
        rgb[c] = rgb[c] + (next[c] - rgb[c]) * mixPercent;

    // Brighten towards the left and right edges of the screen.
    float edge = fabsf(-1.0f + swrFract(fx / width) * 2.0f) * 0.5f;
    for (int c = 0; c < 3; c++)
        rgb[c] += edge;

    // Bubbles and stars, two bubble layers at different speeds plus one star layer.
    float layer[3];
    swrSampleStage(&ctx->stages[SWR_SHADER_SLOT_BUBBLE],
                   fx / width * 4.0f + time * 1.25f, fy / width * 4.0f + time * 1.20f, layer);
    for (int c = 0; c < 3; c++)
        rgb[c] += layer[c];

    swrSampleStage(&ctx->stages[SWR_SHADER_SLOT_BUBBLE],
                   fx / width * 4.0f + time * 2.65f, fy / width * 4.0f + time * 2.60f, layer);
    for (int c = 0; c < 3; c++)
        rgb[c] += layer[c] * 0.15f;

    swrSampleStage(&ctx->stages[SWR_SHADER_SLOT_STARS],
                   fx / width * 4.0f + time * 1.95f, fy / width * 4.0f + time * 1.90f, layer);
    for (int c = 0; c < 3; c++)
        rgb[c] += layer[c];

    *outRgb555 = swrPackRgb555(rgb[0], rgb[1], rgb[2]);
}
