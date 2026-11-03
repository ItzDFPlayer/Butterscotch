#ifndef _BS_SAVE_STATE_H_
#define _BS_SAVE_STATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Runner;
struct DataWin;

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

// Why a load did not happen. The caller shows one short banner for a failed load,
// and "no state" for a file that exists but belongs to another chapter sends the
// wrong person looking for the wrong problem.
typedef enum SaveStateLoadResult {
    SaveStateLoad_Ok = 0,
    SaveStateLoad_BadArgs,
    SaveStateLoad_NoFile,   // nothing has been saved to that path yet
    SaveStateLoad_Unreadable, // exists, but truncated, or not this format/version
    SaveStateLoad_OtherGame,  // written by a different data.win
} SaveStateLoadResult;

SaveStateLoadResult SaveState_load(struct Runner* runner, const char* path);

// 32-bit identity of a data.win: the game name, its id and a few chunk counts. Used
// both to reject a state that belongs to another game and to give each game its own
// state file, so chapters and games cannot overwrite each other's snapshot.
uint32_t SaveState_gameId(struct DataWin* dataWin);

// Renders the identity fields behind SaveState_gameId into buf, so a mismatch can be
// reported with the part that moved (name, game id, chunk counts) instead of just
// "different game".
void SaveState_describeIdentity(struct DataWin* dataWin, char* buf, size_t size);

// Fills buf with the state file to use for the running game: a per-chapter name built
// from the folder dataWinPath lives in, so chapters sharing one save folder each keep
// their own snapshot. A shared "butterscotch-state.sav" from an older build is renamed
// into place when its fingerprint matches this game.
void SaveState_defaultPath(const char* saveDir, const char* dataWinPath, struct DataWin* dataWin,
                           char* buf, size_t size);

#endif//_BS_SAVE_STATE_H_
