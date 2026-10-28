#ifndef _BS_BAKED_TEXTURES_H_
#define _BS_BAKED_TEXTURES_H_

#include "common.h"
#include <stdint.h>
#include <stddef.h>

#include "image/image_decoder.h"

// Sidecar written by --bake-textures and mapped by the runner.
//
// Each TXTR page is stored as a "page blob": a small header, one QOI decoder
// checkpoint per strip of QOI_STRIP_ROWS rows, then the plain "fioq" QOI data.
// Storing the checkpoints lets the runtime decode any horizontal strip of the
// page independently, so a room only has to materialise the parts of a page it
// actually draws (see swEnsureTextureRegion). Legacy sidecars that hold a bare
// QOI blob per page are still accepted and simply decode whole pages.

#define BAKED_TEXTURES_MAGIC "BSQOI1"
#define BAKED_TEXTURES_MAGIC_LEN 8
#define BAKED_TEXTURES_VERSION 3u
#define BAKED_TEXTURES_FORMAT_QOI 1u

#define BAKED_PAGE_MAGIC "SQP1"
#define BAKED_PAGE_VERSION 1u

typedef struct {
    uint64_t offset;  // absolute offset of the page blob inside the sidecar
    uint32_t size;    // blob size in bytes (0 = not baked)
    uint32_t present; // 1 when this page has a blob
} BakedTextureEntry;

// Start of a page blob. Followed by `stripCount` QoiCheckpoint records, then
// the "fioq" data at `qoiOffset`.
typedef struct {
    char magic[4];        // BAKED_PAGE_MAGIC
    uint16_t version;     // BAKED_PAGE_VERSION
    uint16_t stripRows;   // rows covered by one checkpoint
    uint32_t stripCount;  // number of checkpoints that follow this header
    uint32_t qoiSize;
    uint32_t qoiOffset;   // from the start of this page blob
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
} BakedPageHeader; // 32 bytes, keeps the checkpoints 4-byte aligned

typedef struct {
    char magic[BAKED_TEXTURES_MAGIC_LEN]; // "BSQOI1\0\0"
    uint32_t version;                     // BAKED_TEXTURES_VERSION
    uint32_t format;                      // BAKED_TEXTURES_FORMAT_QOI
    uint32_t pageCount;
    uint32_t entrySize;                   // sizeof(BakedTextureEntry)
    uint64_t sourceSize;                  // data.win size, used to reject stale sidecars
    uint32_t complete;                    // 1 once a full bake pass has finished
    uint32_t reserved;
} BakedTexturesHeader;

#endif /* _BS_BAKED_TEXTURES_H_ */
