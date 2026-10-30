#include "image_decoder.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"

#include "log.h"
#include "utils.h"

#ifdef USE_WUFFS_IMGDEC
#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__BZIP2
#include "wuffs-v0.4.c"
#elif defined(USE_STB_IMGDEC)
// fallbacks for older compilers
#include "stb_image.h"
#include <bzlib.h>
#endif

#define QOI_HEADER_SIZE 12
#define COMPRESSED_QOI_HEADER_SIZE_OLD 8
#define COMPRESSED_QOI_HEADER_SIZE_NEW 12

// Sign-extend the low "bits" bits of "val" to an 8-bit two's-complement value.
static inline uint8_t signExtend(uint32_t val, int bits) {
    uint32_t mask = 1U << (bits - 1);
    return (uint8_t)((val ^ mask) - mask);
}

// Decompresses a raw BZip2 stream into `destination`. Both decompressors the build can
// use are behind this one call, so the strip decoder and the whole-blob extractor do not
// care which is compiled in.
static bool decompressBz2(const uint8_t* source, size_t sourceSize, uint8_t* destination,
                          size_t capacity, size_t* outLength) {
    if (source == nullptr || destination == nullptr || sourceSize == 0 || capacity == 0) return false;
#ifdef USE_WUFFS_IMGDEC
    wuffs_bzip2__decoder dec;
    wuffs_base__status status = wuffs_bzip2__decoder__initialize(&dec, sizeof(dec), WUFFS_VERSION, 0);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer dstBuf = wuffs_base__ptr_u8__writer(destination, capacity);
    wuffs_base__io_buffer srcBuf = wuffs_base__ptr_u8__reader((uint8_t*) source, sourceSize, true);

    wuffs_base__range_ii_u64 workbufLen = wuffs_bzip2__decoder__workbuf_len(&dec);
    wuffs_base__slice_u8 workbuf = wuffs_base__empty_slice_u8();
    if (workbufLen.max_incl > 0) {
        uint8_t* workPtr = (uint8_t*) malloc(workbufLen.max_incl);
        if (workPtr == nullptr) return false;
        workbuf = wuffs_base__make_slice_u8(workPtr, workbufLen.max_incl);
    }

    status = wuffs_bzip2__decoder__transform_io(&dec, &dstBuf, &srcBuf, workbuf);
    if (workbuf.ptr != nullptr) free(workbuf.ptr);
    if (!wuffs_base__status__is_ok(&status)) {
        logWarn("ImageDecoder: BZ2 decompress failed\n");
        return false;
    }
    if (outLength != nullptr) *outLength = dstBuf.meta.wi;
    return true;
#elif defined(USE_STB_IMGDEC)
    unsigned int destLen = (unsigned int) capacity;
    int rc = BZ2_bzBuffToBuffDecompress((char*) destination, &destLen, (char*) source,
                                        (unsigned int) sourceSize, 0, 0);
    if (rc != BZ_OK) {
        logWarn("ImageDecoder: BZ2 decompress failed (rc=%d)\n", rc);
        return false;
    }
    if (outLength != nullptr) *outLength = (size_t) destLen;
    return true;
#else
    (void) source; (void) sourceSize; (void) destination; (void) capacity; (void) outLength;
    return false;
#endif
}

// ===[ Streaming QOI decoder ]===
//
// The decoder is a resumable state machine so a page can be decoded in strips
// (see QoiCheckpoint in the header). "runRemaining" counts the pixels still to
// be emitted with the current colour, which makes the state valid at any pixel
// boundary - including the middle of a long QOI_RUN, which is what makes strip
// checkpoints possible at all.

typedef struct {
    const uint8_t* pixelData;
    size_t pixelDataSize;
    size_t pos;
    uint32_t runRemaining; // pixels still to emit with (r,g,b,a); 0 = read the next opcode
    uint8_t r, g, b, a;
    uint8_t index[64 * 4];
} QoiStream;

typedef enum {
    QOI_OP_OK = 0,
    QOI_OP_END,
    QOI_OP_ERROR
} QoiOpResult;

// Sets up the stream for an already-validated plain "fioq" blob.
static void qoiStreamReset(QoiStream* s, const uint8_t* data, size_t dataSize) {
    (void) dataSize;
    uint32_t length = (uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

    memset(s, 0, sizeof(*s));
    s->pixelData = data + QOI_HEADER_SIZE;
    s->pixelDataSize = length;
    s->a = 255;
}

// Validates the header and sets up the stream. Returns false for non-QOI data.
static bool qoiStreamInit(QoiStream* s, const uint8_t* data, size_t dataSize, int* outW, int* outH) {
    if (QOI_HEADER_SIZE > dataSize) return false;
    if (data[0] != 'f' || data[1] != 'i' || data[2] != 'o' || data[3] != 'q') return false;

    int width = data[4] | (data[5] << 8);
    int height = data[6] | (data[7] << 8);
    uint32_t length = (uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

    if (QOI_HEADER_SIZE + (size_t) length > dataSize) return false;
    // 16384 is well above GameMaker's largest texture page; rejecting more also
    // prevents size_t overflow in the output allocation on 32-bit targets.
    if (0 >= width || 0 >= height || width > 16384 || height > 16384) return false;

    qoiStreamReset(s, data, dataSize);
    if (outW) *outW = width;
    if (outH) *outH = height;
    return true;
}

// Reads the next opcode and updates the colour state. On a run opcode it also
// sets runRemaining to the total pixels that colour covers (the current one
// included).
static QoiOpResult qoiStreamNextOpcode(QoiStream* s) {
    if (s->pixelDataSize <= s->pos) return QOI_OP_END;

    const uint8_t* const pixelData = s->pixelData;
    const size_t pixelDataSize = s->pixelDataSize;
    size_t pos = s->pos;
    uint8_t r = s->r, g = s->g, b = s->b, a = s->a;

    uint8_t b1 = pixelData[pos++];

    if ((b1 & 0xC0) == 0x00) {
        // QOI_INDEX
        int indexPos = (b1 & 0x3F) << 2;
        r = s->index[indexPos];
        g = s->index[indexPos + 1];
        b = s->index[indexPos + 2];
        a = s->index[indexPos + 3];
        s->runRemaining = 1;
    } else if ((b1 & 0xE0) == 0x40) {
        // QOI_RUN_8
        s->runRemaining = (uint32_t) (b1 & 0x1F) + 1;
    } else if ((b1 & 0xE0) == 0x60) {
        // QOI_RUN_16
        if (pixelDataSize <= pos) return QOI_OP_ERROR;
        uint8_t b2 = pixelData[pos++];
        s->runRemaining = (uint32_t) ((((b1 & 0x1F) << 8) | b2) + 32) + 1;
    } else if ((b1 & 0xC0) == 0x80) {
        // QOI_DIFF_8 (2-2-2 signed deltas on r,g,b)
        r += signExtend((b1 >> 4) & 3, 2);
        g += signExtend((b1 >> 2) & 3, 2);
        b += signExtend(b1 & 3, 2);
        s->runRemaining = 1;
    } else if ((b1 & 0xE0) == 0xC0) {
        // QOI_DIFF_16 (5-4-4 signed deltas on r,g,b)
        if (pixelDataSize <= pos) return QOI_OP_ERROR;
        uint8_t b2 = pixelData[pos++];
        uint32_t merged = ((uint32_t)b1 << 8) | b2;
        r += signExtend((merged >> 8) & 0x1F, 5);
        g += signExtend((merged >> 4) & 0x0F, 4);
        b += signExtend(merged & 0x0F, 4);
        s->runRemaining = 1;
    } else if ((b1 & 0xF0) == 0xE0) {
        // QOI_DIFF_24 (5-5-5-5 signed deltas on r,g,b,a)
        if (pixelDataSize <= pos + 1) return QOI_OP_ERROR;
        uint8_t b2 = pixelData[pos++];
        uint8_t b3 = pixelData[pos++];
        uint32_t merged = ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
        r += signExtend((merged >> 15) & 0x1F, 5);
        g += signExtend((merged >> 10) & 0x1F, 5);
        b += signExtend((merged >> 5) & 0x1F, 5);
        a += signExtend(merged & 0x1F, 5);
        s->runRemaining = 1;
    } else if ((b1 & 0xF0) == 0xF0) {
        // QOI_COLOR (per-channel raw bytes, only those with set bit flag)
        if (b1 & 8) { if (pixelDataSize <= pos) return QOI_OP_ERROR; r = pixelData[pos++]; }
        if (b1 & 4) { if (pixelDataSize <= pos) return QOI_OP_ERROR; g = pixelData[pos++]; }
        if (b1 & 2) { if (pixelDataSize <= pos) return QOI_OP_ERROR; b = pixelData[pos++]; }
        if (b1 & 1) { if (pixelDataSize <= pos) return QOI_OP_ERROR; a = pixelData[pos++]; }
        s->runRemaining = 1;
    }

    s->pos = pos;
    s->r = r;
    s->g = g;
    s->b = b;
    s->a = a;

    int indexPos2 = ((r ^ g ^ b ^ a) & 63) << 2;
    s->index[indexPos2] = r;
    s->index[indexPos2 + 1] = g;
    s->index[indexPos2 + 2] = b;
    s->index[indexPos2 + 3] = a;

    return QOI_OP_OK;
}

// Emits `pixels` pixels of the current colour run into dst (RGB565 or RGBA8).
static bool qoiStreamFill(QoiStream* s, void* dst, size_t pixels, bool to565) {
    size_t emitted = 0;

    while (emitted < pixels) {
        if (s->runRemaining == 0) {
            QoiOpResult result = qoiStreamNextOpcode(s);
            if (result == QOI_OP_ERROR) return false;
            // Truncated data: the original decoder kept emitting the current
            // colour for the remaining pixels, so keep doing that.
            if (result == QOI_OP_END) s->runRemaining = 1;
        }

        size_t emit = s->runRemaining;
        if (emit > pixels - emitted) emit = pixels - emitted;

        if (to565) {
            // Match swrConvertPixelTexture()/abgr8888_to_rgb1555: RGB555 + alpha bit.
            uint16_t px = (uint16_t) ((s->b >> 3) | ((s->g >> 3) << 5) | ((s->r >> 3) << 10) | ((s->a >> 7) << 15));
            uint16_t* out = (uint16_t*) dst + emitted;
            size_t left = emit;
            while (left >= 4) {
                out[0] = px;
                out[1] = px;
                out[2] = px;
                out[3] = px;
                out += 4;
                left -= 4;
            }
            while (left > 0) {
                *out++ = px;
                left--;
            }
        } else {
            uint32_t px = (uint32_t) s->r | ((uint32_t) s->g << 8) | ((uint32_t) s->b << 16) | ((uint32_t) s->a << 24);
            uint8_t* out = (uint8_t*) dst + emitted * 4;
            size_t left = emit;
            while (left >= 4) {
                memcpy(out, &px, 4);
                memcpy(out + 4, &px, 4);
                memcpy(out + 8, &px, 4);
                memcpy(out + 12, &px, 4);
                out += 16;
                left -= 4;
            }
            while (left > 0) {
                memcpy(out, &px, 4);
                out += 4;
                left--;
            }
        }

        s->runRemaining -= (uint32_t) emit;
        emitted += emit;
    }

    return true;
}

// Decodes GameMaker's custom QOI format (ported from UndertaleModTool's QoiConverter).
// When to565 is true, writes 16-bit RGB1555 pixels instead of RGBA8, avoiding a
// full-size RGBA intermediate buffer on memory-constrained devices.
static void* decodeQoiCore(const uint8_t* data, size_t dataSize, int* outW, int* outH, bool to565) {
    if (QOI_HEADER_SIZE > dataSize) return nullptr;
    if (data[0] != 'f' || data[1] != 'i' || data[2] != 'o' || data[3] != 'q') return nullptr;

    int width = data[4] | (data[5] << 8);
    int height = data[6] | (data[7] << 8);
    uint32_t length = (uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

    if (QOI_HEADER_SIZE + (size_t) length > dataSize) return nullptr;
    // 16384 is well above GameMaker's largest texture page; rejecting more also
    // prevents size_t overflow in the output allocation on 32-bit targets.
    if (0 >= width || 0 >= height || width > 16384 || height > 16384) return nullptr;

    size_t pixelCount = (size_t)width * (size_t)height;
    void* out = malloc(pixelCount * (to565 ? 2u : 4u));
    if (!out) return nullptr;

    QoiStream stream;
    qoiStreamReset(&stream, data, dataSize);
    if (!qoiStreamFill(&stream, out, pixelCount, to565)) {
        free(out);
        return nullptr;
    }

    *outW = width;
    *outH = height;
    return out;
}

static uint8_t* decodeQoi(const uint8_t* data, size_t dataSize, int* outW, int* outH) {
    return (uint8_t*) decodeQoiCore(data, dataSize, outW, outH, false);
}

// Decodes BZip2-compressed custom QOI. The blob layout is:
//   bytes 0..3 = "2zoq" magic
//   bytes 4..5 = width (LE uint16)
//   bytes 6..7 = height (LE uint16)
//   bytes 8..11 = uncompressed BZ2 length (LE uint32) -- ONLY when gm2022_5 is true
//   bytes 8.. (or 12.. if gm2022_5) = raw BZip2 stream, which decompresses into a full "fioq" QOI file.
static void* decodeBz2QoiCore(const uint8_t* blob, size_t blobSize, bool gm2022_5, int* outW, int* outH, bool to565) {
    size_t headerSize = gm2022_5 ? COMPRESSED_QOI_HEADER_SIZE_NEW : COMPRESSED_QOI_HEADER_SIZE_OLD;
    if (headerSize > blobSize) return nullptr;

    int width = blob[4] | (blob[5] << 8);
    int height = blob[6] | (blob[7] << 8);
    if (0 >= width || 0 >= height) return nullptr;

    // Upper bound on decompressed QOI: header size + width*height*5 pixel data.
    size_t uncompressedCapacity = QOI_HEADER_SIZE + (size_t) width * (size_t) height * 5;
    // GameMaker 2022.5+ stores the exact decompressed size in the header. Using it
    // instead of the worst-case bound saves ~16 MB of transient RAM per 2048x2048 page.
    if (gm2022_5) {
        uint32_t declared = (uint32_t) blob[8] | ((uint32_t) blob[9] << 8) |
                            ((uint32_t) blob[10] << 16) | ((uint32_t) blob[11] << 24);
        if (declared >= QOI_HEADER_SIZE && (size_t) declared <= uncompressedCapacity) {
            uncompressedCapacity = (size_t) declared;
        }
    }
    uint8_t* uncompressed = (uint8_t*) malloc(uncompressedCapacity);
    if (!uncompressed) return nullptr;

    size_t destLen = 0;
    if (!decompressBz2(blob + headerSize, blobSize - headerSize, uncompressed, uncompressedCapacity, &destLen)) {
        free(uncompressed);
        return nullptr;
    }

    void* result = decodeQoiCore(uncompressed, destLen, outW, outH, to565);
    free(uncompressed);
    return result;
}

static uint8_t* decodeBz2Qoi(const uint8_t* blob, size_t blobSize, bool gm2022_5, int* outW, int* outH) {
    return (uint8_t*) decodeBz2QoiCore(blob, blobSize, gm2022_5, outW, outH, false);
}
uint8_t* ImageDecoder_extractQoi(const uint8_t* blob, size_t blobSize, bool gm2022_5, size_t* outSize) {
    if (!blob || blobSize < 4) return nullptr;

    if (blob[0] == 'f' && blob[1] == 'i' && blob[2] == 'o' && blob[3] == 'q') {
        uint8_t* copy = (uint8_t*) malloc(blobSize);
        if (!copy) return nullptr;
        memcpy(copy, blob, blobSize);
        if (outSize) *outSize = blobSize;
        return copy;
    }

    if (!(blob[0] == '2' && blob[1] == 'z' && blob[2] == 'o' && blob[3] == 'q')) {
        // PNG or unknown; not useful for a QOI sidecar.
        return nullptr;
    }

    size_t headerSize = gm2022_5 ? COMPRESSED_QOI_HEADER_SIZE_NEW : COMPRESSED_QOI_HEADER_SIZE_OLD;
    if (headerSize > blobSize) return nullptr;

    int width = blob[4] | (blob[5] << 8);
    int height = blob[6] | (blob[7] << 8);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return nullptr;

    size_t capacity = QOI_HEADER_SIZE + (size_t) width * (size_t) height * 5;
    if (gm2022_5) {
        uint32_t declared = (uint32_t) blob[8] | ((uint32_t) blob[9] << 8) |
                            ((uint32_t) blob[10] << 16) | ((uint32_t) blob[11] << 24);
        if (declared >= QOI_HEADER_SIZE && (size_t) declared <= capacity) {
            capacity = (size_t) declared;
        }
    }

    uint8_t* uncompressed = (uint8_t*) malloc(capacity);
    if (!uncompressed) return nullptr;

    size_t destLen = 0;
    if (!decompressBz2(blob + headerSize, blobSize - headerSize, uncompressed, capacity, &destLen) ||
        destLen < QOI_HEADER_SIZE ||
        !(uncompressed[0] == 'f' && uncompressed[1] == 'i' && uncompressed[2] == 'o' && uncompressed[3] == 'q')) {
        free(uncompressed);
        return nullptr;
    }

    if (outSize) *outSize = (size_t) destLen;
    return uncompressed;
}

// ===[ QOI encoder ]===
//
// Writes the same "fioq" layout the streaming decoder above reads:
//   bytes 0..3  = "fioq"
//   bytes 4..5  = width  (LE uint16)
//   bytes 6..7  = height (LE uint16)
//   bytes 8..11 = length of the pixel data that follows (LE uint32)
//
// Only the opcodes the decoder implements are emitted (index, run, 8-bit diff,
// 16-bit diff and literal colour), and the index hash matches the decoder's
// ((r ^ g ^ b ^ a) & 63) rather than the reference QOI hash, so the output can be
// fed straight back into it.

static void qoiEmitRun(uint8_t* out, size_t* pos, uint32_t run) {
    while (run > 0) {
        if (run <= 32) {
            out[(*pos)++] = (uint8_t) (0x40u | (run - 1u));
            return;
        }
        uint32_t chunk = run > 8224u ? 8224u : run;
        uint32_t value = chunk - 33u;
        out[(*pos)++] = (uint8_t) (0x60u | ((value >> 8) & 0x1Fu));
        out[(*pos)++] = (uint8_t) (value & 0xFFu);
        run -= chunk;
    }
}

uint8_t* ImageDecoder_encodeQoi(const uint8_t* rgba, int width, int height, size_t* outSize) {
    if (rgba == nullptr || width <= 0 || height <= 0) return nullptr;
    if (width > 16384 || height > 16384) return nullptr;

    size_t pixels = (size_t) width * (size_t) height;
    // Worst case is one literal colour per pixel: 5 bytes each, plus the header.
    size_t capacity = QOI_HEADER_SIZE + pixels * 5u + 16u;
    uint8_t* out = (uint8_t*) malloc(capacity);
    if (out == nullptr) return nullptr;

    out[0] = 'f';
    out[1] = 'i';
    out[2] = 'o';
    out[3] = 'q';
    out[4] = (uint8_t) (width & 0xFF);
    out[5] = (uint8_t) ((width >> 8) & 0xFF);
    out[6] = (uint8_t) (height & 0xFF);
    out[7] = (uint8_t) ((height >> 8) & 0xFF);

    uint8_t index[64 * 4];
    memset(index, 0, sizeof(index));

    uint8_t prevR = 0, prevG = 0, prevB = 0, prevA = 255;
    uint32_t run = 0;
    size_t pos = QOI_HEADER_SIZE;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];

        if (r == prevR && g == prevG && b == prevB && a == prevA) {
            run++;
            continue;
        }
        if (run > 0) {
            qoiEmitRun(out, &pos, run);
            run = 0;
        }

        size_t hash = (size_t) ((r ^ g ^ b ^ a) & 63) * 4;
        if (index[hash] == r && index[hash + 1] == g && index[hash + 2] == b && index[hash + 3] == a) {
            out[pos++] = (uint8_t) (hash >> 2);
        } else {
            index[hash] = r;
            index[hash + 1] = g;
            index[hash + 2] = b;
            index[hash + 3] = a;

            int dr = (int) r - (int) prevR;
            int dg = (int) g - (int) prevG;
            int db = (int) b - (int) prevB;
            int da = (int) a - (int) prevA;

            if (da == 0 && dr > -3 && dr < 2 && dg > -3 && dg < 2 && db > -3 && db < 2) {
                out[pos++] = (uint8_t) (0x80 | ((dr & 3) << 4) | ((dg & 3) << 2) | (db & 3));
            } else if (da == 0 && dr > -17 && dr < 16 && dg > -9 && dg < 8 && db > -9 && db < 8) {
                // dr is a 5-bit field, dg and db are 4-bit fields on the wire.
                out[pos++] = (uint8_t) (0xC0 | (dr & 0x1F));
                out[pos++] = (uint8_t) (((dg & 0x0F) << 4) | (db & 0x0F));
            } else {
                size_t opcodeAt = pos++;
                uint8_t flags = 0;
                if (dr != 0) { flags |= 8; out[pos++] = r; }
                if (dg != 0) { flags |= 4; out[pos++] = g; }
                if (db != 0) { flags |= 2; out[pos++] = b; }
                if (da != 0) { flags |= 1; out[pos++] = a; }
                out[opcodeAt] = (uint8_t) (0xF0 | flags);
            }
        }

        prevR = r;
        prevG = g;
        prevB = b;
        prevA = a;
    }

    if (run > 0) qoiEmitRun(out, &pos, run);

    uint32_t pixelBytes = (uint32_t) (pos - QOI_HEADER_SIZE);
    out[8] = (uint8_t) (pixelBytes & 0xFF);
    out[9] = (uint8_t) ((pixelBytes >> 8) & 0xFF);
    out[10] = (uint8_t) ((pixelBytes >> 16) & 0xFF);
    out[11] = (uint8_t) ((pixelBytes >> 24) & 0xFF);

    if (outSize) *outSize = pos;
    return out;
}
bool ImageDecoder_probeDimensions(const uint8_t* blob, size_t blobSize, bool gm2022_5, int* outW, int* outH) {
    (void) gm2022_5;
    if (!blob || blobSize < 8) return false;

    int width = 0;
    int height = 0;

    if ((blob[0] == 'f' && blob[1] == 'i' && blob[2] == 'o' && blob[3] == 'q') ||
        (blob[0] == '2' && blob[1] == 'z' && blob[2] == 'o' && blob[3] == 'q')) {
        width = blob[4] | (blob[5] << 8);
        height = blob[6] | (blob[7] << 8);
    } else if (blobSize >= 24 &&
               blob[0] == 0x89 && blob[1] == 'P' && blob[2] == 'N' && blob[3] == 'G' &&
               blob[12] == 'I' && blob[13] == 'H' && blob[14] == 'D' && blob[15] == 'R') {
        width  = ((int) blob[16] << 24) | ((int) blob[17] << 16) | ((int) blob[18] << 8) | (int) blob[19];
        height = ((int) blob[20] << 24) | ((int) blob[21] << 16) | ((int) blob[22] << 8) | (int) blob[23];
    } else {
        return false;
    }

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
    if (outW) *outW = width;
    if (outH) *outH = height;
    return true;
}

uint8_t* ImageDecoder_decodeToRgba(const uint8_t* blob, size_t blobSize, bool gm2022_5, int* outW, int* outH) {
    if (4 > blobSize || !blob) return nullptr;

    // GameMaker custom QOI ("fioq")
    if (blob[0] == 'f' && blob[1] == 'i' && blob[2] == 'o' && blob[3] == 'q') {
        return decodeQoi(blob, blobSize, outW, outH);
    }

    // BZip2 + GameMaker custom QOI ("2zoq")
    if (blob[0] == '2' && blob[1] == 'z' && blob[2] == 'o' && blob[3] == 'q') {
        return decodeBz2Qoi(blob, blobSize, gm2022_5, outW, outH);
    }

#ifdef USE_WUFFS_IMGDEC
    wuffs_png__decoder dec;
    wuffs_base__status status = wuffs_png__decoder__initialize(&dec, sizeof(dec), WUFFS_VERSION, 0);
    if (!wuffs_base__status__is_ok(&status)) return nullptr;

    wuffs_base__image_decoder* imgDec = wuffs_png__decoder__upcast_as__wuffs_base__image_decoder(&dec);

    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader((uint8_t*)blob, blobSize, true);

    wuffs_base__image_config ic;
    status = wuffs_base__image_decoder__decode_image_config(imgDec, &ic, &src);
    if (!wuffs_base__status__is_ok(&status)) return nullptr;

    uint32_t w = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t h = wuffs_base__pixel_config__height(&ic.pixcfg);

    wuffs_base__pixel_config__set(
        &ic.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE,
        w, h
    );

    size_t pixBufSize = (size_t)w * (size_t)h * 4;
    uint8_t* pixels = (uint8_t*)safeMalloc(pixBufSize);
    if (!pixels) return nullptr;

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(pixels, pixBufSize));
    if (!wuffs_base__status__is_ok(&status)) {
        free(pixels);
        return nullptr;
    }

    wuffs_base__range_ii_u64 workbufLen = wuffs_base__image_decoder__workbuf_len(imgDec);
    wuffs_base__slice_u8 workbuf = wuffs_base__empty_slice_u8();
    if (workbufLen.max_incl > 0) {
        uint8_t* workPtr = (uint8_t*)safeMalloc(workbufLen.max_incl);
        if (workPtr == nullptr) {
            free(pixels);
            return nullptr;
        }
        workbuf = wuffs_base__make_slice_u8(workPtr, workbufLen.max_incl);
    }

    status = wuffs_base__image_decoder__decode_frame(imgDec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, workbuf, nullptr);

    if (workbuf.ptr) free(workbuf.ptr);

    if (!wuffs_base__status__is_ok(&status)) {
        free(pixels);
        return nullptr;
    }
#elif defined(USE_STB_IMGDEC)
    // PNG (or anything else stbi recognizes).
    int w, h, channels;
    uint8_t* pixels = stbi_load_from_memory(blob, (int) blobSize, &w, &h, &channels, 4);
    if (!pixels) return nullptr;
#else
    (void) blob; (void) blobSize; (void) gm2022_5;
    return nullptr;
#endif
    *outW = (int) w;
    *outH = (int) h;
    return pixels;
}

uint16_t* ImageDecoder_decodeToRgb565(const uint8_t* blob, size_t blobSize, bool gm2022_5, int* outW, int* outH) {
    if (4 > blobSize || !blob) return nullptr;

    if (blob[0] == 'f' && blob[1] == 'i' && blob[2] == 'o' && blob[3] == 'q') {
        return (uint16_t*) decodeQoiCore(blob, blobSize, outW, outH, true);
    }

    if (blob[0] == '2' && blob[1] == 'z' && blob[2] == 'o' && blob[3] == 'q') {
        return (uint16_t*) decodeBz2QoiCore(blob, blobSize, gm2022_5, outW, outH, true);
    }

    // PNG fallback: decode RGBA and convert. Only older games (WAD < 2022.5) use this.
    int w = 0, h = 0;
    uint8_t* pixels = (uint8_t*) ImageDecoder_decodeToRgba(blob, blobSize, gm2022_5, &w, &h);
    if (!pixels) return nullptr;

    size_t count = (size_t) w * (size_t) h;
    uint16_t* out = (uint16_t*) malloc(count * sizeof(uint16_t));
    if (!out) {
        free(pixels);
        return nullptr;
    }
    for (size_t i = 0; i < count; i++) {
        uint8_t r = pixels[i * 4];
        uint8_t g = pixels[i * 4 + 1];
        uint8_t b = pixels[i * 4 + 2];
        uint8_t a = pixels[i * 4 + 3];
        out[i] = (uint16_t) ((b >> 3) | ((g >> 3) << 5) | ((r >> 3) << 10) | ((a >> 7) << 15));
    }
    free(pixels);

    *outW = w;
    *outH = h;
    return out;
}

// ===[ Sub-page (strip) decoding ]===

bool ImageDecoder_buildCheckpoints(const uint8_t* blob, size_t blobSize,
                                   int stripRows, QoiCheckpoint* checkpoints, int checkpointCapacity,
                                   int* outW, int* outH, int* outStripCount) {
    if (blob == nullptr || checkpoints == nullptr || checkpointCapacity <= 0) return false;

    QoiStream stream;
    int width = 0, height = 0;
    if (!qoiStreamInit(&stream, blob, blobSize, &width, &height)) return false;

    if (stripRows <= 0) stripRows = QOI_STRIP_ROWS;
    if (stripRows > height) stripRows = height;

    int stripCount = (height + stripRows - 1) / stripRows;
    if (stripCount > checkpointCapacity) return false;

    // Reaching the next checkpoint requires running the decoder over the strip,
    // so decode into a scratch buffer we immediately discard.
    size_t scratchPixels = (size_t) width * (size_t) stripRows;
    uint16_t* scratch = (uint16_t*) malloc(scratchPixels * sizeof(uint16_t));
    if (scratch == nullptr) return false;

    bool ok = true;
    for (int strip = 0; strip < stripCount; strip++) {
        QoiCheckpoint* checkpoint = &checkpoints[strip];
        checkpoint->pos = (uint32_t) stream.pos;
        checkpoint->runRemaining = stream.runRemaining;
        checkpoint->rgba[0] = stream.r;
        checkpoint->rgba[1] = stream.g;
        checkpoint->rgba[2] = stream.b;
        checkpoint->rgba[3] = stream.a;
        memcpy(checkpoint->index, stream.index, sizeof(checkpoint->index));

        int firstRow = strip * stripRows;
        int rows = height - firstRow;
        if (rows > stripRows) rows = stripRows;

        if (!qoiStreamFill(&stream, scratch, (size_t) width * (size_t) rows, true)) {
            ok = false;
            break;
        }
    }

    free(scratch);
    if (!ok) return false;

    if (outW) *outW = width;
    if (outH) *outH = height;
    if (outStripCount) *outStripCount = stripCount;
    return true;
}

bool ImageDecoder_decodeStrip(const uint8_t* blob, size_t blobSize, const QoiCheckpoint* start,
                              uint16_t* dst, size_t pixelCount) {
    if (blob == nullptr || start == nullptr || dst == nullptr) return false;

    // Only the header is needed here; the dimensions were validated when the
    // checkpoints were built.
    QoiStream stream;
    int width = 0, height = 0;
    if (!qoiStreamInit(&stream, blob, blobSize, &width, &height)) return false;

    stream.pos = start->pos;
    stream.runRemaining = start->runRemaining;
    stream.r = start->rgba[0];
    stream.g = start->rgba[1];
    stream.b = start->rgba[2];
    stream.a = start->rgba[3];
    memcpy(stream.index, start->index, sizeof(stream.index));

    return qoiStreamFill(&stream, dst, pixelCount, true);
}
