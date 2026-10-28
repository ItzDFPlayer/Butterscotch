#ifndef _SW_TEXTURE_H
#define _SW_TEXTURE_H

SWTexture* swrCreateTextureEx(const void* srcBuffer, int width, int height, bool convert);
SWTexture* swrCreateTexture(const uint8_t* srcBuffer, int width, int height);
// Takes ownership of a malloc'd pixel buffer in the renderer's native format
// (no conversion, no copy).
SWTexture* swrAdoptTexture(uintpixel_t* buffer, int width, int height);

// Allocates an empty (calloc'd) page whose pixels are decoded per strip on
// demand. Returns NULL when the allocation fails.
SWTexture* swrCreateSparseTexture(int width, int height, int stripRows, int stripCount);
SWTexture* swrCopyTexture(SWTexture* texture);
void swrFreeTexture(SWTexture* texture);

#endif//_SW_TEXTURE_H
