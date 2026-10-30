// The fps/memory overlay and the draw tracer live here; they are small, and only
// the software renderer needs them.

#include <math.h>
#include <string.h>

#include "debug_font/debug_font.h"
#include "sw_renderer.h"
#include "sw_renderer_private.h"

// Blits one glyph of the debug font at `penX`,`top`, scaled by `scale` (0..1).
static void swrOverlayGlyph(SWRenderer* swr, const DebugFontGlyphEntry* g,
                            float penX, float top, float scale, uintpixel_t color,
                            int shadowDx, int shadowDy)
{
    int gw = (int) ceilf((float) g->w * scale);
    int gh = (int) ceilf((float) g->h * scale);
    if (gw <= 0 || gh <= 0) return;

    for (int dy = 0; dy < gh; dy++)
    {
        int sy = g->y + (int) ((float) dy / scale);
        if (sy >= g->y + g->h) sy = g->y + g->h - 1;

        int py = (int) (top + (float) g->yoffset * scale) + dy;
        if (py < 0 || py >= swr->height) continue;

        uintpixel_t* row = &swr->fb[(size_t) py * swr->fbPitch];
        for (int dx = 0; dx < gw; dx++)
        {
            int sx = g->x + (int) ((float) dx / scale);
            if (sx >= g->x + g->w) sx = g->x + g->w - 1;
            if (debugFontPixels[(size_t) sy * DEBUGFONT_ATLAS_W + sx] < 96) continue;

            int px = (int) (penX + (float) g->xoffset * scale) + dx;
            if (px < 0 || px >= swr->width) continue;

            int shx = px + shadowDx;
            int shy = py + shadowDy;
            if ((shadowDx | shadowDy) != 0 && shx >= 0 && shx < swr->width && shy >= 0 && shy < swr->height)
                swr->fb[(size_t) shy * swr->fbPitch + shx] = 0;

            row[px] = color;
        }
    }
}

void SWRenderer_drawOverlayText(Renderer* renderer, int x, int y, float scale, const char* text)
{
    if (renderer == nullptr || text == nullptr) return;

    SWRenderer* swr = (SWRenderer*) renderer;
    if (swr->fb == nullptr || scale <= 0.0f) return;

    // Shadow pass first, then the text, so a glyph never covers its own shadow.
    for (int pass = 0; pass < 2; pass++)
    {
        uintpixel_t color = (pass == 0) ? 0x0000 : 0x7FFF;
        int shadowDx = (pass == 0) ? 1 : 0;
        int shadowDy = (pass == 0) ? 1 : 0;

        float penX = (float) x;
        float top = (float) y;
        for (const char* c = text; *c != '\0'; c++)
        {
            if (*c == '\n')
            {
                penX = (float) x;
                top += (float) DEBUGFONT_LINE_HEIGHT * scale;
                continue;
            }
            if ((unsigned char) *c < DEBUGFONT_FIRST_CP || (unsigned char) *c > DEBUGFONT_LAST_CP) continue;

            const DebugFontGlyphEntry* g = &debugFontGlyphs[(unsigned char) *c - DEBUGFONT_FIRST_CP];
            swrOverlayGlyph(swr, g, penX, top, scale, color, shadowDx, shadowDy);
            penX += (float) g->xadvance * scale;
        }
    }
}

// Width of one line of overlay text, so a line can be placed by its right edge.
static float swrOverlayLineWidth(const char* text, float scale)
{
    float width = 0.0f;
    for (const char* c = text; *c != '\0' && *c != '\n'; c++)
    {
        if ((unsigned char) *c < DEBUGFONT_FIRST_CP || (unsigned char) *c > DEBUGFONT_LAST_CP) continue;
        width += (float) debugFontGlyphs[(unsigned char) *c - DEBUGFONT_FIRST_CP].xadvance * scale;
    }
    return width;
}

void SWRenderer_drawOverlayTextRight(Renderer* renderer, int marginRight, int y, float scale,
                                     const char* text)
{
    if (renderer == nullptr || text == nullptr) return;

    SWRenderer* swr = (SWRenderer*) renderer;
    if (swr->fb == nullptr || scale <= 0.0f) return;

    // One line at a time, each placed by its own right edge.
    float top = (float) y;

    for (const char* line = text; *line != '\0'; )
    {
        const char* end = line;
        while (*end != '\0' && *end != '\n') end++;

        char buf[128];
        size_t length = (size_t) (end - line);
        if (length >= sizeof(buf)) length = sizeof(buf) - 1;
        memcpy(buf, line, length);
        buf[length] = '\0';

        float lineWidth = swrOverlayLineWidth(buf, scale);
        int x = (int) ((float) swr->width - (float) marginRight - lineWidth);
        SWRenderer_drawOverlayText(renderer, x, (int) top, scale, buf);

        top += (float) DEBUGFONT_LINE_HEIGHT * scale;
        line = (*end == '\n') ? end + 1 : end;
    }
}
