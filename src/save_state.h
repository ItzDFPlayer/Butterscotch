#ifndef _BS_SAVE_STATE_H_
#define _BS_SAVE_STATE_H_

#include <stdbool.h>

struct Runner;

// ===[ Save states ]===
//
// A save state is a snapshot of everything the VM owns: instances (including GML
// structs), global variables, the ds_* pools, the RNG, the view/camera and
// background state, and the current room. Loading restores that snapshot without
// running any game code, so a state taken before a hard section can be reloaded
// even when the game's own save points are far away.
//
// What is deliberately NOT part of a state: audio playback (music stops), GPU and
// renderer state (surfaces are rebuilt by the game), particles, video, sound
// emitters, and anything the game wrote to disk. Those either cannot be captured
// portably or are regenerated every frame.
//
// The file is tied to one data.win: a state records the game name, id and a few
// chunk counts, and loading refuses a mismatch.

bool SaveState_save(struct Runner* runner, const char* path);
bool SaveState_load(struct Runner* runner, const char* path);

#endif//_BS_SAVE_STATE_H_
