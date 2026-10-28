#include "texture_baker.h"

#include "data_win.h"
#include "image/image_decoder.h"
#include "stdio_compat.h"
#include "utils.h"

#include <stdlib.h>
#include "string_compat.h"

static const char kBakedMagic[BAKED_TEXTURES_MAGIC_LEN] = { 'B', 'S', 'Q', 'O', 'I', '1', 0, 0 };

static long getFileSize(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size;
}

static bool headerIsValid(const BakedTexturesHeader* header, uint64_t wadSize) {
    return memcmp(header->magic, kBakedMagic, BAKED_TEXTURES_MAGIC_LEN) == 0 &&
           header->version == BAKED_TEXTURES_VERSION &&
           header->format == BAKED_TEXTURES_FORMAT_QOI &&
           header->entrySize == (uint32_t) sizeof(BakedTextureEntry) &&
           header->sourceSize == wadSize &&
           header->pageCount <= 65536;
}

static char* makeSidecarPath(const char* dataWinPath) {
    size_t len = strlen(dataWinPath) + 5;
    char* path = (char *) safeMalloc(len);
    snprintf(path, len, "%s.bsq", dataWinPath);
    return path;
}

bool TextureBaker_isComplete(const char* dataWinPath) {
    if (dataWinPath == nullptr) return false;

    long wadSize = getFileSize(dataWinPath);
    if (wadSize <= 0) return false;

    char* path = makeSidecarPath(dataWinPath);
    FILE* f = fopen(path, "rb");
    free(path);
    if (f == nullptr) return false;

    BakedTexturesHeader header;
    bool complete = fread(&header, sizeof(header), 1, f) == 1 &&
                    headerIsValid(&header, (uint64_t) wadSize) &&
                    header.complete != 0;
    fclose(f);
    return complete;
}

// ===[ Sidecar writer ]===

typedef struct {
    FILE* file;
    BakedTextureEntry* entries;
    uint32_t pageCount;
    size_t appendOffset;
} SidecarUpdate;

static bool sidecarOpen(SidecarUpdate* sidecar, const char* path, uint64_t wadSize, uint32_t pageCount) {
    memset(sidecar, 0, sizeof(*sidecar));
    sidecar->pageCount = pageCount;
    sidecar->entries = (BakedTextureEntry*) safeCalloc(pageCount > 0 ? pageCount : 1, sizeof(BakedTextureEntry));

    FILE* f = fopen(path, "r+b");
    if (f != nullptr) {
        BakedTexturesHeader header;
        if (fread(&header, sizeof(header), 1, f) == 1 &&
            headerIsValid(&header, wadSize) &&
            header.pageCount == pageCount &&
            fread(sidecar->entries, sizeof(BakedTextureEntry), pageCount, f) == pageCount) {
            long end = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
            if (end >= 0) {
                sidecar->file = f;
                sidecar->appendOffset = (size_t) end;
                return true;
            }
        }
        fclose(f);
    }

    f = fopen(path, "w+b");
    if (f == nullptr) {
        free(sidecar->entries);
        sidecar->entries = nullptr;
        return false;
    }

    BakedTexturesHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, kBakedMagic, BAKED_TEXTURES_MAGIC_LEN);
    header.version = BAKED_TEXTURES_VERSION;
    header.format = BAKED_TEXTURES_FORMAT_QOI;
    header.pageCount = pageCount;
    header.entrySize = (uint32_t) sizeof(BakedTextureEntry);
    header.sourceSize = wadSize;
    header.complete = 0;

    memset(sidecar->entries, 0, (size_t) pageCount * sizeof(BakedTextureEntry));

    if (fwrite(&header, sizeof(header), 1, f) != 1 ||
        fwrite(sidecar->entries, sizeof(BakedTextureEntry), pageCount, f) != pageCount) {
        fclose(f);
        free(sidecar->entries);
        sidecar->entries = nullptr;
        return false;
    }

    fflush(f);
    sidecar->file = f;
    sidecar->appendOffset = sizeof(header) + (size_t) pageCount * sizeof(BakedTextureEntry);
    return true;
}

static bool sidecarWrite(SidecarUpdate* sidecar, uint32_t pageId, const uint8_t* data, size_t size) {
    if (pageId >= sidecar->pageCount || size == 0 || size > UINT32_MAX) return false;

    size_t offset = sidecar->appendOffset;
    while ((offset & 3) != 0) offset++;

    BakedTextureEntry entry;
    entry.offset = (uint64_t) offset;
    entry.size = (uint32_t) size;
    entry.present = 1;

    long indexPosition = (long) (sizeof(BakedTexturesHeader) + (size_t) pageId * sizeof(BakedTextureEntry));

    bool ok =
        fseek(sidecar->file, (long) offset, SEEK_SET) == 0 &&
        fwrite(data, 1, size, sidecar->file) == size &&
        fseek(sidecar->file, indexPosition, SEEK_SET) == 0 &&
        fwrite(&entry, sizeof(entry), 1, sidecar->file) == 1 &&
        fseek(sidecar->file, 0, SEEK_END) == 0;

    if (!ok) return false;

    fflush(sidecar->file);
    sidecar->entries[pageId] = entry;
    sidecar->appendOffset = offset + size;
    return true;
}

// Writes one page as: BakedPageHeader, one QoiCheckpoint per strip, then the
// plain "fioq" data. The checkpoints let the runtime decode a single strip of
// the page instead of all of it, which is what keeps big-room texture memory
// down. Falls back to a flat (whole-page) blob when checkpoints cannot be built.
static bool writeBakedPage(SidecarUpdate* sidecar, uint32_t pageId, const uint8_t* qoi, size_t qoiSize) {
    int width = 0, height = 0;
    int stripRows = QOI_STRIP_ROWS;

    if (qoiSize < 12) return false;
    width = qoi[4] | (qoi[5] << 8);
    height = qoi[6] | (qoi[7] << 8);
    if (width <= 0 || height <= 0) {
        return sidecarWrite(sidecar, pageId, qoi, qoiSize);
    }

    int maxStrips = (height + stripRows - 1) / stripRows;
    QoiCheckpoint* checkpoints = (QoiCheckpoint*) safeCalloc((size_t) maxStrips, sizeof(QoiCheckpoint));

    int builtW = 0, builtH = 0, builtStrips = 0;
    bool built = ImageDecoder_buildCheckpoints(qoi, qoiSize, stripRows, checkpoints, maxStrips,
                                               &builtW, &builtH, &builtStrips);
    if (!built || builtStrips <= 0) {
        free(checkpoints);
        return sidecarWrite(sidecar, pageId, qoi, qoiSize);
    }

    size_t headerBytes = sizeof(BakedPageHeader);
    size_t checkpointBytes = (size_t) builtStrips * sizeof(QoiCheckpoint);
    size_t total = headerBytes + checkpointBytes + qoiSize;

    uint8_t* blob = (uint8_t*) safeMalloc(total);
    BakedPageHeader* page = (BakedPageHeader*) blob;
    memcpy(page->magic, BAKED_PAGE_MAGIC, 4);
    page->version = BAKED_PAGE_VERSION;
    page->stripRows = (uint16_t) stripRows;
    page->stripCount = (uint32_t) builtStrips;
    page->qoiSize = (uint32_t) qoiSize;
    page->qoiOffset = (uint32_t) (headerBytes + checkpointBytes);
    page->width = (uint32_t) builtW;
    page->height = (uint32_t) builtH;
    page->reserved = 0;

    memcpy(blob + headerBytes, checkpoints, checkpointBytes);
    memcpy(blob + page->qoiOffset, qoi, qoiSize);

    bool ok = sidecarWrite(sidecar, pageId, blob, total);

    free(blob);
    free(checkpoints);
    return ok;
}

static void sidecarClose(SidecarUpdate* sidecar, bool complete) {
    if (sidecar->file != nullptr) {
        BakedTexturesHeader header;
        if (fseek(sidecar->file, 0, SEEK_SET) == 0 && fread(&header, sizeof(header), 1, sidecar->file) == 1) {
            header.complete = complete ? 1u : 0u;
            if (fseek(sidecar->file, 0, SEEK_SET) == 0) {
                fwrite(&header, sizeof(header), 1, sidecar->file);
            }
        }
        fflush(sidecar->file);
        fclose(sidecar->file);
        sidecar->file = nullptr;
    }
    free(sidecar->entries);
    sidecar->entries = nullptr;
}

// ===[ Public API ]===

int TextureBaker_bake(const char* dataWinPath, const char* outputPath,
                      TextureBakerProgressFn progress, void* userData) {
    long wadSize = getFileSize(dataWinPath);
    if (wadSize <= 0) {
        logError("TextureBaker: cannot open %s\n", dataWinPath);
        return 1;
    }

    // Show the loading screen as early as possible.
    if (progress) progress(userData, 0, 0);

    // Map the WAD so baking stays within the handheld's RAM budget. Only the
    // chunks needed for TXTR metadata/decoding and STRG are parsed.
    DataWinParserOptions options = {0};
    options.parseGen8 = true;
    options.parseStrg = true;
    options.parseTxtr = true;
    options.loadType = DATAWINLOADTYPE_MAP_FILE;
    options.disableBakedTextures = true;
    options.autoBakeTextures = false;

    logInfo("TextureBaker: reading %s (%ld MB)\n", dataWinPath, wadSize / (1024 * 1024));
    DataWin* dw = DataWin_parse(dataWinPath, options);

    uint32_t pageCount = dw->txtr.count;
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);

    SidecarUpdate sidecar;
    if (!sidecarOpen(&sidecar, outputPath, (uint64_t) wadSize, pageCount)) {
        logError("TextureBaker: cannot open %s for writing\n", outputPath);
        DataWin_free(dw);
        return 1;
    }

    // Pass 1: find bakeable pages that are not present yet.
    uint32_t* missing = pageCount > 0 ? (uint32_t*) safeMalloc((size_t) pageCount * sizeof(uint32_t)) : nullptr;
    uint32_t missingCount = 0;

    for (uint32_t i = 0; i < pageCount; i++) {
        Texture* texture = &dw->txtr.textures[i];
        if (!texture->present || texture->blobOffset == 0 || texture->blobSize == 0) continue;

        bool loadedHere = false;
        if (texture->blobData == nullptr) {
            DataWin_loadTxtrIfNeeded(dw, i);
            loadedHere = true;
        }
        if (texture->blobData == nullptr || texture->blobSize < 4) {
            if (loadedHere && !texture->mapped) {
                free(texture->blobData);
                texture->blobData = nullptr;
            }
            continue;
        }

        const uint8_t* blob = texture->blobData;
        bool bakeable =
            (blob[0] == '2' && blob[1] == 'z' && blob[2] == 'o' && blob[3] == 'q') ||
            (blob[0] == 'f' && blob[1] == 'i' && blob[2] == 'o' && blob[3] == 'q');

        if (bakeable) {
            BakedTextureEntry* entry = &sidecar.entries[i];
            bool presentValid = entry->present && entry->size >= 12 &&
                                entry->offset <= (uint64_t) sidecar.appendOffset &&
                                entry->size <= (uint64_t) sidecar.appendOffset - entry->offset;
            if (presentValid) {
                uint8_t magic[5];
                presentValid = fseek(sidecar.file, (long) entry->offset, SEEK_SET) == 0 &&
                               fread(magic, 1, 5, sidecar.file) >= 4 &&
                               ((magic[0] == 'f' && magic[1] == 'i' && magic[2] == 'o' && magic[3] == 'q') ||
                                (magic[0] == 'S' && magic[1] == 'Q' && magic[2] == 'P' && magic[3] == '1'));
            }
            if (!presentValid) {
                missing[missingCount++] = i;
            }
        }

        if (loadedHere && !texture->mapped) {
            free(texture->blobData);
            texture->blobData = nullptr;
        }
    }

    uint32_t total = missingCount;
    uint32_t done = 0;
    uint64_t totalBytes = 0;
    bool ok = true;

    if (progress) progress(userData, 0, total);

    // Pass 2: bake.
    for (uint32_t k = 0; k < total; k++) {
        uint32_t i = missing[k];
        Texture* texture = &dw->txtr.textures[i];

        bool loadedHere = false;
        if (texture->blobData == nullptr) {
            DataWin_loadTxtrIfNeeded(dw, i);
            loadedHere = true;
        }
        if (texture->blobData == nullptr) {
            ok = false;
            break;
        }

        const uint8_t* blob = texture->blobData;
        size_t blobSize = (size_t) texture->blobSize;
        uint8_t* ownedQoi = nullptr;
        size_t qoiSize = 0;

        if (blob[0] == 'f' && blob[1] == 'i' && blob[2] == 'o' && blob[3] == 'q') {
            qoiSize = blobSize;
        } else {
            ownedQoi = ImageDecoder_extractQoi(blob, blobSize, gm2022_5, &qoiSize);
            if (ownedQoi == nullptr) {
                free(ownedQoi);
                if (loadedHere && !texture->mapped) {
                    free(texture->blobData);
                    texture->blobData = nullptr;
                }
                ok = false;
                break;
            }
            blob = ownedQoi;
        }

        if (!writeBakedPage(&sidecar, i, blob, qoiSize)) {
            ok = false;
        } else {
            totalBytes += qoiSize;
            done++;
        }

        free(ownedQoi);
        if (loadedHere && !texture->mapped) {
            free(texture->blobData);
            texture->blobData = nullptr;
        }
        if (!ok) break;

        if (progress) progress(userData, done, total);
        if ((done % 8) == 0 || done == total) {
            logInfo("TextureBaker: %u/%u pages\n", done, total);
        }
    }

    sidecarClose(&sidecar, ok);
    free(missing);

    if (!ok) {
        logError("TextureBaker: failed while writing %s\n", outputPath);
        DataWin_free(dw);
        return 1;
    }

    logInfo("TextureBaker: prepared %u/%u pages (%llu MB) in %s\n",
            done, total, (unsigned long long) (totalBytes / (1024 * 1024)), outputPath);

    DataWin_free(dw);
    return 0;
}
