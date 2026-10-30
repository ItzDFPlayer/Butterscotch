#include "sw_draw_profile.h"

#include <stdlib.h>
#include <string.h>

#include "gettime.h"
#include "log.h"

// One entry per (kind, key) pair seen since the last report. Linear search on the
// way in is fine: a heavy room touches tens of distinct page items per frame, not
// thousands, and the alternative (a hash map touched a few hundred times a frame)
// would cost more than the timing saves.
#define SWR_PROFILE_SLOTS 48

typedef struct {
    int32_t kind;
    int32_t key;
    uint64_t nanos;
    uint32_t draws;
    uint64_t pixels;
} SwrDrawProfileEntry;

static SwrDrawProfileEntry sProfile[SWR_PROFILE_SLOTS];
static int sProfileCount;
static int sProfileEnabled = -1;

bool swrDrawProfileEnabled(void) {
    if (sProfileEnabled < 0) {
        const char* env = getenv("BUTTERSCOTCH_PROFILE_DRAWS");
        sProfileEnabled = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
        if (sProfileEnabled) {
            logInfo("Draw profile: on (report once a second)\n");
        }
    }
    return sProfileEnabled != 0;
}

uint64_t swrDrawProfileBegin(void) {
    if (!swrDrawProfileEnabled()) return 0;
    return nowNanos();
}

void swrDrawProfileEnd(int kind, int key, int pixels, uint64_t started) {
    if (started == 0) return;
    uint64_t elapsed = nowNanos() - started;
    if (elapsed > 200000000ull) elapsed = 200000000ull; // never let one stall skew a second

    SwrDrawProfileEntry* entry = nullptr;
    for (int i = 0; i < sProfileCount; i++) {
        if (sProfile[i].kind == kind && sProfile[i].key == key) {
            entry = &sProfile[i];
            break;
        }
    }
    if (entry == nullptr) {
        if (sProfileCount < SWR_PROFILE_SLOTS) {
            entry = &sProfile[sProfileCount++];
            memset(entry, 0, sizeof(*entry));
            entry->kind = kind;
            entry->key = key;
        } else {
            // Full: replace the cheapest entry, so a one-off draw cannot crowd out
            // a page that is drawn every frame.
            entry = &sProfile[0];
            for (int i = 1; i < sProfileCount; i++) {
                if (sProfile[i].nanos < entry->nanos) entry = &sProfile[i];
            }
        }
    }

    entry->nanos += elapsed;
    entry->draws++;
    if (pixels > 0) entry->pixels += (uint64_t) pixels;
}

static const char* swrDrawKindName(int kind) {
    switch (kind) {
        case SWR_DRAW_SPRITE: return "sprite";
        case SWR_DRAW_SPRITE_PART: return "part";
        case SWR_DRAW_SPRITE_POS: return "pos";
        case SWR_DRAW_SPRITE_TILED: return "tiled";
        case SWR_DRAW_SURFACE: return "surface";
        case SWR_DRAW_RECT: return "rect";
        default: return "?";
    }
}

void SWRenderer_reportDrawProfile(void) {
    if (!swrDrawProfileEnabled()) return;
    if (sProfileCount == 0) return;

    // Selection sort of the top few; the array is tiny.
    int printed = 0;
    uint64_t total = 0;
    for (int i = 0; i < sProfileCount; i++) total += sProfile[i].nanos;

    logInfo("Draw profile: %.1f ms in %d kinds\n", (double) total / 1e6, sProfileCount);
    for (int rank = 0; rank < 8 && rank < sProfileCount; rank++) {
        int best = -1;
        for (int i = 0; i < sProfileCount; i++) {
            if (sProfile[i].draws == 0) continue;
            if (best < 0 || sProfile[i].nanos > sProfile[best].nanos) best = i;
        }
        if (best < 0) break;

        SwrDrawProfileEntry* e = &sProfile[best];
        logInfo("  %-7s %5d  %7.2f ms  %6.2f us/draw  %5.1fK px/draw\n",
                swrDrawKindName(e->kind), e->key, (double) e->nanos / 1e6,
                (double) e->nanos / 1e3 / (double) e->draws,
                e->draws > 0 ? (double) e->pixels / (double) e->draws / 1024.0 : 0.0);
        e->draws = 0; // consumed, so the next rank search skips it
        printed++;
    }
    (void) printed;
    sProfileCount = 0;
}
