// ===[ FFmpeg video backend ]===
//
// Ported from upstream Butterscotch (src/video/ffmpeg/ffmpeg.c), with two changes
// that matter for the handheld:
//
//  * the frames are converted straight to RGB555LE, the software renderer's native
//    format, instead of RGBA, so the surface upload is a 2-byte-per-pixel copy and
//    no per-pixel channel packing happens anywhere;
//  * playback is paced by a monotonic clock and the audio track's position rather
//    than by how often the game happens to call video_draw(). Upstream fixed this
//    in "Fix video playback rate being tied to game framerate" (#566), which is
//    exactly the bug that made the video crawl on a 1.2 GHz A7: the old code
//    decoded a fixed four frames per game frame, so a slow device played the video
//    in slow motion and only ever fired video_end minutes later.

#include "video.h"

#include "gettime.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "stdio_compat.h"
#include "string_compat.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

// libavformat wants a protocol prefix on paths that contain a drive letter, and the
// consoles (and the Miyoo) hand us plain absolute paths.
static char* ffmpegFileUrl(const char* path) {
    if (path == nullptr) return nullptr;
    if (strncmp(path, "file:", 5) == 0) return safeStrdup(path);
    const char* colon = strchr(path, ':');
    if (colon != nullptr && colon[1] == '/' && colon[2] == '/') {
        return safeStrdup(path);
    }
    size_t len = strlen(path);
    char* out = (char*) safeMalloc(len + 6);
    memcpy(out, "file:", 5);
    memcpy(out + 5, path, len + 1);
    return out;
}

#define VIDEO_AUDIO_CHANNELS 2
#define VIDEO_AUDIO_SAMPLE_RATE 48000

// A smaller surface would make the upload and the game's draw of it much cheaper, but
// the game lays the video out itself and assumes it is the size the file says: a
// downscaled surface shows up in the top left corner, cropped. Off by default, on with
// BUTTERSCOTCH_VIDEO_SCALE for anyone who wants to trade framing for speed.
#define VIDEO_DEFAULT_MAX_WIDTH 0

#define VIDEO_FALLBACK_FPS 30.0
#define VIDEO_MIN_FPS 1.0
#define VIDEO_MAX_FPS 1000.0
// How many frames we let a single update decode when it has fallen behind. A slow
// decoder burns its whole frame budget here, so the cap keeps a hitch bounded.
#define VIDEO_MAX_CATCHUP_FRAMES 12
#define VIDEO_MASTER_JUMP_SECONDS 0.25
// How far the picture may fall behind before the decoder jumps forward instead of
// decoding every frame. A 1280x720 H.264 cutscene decodes at ~14 fps on the A7, so a
// 24 fps video can never keep up: without a jump the picture drifts further behind for
// the whole cutscene and the video ends long after the game thinks it did.
#define VIDEO_SEEK_AHEAD_SECONDS 1.0
// How far past the clock a jump aims. avformat_seek_file lands on the keyframe before
// the requested time, and on this CPU decoding forward from that keyframe costs far
// more than the jump saves, so aim past the clock and show whatever keyframe is there.
#define VIDEO_SEEK_LEAD_SECONDS 1.0
// How many updates in a row a master (audio) clock may report the same position
// before it is treated as unusable. A backend that cannot report the position of a
// streamed sound returns 0 forever, and trusting that froze the first frame while
// the sound played on.
#define VIDEO_MASTER_STALL_TICKS 30

typedef struct {
    AVFormatContext* formatCtx;
    int32_t videoStreamIndex;
    AVStream* videoStream;
    AVCodecContext* videoCodecCtx;
    AVFrame* videoFrame;
    struct SwsContext* swsCtx;
    uint16_t* drawPixels; // width * height packed RGB555, the renderer's format
    int32_t width;                // Output (surface) size, possibly scaled down.
    int32_t height;
    int32_t srcWidth;             // Size the video is coded at.
    int32_t srcHeight;
    int64_t startPts;

    double frameDurationSeconds; // Nominal frame period of the source video.
    double presentedSeconds;     // Video time of the frame sitting in drawPixels.
    double nextDueSeconds;       // Video time at which the next frame slot opens.
    uint64_t startNanos;         // Monotonic timestamp the playback clock started at.
    bool clockStarted;
    bool hasFrame;

    double masterClock;           // Latest position reported by the audio track.
    double masterAnchorClock;     // Audio position that masterAnchorVideoTime maps to.
    double masterAnchorVideoTime; // Video time the master clock is anchored to.
    double lastMasterClock;
    double stallLastClock;        // Previous master clock, for the stall check.
    int32_t stalledMasterTicks;   // Consecutive updates the master clock did not move.
    bool hasMasterSample;
    bool masterClockValid;        // The audio track is driving playback.
    bool masterClockUnusable;     // Never trust this video's audio clock again.
    bool seekPending;             // A jump is waiting for its first frame to decode.
    double seekTargetSeconds;     // Video time that jump is heading for.
    uint32_t decodedSinceLog;     // Frames converted since the last status line.

    double durationSeconds;
    bool hasVideo;
    bool eof;
    bool finished;
    bool paused;
    bool loop;
} FfmpegVideoDecoder;

static double ffmpegVideoDecoderPtsSeconds(FfmpegVideoDecoder* d, int64_t pts) {
    if (d->videoStream == nullptr || pts == AV_NOPTS_VALUE) return -1;
    return (double) (pts - d->startPts) * av_q2d(d->videoStream->time_base);
}

static bool ffmpegVideoDecoderMasterClockJumped(FfmpegVideoDecoder* d, double positionSeconds) {
    return d->hasMasterSample && positionSeconds < d->lastMasterClock - VIDEO_MASTER_JUMP_SECONDS;
}

static double ffmpegVideoDecoderTargetSeconds(FfmpegVideoDecoder* d, uint64_t now) {
    if (d->masterClockValid) {
        // A clock that never moves cannot drive the picture. Some audio backends
        // report a constant (or zero) position for a streamed sound, which left the
        // video frozen on its first frame for the whole cutscene while the sound kept
        // playing, so give up on such a clock and fall back to the monotonic one.
        if (d->masterClock <= d->stallLastClock + 0.000001) {
            d->stalledMasterTicks++;
        } else {
            d->stalledMasterTicks = 0;
        }
        d->stallLastClock = d->masterClock;

        if (d->stalledMasterTicks > VIDEO_MASTER_STALL_TICKS) {
            logInfo("Video: audio clock stuck at %.3fs, following the wall clock\n", d->masterClock);
            d->masterClockValid = false;
            d->masterClockUnusable = true;
            d->hasMasterSample = false;
            d->stalledMasterTicks = 0;
            d->clockStarted = false;
        } else {
            if (!d->hasMasterSample || ffmpegVideoDecoderMasterClockJumped(d, d->masterClock)) {
                d->masterAnchorClock = d->masterClock;
                d->masterAnchorVideoTime = d->hasFrame ? d->presentedSeconds : 0;
                d->hasMasterSample = true;
            }
            return d->masterAnchorVideoTime + (d->masterClock - d->masterAnchorClock);
        }
    }
    if (!d->clockStarted) {
        // Start the wall clock at the frame that is on screen, not at zero: this also
        // runs when the audio clock was abandoned part-way through a video.
        double baseSeconds = d->hasFrame ? d->presentedSeconds : 0.0;
        d->startNanos = now - (uint64_t) (baseSeconds * 1e9);
        d->clockStarted = true;
    }
    return (double) (now - d->startNanos) / 1000000000.0;
}

static int32_t ffmpegVideoDecoderFramesDue(FfmpegVideoDecoder* d, double targetSeconds) {
    double behind = targetSeconds - d->nextDueSeconds;
    if (behind < 0) return 0;
    int32_t due = 1 + (int32_t) (behind / d->frameDurationSeconds);
    if (due <= VIDEO_MAX_CATCHUP_FRAMES) return due;

    // Far behind: give up on catching up and drop to the frame we are on, otherwise
    // one slow update would cascade into every later update.
    d->nextDueSeconds = targetSeconds;
    return 1;
}

static int32_t ffmpegVideoDecoderConvertFrame(FfmpegVideoDecoder* d, AVFrame* frame) {
    if (d->swsCtx == nullptr || d->drawPixels == nullptr) return -1;
    uint8_t* dst[1] = { (uint8_t*) d->drawPixels };
    int32_t dstStride[1] = { d->width * 2 };
    // The fifth argument is how many *source* rows to convert, so it is the coded
    // height and not the output height. Passing the output height converted only the
    // top half of a downscaled frame and left the rest of the surface black.
    sws_scale(d->swsCtx, (const uint8_t**) frame->data, frame->linesize, 0, d->srcHeight, dst, dstStride);

    double ptsSeconds = ffmpegVideoDecoderPtsSeconds(d, frame->pts);
    if (ptsSeconds >= 0) {
        d->presentedSeconds = ptsSeconds;
    } else {
        d->presentedSeconds = d->hasFrame ? d->presentedSeconds + d->frameDurationSeconds : 0;
    }
    d->hasFrame = true;
    d->decodedSinceLog++;
    return 0;
}

static bool ffmpegVideoDecoderPullFrame(FfmpegVideoDecoder* d) {
    while (true) {
        if (d->eof) return avcodec_receive_frame(d->videoCodecCtx, d->videoFrame) == 0;

        AVPacket packet = {0};
        if (av_read_frame(d->formatCtx, &packet) < 0) {
            av_packet_unref(&packet);
            d->eof = true;
            avcodec_send_packet(d->videoCodecCtx, nullptr);
            continue;
        }
        bool sent = packet.stream_index == d->videoStreamIndex
                    && avcodec_send_packet(d->videoCodecCtx, &packet) == 0;
        av_packet_unref(&packet);
        if (sent && avcodec_receive_frame(d->videoCodecCtx, d->videoFrame) == 0) return true;
    }
}

static bool ffmpegVideoDecoderRewind(FfmpegVideoDecoder* d) {
    avcodec_flush_buffers(d->videoCodecCtx);
    if (avformat_seek_file(d->formatCtx, d->videoStreamIndex, INT64_MIN, d->startPts, INT64_MAX, 0) < 0) {
        return false;
    }
    d->eof = false;
    d->presentedSeconds = 0;
    d->hasFrame = false;
    d->seekPending = false;
    return true;
}

// Jumps to `seconds` instead of decoding every frame in between. Used when the device
// cannot decode the video in real time: on this hardware a 720p cutscene decodes at
// about 14 fps, so without the jump the picture falls further behind every second and
// the cutscene overruns its own length.
static bool ffmpegVideoDecoderSeekAhead(FfmpegVideoDecoder* d, double seconds) {
    if (d->videoStream == nullptr) return false;
    double seekTo = seconds + VIDEO_SEEK_LEAD_SECONDS;
    if (d->durationSeconds > 0 && seekTo > d->durationSeconds - 0.2) {
        seekTo = d->durationSeconds > 0.2 ? d->durationSeconds - 0.2 : 0;
    }
    // A jump that would not move forward is the end of the file in disguise: past the
    // last keyframe every seek lands on that same keyframe again. Let the normal decode
    // path run to eof instead, which is what reports the video as finished.
    if (seekTo <= d->presentedSeconds + d->frameDurationSeconds) return false;
    int64_t ts = d->startPts + (int64_t) (seekTo / av_q2d(d->videoStream->time_base));
    if (avformat_seek_file(d->formatCtx, d->videoStreamIndex, INT64_MIN, ts, INT64_MAX, 0) < 0) {
        return false;
    }
    avcodec_flush_buffers(d->videoCodecCtx);
    d->eof = false;
    d->nextDueSeconds = seekTo;
    d->stalledMasterTicks = 0;
    d->seekTargetSeconds = seekTo;
    d->seekPending = true;
    return true;
}

// Shows the first frame the jump lands on, without decoding towards the requested time.
// Walking from the keyframe to the exact time would cost a whole group of pictures,
// which on this hardware is slower than simply playing and never getting there.
static bool ffmpegVideoDecoderTakeSeekFrame(FfmpegVideoDecoder* d) {
    if (!ffmpegVideoDecoderPullFrame(d)) {
        if (d->eof && !d->loop) d->finished = true;
        return false;
    }
    bool shown = ffmpegVideoDecoderConvertFrame(d, d->videoFrame) == 0;
    av_frame_unref(d->videoFrame);
    if (shown) {
        d->seekPending = false;
        d->nextDueSeconds = d->presentedSeconds + d->frameDurationSeconds;
    }
    return shown;
}

static void ffmpegVideoDecoderSetMasterClock(VideoDecoder* decoder, double positionSeconds) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d == nullptr) return;
    if (positionSeconds < 0 && d->masterClockValid && !d->clockStarted) {
        // The audio track stopped driving playback (it ended, or it is paused).
        // Continue the wall clock from the frame we are on rather than restarting it
        // at zero, which would rewind the video.
        d->startNanos = nowNanos() - (uint64_t) (d->presentedSeconds * 1e9);
        d->clockStarted = true;
    }
    d->masterClock = positionSeconds;
    d->lastMasterClock = positionSeconds;
    d->hasMasterSample = positionSeconds >= 0;
    d->masterClockValid = positionSeconds >= 0 && !d->masterClockUnusable;
}

static int32_t ffmpegVideoDecoderUpdate(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d == nullptr || d->formatCtx == nullptr || !d->hasVideo) return VIDEO_FRAME_NONE;
    if (d->finished) return VIDEO_FRAME_NONE;

    // BUTTERSCOTCH_LOG_VIDEO=1 prints one line a second with everything that decides
    // whether the picture moves: the clock the decoder follows, the frame time it is
    // showing, and how many frames each update considered due.
    static int logVideo = -1;
    if (logVideo < 0) {
        const char* env = getenv("BUTTERSCOTCH_LOG_VIDEO");
        logVideo = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    uint64_t now = nowNanos();
    double targetSeconds = ffmpegVideoDecoderTargetSeconds(d, now);

    int32_t framesToDecode;
    if (d->hasFrame && d->paused) {
        return VIDEO_FRAME_HELD;
    }

    // The clock has run past the end of the file. Say so instead of chasing it: the
    // picture cannot advance any further, and the game is waiting for video_end to
    // move on.
    if (!d->loop && d->durationSeconds > 0 && targetSeconds >= d->durationSeconds) {
        d->presentedSeconds = d->durationSeconds;
        d->finished = true;
        return VIDEO_FRAME_NONE;
    }

    // Too far behind to catch up by decoding: jump to where the clock is. Not again
    // until the frame that jump landed on has been decoded, or a slow keyframe would
    // make every following update jump again.
    if (d->hasFrame && !d->seekPending && targetSeconds > d->presentedSeconds + VIDEO_SEEK_AHEAD_SECONDS) {
        double drift = targetSeconds - d->presentedSeconds;
        if (ffmpegVideoDecoderSeekAhead(d, targetSeconds)) {
            logInfo("Video: %.2fs behind, jumped to %.2fs\n", drift, targetSeconds);
        }
    }
    if (d->seekPending) {
        if (ffmpegVideoDecoderTakeSeekFrame(d)) {
            logInfo("Video: jumped, showing %.2fs (clock %.2fs)\n", d->presentedSeconds, targetSeconds);
            return VIDEO_FRAME_READY;
        }
        if (d->finished) return VIDEO_FRAME_NONE;
    }

    framesToDecode = ffmpegVideoDecoderFramesDue(d, targetSeconds);

    if (logVideo) {
        static uint64_t lastLog = 0;
        if (now - lastLog >= 1000000000ull) {
            lastLog = now;
            logInfo("Video: target=%.2f shown=%.2f nextDue=%.2f due=%d fps=%u clock=%s(%.2f) audio=%d\n",
                    targetSeconds, d->presentedSeconds, d->nextDueSeconds, framesToDecode,
                    d->decodedSinceLog,
                    d->masterClockValid ? "audio" : "wall", d->masterClock,
                    d->masterClockUnusable ? -1 : 0);
            d->decodedSinceLog = 0;
        }
    }

    if (framesToDecode <= 0) return VIDEO_FRAME_HELD;

    bool gotFrame = false;
    bool wrapped = false;
    for (int32_t i = 0; i < framesToDecode; i++) {
        // Only the frame we are going to show is worth converting; on a slow CPU
        // sws_scale is a real share of the decode cost.
        bool keep = (i == framesToDecode - 1);
        while (true) {
            if (ffmpegVideoDecoderPullFrame(d)) {
                if (keep && ffmpegVideoDecoderConvertFrame(d, d->videoFrame) == 0) gotFrame = true;
                av_frame_unref(d->videoFrame);
                break;
            }
            if (!d->loop) {
                d->finished = true;
                break;
            }
            if (!ffmpegVideoDecoderRewind(d)) {
                d->finished = true;
                break;
            }
            wrapped = true;
        }
        if (d->finished) break;
    }

    d->nextDueSeconds += framesToDecode * d->frameDurationSeconds;

    if (!gotFrame) return VIDEO_FRAME_NONE;
    return wrapped ? VIDEO_FRAME_LOOPED : VIDEO_FRAME_READY;
}

static void ffmpegVideoDecoderDraw(VideoDecoder* decoder, Runner* runner, int32_t surfaceId) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d == nullptr || d->drawPixels == nullptr || runner->renderer == nullptr) return;

    if (runner->renderer->vtable->surfaceUploadRgb555 != nullptr) {
        Renderer_surfaceUploadRgb555(runner->renderer, surfaceId, d->drawPixels, d->width, d->height, d->width);
        return;
    }

    // Other renderers take RGBA8. Expanding is only worth doing on the frame that is
    // actually drawn, so the scratch buffer lives here rather than in the decoder.
    if (runner->renderer->vtable->surfaceUploadPixels != nullptr) {
        size_t pixels = (size_t) d->width * (size_t) d->height;
        uint8_t* rgba = (uint8_t*) safeMalloc(pixels * 4);
        if (rgba == nullptr) return;
        for (size_t i = 0; i < pixels; i++) {
            uint16_t px = d->drawPixels[i];
            uint8_t r = (uint8_t) (((px >> 10) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t) (((px >> 5) & 0x1F) * 255 / 31);
            uint8_t b = (uint8_t) ((px & 0x1F) * 255 / 31);
            rgba[i * 4 + 0] = r;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = b;
            rgba[i * 4 + 3] = 255;
        }
        Renderer_surfaceUploadPixels(runner->renderer, surfaceId, d->width, d->height, rgba);
        free(rgba);
    }
}

static bool ffmpegVideoDecoderIsRunning(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    return d != nullptr && d->hasVideo && !d->finished;
}

static bool ffmpegVideoDecoderIsPaused(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    return d != nullptr && d->paused;
}

static void ffmpegVideoDecoderPause(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d != nullptr) d->paused = true;
}

static void ffmpegVideoDecoderResume(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d == nullptr) return;
    d->paused = false;
    // Resume from the frame that is on screen, with or without an audio clock.
    d->startNanos = nowNanos() - (uint64_t) (d->presentedSeconds * 1e9);
    d->clockStarted = true;
    d->hasMasterSample = false;
}

static void ffmpegVideoDecoderSetLoop(VideoDecoder* decoder, bool loop) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d != nullptr) d->loop = loop;
}

static double ffmpegVideoDecoderDuration(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d == nullptr) return 0;
    return d->durationSeconds;
}

static double ffmpegVideoDecoderPosition(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d == nullptr || !d->hasFrame) return 0;
    return d->presentedSeconds;
}

static int32_t ffmpegVideoDecoderWidth(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    return d == nullptr ? 0 : d->width;
}

static int32_t ffmpegVideoDecoderHeight(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    return d == nullptr ? 0 : d->height;
}

static bool ffmpegVideoDecoderInit(void) {
    return true;
}

static void ffmpegVideoDecoderQuit(VideoDecoder* decoder) {
}

static void ffmpegVideoDecoderClose(VideoDecoder* decoder) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    if (d == nullptr) return;
    if (d->swsCtx != nullptr) { sws_freeContext(d->swsCtx); d->swsCtx = nullptr; }
    free(d->drawPixels);
    d->drawPixels = nullptr;
    if (d->videoFrame != nullptr) { av_frame_free(&d->videoFrame); d->videoFrame = nullptr; }
    if (d->videoCodecCtx != nullptr) { avcodec_free_context(&d->videoCodecCtx); d->videoCodecCtx = nullptr; }
    if (d->formatCtx != nullptr) { avformat_close_input(&d->formatCtx); d->formatCtx = nullptr; }
    d->videoStream = nullptr;
    d->videoStreamIndex = -1;
    d->width = 0;
    d->height = 0;
    d->srcWidth = 0;
    d->srcHeight = 0;
    d->startPts = 0;
    d->frameDurationSeconds = 1.0 / VIDEO_FALLBACK_FPS;
    d->presentedSeconds = 0;
    d->nextDueSeconds = 0;
    d->startNanos = 0;
    d->clockStarted = false;
    d->hasFrame = false;
    d->masterClock = 0;
    d->masterAnchorClock = 0;
    d->masterAnchorVideoTime = 0;
    d->lastMasterClock = 0;
    d->stallLastClock = 0;
    d->stalledMasterTicks = 0;
    d->hasMasterSample = false;
    d->masterClockValid = false;
    d->masterClockUnusable = false;
    d->seekPending = false;
    d->durationSeconds = 0;
    d->hasVideo = false;
    d->eof = false;
    d->finished = false;
    d->paused = false;
    d->loop = false;
}

static bool ffmpegVideoDecoderOpen(VideoDecoder* decoder, const char* url) {
    FfmpegVideoDecoder* d = (FfmpegVideoDecoder*) decoder->impl;
    char* ffmpegUrl = ffmpegFileUrl(url);
    int openResult = avformat_open_input(&d->formatCtx, ffmpegUrl, nullptr, nullptr);
    free(ffmpegUrl);
    if (openResult != 0) {
        logWarn("Video: could not open '%s'\n", url);
        return false;
    }
    if (avformat_find_stream_info(d->formatCtx, nullptr) < 0) {
        avformat_close_input(&d->formatCtx);
        d->formatCtx = nullptr;
        logWarn("Video: no stream info in '%s'\n", url);
        return false;
    }
    d->durationSeconds = d->formatCtx->duration > 0 ? (double) d->formatCtx->duration / (double) AV_TIME_BASE : 0;

    const AVCodec* videoCodec = nullptr;
    d->videoStreamIndex = av_find_best_stream(d->formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &videoCodec, 0);
    if (d->videoStreamIndex >= 0 && videoCodec != nullptr) {
        d->videoStream = d->formatCtx->streams[d->videoStreamIndex];
        d->videoCodecCtx = avcodec_alloc_context3(videoCodec);
        if (d->videoCodecCtx != nullptr) {
            if (avcodec_parameters_to_context(d->videoCodecCtx, d->videoStream->codecpar) == 0) {
                // Extra threads cost memory (128 MB on the Miyoo), so this stays at one
                // by default; BUTTERSCOTCH_VIDEO_THREADS=2 halves the decode time of a
                // 720p cutscene on the dual-core A7 if there is memory to spare.
                {
                    const char* threadsEnv = getenv("BUTTERSCOTCH_VIDEO_THREADS");
                    long threads = threadsEnv != nullptr ? strtol(threadsEnv, nullptr, 10) : 0;
                    if (threads <= 0) threads = 1;
                    if (threads > 4) threads = 4;
                    d->videoCodecCtx->thread_count = (int) threads;
                }
                if (avcodec_open2(d->videoCodecCtx, videoCodec, nullptr) == 0) d->hasVideo = true;
            }
            if (!d->hasVideo) {
                avcodec_free_context(&d->videoCodecCtx);
                d->videoCodecCtx = nullptr;
                d->videoStream = nullptr;
                d->videoStreamIndex = -1;
            }
        }
    }

    if (!d->hasVideo) {
        logWarn("Video: no usable video stream in '%s'\n", url);
        ffmpegVideoDecoderClose(decoder);
        return false;
    }

    // Deliberately native size by default. Scaling down does not reduce the H.264
    // decode cost, and the game positions the video by what the file says it is, so a
    // smaller surface is drawn too small and cropped. BUTTERSCOTCH_VIDEO_SCALE is there
    // for anyone who wants to trade the framing for a cheaper upload and draw.
    d->srcWidth = d->videoCodecCtx->width;
    d->srcHeight = d->videoCodecCtx->height;
    {
        const char* scaleEnv = getenv("BUTTERSCOTCH_VIDEO_SCALE");
        long maxWidth = scaleEnv != nullptr ? strtol(scaleEnv, nullptr, 10) : VIDEO_DEFAULT_MAX_WIDTH;
        d->width = d->srcWidth;
        d->height = d->srcHeight;
        if (maxWidth > 0 && d->srcWidth > maxWidth) {
            d->width = (int32_t) maxWidth;
            d->height = (int32_t) (((long) d->srcHeight * maxWidth + d->srcWidth / 2) / d->srcWidth);
        }
        // An odd dimension breaks the 2-byte row assumption of the upload.
        d->width &= ~1;
        d->height &= ~1;
        if (d->width <= 0) d->width = 2;
        if (d->height <= 0) d->height = 2;
    }
    d->startPts = d->videoStream->start_time == AV_NOPTS_VALUE ? 0 : d->videoStream->start_time;
    if (d->durationSeconds <= 0 && d->videoStream->duration > 0) {
        d->durationSeconds = (double) d->videoStream->duration * av_q2d(d->videoStream->time_base);
    }

    d->videoFrame = av_frame_alloc();
    if (d->videoFrame == nullptr) {
        ffmpegVideoDecoderClose(decoder);
        return false;
    }
    d->swsCtx = sws_getContext(d->srcWidth, d->srcHeight, d->videoCodecCtx->pix_fmt,
                               d->width, d->height, AV_PIX_FMT_RGB555LE,
                               SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    d->drawPixels = (uint16_t*) safeMalloc((size_t) d->width * (size_t) d->height * sizeof(uint16_t));
    if (d->swsCtx == nullptr || d->drawPixels == nullptr) {
        logWarn("Video: could not set up the RGB555 conversion for '%s'\n", url);
        ffmpegVideoDecoderClose(decoder);
        return false;
    }
    memset(d->drawPixels, 0, (size_t) d->width * (size_t) d->height * sizeof(uint16_t));

    AVRational frameRate = d->videoStream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0) frameRate = d->videoStream->r_frame_rate;
    double fps = (frameRate.num > 0 && frameRate.den > 0) ? av_q2d(frameRate) : 0.0;
    if (fps < VIDEO_MIN_FPS || fps > VIDEO_MAX_FPS) fps = VIDEO_FALLBACK_FPS;
    d->frameDurationSeconds = 1.0 / fps;

    d->eof = false;
    d->finished = false;
    d->paused = false;
    d->loop = false;
    d->presentedSeconds = 0;
    d->nextDueSeconds = 0;
    d->startNanos = 0;
    d->clockStarted = false;
    d->hasFrame = false;
    d->masterClock = 0;
    d->masterAnchorClock = 0;
    d->masterAnchorVideoTime = 0;
    d->lastMasterClock = 0;
    d->stallLastClock = 0;
    d->stalledMasterTicks = 0;
    d->hasMasterSample = false;
    d->masterClockValid = false;
    d->masterClockUnusable = false;
    d->seekPending = false;

    logInfo("Video: opened '%s' (%dx%d -> %dx%d, %.1fs, %.2f fps)\n", url,
            d->srcWidth, d->srcHeight, d->width, d->height, d->durationSeconds, fps);
    return true;
}

// ===[ Audio track extraction ]===
//
// The audio system can only stream WAV, so the whole (short) audio track is
// decoded once at open and handed to it as a WAV in the save folder. The stream's
// playback position then becomes the master clock for the video.

typedef struct VideoWavWriter {
    FILE* file;
    uint8_t* pcm;
    uint32_t pcmSize;
    uint32_t pcmCapacity;
    int32_t channels;
    int32_t sampleRate;
} VideoWavWriter;

static void videoAudioWriteWavHeader(uint8_t* wav, uint32_t dataSize, int32_t channels, int32_t sampleRate) {
    memcpy(wav, "RIFF", 4);
    uint32_t riffSize = 36 + dataSize;
    memcpy(wav + 4, &riffSize, 4);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    uint32_t fmtSize = 16;
    memcpy(wav + 16, &fmtSize, 4);
    uint16_t audioFormat = 1;
    memcpy(wav + 20, &audioFormat, 2);
    uint16_t numChannels = (uint16_t) channels;
    memcpy(wav + 22, &numChannels, 2);
    memcpy(wav + 24, &sampleRate, 4);
    uint32_t byteRate = (uint32_t) (sampleRate * channels * 2);
    memcpy(wav + 28, &byteRate, 4);
    uint16_t blockAlign = (uint16_t) (channels * 2);
    memcpy(wav + 32, &blockAlign, 2);
    uint16_t bitsPerSample = 16;
    memcpy(wav + 34, &bitsPerSample, 2);
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &dataSize, 4);
}

static void videoWavWriterOpen(VideoWavWriter* w, const char* absPath, int32_t channels, int32_t sampleRate) {
    w->file = (absPath != nullptr) ? fopen(absPath, "wb") : nullptr;
    w->pcm = nullptr;
    w->pcmSize = 0;
    w->pcmCapacity = 0;
    w->channels = channels;
    w->sampleRate = sampleRate;
    if (w->file != nullptr) {
        uint8_t header[44];
        videoAudioWriteWavHeader(header, 0, channels, sampleRate);
        fwrite(header, 1, 44, w->file);
    }
}

static void videoWavWriterPush(VideoWavWriter* w, const uint8_t* data, uint32_t size) {
    if (w->file != nullptr) {
        fwrite(data, 1, size, w->file);
        return;
    }
    if (w->pcmSize + size > w->pcmCapacity) {
        w->pcmCapacity = w->pcmSize + size + 65536;
        w->pcm = (uint8_t*) safeRealloc(w->pcm, w->pcmCapacity);
    }
    memcpy(w->pcm + w->pcmSize, data, size);
    w->pcmSize += size;
}

static bool videoWavWriterFinish(VideoWavWriter* w, const char* wavPath, FileSystem* fs) {
    if (w->file != nullptr) {
        long pos = ftell(w->file);
        if (pos > 44) {
            uint32_t dataSize = (uint32_t) (pos - 44);
            uint32_t riffSize = 36 + dataSize;
            if (fseek(w->file, 4, SEEK_SET) == 0) fwrite(&riffSize, 1, 4, w->file);
            if (fseek(w->file, 40, SEEK_SET) == 0) fwrite(&dataSize, 1, 4, w->file);
        }
        fclose(w->file);
        w->file = nullptr;
        return pos > 44;
    }
    if (w->pcm != nullptr && w->pcmSize > 0 && fs != nullptr) {
        uint32_t wavSize = 44 + w->pcmSize;
        uint8_t* wav = (uint8_t*) safeMalloc(wavSize);
        videoAudioWriteWavHeader(wav, w->pcmSize, w->channels, w->sampleRate);
        memcpy(wav + 44, w->pcm, w->pcmSize);
        bool wrote = fs->vtable->writeFileBinary(fs, wavPath, wav, (int32_t) wavSize);
        free(wav);
        return wrote;
    }
    return false;
}

static void videoAudioAppendConverted(VideoWavWriter* w, struct SwrContext* swrCtx, AVFrame* frame) {
    if (swrCtx == nullptr || frame->nb_samples <= 0) return;
    int32_t outSamples = swr_get_out_samples(swrCtx, frame->nb_samples);
    if (outSamples <= 0) return;
    int32_t bytes = outSamples * VIDEO_AUDIO_CHANNELS * (int32_t) sizeof(int16_t);
    uint8_t* tmp = (uint8_t*) safeMalloc((size_t) bytes);
    int32_t converted = swr_convert(swrCtx, &tmp, outSamples, (const uint8_t**) frame->data, frame->nb_samples);
    if (converted > 0) {
        uint32_t convBytes = (uint32_t) converted * VIDEO_AUDIO_CHANNELS * (uint32_t) sizeof(int16_t);
        videoWavWriterPush(w, tmp, convBytes);
    }
    free(tmp);
}

static bool videoAudioExtract(VideoWavWriter* w, const char* url) {
    AVFormatContext* fmtCtx = nullptr;
    char* ffmpegUrl = ffmpegFileUrl(url);
    int openResult = avformat_open_input(&fmtCtx, ffmpegUrl, nullptr, nullptr);
    free(ffmpegUrl);
    if (openResult != 0) return false;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }
    const AVCodec* audioCodec = nullptr;
    int32_t audioIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &audioCodec, 0);
    if (audioIndex < 0 || audioCodec == nullptr) {
        avformat_close_input(&fmtCtx);
        return false;
    }
    AVCodecContext* audioCodecCtx = avcodec_alloc_context3(audioCodec);
    if (audioCodecCtx == nullptr) {
        avformat_close_input(&fmtCtx);
        return false;
    }
    bool ok = false;
    if (avcodec_parameters_to_context(audioCodecCtx, fmtCtx->streams[audioIndex]->codecpar) == 0 &&
        avcodec_open2(audioCodecCtx, audioCodec, nullptr) == 0) {
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        struct SwrContext* swrCtx = nullptr;
        swr_alloc_set_opts2(&swrCtx, &outLayout, AV_SAMPLE_FMT_S16, VIDEO_AUDIO_SAMPLE_RATE,
                            &audioCodecCtx->ch_layout, audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate, 0, nullptr);
        if (swrCtx != nullptr) swr_init(swrCtx);
        AVFrame* frame = av_frame_alloc();
        if (frame != nullptr) {
            AVPacket packet = {0};
            while (av_read_frame(fmtCtx, &packet) >= 0) {
                if (packet.stream_index == audioIndex) {
                    if (avcodec_send_packet(audioCodecCtx, &packet) == 0) {
                        while (avcodec_receive_frame(audioCodecCtx, frame) == 0) {
                            videoAudioAppendConverted(w, swrCtx, frame);
                            av_frame_unref(frame);
                        }
                    }
                }
                av_packet_unref(&packet);
            }
            avcodec_send_packet(audioCodecCtx, nullptr);
            while (avcodec_receive_frame(audioCodecCtx, frame) == 0) {
                videoAudioAppendConverted(w, swrCtx, frame);
                av_frame_unref(frame);
            }
            ok = true;
            av_frame_free(&frame);
        }
        if (swrCtx != nullptr) swr_free(&swrCtx);
    }
    avcodec_free_context(&audioCodecCtx);
    avformat_close_input(&fmtCtx);
    return ok;
}

static bool ffmpegVideoDecoderExtractAudio(VideoDecoder* decoder, Runner* runner, const char* url) {
    if (runner == nullptr || runner->fileSystem == nullptr) return false;
    const char* wavPath = "butterscotch_video_extract.wav";
    char* absPath = runner->fileSystem->vtable->resolvePath(runner->fileSystem, wavPath);
    VideoWavWriter writer;
    videoWavWriterOpen(&writer, absPath, VIDEO_AUDIO_CHANNELS, VIDEO_AUDIO_SAMPLE_RATE);
    bool extracted = videoAudioExtract(&writer, url);
    bool finished = videoWavWriterFinish(&writer, wavPath, runner->fileSystem);
    free(absPath);
    return extracted && finished;
}

static VideoDecoderVtable ffmpegVideoDecoderVtable;

VideoDecoder* VideoDecoder_createBackend(void) {
    VideoDecoder* decoder = (VideoDecoder*) safeCalloc(1, sizeof(VideoDecoder));
    FfmpegVideoDecoder* impl = (FfmpegVideoDecoder*) safeCalloc(1, sizeof(FfmpegVideoDecoder));
    impl->videoStreamIndex = -1;
    impl->frameDurationSeconds = 1.0 / VIDEO_FALLBACK_FPS;
    ffmpegVideoDecoderVtable.init = ffmpegVideoDecoderInit;
    ffmpegVideoDecoderVtable.quit = ffmpegVideoDecoderQuit;
    ffmpegVideoDecoderVtable.open = ffmpegVideoDecoderOpen;
    ffmpegVideoDecoderVtable.close = ffmpegVideoDecoderClose;
    ffmpegVideoDecoderVtable.extractAudio = ffmpegVideoDecoderExtractAudio;
    ffmpegVideoDecoderVtable.isRunning = ffmpegVideoDecoderIsRunning;
    ffmpegVideoDecoderVtable.isPaused = ffmpegVideoDecoderIsPaused;
    ffmpegVideoDecoderVtable.pause = ffmpegVideoDecoderPause;
    ffmpegVideoDecoderVtable.resume = ffmpegVideoDecoderResume;
    ffmpegVideoDecoderVtable.setLoop = ffmpegVideoDecoderSetLoop;
    ffmpegVideoDecoderVtable.setMasterClock = ffmpegVideoDecoderSetMasterClock;
    ffmpegVideoDecoderVtable.update = ffmpegVideoDecoderUpdate;
    ffmpegVideoDecoderVtable.draw = ffmpegVideoDecoderDraw;
    ffmpegVideoDecoderVtable.duration = ffmpegVideoDecoderDuration;
    ffmpegVideoDecoderVtable.position = ffmpegVideoDecoderPosition;
    ffmpegVideoDecoderVtable.width = ffmpegVideoDecoderWidth;
    ffmpegVideoDecoderVtable.height = ffmpegVideoDecoderHeight;
    decoder->vtable = &ffmpegVideoDecoderVtable;
    decoder->impl = impl;
    return decoder;
}
