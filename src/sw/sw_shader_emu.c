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
