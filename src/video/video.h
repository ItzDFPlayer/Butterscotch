#ifndef _BS_VIDEO_H_
#define _BS_VIDEO_H_

#include "common.h"

// ===[ Video playback ]===
//
// GameMaker's video_* API. Deltarune Chapter 3 and 5 boot their cutscenes by
// opening an .mp4 from vid/ and blitting the decoded frames through a surface, so
// without a decoder those rooms never advance (they wait for a "video_end" that
// never comes).
//
// This is the decoder half of that API: the runner owns one of these, feeds it the
// master clock of the audio track and asks it for the frames that are due. Playback
// is paced by a monotonic clock rather than by the game's frame rate, so a machine
// that renders at 10 fps still plays the video at 30 fps (decoding several frames
// per game frame), and a machine that cannot decode fast enough holds the last
// frame instead of skipping ahead.
//
// When this build has no libav* (see BUTTERSCOTCH_FFMPEG) VideoDecoder_createBackend
// returns NULL, which makes the game take its own fallback path.

struct Runner;
typedef struct VideoDecoder VideoDecoder;

typedef enum {
    VIDEO_FRAME_NONE = 0,   // nothing to show: finished, or no frame was due yet
    VIDEO_FRAME_READY = 1,  // a new frame was converted, upload it
    VIDEO_FRAME_LOOPED = 2, // ...and the video wrapped back to the start
    VIDEO_FRAME_HELD = 3,   // no new frame, the surface already holds the right one
} VideoFrameResult;

typedef struct {
    bool (*init)(void);
    void (*quit)(VideoDecoder* decoder);
    bool (*open)(VideoDecoder* decoder, const char* url);
    void (*close)(VideoDecoder* decoder);
    // Dumps the audio track into a WAV the audio system can stream. Optional: a
    // decoder without audio support simply returns false and the video is silent.
    bool (*extractAudio)(VideoDecoder* decoder, struct Runner* runner, const char* url);
    bool (*isRunning)(VideoDecoder* decoder);
    bool (*isPaused)(VideoDecoder* decoder);
    void (*pause)(VideoDecoder* decoder);
    void (*resume)(VideoDecoder* decoder);
    void (*setLoop)(VideoDecoder* decoder, bool loop);
    // Negative means "no audio clock, use the wall clock".
    void (*setMasterClock)(VideoDecoder* decoder, double positionSeconds);
    int32_t (*update)(VideoDecoder* decoder);
    void (*draw)(VideoDecoder* decoder, struct Runner* runner, int32_t surfaceId);
    double (*duration)(VideoDecoder* decoder);
    double (*position)(VideoDecoder* decoder);
    int32_t (*width)(VideoDecoder* decoder);
    int32_t (*height)(VideoDecoder* decoder);
} VideoDecoderVtable;

struct VideoDecoder {
    VideoDecoderVtable* vtable;
    void* impl;
};

// Returns NULL when this build has no video decoder.
VideoDecoder* VideoDecoder_createBackend(void);

#endif // _BS_VIDEO_H_
