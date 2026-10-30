#include <ctype.h>
#include "string_compat.h"
#include "stdio_compat.h"
#include <ctype.h>
#include <stdlib.h>

#include <SDL/SDL.h>

#include "common.h"
#include "input_recording.h"
#include "platformdefs.h"
#include "gettime.h"

#ifndef SDL_BUTTON_WHEELUP
#define SDL_BUTTON_WHEELUP 4
#endif
#ifndef SDL_BUTTON_WHEELDOWN
#define SDL_BUTTON_WHEELDOWN 5
#endif
#include "runner_mouse.h"

static Runner *g_runner;
static int32_t fbWidth, fbHeight;
static SDL_Surface* scr;
static SDL_Joystick* openJoysticks[MAX_GAMEPADS];

typedef struct {
    bool valid;
    int max_button;
    int max_axis;
    int max_hat;

    int button_map[GP_BUTTON_COUNT];
    int axis_map[GP_AXIS_COUNT];
    int hat_map[GP_BUTTON_COUNT];
    int hat_mask[GP_BUTTON_COUNT];
    int axis_button_map[GP_BUTTON_COUNT];
    float axis_button_sign[GP_BUTTON_COUNT];
} GamepadMapping;

static GamepadMapping joystickMappings[MAX_GAMEPADS];
// Slots whose "pad present but unmapped" line was already logged.
static bool unmappedReported[MAX_GAMEPADS];
// SDL 1.2 keysyms are well below this; used to remember which keys were logged once.
#define SDL_LOG_KEY_SYMS 1024

static void parseMappingField(GamepadMapping* mapping, const char* key, const char* val) {
    int gml_btn = -1;
    int gml_axis = -1;

    if (strcmp(key, "a") == 0) gml_btn = 0;
    else if (strcmp(key, "b") == 0) gml_btn = 1;
    else if (strcmp(key, "x") == 0) gml_btn = 2;
    else if (strcmp(key, "y") == 0) gml_btn = 3;
    else if (strcmp(key, "leftshoulder") == 0) gml_btn = 4;
    else if (strcmp(key, "rightshoulder") == 0) gml_btn = 5;
    else if (strcmp(key, "lefttrigger") == 0) gml_btn = 6;
    else if (strcmp(key, "righttrigger") == 0) gml_btn = 7;
    else if (strcmp(key, "back") == 0) gml_btn = 8;
    else if (strcmp(key, "start") == 0) gml_btn = 9;
    else if (strcmp(key, "leftstick") == 0) gml_btn = 10;
    else if (strcmp(key, "rightstick") == 0) gml_btn = 11;
    else if (strcmp(key, "dpup") == 0) gml_btn = 12;
    else if (strcmp(key, "dpdown") == 0) gml_btn = 13;
    else if (strcmp(key, "dpleft") == 0) gml_btn = 14;
    else if (strcmp(key, "dpright") == 0) gml_btn = 15;
    else if (strcmp(key, "guide") == 0) gml_btn = 16;
    else if (strcmp(key, "leftx") == 0) gml_axis = 0;
    else if (strcmp(key, "lefty") == 0) gml_axis = 1;
    else if (strcmp(key, "rightx") == 0) gml_axis = 2;
    else if (strcmp(key, "righty") == 0) gml_axis = 3;

    if (gml_btn == -1 && gml_axis == -1) return;

    if (val[0] == 'b') {
        int b = atoi(val + 1);
        if (gml_btn != -1) {
            mapping->button_map[gml_btn] = b;
            if (b > mapping->max_button) mapping->max_button = b;
        }
    } else if (val[0] == 'a') {
        int a = atoi(val + 1);
        if (gml_axis != -1) {
            mapping->axis_map[gml_axis] = a;
            if (a > mapping->max_axis) mapping->max_axis = a;
        } else if (gml_btn != -1) {
            mapping->axis_button_map[gml_btn] = a;
            mapping->axis_button_sign[gml_btn] = 1.0f;
            if (a > mapping->max_axis) mapping->max_axis = a;
        }
    } else if (val[0] == '+' && val[1] == 'a') {
        int a = atoi(val + 2);
        if (gml_btn != -1) {
            mapping->axis_button_map[gml_btn] = a;
            mapping->axis_button_sign[gml_btn] = 1.0f;
            if (a > mapping->max_axis) mapping->max_axis = a;
        } else if (gml_axis != -1) {
            mapping->axis_map[gml_axis] = a;
            if (a > mapping->max_axis) mapping->max_axis = a;
        }
    } else if (val[0] == '-' && val[1] == 'a') {
        int a = atoi(val + 2);
        if (gml_btn != -1) {
            mapping->axis_button_map[gml_btn] = a;
            mapping->axis_button_sign[gml_btn] = -1.0f;
            if (a > mapping->max_axis) mapping->max_axis = a;
        } else if (gml_axis != -1) {
            mapping->axis_map[gml_axis] = a;
            if (a > mapping->max_axis) mapping->max_axis = a;
        }
    } else if (val[0] == 'h') {
        int h = atoi(val + 1);
        const char* dot = strchr(val, '.');
        if (dot && gml_btn != -1) {
            int dir = atoi(dot + 1);
            mapping->hat_map[gml_btn] = h;
            mapping->hat_mask[gml_btn] = dir;
            if (h > mapping->max_hat) mapping->max_hat = h;
        }
    }
}

#if defined(_WIN32) || defined(__WIN32__) || defined(__WINDOWS__)
#define MAPPING_PLATFORM_STR "platform:Windows"
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
        #define MAPPING_PLATFORM_STR "platform:iOS"
    #else
        #define MAPPING_PLATFORM_STR "platform:Mac OS X"
    #endif
#elif defined(__ANDROID__)
#define MAPPING_PLATFORM_STR "platform:Android"
#elif defined(__linux__) || defined(__gnu_linux__)
#define MAPPING_PLATFORM_STR "platform:Linux"
#else
#define MAPPING_PLATFORM_STR "platform:Unknown" //Gamepad wont work at all here
#endif

static void loadGamepadMappings(void) {
    const char* dbPath = "gamecontrollerdb.txt";
    FILE* f = fopen(dbPath, "rb");
    if (!f) {
        logWarn("Gamepad: SDL gamecontrollerdb.txt not found at %s, ignoring mappings\n", dbPath);
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') continue;

        char* guid = line;
        char* name = strchr(guid, ',');
        if (!name) continue;
        *name++ = '\0';

        char* mapping_str = strchr(name, ',');
        if (!mapping_str) continue;
        *mapping_str++ = '\0';

        if (strstr(mapping_str, "platform:") != NULL && strstr(mapping_str, MAPPING_PLATFORM_STR) == NULL) {
            continue;
        }

        GamepadMapping temp;
        temp.valid = true;
        temp.max_button = -1;
        temp.max_axis = -1;
        temp.max_hat = -1;
        for (int i = 0; i < GP_BUTTON_COUNT; i++) {
            temp.button_map[i] = -1;
            temp.hat_map[i] = -1;
            temp.hat_mask[i] = -1;
            temp.axis_button_map[i] = -1;
            temp.axis_button_sign[i] = 1.0f;
        }
        for (int i = 0; i < GP_AXIS_COUNT; i++) {
            temp.axis_map[i] = -1;
        }

        char* token = strtok(mapping_str, ",");
        while (token) {
            char* colon = strchr(token, ':');
            if (colon) {
                *colon = '\0';
                parseMappingField(&temp, token, colon + 1);
            }
            token = strtok(NULL, ",");
        }

        for (int i = 0; i < MAX_GAMEPADS; i++) {
            SDL_Joystick* joy = openJoysticks[i];
            if (joy && !joystickMappings[i].valid) {
                const char* jname = SDL_JoystickName(i);
                if (jname && strcasecmp(jname, name) == 0) {
                    joystickMappings[i] = temp;
                    logInfo("Gamepad: Mapped '%s' (slot %d)\n", jname, i);
                }
            }
        }
    }
    fclose(f);
}
void platformSetWindowTitle(const char* title) {
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", title);
    SDL_WM_SetCaption(windowTitle, NULL);
}

bool platformGetWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    *outW = fbWidth;
    *outH = fbHeight;
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;
    fbWidth = width;
    fbHeight = height;
    // 16bpp for the software renderer: the handheld's panel and our framebuffer are
    // both RGB555, so the screen surface is then a straight copy target instead of
    // something SDL has to convert per pixel. With 0 here the driver hands back a
    // 32bpp shadow buffer, and every frame pays two full-frame conversions
    // (555 -> 32 -> panel), which measured ~18 ms on the device.
    scr = SDL_SetVideoMode(width, height, gfx == SOFTWARE ? 16 : 0,
                           (gfx == SOFTWARE ? 0 : SDL_OPENGL) | SDL_RESIZABLE);
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos) return;
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);

    *xPos = (double)mx;
    *yPos = (double)my;
}

static bool platformGetWindowFocus(void) {
    return SDL_GetAppState() & SDL_APPINPUTFOCUS;
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    if (headless && gfx != SOFTWARE) {
        logError("Headless mode on SDL 1.2 requires the software renderer!\n");
        return false;
    }

    // Init SDL
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_JOYSTICK)) {
        logError("Failed to initialize SDL\n");
        return false;
    }

    SDL_JoystickEventState(SDL_IGNORE);
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        memset(&joystickMappings[i], 0, sizeof(GamepadMapping));
        joystickMappings[i].valid = false;

        if (i < numJoysticks) {
            openJoysticks[i] = SDL_JoystickOpen(i);
        } else {
            openJoysticks[i] = NULL;
        }
    }
    loadGamepadMappings();

    fbWidth = reqW;
    fbHeight = reqH;
    if(!headless) {
        scr = SDL_SetVideoMode(fbWidth, fbHeight, gfx == SOFTWARE ? 16 : 0,
                               (gfx == SOFTWARE ? 0 : SDL_OPENGL) | SDL_RESIZABLE);
        if (!scr && gfx == SOFTWARE) {
            SDL_Rect** modes = SDL_ListModes(NULL, SDL_FULLSCREEN);
            if (modes && modes != (SDL_Rect**) -1 && modes[0]) {
                logWarn("%dx%d unavailable, falling back to %dx%d: %s\n",
                        reqW, reqH, modes[0]->w, modes[0]->h, SDL_GetError());
                scr = SDL_SetVideoMode(modes[0]->w, modes[0]->h, 16, 0);
                fbWidth = modes[0]->w;
                fbHeight = modes[0]->h;
            }
        }
        if (!scr) {
            logError("Fatal: Could not set any video mode: %s\n", SDL_GetError());
            return false;
        }
    }

    SDL_WM_SetCaption(title, NULL);

    SDL_EnableKeyRepeat(0, 0);

    return true;
}

void platformExit(void) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (openJoysticks[i]) {
            SDL_JoystickClose(openJoysticks[i]);
            openJoysticks[i] = NULL;
        }
    }
    SDL_Quit();
}

static void platformSetCursor(int32_t cursorType) {
    // SDL1.2 only supports showing/hiding
    SDL_ShowCursor(cursorType == GML_CR_NONE ? SDL_DISABLE : SDL_ENABLE);
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->windowHasFocus = platformGetWindowFocus;
    runner->setCursor = platformSetCursor;
    runner->currentCursor = GML_CR_DEFAULT;
}

#ifdef ENABLE_SW_RENDERER

static SDL_Surface* nextFb = NULL;

void platformSetNextFramebuffer(uint32_t* framebuffer, int width, int height) {
    // Called once per frame with the same buffer, so only rebuild the wrapper when
    // the buffer or its geometry actually changed.
    if (nextFb && nextFb->pixels == (void*) framebuffer && nextFb->w == width && nextFb->h == height)
        return;

    if (nextFb) {
        SDL_FreeSurface(nextFb);
        nextFb = NULL;
    }

    nextFb = SDL_CreateRGBSurfaceFrom(
        framebuffer,
        width,
        height,
        16,
        width * 2,
        0x7C00, // Rmask
        0x03E0, // Gmask
        0x001F, // Bmask
        0x0000  // Amask
    );
}

// Copies the finished 16bpp framebuffer into the display surface.
// SDL_BlitSurface goes through the generic blitter, which on this handheld costs
// much more than the copy itself (the screen is either a shadow buffer or video
// memory). Both surfaces are plain 16bpp rows of the same size, so copying row by
// row is both exact and far cheaper.
static void swrBlitToScreen(void) {
    if (nextFb == NULL || scr == NULL) return;

    // Only a straight pixel copy when the screen really has our RGB555 layout;
    // anything else still goes through SDL, which converts per pixel.
    if (nextFb->w == scr->w && nextFb->h == scr->h &&
        nextFb->format->BitsPerPixel == 16 && scr->format->BitsPerPixel == 16 &&
        scr->format->Rmask == 0x7C00 && scr->format->Gmask == 0x03E0 &&
        scr->format->Bmask == 0x001F) {
        const uint8_t* src = (const uint8_t*) nextFb->pixels;
        uint8_t* dst = (uint8_t*) scr->pixels;
        size_t rowBytes = (size_t) nextFb->w * 2;
        for (int y = 0; y < nextFb->h; y++) {
            memcpy(dst + (size_t) y * scr->pitch, src + (size_t) y * nextFb->pitch, rowBytes);
        }
        return;
    }

    SDL_BlitSurface(nextFb, NULL, scr, NULL);
}

#endif

void platformSwapBuffers(void) {
#ifdef ENABLE_SW_RENDERER
    if(gfx == SOFTWARE) {
        if (!scr)
            return;
        // One line about the display surface: whether the screen is a shadow buffer
        // or video memory decides how expensive presenting a frame can be.
        static bool reportedScreen = false;
        if (!reportedScreen) {
            reportedScreen = true;
            logInfo("Video: screen %dx%d %dbpp, pitch %d, masks %04x/%04x/%04x, %s\n",
                    scr->w, scr->h, scr->format->BitsPerPixel, scr->pitch,
                    (unsigned) scr->format->Rmask, (unsigned) scr->format->Gmask,
                    (unsigned) scr->format->Bmask,
                    (scr->flags & SDL_HWSURFACE) ? "video memory" : "shadow buffer");
        }
        swrBlitToScreen();
        SDL_Flip(scr);
    }
#endif
#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
    if (gfx == LEGACY_GL || gfx == MODERN_GL)
        SDL_GL_SwapBuffers();
#endif
}

#if defined(ENABLE_MODERN_GL) || defined(ENABLE_LEGACY_GL)

void *platformGetProcAddress(const char *name) {
    return SDL_GL_GetProcAddress(name);
}

#endif

static int32_t SDLKeyToGml(int sdlkey) {
    // miyoomini controls for undertale hacky wacky
    switch (sdlkey) {
        case SDLK_SPACE: return toupper('z');
        case SDLK_LCTRL: return toupper('x');
        case SDLK_LSHIFT: return toupper('c');
    }
    // Letters and numbers are the same as GML
    if (sdlkey >= 'a' && sdlkey <= 'z') return toupper(sdlkey);
    if (sdlkey >= '0' && sdlkey <= '9') return sdlkey;
    // Special keys need mapping
    switch (sdlkey) {
        case SDLK_ESCAPE:    return VK_ESCAPE;
        case SDLK_RETURN:    return VK_ENTER;
        case SDLK_TAB:       return VK_TAB;
        case SDLK_BACKSPACE: return VK_BACKSPACE;
        case SDLK_SPACE:     return VK_SPACE;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:    return VK_SHIFT;
        case SDLK_LCTRL:
        case SDLK_RCTRL:     return VK_CONTROL;
        case SDLK_LALT:
        case SDLK_RALT:      return VK_ALT;
case SDLK_KP8:        case SDLK_UP:        return VK_UP;
case SDLK_KP5:case SDLK_KP2:        case SDLK_DOWN:      return VK_DOWN;
case SDLK_KP4:        case SDLK_LEFT:      return VK_LEFT;
case SDLK_KP6:        case SDLK_RIGHT:     return VK_RIGHT;
        case SDLK_F1:        return VK_F1;
        case SDLK_F2:        return VK_F2;
        case SDLK_F3:        return VK_F3;
        case SDLK_F4:        return VK_F4;
        case SDLK_F5:        return VK_F5;
        case SDLK_F6:        return VK_F6;
        case SDLK_F7:        return VK_F7;
        case SDLK_F8:        return VK_F8;
        case SDLK_F9:        return VK_F9;
        case SDLK_F10:       return VK_F10;
        case SDLK_F11:       return VK_F11;
        case SDLK_F12:       return VK_F12;
        case SDLK_INSERT:    return VK_INSERT;
        case SDLK_DELETE:    return VK_DELETE;
        case SDLK_HOME:      return VK_HOME;
        case SDLK_END:       return VK_END;
        case SDLK_PAGEUP:    return VK_PAGEUP;
        case SDLK_PAGEDOWN:  return VK_PAGEDOWN;
        default:             return -1; // Unknown
    }
}

static void platformResetJoysticks(void) {
    char oldNames[MAX_GAMEPADS][256] = {0};
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (openJoysticks[i]) {
            const char* name = SDL_JoystickName(i);
            if (name) {
                strncpy(oldNames[i], name, sizeof(oldNames[i]) - 1);
            }
            SDL_JoystickClose(openJoysticks[i]);
            openJoysticks[i] = NULL;
        }
    }

    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    SDL_JoystickEventState(SDL_IGNORE);

    int numJoysticks = SDL_NumJoysticks();
    bool needsRemap = false;
    for (int i = 0; i < numJoysticks && i < MAX_GAMEPADS; i++) {
        openJoysticks[i] = SDL_JoystickOpen(i);
        const char* newName = SDL_JoystickName(i);
        if (!newName) newName = "";
        if (strcmp(oldNames[i], newName) != 0) {
            joystickMappings[i].valid = false;
            needsRemap = true;
        }
    }

    for (int i = numJoysticks; i < MAX_GAMEPADS; i++) {
        joystickMappings[i].valid = false;
    }

    if (needsRemap) {
        loadGamepadMappings();
    }
}

static int32_t SDLMouseButtonToGml(int sdlButton) {
    switch (sdlButton) {
        case SDL_BUTTON_LEFT: return GML_MB_LEFT;
        case SDL_BUTTON_RIGHT: return GML_MB_RIGHT;
        case SDL_BUTTON_MIDDLE: return GML_MB_MIDDLE;
        default: return -1;
    }
}

bool platformHandleEvents(void) {
    SDL_JoystickUpdate();

    g_runner->gamepads->connectedCount = 0;
    for (int slotIdx = 0; slotIdx < MAX_GAMEPADS; slotIdx++) {
        GamepadSlot* slot = g_runner->gamepads->slots + slotIdx;
        SDL_Joystick* joy = openJoysticks[slotIdx];

        memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
        memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
        memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
        memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
        memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
        memset(slot->axisValue, 0, sizeof(slot->axisValue));

        if (joy != NULL) {
            slot->connected = true;
            slot->jid = slotIdx;

            const char* name = SDL_JoystickName(slotIdx);
            if (name != NULL) {
                strncpy(slot->description, name, sizeof(slot->description) - 1);
                slot->description[sizeof(slot->description) - 1] = '\0';
            } else {
                slot->description[0] = '\0';
            }

            slot->guid[0] = '\0';

            GamepadMapping* map = &joystickMappings[slotIdx];

            if (map->valid) {
                for (int btn = 0; btn < GP_BUTTON_COUNT; btn++) {
                    if (map->button_map[btn] != -1) {
                        if (SDL_JoystickGetButton(joy, map->button_map[btn])) {
                            slot->buttonDown[btn] = true;
                            slot->buttonValue[btn] = 1.0f;
                        }
                    }
                    if (map->hat_map[btn] != -1) {
                        Uint8 hat = SDL_JoystickGetHat(joy, map->hat_map[btn]);
                        if (hat & map->hat_mask[btn]) {
                            slot->buttonDown[btn] = true;
                            slot->buttonValue[btn] = 1.0f;
                        }
                    }
                    if (map->axis_button_map[btn] != -1) {
                        Sint16 val = SDL_JoystickGetAxis(joy, map->axis_button_map[btn]);
                        float norm = val / 32767.0f;
                        if (map->axis_button_sign[btn] < 0) norm = -norm;
                        if (norm < 0.0f) norm = 0.0f;

                        if (norm > slot->buttonValue[btn]) {
                            slot->buttonValue[btn] = norm;
                        }
                        if (slot->buttonValue[btn] >= slot->triggerThreshold) {
                            slot->buttonDown[btn] = true;
                        }
                    }
                }

                for (int a = 0; a < GP_AXIS_COUNT; a++) {
                    if (map->axis_map[a] != -1) {
                        Sint16 val = SDL_JoystickGetAxis(joy, map->axis_map[a]);
                        slot->axisValue[a] = val / 32767.0f;
                    }
                }
            } else if (!unmappedReported[slotIdx]) {
                // Worth one line: with no entry the runner reads no buttons from this
                // pad at all, which quietly disables every gamepad shortcut.
                unmappedReported[slotIdx] = true;
                logInfo("Gamepad: slot %d ('%s') has no gamecontrollerdb entry, its buttons are ignored\n",
                        slotIdx, slot->description);
            }

            for (int btn = 0; GP_BUTTON_COUNT > btn; btn++) {
                bool wasDown = slot->buttonDownPrev[btn];
                if (slot->buttonDown[btn] && !wasDown) slot->buttonPressed[btn] = true;
                if (!slot->buttonDown[btn] && wasDown) slot->buttonReleased[btn] = true;
            }
            g_runner->gamepads->connectedCount++;
        } else {
            slot->connected = false;
        }
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch(e.type) {
            case SDL_ACTIVEEVENT:
                if ((e.active.state & SDL_APPINPUTFOCUS) && e.active.gain) {
                    platformResetJoysticks();
                }
                break;
            case SDL_KEYDOWN:
                // SDL1.2 needs to manually intercept Alt+F4 to exit properly
                if (e.key.keysym.sym == SDLK_F4 && (e.key.keysym.mod & KMOD_ALT)) {
                    return true;
                }
                // During playback, suppress real keyboard input
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                {
                    int32_t gmlKey = SDLKeyToGml(e.key.keysym.sym);
                    // Handhelds deliver their pad as keyboard events, and the key a
                    // physical button sends differs from unit to unit, so the first press
                    // of every distinct key is logged: a handful of lines per session, and
                    // enough to identify a button. BUTTERSCOTCH_LOG_KEYS=1 logs every press.
                    static bool loggedSyms[SDL_LOG_KEY_SYMS];
                    static int logEveryKey = -1;
                    if (logEveryKey < 0) {
                        const char* env = getenv("BUTTERSCOTCH_LOG_KEYS");
                        logEveryKey = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
                    }
                    unsigned int sym = (unsigned int) e.key.keysym.sym;
                    bool seen = sym < SDL_LOG_KEY_SYMS ? loggedSyms[sym] : false;
                    if (logEveryKey || !seen) {
                        if (sym < SDL_LOG_KEY_SYMS) loggedSyms[sym] = true;
                        // The scancode is what identifies a button that SDL knows no
                        // keysym for - those arrive as 'unknown key' with sym 0.
                        logInfo("Key down: sdl='%s' (%d) scancode %d -> gml %d\n",
                                SDL_GetKeyName(e.key.keysym.sym),
                                (int) e.key.keysym.sym,
                                (int) e.key.keysym.scancode,
                                gmlKey);
                    }
                    RunnerKeyboard_onKeyDown(g_runner->keyboard, gmlKey);
                }
                if (e.key.keysym.unicode != 0)
                    RunnerKeyboard_onCharacter(g_runner->keyboard, e.key.keysym.unicode);
                break;
            case SDL_KEYUP:
                // During playback, suppress real keyboard input
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                RunnerKeyboard_onKeyUp(g_runner->keyboard, SDLKeyToGml(e.key.keysym.sym));
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                if (e.button.button == SDL_BUTTON_WHEELUP) {
                    RunnerMouse_onWheel(g_runner->mouse, 1.0);
                } else if (e.button.button == SDL_BUTTON_WHEELDOWN) {
                    RunnerMouse_onWheel(g_runner->mouse, -1.0);
                } else {
                    int32_t gmlBtn = SDLMouseButtonToGml(e.button.button);
                    if (gmlBtn >= 0) RunnerMouse_onButtonDown(g_runner->mouse, gmlBtn);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (InputRecording_isPlaybackActive(globalInputRecording)) break;
                if (e.button.button != SDL_BUTTON_WHEELUP && e.button.button != SDL_BUTTON_WHEELDOWN) {
                    int32_t gmlBtn = SDLMouseButtonToGml(e.button.button);
                    if (gmlBtn >= 0) RunnerMouse_onButtonUp(g_runner->mouse, gmlBtn);
                }
                break;
            case SDL_VIDEORESIZE:
                fbWidth = e.resize.w;
                fbHeight = e.resize.h;
                scr = SDL_SetVideoMode(fbWidth, fbHeight, 0, (gfx == SOFTWARE ? 0 : SDL_OPENGL) | SDL_RESIZABLE);
                break;
            case SDL_QUIT:
                return true;
        }
    }

    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 2000000)
        SDL_Delay((remaining - 1000000) / 1000000);

    while (nowNanos() < time) {
        // Spin-wait for the remaining sub-millisecond
        YIELD();
    }
}
