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

// ===[ shd_ripple ]===
// Deltarune draws water ripples as filled rectangles under this shader, which keeps
// the draw colour and takes the alpha from the distance to the ripple centre. With
// the shader the rectangle is a soft ring; without it, it is a solid block.
typedef struct {
    float centerX, centerY;
    float radius;      // current ring radius (uniform rippleRad)
    float maxRadius;
    float thickness;   // width of the ring band
    float yRatio;      // vertical squash, so the ring reads as a floor ripple
    int banding;
    bool fading;
}
SWRippleCtx;

// Reads the uniforms for one ripple draw. False when the bound shader is not
// shd_ripple or the game has not set the uniforms yet.
bool swrShaderEmuRippleBegin(SWRenderer* swr, SWRippleCtx* ctx);

// Horizontal extent of the ring on destination row y: [outerX0, outerX1] minus the
// hole [holeX0, holeX1] (holeX0 > holeX1 when there is no hole). False when the row
// misses the ring, which lets the caller skip whole spans instead of every pixel.
bool swrShaderEmuRippleRow(const SWRippleCtx* ctx, int y, int* outerX0, int* outerX1, int* holeX0, int* holeX1);

// Alpha (0..256) the shader would produce at destination pixel (x, y).
int swrShaderEmuRippleAlpha(const SWRippleCtx* ctx, int x, int y);

// Resolves the bound shader and its sampler stages for one draw. Returns false
// when the shader has no emulation, in which case the draw must use the normal
// path.
bool swrShaderEmuBegin(SWRenderer* swr, SWRShaderContext* ctx);

// Colour of the shader at framebuffer pixel (x, y), as RGB555 with the alpha bit
// clear. Destination alpha and the draw's tint are left to the caller, which
// takes them from the sprite: the emulated shaders here only replace the
// fragment's rgb.
void swrShaderEmuPixel(const SWRShaderContext* ctx, int x, int y, uint16_t* outRgb555);

// ===[ shd_shadowblend and shd_forcecolour ]===
//
// Chapter 4 and 5 build the sun shadows by drawing object silhouettes into a
// surface and then blitting that surface with a shader that throws the
// silhouette's colour away entirely. shd_shadowblend paints every covered pixel
// as shadowCol.rgb with alpha min(shadowCol.a, v_vColour.a * mask.a);
// shd_forcecolour does the same for the highlight pass, ramping from black to
// forceColour by the mask's alpha. Both keep the mask's coverage, so the effect
// is a coloured overlay shaped like the silhouettes.
//
// Without them the mask is drawn in its own colours - white - and lands on the
// screen as a solid white band over the ground and white streaks over the
// characters, which is exactly how the chapter 5 cliff scene looked.
typedef struct
{
    bool forceColour;   // false: shd_shadowblend, true: shd_forcecolour
    uint16_t colour;    // RGB555 (alpha bit clear) of shadowCol/forceColour
    int alpha;          // shadowCol.a as 0..256; unused by shd_forcecolour
}
SWRFlatShaderCtx;

// Reads the uniforms for one flat-colour draw. False when the bound shader is
// neither of the two, or the game has not set its colour uniform yet.
bool swrShaderEmuFlatBegin(SWRenderer* swr, SWRFlatShaderCtx* ctx);

// ===[ shd_alpha ]===
//
//   gl_FragColor = v_vColour * texture2D(gm_BaseTexture, v_vTexcoord);
//   gl_FragColor.a = min(gl_FragColor.a, alpha);
//
// The game binds this to a whole layer and animates the `alpha` uniform from a
// layer begin script, so the layer fades as one unit. Chapter 5 uses it on the
// TILES_Newpath layer of the pink room: that layer is the walkable path overlay
// (the room controller reads it as self.path_layer and the game keeps its
// collision data in it), and the script drives its alpha down so the overlay is
// not drawn over the wall. Without the clamp the path tiles paint bright floor
// tiles straight through the wallpaper.
//
// Returns true and writes the clamped alpha (0..256, the range this renderer
// blends in) when the bound shader is shd_alpha.
bool swrShaderEmuAlphaClampBegin(SWRenderer* swr, int* outAlpha);

// Fragment result for one texel: `maskAlpha` is the silhouette coverage (0..255)
// and `drawAlpha` the draw call's alpha scaled to 0..256, which is the range this
// renderer blends in. Writes RGB555 (alpha bit clear) and the final alpha.
void swrShaderEmuFlatPixel(const SWRFlatShaderCtx* ctx, int maskAlpha, int drawAlpha,
                           uint16_t* outRgb, int* outAlpha);

#endif//_SW_SHADER_EMU_H
