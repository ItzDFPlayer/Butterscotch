#ifndef _SW_SHADER_EMU_H
#define _SW_SHADER_EMU_H

// ===[ Software emulation of the game's fragment shaders ]===
//
// The software renderer has no shaders, so a draw that the game makes with a
// shader bound would come out in the sprite's own colours. Some of those effects
// are the whole point of the scene - Deltarune's chapter 5 logo is drawn as a
// white mask under shd_shoujo, which replaces the mask's colour with a scrolling
// gradient plus bubbles and sparkles, masked by the sprite's alpha.
//
// Rather than a GLSL interpreter, the shader state that matters is recorded (see
// SWRenderer.shaderIndex/shaderStages/shaderUniforms) and the fragment function
// of each supported shader is reproduced here, keyed on its name. Draws made with
// any other shader bound keep the old behaviour.

// Sampler slots. shader_get_sampler_index() hands these back to the game, which
// passes them to texture_set_stage(); the fragment emulation then reads the same
// slots, so the two stay in sync by construction.
#define SWR_SHADER_SLOT_GRADIENT 0
#define SWR_SHADER_SLOT_BUBBLE   1
#define SWR_SHADER_SLOT_STARS    2

// One sampler, resolved to a rectangle inside a decoded texture page.
typedef struct
{
    SWTexture* texture;
    int x, y, width, height;
}
SWRShaderSampler;

// Per-draw shader state, resolved once by swrShaderEmuBegin() so that the fragment
// loop only does arithmetic.
typedef struct
{
    SWRShaderSampler stages[SWR_SHADER_STAGES];
    // gl_FragCoord.x/y are divided by this in the shader (application surface
    // width), so it must follow the surface the game draws into.
    float surfaceWidth;
    // The `time` uniform; the game animates the whole effect with it.
    float time;
}
SWRShaderContext;

// True when the bound shader has an emulation, i.e. a draw should take its colour
// from swrShaderEmuPixel() instead of from the texture. Cheap enough to call on
// every sprite draw.
bool swrShaderEmuActive(SWRenderer* swr);

// Resolves the bound shader and its sampler stages for one draw. Returns false
// when the shader has no emulation, in which case the draw must use the normal
// path.
bool swrShaderEmuBegin(SWRenderer* swr, SWRShaderContext* ctx);

// Colour of the shader at framebuffer pixel (x, y), as RGB555 with the alpha bit
// clear. Destination alpha and the draw's tint are left to the caller, which
// takes them from the sprite: the emulated shaders here only replace the
// fragment's rgb.
void swrShaderEmuPixel(const SWRShaderContext* ctx, int x, int y, uint16_t* outRgb555);

#endif//_SW_SHADER_EMU_H
