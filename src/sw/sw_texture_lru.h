#ifndef _SW_TEXTURE_LRU_H
#define _SW_TEXTURE_LRU_H

void swrEvictTextureFromCache(SWRenderer* swr, int textureIndex);
void swrReserveTextureBytes(SWRenderer* swr, size_t needed);
bool swrEnsureTextureIsLoaded(SWRenderer* swr, uint32_t pageId);
// Makes only the strips covering the given source rectangle resident.
bool swrEnsureTextureRegion(SWRenderer* swr, uint32_t pageId, int sx, int sy, int sw, int sh);

#endif//_SW_TEXTURE_LRU_H
