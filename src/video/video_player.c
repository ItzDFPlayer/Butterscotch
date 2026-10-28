#include "video_player.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "common.h"
#include "log.h"
#include "renderer.h"

#if defined(BUTTERSCOTCH_VIDEO)

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

// How many frames we are willing to decode in a single update. A Cortex-A7 cannot
// decode H.264 at 640x480/30, so without a cap a slow machine would spend the whole
// frame here and never present anything; instead we decode a few frames, present the
// newest one and let the video run behind the clock.
#define VIDEO_MAX_DECODES_PER_UPDATE 4

struct VideoPlayer
{
    AVFormatContext* fmt;
    AVCodecContext* decoder;
    struct SwsContext* scaler;
    AVPacket* packet;
    AVFrame* frame;
    int streamIndex;

    uint16_t* rgb;      // width * height packed RGB555, in the renderer's pixel format
    int32_t width;
    int32_t height;

    double duration;
    double position;    // playback clock, seconds
    double frameTime;   // timestamp of the frame currently in `rgb`, seconds

    float volume;
    bool loop;
    bool paused;
    bool finished;
    bool frameValid;
    bool draining;      // the demuxer hit EOF, only decoder output is left
};

bool VideoPlayer_isSupported(void)
{
    return true;
}

static double videoStreamSeconds(const VideoPlayer* player, int64_t pts)
{
    AVRational tb = player->fmt->streams[player->streamIndex]->time_base;
    return (double) pts * av_q2d(tb);
}

static void videoResetStream(VideoPlayer* player)
{
    av_seek_frame(player->fmt, player->streamIndex, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(player->decoder);

    player->draining = false;
    player->finished = false;
    player->position = 0.0;
    player->frameTime = -1.0;
}

// Converts the decoded frame into `player->rgb`.
static bool videoConvertFrame(VideoPlayer* player, AVFrame* frame)
{
    if (frame->width != player->width || frame->height != player->height) {
        int32_t w = frame->width, h = frame->height;
        if (w <= 0 || h <= 0) return false;
        uint16_t* rgb = (uint16_t*) safeRealloc(player->rgb, (size_t) w * (size_t) h * sizeof(uint16_t));
        if (rgb == nullptr) return false;
        player->rgb = rgb;
        player->width = w;
        player->height = h;
        if (player->scaler != nullptr) {
            sws_freeContext(player->scaler);
            player->scaler = nullptr;
        }
    }

    if (player->scaler == nullptr) {
        player->scaler = sws_getContext(
            player->width, player->height, (enum AVPixelFormat) frame->format,
            player->width, player->height, AV_PIX_FMT_RGB555LE,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (player->scaler == nullptr) {
            logWarn("Video: no swscale path from %s to RGB555\n",
                    av_get_pix_fmt_name((enum AVPixelFormat) frame->format));
            return false;
        }
    }

    uint8_t* dstData[4] = { (uint8_t*) player->rgb, nullptr, nullptr, nullptr };
    int dstLinesize[4] = { player->width * 2, 0, 0, 0 };
    sws_scale(player->scaler, (const uint8_t* const*) frame->data, frame->linesize,
              0, player->height, dstData, dstLinesize);
    return true;
}

// Decodes until the newest frame is at or after `target` seconds, until `budget`
// frames have been decoded, or until the stream runs out. Returns false at the end
// of the stream. `gotFrame` reports whether `rgb` was refreshed.
static bool videoAdvanceTo(VideoPlayer* player, double target, int budget, bool* gotFrame)
{
    *gotFrame = false;
    int decoded = 0;

    while (decoded < budget)
    {
        int32_t ret = avcodec_receive_frame(player->decoder, player->frame);

        if (ret == 0)
        {
            decoded++;
            if (player->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                player->frameTime = videoStreamSeconds(player, player->frame->best_effort_timestamp);
            if (videoConvertFrame(player, player->frame)) {
                *gotFrame = true;
                player->frameValid = true;
            }
            if (player->frameTime >= target)
                return true;
            continue;
        }

        if (ret == AVERROR_EOF)
            return false;

        if (ret != AVERROR(EAGAIN)) {
            logWarn("Video: decode error %d\n", ret);
            return false;
        }

        // The decoder wants more input.
        if (player->draining)
            return false;

        for (;;)
        {
            int32_t readRet = av_read_frame(player->fmt, player->packet);
            if (readRet < 0) {
                player->draining = true;
                avcodec_send_packet(player->decoder, nullptr);   // flush
                break;
            }
            bool isVideo = player->packet->stream_index == player->streamIndex;
            int32_t sendRet = 0;
            if (isVideo)
                sendRet = avcodec_send_packet(player->decoder, player->packet);
            av_packet_unref(player->packet);
            if (isVideo) {
                if (sendRet < 0 && sendRet != AVERROR(EAGAIN))
                    logWarn("Video: send_packet failed (%d)\n", sendRet);
                break;
            }
        }
    }

    return true;
}

VideoPlayer* VideoPlayer_open(const char* path)
{
    VideoPlayer* player = (VideoPlayer*) safeCalloc(1, sizeof(VideoPlayer));
    if (player == nullptr) return nullptr;

    player->volume = 1.0f;
    player->streamIndex = -1;
    player->frameTime = -1.0;

    if (avformat_open_input(&player->fmt, path, nullptr, nullptr) < 0) {
        logWarn("Video: could not open '%s'\n", path);
        VideoPlayer_close(player);
        return nullptr;
    }

    if (avformat_find_stream_info(player->fmt, nullptr) < 0) {
        logWarn("Video: no stream info in '%s'\n", path);
        VideoPlayer_close(player);
        return nullptr;
    }

    const AVCodec* codec = nullptr;
    player->streamIndex = av_find_best_stream(player->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (player->streamIndex < 0 || codec == nullptr) {
        logWarn("Video: no video stream in '%s'\n", path);
        VideoPlayer_close(player);
        return nullptr;
    }

    AVStream* stream = player->fmt->streams[player->streamIndex];
    player->decoder = avcodec_alloc_context3(codec);
    if (player->decoder == nullptr || avcodec_parameters_to_context(player->decoder, stream->codecpar) < 0) {
        VideoPlayer_close(player);
        return nullptr;
    }

    // The handheld has four cores at most; more threads just add contention.
    player->decoder->thread_count = 1;

    if (avcodec_open2(player->decoder, codec, nullptr) < 0) {
        logWarn("Video: no decoder for '%s' (%s)\n", path, codec->name);
        VideoPlayer_close(player);
        return nullptr;
    }

    player->width = player->decoder->width;
    player->height = player->decoder->height;
    if (player->width <= 0 || player->height <= 0) {
        VideoPlayer_close(player);
        return nullptr;
    }

    player->rgb = (uint16_t*) safeMalloc((size_t) player->width * (size_t) player->height * sizeof(uint16_t));
    if (player->rgb == nullptr) {
        VideoPlayer_close(player);
        return nullptr;
    }
    memset(player->rgb, 0, (size_t) player->width * (size_t) player->height * sizeof(uint16_t));

    player->duration = (double) stream->duration * av_q2d(stream->time_base);
    if (player->duration <= 0.0 && player->fmt->duration != AV_NOPTS_VALUE)
        player->duration = (double) player->fmt->duration / (double) AV_TIME_BASE;

    player->packet = av_packet_alloc();
    player->frame = av_frame_alloc();
    if (player->packet == nullptr || player->frame == nullptr) {
        VideoPlayer_close(player);
        return nullptr;
    }

    logInfo("Video: opened '%s' (%dx%d, %.1fs)\n", path, player->width, player->height, player->duration);

    // Decode the first frame straight away so the very first Draw has something to
    // show instead of a black surface.
    bool gotFrame = false;
    videoAdvanceTo(player, 0.0, VIDEO_MAX_DECODES_PER_UPDATE * 4, &gotFrame);

    return player;
}

void VideoPlayer_close(VideoPlayer* player)
{
    if (player == nullptr) return;

    if (player->scaler != nullptr) sws_freeContext(player->scaler);
    if (player->frame != nullptr) av_frame_free(&player->frame);
    if (player->packet != nullptr) av_packet_free(&player->packet);
    if (player->decoder != nullptr) avcodec_free_context(&player->decoder);
    if (player->fmt != nullptr) avformat_close_input(&player->fmt);
    free(player->rgb);
    free(player);
}

bool VideoPlayer_update(VideoPlayer* player, float deltaSeconds)
{
    if (player == nullptr) return false;
    if (player->paused || player->finished) return false;

    if (deltaSeconds < 0.0f) deltaSeconds = 0.0f;
    if (deltaSeconds > 0.25f) deltaSeconds = 0.25f;

    player->position += (double) deltaSeconds;

    bool gotFrame = false;
    bool more = videoAdvanceTo(player, player->position, VIDEO_MAX_DECODES_PER_UPDATE, &gotFrame);

    // If the decoder cannot keep up with real time, follow the frames instead of
    // skipping them: a slow machine should show the video in slow motion rather than
    // showing a third of it at the right speed.
    if (gotFrame && player->position > player->frameTime + 0.25)
        player->position = player->frameTime;

    if (!more)
    {
        if (player->loop)
        {
            videoResetStream(player);
            return false;
        }
        player->finished = true;
        return true;
    }

    return false;
}

int VideoPlayer_status(const VideoPlayer* player)
{
    if (player == nullptr) return 2;  // no video open
    if (player->finished) return 2;
    if (player->paused) return 1;
    return 0;                          // playing
}

int VideoPlayer_format(const VideoPlayer* player)
{
    (void) player;
    return 0;                          // RGB frames; the YUV shader path is not used
}

float VideoPlayer_position(const VideoPlayer* player)
{
    return player == nullptr ? 0.0f : (float) player->position;
}

float VideoPlayer_duration(const VideoPlayer* player)
{
    return player == nullptr ? 0.0f : (float) player->duration;
}

void VideoPlayer_setVolume(VideoPlayer* player, float volume)
{
    if (player == nullptr) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    player->volume = volume;
}

void VideoPlayer_setLoop(VideoPlayer* player, bool loop)
{
    if (player == nullptr) return;
    player->loop = loop;
}

void VideoPlayer_pause(VideoPlayer* player)
{
    if (player == nullptr) return;
    player->paused = true;
}

void VideoPlayer_resume(VideoPlayer* player)
{
    if (player == nullptr) return;
    player->paused = false;
}

int32_t VideoPlayer_width(const VideoPlayer* player)
{
    return player == nullptr ? 0 : player->width;
}

int32_t VideoPlayer_height(const VideoPlayer* player)
{
    return player == nullptr ? 0 : player->height;
}

bool VideoPlayer_uploadFrame(VideoPlayer* player, Renderer* renderer, int32_t surfaceId)
{
    if (player == nullptr || renderer == nullptr) return false;
    if (player->rgb == nullptr || !player->frameValid) return false;

    // Frames are decoded at full resolution, so the surface is always the same size
    // and the copy is a plain one-to-one upload.
    return Renderer_surfaceUploadRgb555(renderer, surfaceId, player->rgb,
                                        player->width, player->height, player->width);
}

#else // !BUTTERSCOTCH_VIDEO

// No libav in this build: the game is told there is no video, so its own fallback
// path (which every console version of Deltarune uses) takes over.

struct VideoPlayer
{
    int unused;
};

bool VideoPlayer_isSupported(void) { return false; }
VideoPlayer* VideoPlayer_open(const char* path) { (void) path; return nullptr; }
void VideoPlayer_close(VideoPlayer* player) { (void) player; }
bool VideoPlayer_update(VideoPlayer* player, float deltaSeconds) { (void) player; (void) deltaSeconds; return false; }
int VideoPlayer_status(const VideoPlayer* player) { (void) player; return 2; }
int VideoPlayer_format(const VideoPlayer* player) { (void) player; return 0; }
float VideoPlayer_position(const VideoPlayer* player) { (void) player; return 0.0f; }
float VideoPlayer_duration(const VideoPlayer* player) { (void) player; return 0.0f; }
void VideoPlayer_setVolume(VideoPlayer* player, float volume) { (void) player; (void) volume; }
void VideoPlayer_setLoop(VideoPlayer* player, bool loop) { (void) player; (void) loop; }
void VideoPlayer_pause(VideoPlayer* player) { (void) player; }
void VideoPlayer_resume(VideoPlayer* player) { (void) player; }
int32_t VideoPlayer_width(const VideoPlayer* player) { (void) player; return 0; }
int32_t VideoPlayer_height(const VideoPlayer* player) { (void) player; return 0; }
bool VideoPlayer_uploadFrame(VideoPlayer* player, Renderer* renderer, int32_t surfaceId)
{
    (void) player; (void) renderer; (void) surfaceId;
    return false;
}

#endif//BUTTERSCOTCH_VIDEO
