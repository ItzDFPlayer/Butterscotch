#ifndef _BS_PERF_LOG_H_
#define _BS_PERF_LOG_H_

#include <stdbool.h>
#include <stdint.h>

// Frame-level performance log, meant to be copied off a handheld and shared.
//
// Rather than dumping every frame it writes:
//   - a heartbeat line every 60 frames (average step/draw cost of the window)
//   - a line for every frame slower than the threshold
//   - a line whenever the room changes
//   - optional GML script reports attached to slow frames
//   - a summary (p50/p95/max, slowest frames) when the game exits
//
// The file is plain text; every line is tab separated and prefixed with a type.

typedef struct PerfLog PerfLog;

typedef struct {
    int frame;
    const char* room;        // may be NULL
    int instanceCount;
    double stepMs;           // Runner_step: GML event execution
    double drawMs;           // view / post / GUI drawing
    double frameMs;          // wall time since the previous frame start (includes waiting)
    uint32_t textureBytes;   // decoded texture cache currently resident
    uint32_t textureBudgetBytes;
    uint32_t textureLoads;   // cumulative texture page decodes
    uint32_t textureEvictions; // cumulative texture page evictions
    uint32_t audioCacheBytes;
    uint32_t rssBytes;       // process memory, 0 when unavailable
    uint64_t drawsAttempted; // sprite/tile draws issued this frame
    uint64_t drawsCulled;    // of those, how many were skipped as off-screen
    uint64_t pxWritten;      // destination pixels covered by sprite blits (cumulative)
    uint64_t pxFastCopy;     // of those, how many took the opaque copy path (cumulative)
    uint64_t pxVectorBlend;  // of those, how many took the vectorised blend path (cumulative)
} PerfLogFrame;

// Opens `path` for writing. `slowFrameMs` is the threshold (in wall-frame
// milliseconds) at or above which a frame is logged individually; values below
// 1 are clamped to 1. Returns NULL when the file cannot be created.
PerfLog* PerfLog_open(const char* path, int slowFrameMs);

bool PerfLog_isOpen(const PerfLog* log);

// Records one frame. Returns true when the caller should generate a GML script
// report for the frames since the last report (slow frame, throttled).
bool PerfLog_frame(PerfLog* log, const PerfLogFrame* frame);

// Records a room transition (logged with the new room).
void PerfLog_roomChange(PerfLog* log, const PerfLogFrame* frame);

// Writes a "#" comment line.
void PerfLog_note(PerfLog* log, const char* fmt, ...);

// Appends a multi-line GML profiler report, one "#" comment line each.
void PerfLog_scriptReport(PerfLog* log, const char* report);

// Writes the end-of-run summary.
void PerfLog_summary(PerfLog* log);

// Flushes and closes the log. Safe to call with NULL.
void PerfLog_close(PerfLog* log);

#endif /* _BS_PERF_LOG_H_ */
