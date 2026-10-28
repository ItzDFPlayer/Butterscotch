#ifndef _BS_VIDEO_PLAYER_H_
#define _BS_VIDEO_PLAYER_H_

#include <stdbool.h>
#include <stdint.h>

struct Renderer;

// ===[ Video playback ]===
//
// GameMaker's video_* API. Deltarune Chapter 3 and 5 boot their cutscenes by
// opening an .mp4 from vid/ and blitting the decoded frames through a surface, so
// without a decoder those rooms simply never advance (the room draws nothing and
// waits for a "video_end" that never comes).
//
// Playback is decode-on-demand: every frame the player advances a wall clock and
// decodes until it has the newest frame that is not in the future, so a slow
// decoder degrades to a lower frame rate rather than the whole game slowing down.
//
// When this build has no libav* (see BUTTERSCOTCH_VIDEO), every entry point below
// degrades to "no video open", which makes the game take its own fallback path.

typedef struct VideoPlayer VideoPlayer;

// True when this build can decode video at all.
bool VideoPlayer_isSupported(void);

// Opens `path` (absolute). Returns NULL if the file is missing or cannot be
// decoded; callers must treat that as "no video".
VideoPlayer* VideoPlayer_open(const char* path);
void VideoPlayer_close(VideoPlayer* player);

// Advances playback by `deltaSeconds` and decodes the frame belonging to the new
// position. Returns true on the frame where playback reaches the end.
bool VideoPlayer_update(VideoPlayer* player, float deltaSeconds);

// 0 = playing (a frame is available), 1 = paused, 2 = finished/no video.
int VideoPlayer_status(const VideoPlayer* player);
// 0 = RGB frames, 1 = YUV (we always produce RGB, so this is always 0).
int VideoPlayer_format(const VideoPlayer* player);

float VideoPlayer_position(const VideoPlayer* player);
float VideoPlayer_duration(const VideoPlayer* player);

void VideoPlayer_setVolume(VideoPlayer* player, float volume);
void VideoPlayer_setLoop(VideoPlayer* player, bool loop);
void VideoPlayer_pause(VideoPlayer* player);
void VideoPlayer_resume(VideoPlayer* player);

int32_t VideoPlayer_width(const VideoPlayer* player);
int32_t VideoPlayer_height(const VideoPlayer* player);

// Copies the newest decoded frame into a renderer surface. Returns false when
// there is no frame or the renderer cannot take one.
bool VideoPlayer_uploadFrame(VideoPlayer* player, struct Renderer* renderer, int32_t surfaceId);

#endif//_BS_VIDEO_PLAYER_H_
