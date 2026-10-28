#ifndef _BS_TEXTURE_BAKER_H_
#define _BS_TEXTURE_BAKER_H_

#include "common.h"
#include <stdint.h>

// Progress callback used by the pre-game loading screen.
// `total` may be 0 while the WAD is still being scanned.
typedef void (*TextureBakerProgressFn)(void* userData, uint32_t done, uint32_t total);

// Returns true when <dataWinPath>.bsq exists, matches the WAD, and a full bake
// pass has finished (BakedTexturesHeader.complete).
bool TextureBaker_isComplete(const char* dataWinPath);

// Bakes every not-yet-baked QOI texture page into outputPath.
// Existing valid entries are kept, so an interrupted bake resumes.
// progress/userData may be nullptr.
// Returns 0 on success.
int TextureBaker_bake(const char* dataWinPath, const char* outputPath,
                      TextureBakerProgressFn progress, void* userData);

#endif /* _BS_TEXTURE_BAKER_H_ */
