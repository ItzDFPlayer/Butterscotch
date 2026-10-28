#include <stdlib.h>
#include "sw_renderer_private.h"

SWTexture* swrCreateTextureEx(const void* srcBuffer, int width, int height, bool convert)
{
    SWTexture* txt = (SWTexture*) safeMalloc(sizeof(SWTexture));
    txt->buffer = (uintpixel_t*) safeMalloc(width * height * sizeof(uintpixel_t));
    
    size_t sz = width * height;
    
    if (srcBuffer)
    {
        if (convert) {
            const uint32_t* rgbaSrc = (const uint32_t*) srcBuffer;
            for (size_t i = 0; i < sz; i++)
                txt->buffer[i] = swrConvertPixelTexture(rgbaSrc[i]);
        }
        else {
            const uintpixel_t* rgbaSrc = (const uintpixel_t*) srcBuffer;
            for (size_t i = 0; i < sz; i++)
                txt->buffer[i] = rgbaSrc[i];
        }
    }
    else
    {
        memset(txt->buffer, 0, width * height * sizeof(uintpixel_t));
    }
    
    txt->width = (uint16_t) width;
    txt->height = (uint16_t) height;
    txt->byteSize = (size_t) width * (size_t) height * sizeof(uintpixel_t);
    txt->lastUsedTick = 0;
    txt->stripsLoaded = 0;
    txt->stripRows = 0;
    txt->stripCount = 0;
    
    return txt;
}

SWTexture* swrCreateTexture(const uint8_t* srcBuffer, int width, int height)
{
    return swrCreateTextureEx(srcBuffer, width, height, true);
}

SWTexture* swrCopyTexture(SWTexture* texture)
{
    return swrCreateTextureEx(texture->buffer, texture->width, texture->height, false);
}

SWTexture* swrAdoptTexture(uintpixel_t* buffer, int width, int height)
{
    SWTexture* txt = (SWTexture*) safeMalloc(sizeof(SWTexture));
    txt->buffer = buffer;
    txt->width = (uint16_t) width;
    txt->height = (uint16_t) height;
    txt->byteSize = (size_t) width * (size_t) height * sizeof(uintpixel_t);
    txt->lastUsedTick = 0;
    txt->stripsLoaded = 0;
    txt->stripRows = 0;
    txt->stripCount = 0;
    return txt;
}

SWTexture* swrCreateSparseTexture(int width, int height, int stripRows, int stripCount)
{
    if (width <= 0 || height <= 0 || stripRows <= 0 || stripCount <= 0) return NULL;
    if (stripCount > 32) return NULL; // stripsLoaded is a 32-bit mask

    size_t count = (size_t) width * (size_t) height;
    // calloc, not malloc: strips that are never drawn stay unmapped, so a page
    // only costs the RAM it actually uses.
    uintpixel_t* buffer = (uintpixel_t*) calloc(count, sizeof(uintpixel_t));
    if (buffer == NULL) return NULL;

    SWTexture* txt = (SWTexture*) safeMalloc(sizeof(SWTexture));
    txt->buffer = buffer;
    txt->width = (uint16_t) width;
    txt->height = (uint16_t) height;
    txt->byteSize = 0; // grows as strips are decoded
    txt->lastUsedTick = 0;
    txt->stripsLoaded = 0;
    txt->stripRows = (uint16_t) stripRows;
    txt->stripCount = (uint16_t) stripCount;
    return txt;
}

void swrFreeTexture(SWTexture* texture)
{
    if (UNLIKELY(!texture))
        return;
    
    free(texture->buffer);
    free(texture);
}
