#include "perf_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gettime.h"
#include "utils.h"

#define PERF_LOG_MAX_BYTES ((long) (4 * 1024 * 1024))
#define PERF_HIST_BUCKETS 512 // 1 ms buckets, last bucket = "512 ms or more"
#define PERF_SLOWEST_MAX 10
#define PERF_ROOM_NAME_MAX 64
#define PERF_BEAT_EVERY 60
// Do not attach a script report to every single slow frame; consecutive
// hitches almost always have the same cause.
#define PERF_REPORT_COOLDOWN 45
#define PERF_REPORT_MAX 60

typedef struct {
    int frame;
    double frameMs;
    char room[PERF_ROOM_NAME_MAX];
} PerfSlowFrame;

struct PerfLog {
    FILE* f;
    int slowFrameMs;
    bool limitHit;
    uint64_t startNanos;

    int frameCount;
    int beatFrames;
    double beatSumStep;
    double beatSumDraw;
    double beatSumSwap;
    double beatMaxFrame;
    uint32_t beatLastLoads;
    uint32_t beatLastEvict;
    uint64_t beatLastAttempted;
    uint64_t beatLastCulled;
    uint64_t beatLastPixels;
    uint64_t beatLastFastPixels;
    uint64_t beatLastVectorPixels;
    uint64_t beatLastScalarPixels;
    uint64_t beatLastRotatedPixels;
    uint32_t lastTextureLoads;
    uint32_t lastTextureEvict;
    uint32_t lastBudgetBytes;
    uint32_t lastTextureBytes;

    double sumStep;
    double sumDraw;
    double sumSwap;
    double maxSwap;
    double sumFrame;
    double maxStep;
    double maxDraw;
    double maxFrame;

    uint32_t histStep[PERF_HIST_BUCKETS];
    uint32_t histDraw[PERF_HIST_BUCKETS];
    uint32_t histFrame[PERF_HIST_BUCKETS];

    int slowCount;
    int verySlowCount;
    int hitchOver100;
    int hitchOver250;
    int hitchOver500;

    int lastReportFrame;
    int reportCount;

    PerfSlowFrame slowest[PERF_SLOWEST_MAX];
    int slowestCount;
};

static int perfBucket(double ms) {
    if (ms < 0.0)
        return 0;
    int bucket = (int) ms;
    if (bucket >= PERF_HIST_BUCKETS)
        bucket = PERF_HIST_BUCKETS - 1;
    return bucket;
}

// Counters restart when a new game/renderer is created (game_change), so a
// decrease means "new baseline", not a negative delta.
static unsigned long long perfDelta(unsigned long long current, unsigned long long previous) {
    return current >= previous ? current - previous : current;
}

static void perfHistAdd(uint32_t* hist, double ms) {
    hist[perfBucket(ms)]++;
}

// Returns the bucket index of the given percentile (0..100).
static int perfPercentileBucket(const uint32_t* hist, int total, int percent) {
    if (total <= 0)
        return 0;
    long target = (long) (((long long) total * percent + 99) / 100);
    if (target < 1)
        target = 1;
    long seen = 0;
    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        seen += hist[i];
        if (seen >= target)
            return i;
    }
    return PERF_HIST_BUCKETS - 1;
}

static const char* perfRoomName(const PerfLogFrame* frame) {
    if (frame->room == NULL || frame->room[0] == '\0')
        return "?";
    return frame->room;
}

// First frame number of the window that ends at `frame->frame`. The frame
// counter restarts when a new game is launched (game_change), so clamp it.
static int perfWindowStart(const PerfLog* log, const PerfLogFrame* frame) {
    int start = frame->frame - log->beatFrames + 1;
    return start < 0 ? 0 : start;
}

static bool perfCanWrite(PerfLog* log) {
    if (log == NULL || log->f == NULL || log->limitHit)
        return false;
    if (ftell(log->f) >= PERF_LOG_MAX_BYTES) {
        log->limitHit = true;
        fputs("# perf log size limit reached; further frames are not recorded\n", log->f);
        fflush(log->f);
        return false;
    }
    return true;
}

static void perfWriteFrameLine(PerfLog* log, const char* type, const PerfLogFrame* frame,
                               double stepMs, double drawMs, double swapMs, double frameMs) {
    if (!perfCanWrite(log))
        return;
    fprintf(log->f,
            "%s\t%d\t%s\t%.2f\t%.2f\t%.2f\t%.2f\t%.1f\t%.1f\t%u\t%u\t%.1f\t%.1f\t%d\n",
            type,
            frame->frame,
            perfRoomName(frame),
            stepMs,
            drawMs,
            swapMs,
            frameMs,
            frame->textureBytes / (1024.0 * 1024.0),
            frame->textureBudgetBytes / (1024.0 * 1024.0),
            frame->textureLoads,
            frame->textureEvictions,
            frame->audioCacheBytes / (1024.0 * 1024.0),
            frame->rssBytes / (1024.0 * 1024.0),
            frame->instanceCount);
    fflush(log->f);
}

PerfLog* PerfLog_open(const char* path, int slowFrameMs) {
    if (path == NULL || path[0] == '\0')
        return NULL;

    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return NULL;

    PerfLog* log = (PerfLog*) safeCalloc(1, sizeof(PerfLog));
    log->f = f;
    log->slowFrameMs = slowFrameMs > 0 ? slowFrameMs : 1;
    log->startNanos = nowNanos();
    log->limitHit = false;

    fputs("# Butterscotch performance log v1\n", f);
    fprintf(f, "# slow-frame threshold: %d ms\n", log->slowFrameMs);
    fputs("# Sent with a bug report: frames that spike, room changes, heartbeats\n", f);
    fputs("# and any GML scripts that were running during a spike.\n", f);
    fputs("# columns: type frame room step_ms draw_ms swap_ms frame_ms tex_mb budget_mb"
          " tex_loads tex_evict audio_mb rss_mb instances\n", f);
    fputs("#   swap_ms = presenting the frame (blit to the display surface + SDL_Flip);\n", f);
    fputs("#             frame_ms minus step/draw/swap is the frame pacing wait\n", f);
    fputs("#   beat = heartbeat every 60 frames (averages of that window)\n", f);
    fputs("#   slow = a frame slower than the threshold\n", f);
    fputs("#   room = room change\n", f);
    fputs("#   #    = comment (script reports, summary)\n", f);
    fflush(f);
    return log;
}

bool PerfLog_isOpen(const PerfLog* log) {
    return log != NULL && log->f != NULL;
}

void PerfLog_note(PerfLog* log, const char* fmt, ...) {
    if (!perfCanWrite(log))
        return;
    va_list va;
    va_start(va, fmt);
    fputs("# ", log->f);
    vfprintf(log->f, fmt, va);
    fputc('\n', log->f);
    va_end(va);
    fflush(log->f);
}

void PerfLog_scriptReport(PerfLog* log, const char* report) {
    if (report == NULL || report[0] == '\0' || !perfCanWrite(log))
        return;

    fputs("# --- GML scripts during the slow frames above ---\n", log->f);
    const char* line = report;
    while (*line != '\0') {
        const char* end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
        fputs("# ", log->f);
        fwrite(line, 1, len, log->f);
        fputc('\n', log->f);
        if (end == NULL || end[1] == '\0')
            break;
        line = end + 1;
    }
    fputs("# --- end of script report ---\n", log->f);
    fflush(log->f);
}

bool PerfLog_frame(PerfLog* log, const PerfLogFrame* frame) {
    if (log == NULL || frame == NULL)
        return false;

    log->frameCount++;
    log->sumStep += frame->stepMs;
    log->sumDraw += frame->drawMs;
    log->sumSwap += frame->swapMs;
    log->sumFrame += frame->frameMs;
    log->lastTextureLoads = frame->textureLoads;
    log->lastTextureEvict = frame->textureEvictions;
    log->lastBudgetBytes = frame->textureBudgetBytes;
    log->lastTextureBytes = frame->textureBytes;

    if (frame->stepMs > log->maxStep) log->maxStep = frame->stepMs;
    if (frame->drawMs > log->maxDraw) log->maxDraw = frame->drawMs;
    if (frame->swapMs > log->maxSwap) log->maxSwap = frame->swapMs;
    if (frame->frameMs > log->maxFrame) log->maxFrame = frame->frameMs;

    perfHistAdd(log->histStep, frame->stepMs);
    perfHistAdd(log->histDraw, frame->drawMs);
    perfHistAdd(log->histFrame, frame->frameMs);

    bool isSlow = frame->frameMs >= (double) log->slowFrameMs;
    if (isSlow) {
        log->slowCount++;
        if (frame->frameMs >= (double) (log->slowFrameMs * 2))
            log->verySlowCount++;
        if (frame->frameMs >= 100.0)
            log->hitchOver100++;
        if (frame->frameMs >= 250.0)
            log->hitchOver250++;
        if (frame->frameMs >= 500.0)
            log->hitchOver500++;

        perfWriteFrameLine(log, "slow", frame, frame->stepMs, frame->drawMs, frame->swapMs, frame->frameMs);

        // Keep the slowest frames for the summary.
        const char* room = perfRoomName(frame);
        if (log->slowestCount < PERF_SLOWEST_MAX || frame->frameMs > log->slowest[0].frameMs) {
            int slot = -1;
            if (log->slowestCount < PERF_SLOWEST_MAX) {
                slot = log->slowestCount++;
            } else {
                slot = 0;
                for (int i = 1; i < log->slowestCount; i++) {
                    if (log->slowest[i].frameMs < log->slowest[slot].frameMs)
                        slot = i;
                }
            }
            log->slowest[slot].frame = frame->frame;
            log->slowest[slot].frameMs = frame->frameMs;
            snprintf(log->slowest[slot].room, sizeof(log->slowest[slot].room), "%s", room);
        }
    }

    // Heartbeat so the log still shows where the player was and how the
    // baseline cost developed between spikes.
    log->beatFrames++;
    log->beatSumStep += frame->stepMs;
    log->beatSumDraw += frame->drawMs;
    log->beatSumSwap += frame->swapMs;
    if (frame->frameMs > log->beatMaxFrame)
        log->beatMaxFrame = frame->frameMs;
    if (log->beatFrames >= PERF_BEAT_EVERY) {
        double n = (double) log->beatFrames;
        perfWriteFrameLine(log, "beat", frame, log->beatSumStep / n, log->beatSumDraw / n,
                           log->beatSumSwap / n, log->beatMaxFrame);
        // Texture churn is only meaningful as a delta over the window.
        if (frame->textureLoads != log->beatLastLoads || frame->textureEvictions != log->beatLastEvict)
            PerfLog_note(log, "tex pages in window %d-%d: %llu loaded, %llu evicted (cache %.1f/%.1f MB)",
                         perfWindowStart(log, frame), frame->frame,
                         perfDelta(frame->textureLoads, log->beatLastLoads),
                         perfDelta(frame->textureEvictions, log->beatLastEvict),
                         frame->textureBytes / (1024.0 * 1024.0),
                         frame->textureBudgetBytes / (1024.0 * 1024.0));
        log->beatLastLoads = frame->textureLoads;
        log->beatLastEvict = frame->textureEvictions;
        // Draw culling is also a window delta; it shows how much off-screen content
        // the view is skipping (and therefore which pages never need decoding).
        uint64_t attemptedInWindow = perfDelta(frame->drawsAttempted, log->beatLastAttempted);
        PerfLog_note(log, "draws in window %d-%d: %llu issued, %llu culled as off-screen",
                     perfWindowStart(log, frame), frame->frame,
                     attemptedInWindow,
                     perfDelta(frame->drawsCulled, log->beatLastCulled));
        log->beatLastAttempted = frame->drawsAttempted;
        log->beatLastCulled = frame->drawsCulled;
        // Pixels actually pushed through the rasteriser. This is what separates a
        // frame that is slow because it draws a lot from one that is slow because
        // each draw is expensive.
        {
            uint64_t px = perfDelta(frame->pxWritten, log->beatLastPixels);
            uint64_t fast = perfDelta(frame->pxFastCopy, log->beatLastFastPixels);
            double frames = (double) log->beatFrames;
            uint64_t vec = perfDelta(frame->pxVectorBlend, log->beatLastVectorPixels);
            uint64_t scalar = perfDelta(frame->pxScalarBlend, log->beatLastScalarPixels);
            uint64_t rot = perfDelta(frame->pxRotated, log->beatLastRotatedPixels);
            PerfLog_note(log, "pixels in window %d-%d: %.0fK/frame, %.0f%% opaque copy, %.0f%% vector blend (%.0f per draw)",
                         perfWindowStart(log, frame), frame->frame,
                         (double) px / frames / 1024.0,
                         px > 0 ? 100.0 * (double) fast / (double) px : 0.0,
                         px > 0 ? 100.0 * (double) vec / (double) px : 0.0,
                         attemptedInWindow > 0 ? (double) px / (double) attemptedInWindow : 0.0);
            PerfLog_note(log, "  of which %.0fK/frame scalar blend, %.0fK/frame rotated",
                         (double) scalar / frames / 1024.0, (double) rot / frames / 1024.0);
            log->beatLastPixels = frame->pxWritten;
            log->beatLastFastPixels = frame->pxFastCopy;
            log->beatLastVectorPixels = frame->pxVectorBlend;
            log->beatLastScalarPixels = frame->pxScalarBlend;
            log->beatLastRotatedPixels = frame->pxRotated;
        }
        log->beatFrames = 0;
        log->beatSumStep = 0.0;
        log->beatSumDraw = 0.0;
        log->beatMaxFrame = 0.0;
    }

    if (!isSlow)
        return false;
    if (log->reportCount >= PERF_REPORT_MAX)
        return false;
    if (log->reportCount > 0 && frame->frame - log->lastReportFrame < PERF_REPORT_COOLDOWN)
        return false;

    log->reportCount++;
    log->lastReportFrame = frame->frame;
    return true;
}

void PerfLog_roomChange(PerfLog* log, const PerfLogFrame* frame) {
    if (log == NULL || frame == NULL)
        return;
    perfWriteFrameLine(log, "room", frame, frame->stepMs, frame->drawMs, frame->swapMs, frame->frameMs);
}

void PerfLog_summary(PerfLog* log) {
    if (log == NULL || log->f == NULL)
        return;

    double seconds = (double) (nowNanos() - log->startNanos) / 1000000000.0;
    double count = (double) log->frameCount;
    int stepP50 = perfPercentileBucket(log->histStep, log->frameCount, 50);
    int stepP95 = perfPercentileBucket(log->histStep, log->frameCount, 95);
    int drawP50 = perfPercentileBucket(log->histDraw, log->frameCount, 50);
    int drawP95 = perfPercentileBucket(log->histDraw, log->frameCount, 95);
    int frameP50 = perfPercentileBucket(log->histFrame, log->frameCount, 50);
    int frameP95 = perfPercentileBucket(log->histFrame, log->frameCount, 95);

    fputs("\n# --- summary ---\n", log->f);
    fprintf(log->f, "# frames=%d running=%.1fs (%.1f fps average)\n",
            log->frameCount, seconds, seconds > 0.0 ? count / seconds : 0.0);
    fprintf(log->f, "# slow frames (>=%d ms): %d | over 100 ms: %d | over 250 ms: %d | over 500 ms: %d\n",
            log->slowFrameMs, log->slowCount, log->hitchOver100, log->hitchOver250, log->hitchOver500);
    if (count > 0.0) {
        fprintf(log->f, "# step ms : avg=%.2f p50=%d p95=%d max=%.2f\n",
                log->sumStep / count, stepP50, stepP95, log->maxStep);
        fprintf(log->f, "# draw ms : avg=%.2f p50=%d p95=%d max=%.2f\n",
                log->sumDraw / count, drawP50, drawP95, log->maxDraw);
        fprintf(log->f, "# swap ms : avg=%.2f max=%.2f\n",
                log->sumSwap / count, log->maxSwap);
        fprintf(log->f, "# frame ms: avg=%.2f p50=%d p95=%d max=%.2f\n",
                log->sumFrame / count, frameP50, frameP95, log->maxFrame);
    }

    if (log->lastTextureLoads > 0 || log->lastTextureEvict > 0) {
        fprintf(log->f, "# texture cache: %u page decodes, %u evictions, %u/%u MB in use at exit",
                log->lastTextureLoads, log->lastTextureEvict,
                log->lastTextureBytes / (1024u * 1024u), log->lastBudgetBytes / (1024u * 1024u));
        if (log->lastTextureEvict > log->lastTextureLoads / 2 && log->lastBudgetBytes > 0)
            fputs(" - the room's working set is larger than the cache, so pages are\n"
                  "#   being decoded again and again; raise BUTTERSCOTCH_TEXTURE_MAX_MB or expect slowdowns", log->f);
        fputc('\n', log->f);
    }

    if (log->slowestCount > 0) {
        fputs("# slowest frames:\n", log->f);
        // Insertion sort, at most 10 entries.
        for (int i = 1; i < log->slowestCount; i++) {
            PerfSlowFrame key = log->slowest[i];
            int j = i - 1;
            while (j >= 0 && log->slowest[j].frameMs < key.frameMs) {
                log->slowest[j + 1] = log->slowest[j];
                j--;
            }
            log->slowest[j + 1] = key;
        }
        for (int i = 0; i < log->slowestCount; i++) {
            fprintf(log->f, "#   frame=%d room=%s frame_ms=%.2f\n",
                    log->slowest[i].frame, log->slowest[i].room, log->slowest[i].frameMs);
        }
    }
    fputs("# --- end of summary ---\n", log->f);
    fflush(log->f);
}

void PerfLog_close(PerfLog* log) {
    if (log == NULL)
        return;
    if (log->f != NULL) {
        fflush(log->f);
        fclose(log->f);
        log->f = NULL;
    }
    free(log);
}
