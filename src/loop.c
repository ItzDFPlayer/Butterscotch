#include <ctype.h>

#include "data_win.h"
#include "vm.h"
#include "loop.h"

#include "platformdefs.h"
#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <io.h>
#include <direct.h>
#include <psapi.h>
#define getcwd _getcwd
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#ifdef __GLIBC__
#include <malloc.h>
#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 33)
#define HAVE_MALLINFO2
#endif
#endif
#endif
#ifndef __wasi__
#include <signal.h>
#endif

#include "runner_keyboard.h"
#include "runner.h"
#include "input_recording.h"
#include "debug_overlay.h"
#include "default_font/default_font.h"
#if (defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL) || ((defined(USE_GLFW3) || defined(USE_GLFW2)) && defined(ENABLE_SW_RENDERER))) && \
    !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(PLATFORM_PS3) && !defined(PLATFORM_VITA) && !defined(__SWITCH__)
#define USE_GLAD
#include <glad/glad.h>
#endif
#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
#include "gl_renderer.h"
#ifdef ENABLE_LEGACY_GL
#include "gl_legacy_renderer.h"
#endif
#include "gl_common.h"
#endif
#ifdef ENABLE_SW_RENDERER
#include "sw_renderer.h"
#endif
#ifdef ENABLE_NOOP_RENDERER
#include "noop_renderer.h"
#endif
#include "overlay_file_system.h"
#include "texture_baker.h"
#if defined(USE_SDL1_AUDIO)
#include "sdl_audio_system.h"
#elif defined(USE_MINIAUDIO)
#include "ma_audio_system.h"
#elif defined(USE_OPENAL)
#include "al_audio_system.h"
#endif
#include "noop_audio_system.h"
#include "stb_ds.h"
#include "stb_image_write.h"

#include "utils.h"
#include "profiler.h"
#include "gettime.h"
#include "perf_log.h"

#ifdef PLATFORM_VITA
#include "vita_textures.h"
#endif

enum GraphicsAPI gfx;

#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
const GLuint *hostFramebuffer;
#endif

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
/* we define this ourselves because psapi.h isn't available in msvc 4.0 */
typedef struct {
    DWORD cb;
    DWORD PageFaultCount;
    size_t PeakWorkingSetSize;
    size_t WorkingSetSize;
    size_t QuotaPeakPagedPoolUsage;
    size_t QuotaPagedPoolUsage;
    size_t QuotaPeakNonPagedPoolUsage;
    size_t QuotaNonPagedPoolUsage;
    size_t PagefileUsage;
    size_t PeakPagefileUsage;
} BS_PROCESS_MEMORY_COUNTERS;
#endif

static size_t get_used_memory(void) {
#if defined(__linux__)
    {
        // Prefer smaps_rollup (private anonymous memory). Some handheld kernels do
        // not expose it, so fall back to the plain resident set size.
        int fd = open("/proc/self/smaps_rollup", O_RDONLY);
        if (fd >= 0) {
            char buf[512];
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                char *p = buf;
                while (*p) {
                    if (strncmp(p, "Anonymous:", 10) == 0) {
                        p += 10;
                        while (*p == ' ' || *p == '\t')
                            p++;
                        size_t kb = 0;
                        while (*p >= '0' && *p <= '9')
                            kb = kb * 10 + (size_t)(*p++ - '0');
                        return kb * 1024;
                    }
                    while (*p && *p != '\n')
                        p++;
                    if (*p)
                        p++;
                }
            }
        }
    }
    {
        // /proc/self/statm: fields are "size resident shared ..." in pages.
        FILE *statm = fopen("/proc/self/statm", "r");
        if (statm != NULL) {
            unsigned long totalPages = 0, residentPages = 0;
            if (fscanf(statm, "%lu %lu", &totalPages, &residentPages) == 2) {
                fclose(statm);
                long pageSize = sysconf(_SC_PAGESIZE);
                if (pageSize <= 0)
                    pageSize = 4096;
                return (size_t) residentPages * (size_t) pageSize;
            }
            fclose(statm);
        }
    }
#elif defined(__APPLE__)
    task_basic_info_data_t info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
#elif defined(_WIN32)
    typedef BOOL (WINAPI *GetProcessMemoryInfo_t)(HANDLE, BS_PROCESS_MEMORY_COUNTERS*, DWORD);
    static GetProcessMemoryInfo_t func = NULL;
    static bool initialized = false;

    if (!initialized) {
        initialized = true;
        HMODULE dll = LoadLibrary("psapi.dll");
        if (dll) {
            FARPROC p = GetProcAddress(dll, "GetProcessMemoryInfo");
            memcpy(&func, &p, sizeof(func));
        }
    }

    if (func) {
        BS_PROCESS_MEMORY_COUNTERS pmc;
        pmc.cb = sizeof(pmc);
        if (func(GetCurrentProcess(), &pmc, sizeof(pmc)))
            return pmc.WorkingSetSize;
    }
#endif
    return 0;
}

#ifdef USE_GLAD
static bool platformInitGlad(void) {
    glGetString = (PFNGLGETSTRINGPROC)platformGetProcAddress("glGetString");
    if (!glGetString)
        return 0;

    logInfo("OpenGL Version: %s\n", (const char*)glGetString(GL_VERSION));
    GLVer ver = GLCommon_getGLVersion();

    if (ver.isGLES) {
        if (!gladLoadGLES2Loader(platformGetProcAddress))
            return false;
    } else {
        if (!gladLoadGLLoader(platformGetProcAddress))
            return false;
    }
    return true;
}
#endif

#if (defined(ENABLE_MODERN_GL) || defined(ENABLE_LEGACY_GL)) && !defined(NDEBUG) && !defined(PLATFORM_VITA) && !defined(PLATFORM_WEB)
#define USE_OPENGL_DEBUG
static void APIENTRY glDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, MAYBE_UNUSED GLsizei length, const GLchar* message, MAYBE_UNUSED const void* userParam) {
    const char* sourceStr;
    switch (source) {
        case GL_DEBUG_SOURCE_API: sourceStr = "API"; break;
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM: sourceStr = "Window System"; break;
        case GL_DEBUG_SOURCE_SHADER_COMPILER: sourceStr = "Shader Compiler"; break;
        case GL_DEBUG_SOURCE_THIRD_PARTY: sourceStr = "Third Party"; break;
        case GL_DEBUG_SOURCE_APPLICATION: sourceStr = "Application"; break;
        case GL_DEBUG_SOURCE_OTHER: sourceStr = "Other"; break;
        default: sourceStr = "Unknown"; break;
    }

    const char* typeStr;
    switch (type) {
        case GL_DEBUG_TYPE_ERROR: typeStr = "Error"; break;
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: typeStr = "Deprecated Behaviour"; break;
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: typeStr = "Undefined Behaviour"; break;
        case GL_DEBUG_TYPE_PORTABILITY: typeStr = "Portability"; break;
        case GL_DEBUG_TYPE_PERFORMANCE: typeStr = "Performance"; break;
        case GL_DEBUG_TYPE_MARKER: typeStr = "Marker"; break;
        case GL_DEBUG_TYPE_PUSH_GROUP: typeStr = "Push Group"; break;
        case GL_DEBUG_TYPE_POP_GROUP: typeStr = "Pop Group"; break;
        case GL_DEBUG_TYPE_OTHER: typeStr = "Other"; break;
        default: typeStr = "Unknown"; break;
    }

    const char* severityStr;
    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH: severityStr = "High"; break;
        case GL_DEBUG_SEVERITY_MEDIUM: severityStr = "Medium"; break;
        case GL_DEBUG_SEVERITY_LOW: severityStr = "Low"; break;
        case GL_DEBUG_SEVERITY_NOTIFICATION: severityStr = "Notification"; break;
        default: severityStr = "Unknown"; break;
    }

    logInfo("[OpenGL %s] id=%u Type: %s; Severity: %s; Message: %.*s\n", sourceStr, id, typeStr, severityStr, (int) length, message);
}

static void installGLDebugCallback(void) {
    if (glDebugMessageCallback && glDebugMessageControl) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDebugCallback, NULL);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
        return;
    }

    if (glDebugMessageCallbackKHR && glDebugMessageControlKHR) {
        glEnable(GL_DEBUG_OUTPUT_KHR);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
        glDebugMessageCallbackKHR(glDebugCallback, NULL);
        glDebugMessageControlKHR(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
        return;
    }

    if (glDebugMessageCallbackARB && glDebugMessageControlARB) {
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
        glDebugMessageCallbackARB(glDebugCallback, NULL);
        glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
        return;
    }
}
#endif

// Resolves the window size for the specified operating system.
// The "--window-size" argument takes precedence over the default resolution for each platform.
static void resolveWindowSize(const CommandLineArgs* args, uint32_t gen8Width, uint32_t gen8Height, int32_t* outW, int32_t* outH) {
    if (args->windowWidth > 0 && args->windowHeight > 0) {
        *outW = args->windowWidth;
        *outH = args->windowHeight;
        return;
    }

    switch (args->osType) {
        case OS_PS4:
        case OS_XBOXONE:
        case OS_PS3:
        case OS_XBOX360:
            *outW = 1920;
            *outH = 1080;
            break;
        case OS_SWITCH:
            *outW = 1280;
            *outH = 720;
            break;
        case OS_PSVITA:
            *outW = 960;
            *outH = 544;
            break;
        default:
            *outW = (int32_t) gen8Width;
            *outH = (int32_t) gen8Height;
            break;
    }

    // Widescreen hack handling to grow the window size to match
    if (args->widescreenAspect > 0.0f && *outW > 0 && *outH > 0) {
        float nativeAspect = (float) *outW / (float) *outH;
        if (args->widescreenAspect > nativeAspect) {
            int widened = (int) ((float) *outH * args->widescreenAspect + 0.5f);
            if (widened > *outW) *outW = widened;
        } else if (args->widescreenAspect < nativeAspect) {
            int heightened = (int) ((float) *outW / args->widescreenAspect + 0.5f);
            if (heightened > *outH) *outH = heightened;
        }
    }
}

// Extracts the Runner arguments from a string, returning the values on stb_ds array
// The "Runner arguments" is used for the "--game-args" and for the game_change GML function
// Returns the modified array
char** extractRunnerArguments(char* rawArguments) {
    // The "saveptr" is used for strtok_r to store its state
    // So it is thread safe™
    char *saveptr;
    // We create a copy because strtok_r completely obliterates the original char buffer
    char* copy = safeStrdup(rawArguments);
    char* token = strtok_r(copy, " \t\r\n", &saveptr);
    char** array = nullptr;

    while (token != nullptr) {
        arrput(array, safeStrdup(token));
        token = strtok_r(nullptr, " \t\r\n", &saveptr);
    }

    free(copy);

    return array;
}

static char* buildGameChangeTargetPath(const char* currentDataWinPath, const char* workingDirectory, const char* dataWinFilename) {
    if (dataWinFilename == nullptr || dataWinFilename[0] == '\0') {
        return nullptr;
    }

    char* parentDir = safeStrdup(currentDataWinPath);
    bsGetDirname(parentDir);

    const char* normalizedWorkingDir = workingDirectory;
    while (*normalizedWorkingDir == '/' || *normalizedWorkingDir == '\\') {
        normalizedWorkingDir++;
    }

    bool needParentSeparator = parentDir[0] != '\0' && parentDir[strlen(parentDir) - 1] != '/' && parentDir[strlen(parentDir) - 1] != '\\';
    size_t newPathLen = strlen(parentDir) + (needParentSeparator ? 1 : 0) + strlen(normalizedWorkingDir) + 1 + strlen(dataWinFilename) + 1;
    char* newPath = (char *)safeMalloc(newPathLen);
    if (normalizedWorkingDir[0] == '\0') {
        snprintf(newPath, newPathLen, "%s%s%s", parentDir, needParentSeparator ? "/" : "", dataWinFilename);
    } else {
        snprintf(newPath, newPathLen, "%s%s%s/%s", parentDir, needParentSeparator ? "/" : "", normalizedWorkingDir, dataWinFilename);
    }

    free(parentDir);
    return newPath;
}

// ===[ SCREENSHOT ]===
// Reads the contents of an FBO (use 0 for the default framebuffer) into a PNG file.
// If forceOpaque is true, the alpha channel is overwritten with 255, fixing any clobbering done by blending modes.
#ifdef ENABLE_SCREENSHOTS
// When flipY is true, the image will be flipped vertically.
static void writeFramebufferAsPng(GLuint fbo, int width, int height, const char* filename, const char* logPrefix, bool forceOpaque, bool flipY) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    int stride = width * 4;
    unsigned char* pixels = (unsigned char *)safeMalloc(stride * height);
    if (pixels == nullptr) {
        logWarn("Failed to allocate memory for %s (%dx%d)\n", logPrefix, width, height);
        return;
    }

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (forceOpaque) {
        int totalPixels = width * height;
        repeat(totalPixels, i) pixels[i * 4 + 3] = 255;
    }

    if (flipY) {
        // Use stb's negative stride trick: point to the last row and use a negative stride to flip vertically.
        unsigned char* lastRow = pixels + (height - 1) * stride;
        stbi_write_png(filename, width, height, 4, lastRow, -stride);
    } else {
        stbi_write_png(filename, width, height, 4, pixels, stride);
    }

    free(pixels);
    logInfo("%s: %s (%dx%d)\n", logPrefix, filename, width, height);
}

static void captureScreenshot(GLuint fbo, const char* filenamePattern, int frameNumber, int width, int height, bool flipY) {
    if (filenamePattern == nullptr) {
        logWarn("Screenshot capture requested without a filename pattern\n");
        return;
    }

    char filename[512];
    snprintf(filename, sizeof(filename), filenamePattern, frameNumber);
    writeFramebufferAsPng(fbo, width, height, filename, "Screenshot saved", true, flipY);
}

// Dumps every live surface in the GL renderer as a PNG.
// Filename pattern takes two %d slots: frame number, then surface ID.
static void dumpAllSurfaces(GLRenderer* gl, const char* filenamePattern, int frameNumber) {
    if (filenamePattern == nullptr) {
        logWarn("Surface dump requested without a filename pattern\n");
        return;
    }
    
    repeat(gl->surfaceCount, surfaceId) {
        if (gl->surfaces[surfaceId] == 0)
            continue;

        int width = gl->surfaceWidth[surfaceId];
        int height = gl->surfaceHeight[surfaceId];
        if (0 >= width || 0 >= height) continue;

        char filename[512];
        snprintf(filename, sizeof(filename), filenamePattern, frameNumber, (int) surfaceId);
        writeFramebufferAsPng(gl->surfaces[surfaceId], width, height, filename, "Surface dump", false, false);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, *hostFramebuffer);
}
#endif

// ===[ KEYBOARD INPUT ]===

InputRecording* globalInputRecording = nullptr;

#ifndef __wasi__

#if defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define BUTTERSCOTCH_HAS_ASAN 1
    #endif
#endif
#if defined(__SANITIZE_ADDRESS__)
    #define BUTTERSCOTCH_HAS_ASAN 1
#endif

#if BUTTERSCOTCH_HAS_ASAN
void __asan_set_death_callback(void (*callback)(void));
#endif

static volatile sig_atomic_t crashSaveInProgress = 0;

static void saveRecordingOnCrash(void) {
    if (crashSaveInProgress) return;
    crashSaveInProgress = 1;
    if (globalInputRecording != nullptr && globalInputRecording->isRecording) {
        InputRecording_save(globalInputRecording);
    }
}

static void crashSignalHandler(int sig) {
    saveRecordingOnCrash();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashHandlers(void) {
#if BUTTERSCOTCH_HAS_ASAN
    __asan_set_death_callback(saveRecordingOnCrash);
#endif
    signal(SIGSEGV, crashSignalHandler);
    signal(SIGABRT, crashSignalHandler);
#ifdef SIGBUS
    signal(SIGBUS,  crashSignalHandler);
#endif
    signal(SIGFPE,  crashSignalHandler);
    signal(SIGILL,  crashSignalHandler);
}

#endif

void saveInputRecording() {
    // Save input recording if active, then free
    if (globalInputRecording != nullptr) {
        if (globalInputRecording->isRecording) {
            InputRecording_save(globalInputRecording);
        }
        InputRecording_free(globalInputRecording);
        globalInputRecording = nullptr;
    }
}

#if !defined(_WIN32) && !defined(PLATFORM_VITA) && !defined(__SWITCH__) && !defined(__wasi__)
#define USE_CRASH_SIGNAL_HANDLER
typedef struct { int key; struct sigaction value; } PreviousSignalActionEntry;
static PreviousSignalActionEntry* previousSignalActions = nullptr;

static void onCrashSignal(int sig) {
    saveInputRecording();
    // Restore the previous handler (ASAN) and re-raise so it can report the fault
    ssize_t idx = hmgeti(previousSignalActions, sig);
    sigaction(sig, &previousSignalActions[idx].value, nullptr);
    raise(sig);
}
#endif

char* collapseNewlines(const char *input) {
    if (input == nullptr) {
        return nullptr;
    }

    size_t len = strlen(input);
    char *result = (char *)malloc(len + 1);
    if (result == nullptr) {
        return nullptr;
    }

    size_t j = 0;
    bool isNewline = false;
    repeat(len, i) {
        if (input[i] == '\n' || input[i] == '\r') {
            if (isNewline)
                continue;

            isNewline = true;
            result[j++] = '\n';
            continue;
        } else {
            isNewline = false;
        }
        result[j++] = input[i];
    }
    result[j] = '\0';

    return result;
}

static void PreProcessedStuff_free(void) {
#ifdef PLATFORM_VITA
    VitaTextures_Free();
#endif
}

// Deltarune keeps one shared save directory at the game root, while each chapter
// ships its own data.win in a subdirectory (chapter3_windows/, ...). If the
// directory above the WAD also contains a data.win (the chapter-select launcher),
// use that directory for saves so every chapter and the selector see the same
// files. Returns nullptr when the parent has no data.win (normal single-game ports).
static char* resolveSharedSaveDir(const char* dataWinDir) {
    if (dataWinDir == nullptr) return nullptr;

    // Work on an absolute directory path: ports are often launched after a
    // `cd` into the game folder with a bare "data.win" argument, which leaves
    // dataWinDir as "./".
    bool isAbsolute =
        dataWinDir[0] == '/' || dataWinDir[0] == '\\'
#ifdef _WIN32
        || (dataWinDir[0] != '\0' && dataWinDir[1] == ':') // C:\...
#endif
        ;

    char* absDir;
    if (isAbsolute) {
        absDir = safeStrdup(dataWinDir);
    } else {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) return nullptr;
        size_t absLen = strlen(cwd) + 1 + strlen(dataWinDir) + 1;
        absDir = (char *) safeMalloc(absLen);
        snprintf(absDir, absLen, "%s/%s", cwd, dataWinDir);
    }

    // Remove trailing separators and a trailing "." component (cwd/./).
    size_t len = strlen(absDir);
    while (len > 0 && (absDir[len - 1] == '/' || absDir[len - 1] == '\\')) {
        len--;
    }
    if (len >= 2 && absDir[len - 1] == '.' && (absDir[len - 2] == '/' || absDir[len - 2] == '\\')) {
        len -= 2;
    }
    absDir[len] = '\0';
    if (len == 0) {
        free(absDir);
        return nullptr;
    }

    // Find the start of the final path component. `sep` ends up just past the
    // separator that precedes it (so it doubles as the parent's length).
    size_t sep = len;
    while (sep > 0 && absDir[sep - 1] != '/' && absDir[sep - 1] != '\\') {
        sep--;
    }
    if (sep == 0) {
        free(absDir);
        return nullptr;
    }

    char* parentDir = (char *) safeMalloc(sep + 1);
    memcpy(parentDir, absDir, sep);
    parentDir[sep] = '\0';
    free(absDir);

    size_t probeLen = strlen(parentDir) + strlen("data.win") + 1;
    char* probe = (char *) safeMalloc(probeLen);
    snprintf(probe, probeLen, "%s%s", parentDir, "data.win");

    FILE* probeFile = fopen(probe, "rb");
    bool exists = probeFile != nullptr;
    if (probeFile != nullptr) {
        fclose(probeFile);
    }
    free(probe);

    if (!exists) {
        free(parentDir);
        return nullptr;
    }
    return parentDir;
}

// Paint the texture-preparation progress bar straight into the framebuffer while the
// <data.win>.bsq sidecar is being built. Off by default: the bake pass runs before the
// first frame is presented, per-texture progress is already written to the log, and on
// a small screen the bar mostly flickers. Set this to true, or export
// BUTTERSCOTCH_BAKE_PROGRESS_BAR=1, to show it.
static bool showBakeProgressBar = false;

typedef struct {
    Renderer* renderer;
    int32_t width;
    int32_t height;
    bool graphical;
} BakeProgressContext;

static void onTextureBakeProgress(void* userData, uint32_t done, uint32_t total) {
    BakeProgressContext* ctx = (BakeProgressContext*) userData;
    float progress = total > 0 ? (float) done / (float) total : 1.0f;

#ifdef ENABLE_SW_RENDERER
    if (showBakeProgressBar && ctx->graphical && ctx->renderer != nullptr) {
        SWRenderer_showLoadingProgress(ctx->renderer, ctx->width, ctx->height, progress);
    }
#endif

    if (done == 0 || done == total || (done % 8) == 0) {
        logInfo("Preparing textures: %u/%u\n", done, total);
    }
}

#ifdef ENABLE_SW_RENDERER
// Adds the texture page behind a TPAG item to the (deduplicated) list.
static void prefetchAddTPag(DataWin* dw, int32_t tpagIndex, bool* seen, int32_t** pages) {
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count)
        return;

    int16_t pageId = dw->tpag.items[tpagIndex].texturePageId;
    if (pageId < 0 || (uint32_t) pageId >= dw->txtr.count)
        return;
    if (seen[pageId])
        return;

    seen[pageId] = true;
    arrput(*pages, (int32_t) pageId);
}

// Decodes the texture pages a room needs right after the room change, behind the
// loading bar. Without this, the first frames of a large room stall for seconds
// because each page (2048x2048, ~100 ms on a Cortex-A7) is decoded inside a draw
// call the first time a sprite or tile on it is touched.
//
// Only content the camera can see (plus a margin, so scrolling does not page in
// straight away) is prefetched: pulling in a whole large room would mean a much
// longer bar for pages the player may never look at.
static void prefetchRoomTextures(Runner* runner, Renderer* renderer, int32_t barW, int32_t barH) {
    if (runner == nullptr || renderer == nullptr || runner->dataWin == nullptr)
        return;

    DataWin* dw = runner->dataWin;
    Room* room = runner->currentRoom;
    if (room == nullptr || dw->txtr.count == 0)
        return;

    // Visible room area, with a margin for the camera panning.
    const float prefetchMargin = 256.0f;
    bool filterToView = false;
    float visX = 0.0f, visY = 0.0f, visW = (float) room->width, visH = (float) room->height;
    if (runner->viewsEnabled) {
        GMLCamera* camera = Runner_getCameraForView(runner, 0);
        if (camera != nullptr && camera->viewWidth > 0 && camera->viewHeight > 0) {
            visX = camera->viewX;
            visY = camera->viewY;
            visW = (float) camera->viewWidth;
            visH = (float) camera->viewHeight;
            filterToView = true;
        }
    }
    if (filterToView) {
        visX -= prefetchMargin;
        visY -= prefetchMargin;
        visW += prefetchMargin * 2.0f;
        visH += prefetchMargin * 2.0f;
    }

    bool* seen = (bool*) safeCalloc(dw->txtr.count, sizeof(bool));
    int32_t* pages = nullptr;

    // Tiles are usually the bulk of a room's texture references.
    for (uint32_t i = 0; i < room->tileCount; i++) {
        RoomTile* tile = &room->tiles[i];
        if (filterToView) {
            float tileW = (float) tile->width * (tile->scaleX != 0.0f ? tile->scaleX : 1.0f);
            float tileH = (float) tile->height * (tile->scaleY != 0.0f ? tile->scaleY : 1.0f);
            if ((float) tile->x + tileW < visX || (float) tile->y + tileH < visY)
                continue;
            if ((float) tile->x > visX + visW || (float) tile->y > visY + visH)
                continue;
        }
        prefetchAddTPag(dw, Renderer_resolveObjectTPAGIndex(dw, tile), seen, &pages);
    }

    // GMS1-style room backgrounds (they cover the room, so always keep them).
    if (room->backgrounds != nullptr) {
        for (int32_t i = 0; i < 8; i++) {
            if (!room->backgrounds[i].enabled)
                continue;
            prefetchAddTPag(dw, Renderer_resolveBackgroundTPAGIndex(dw, room->backgrounds[i].backgroundDefinition), seen, &pages);
        }
    }

    // The sprite frame each instance is currently showing.
    for (ptrdiff_t i = 0; i < arrlen(runner->instances); i++) {
        Instance* inst = runner->instances[i];
        if (inst == nullptr || !inst->active || !inst->visible)
            continue;
        if (filterToView) {
            float instX = (float) inst->x;
            float instY = (float) inst->y;
            if (instX < visX || instY < visY || instX > visX + visW || instY > visY + visH)
                continue;
        }
        if (inst->spriteIndex < 0 || (uint32_t) inst->spriteIndex >= dw->sprt.count)
            continue;

        Sprite* spr = &dw->sprt.sprites[inst->spriteIndex];
        if (spr->textureCount == 0)
            continue;

        int32_t frame = (int32_t) inst->imageIndex;
        if (frame < 0 || (uint32_t) frame >= spr->textureCount)
            frame = 0;

        prefetchAddTPag(dw, spr->tpagIndices[frame], seen, &pages);
    }

    free(seen);

    ptrdiff_t pageCount = arrlen(pages);
    if (pageCount <= 0) {
        arrfree(pages);
        return;
    }

    // Only prefetch when the room actually fits: otherwise the pages would evict
    // each other and the work is wasted (the lazy path handles that case).
    int subPageCount = 0;
    uint64_t requiredBytes = 0;
    for (ptrdiff_t i = 0; i < pageCount; i++) {
        if (SWRenderer_pageIsSubPage(renderer, pages[i]))
            subPageCount++;
        requiredBytes += SWRenderer_estimateTexturePageBytes(renderer, pages[i]);
    }

    if (subPageCount == (int) pageCount) {
        // These pages are decoded per strip as they are drawn, so there is nothing
        // worth prefetching and no full page ever has to be resident.
        arrfree(pages);
        return;
    }

    SWRendererTextureStats stats;
    SWRenderer_getTextureStats(renderer, &stats);

    if (requiredBytes > (uint64_t) stats.budgetBytes) {
        logInfo("Room \"%s\" needs %llu MB of textures (cache %u MB); keeping lazy loading for it\n",
                room->name,
                (unsigned long long) (requiredBytes / (1024ull * 1024ull)),
                stats.budgetBytes / (1024u * 1024u));
        arrfree(pages);
        return;
    }

    uint64_t startedAt = nowNanos();
    int decoded = 0;
    int32_t total = (int32_t) pageCount;

    SWRenderer_showLoadingProgress(renderer, barW, barH, 0.0f);
    for (int32_t i = 0; i < total; i++) {
        if (SWRenderer_prefetchTexturePage(renderer, pages[i]))
            decoded++;
        SWRenderer_showLoadingProgress(renderer, barW, barH, (float) (i + 1) / (float) total);
    }

    double elapsedMs = (double) (nowNanos() - startedAt) / 1000000.0;
    logInfo("Prefetched %d/%d texture pages for room \"%s\" in %.0f ms\n",
            decoded, total, room->name, elapsedMs);

    arrfree(pages);
}
#endif /* ENABLE_SW_RENDERER */

// ===[ MAIN ]===
int loop(CommandLineArgs args, const char *argv0) {
    // The Miyoo release launcher only passes "--renderer software data.win". The
    // eager defaults (whole data.win in RAM, every AUDO/TXTR/ROOM payload parsed up
    // front) cannot fit Chapters 3+ in 128 MB. Default to a memory-mapped data.win
    // and lazy rooms unless the user explicitly chose something else.
    if (args.bakeTexturesPath != nullptr) {
        return TextureBaker_bake(args.dataWinPath, args.bakeTexturesPath, nullptr, nullptr);
    }

    if (args.renderer == SOFTWARE) {
        if (!args.loadTypeExplicit) {
            args.loadType = DATAWINLOADTYPE_MAP_FILE;
        }
        if (!args.lazyRoomsExplicit) {
            args.lazyRooms = true;
        }
        // With a mapped WAD these defer the AUDO/TXTR chunk walks to first use and
        // decode straight from the (reclaimable) mapping instead of the heap.
        if (!args.lazyTexturesExplicit) {
            args.lazyTextures = true;
        }
        if (!args.lazyAudioExplicit) {
            args.lazyAudio = true;
        }
        logInfo("Low-memory defaults: load-type=%s, lazy-rooms=%s, lazy-textures=%s, lazy-audio=%s\n",
                args.loadType == DATAWINLOADTYPE_MAP_FILE ? "map-file" :
                args.loadType == DATAWINLOADTYPE_LOAD_PER_CHUNK ? "load-per-chunk" : "load-in-memory-ahead-of-time",
                args.lazyRooms ? "on" : "off",
                args.lazyTextures ? "on" : "off",
                args.lazyAudio ? "on" : "off");
    }
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    // Frame-level performance log. Deliberately small (heartbeats, slow frames,
    // room changes and the GML scripts running during spikes) so it can be
    // copied off an SD card and attached to a bug report.
    PerfLog* perfLog = nullptr;
    if (args.perfLogPath != nullptr && args.perfLogPath[0] != '\0') {
        perfLog = PerfLog_open(args.perfLogPath, args.perfLogSlowMs);
        if (perfLog == nullptr) {
            logWarn("Could not create performance log '%s'\n", args.perfLogPath);
        } else {
            logInfo("Writing performance log to '%s' (slow frame >= %d ms)\n",
                    args.perfLogPath, args.perfLogSlowMs);
            PerfLog_note(perfLog, "Game: %s", args.dataWinPath);
            PerfLog_note(perfLog, "Renderer: %s",
                         args.renderer == SOFTWARE ? "software" :
                         args.renderer == LEGACY_GL ? "legacy-gl" : "modern-gl");
        }
    }

    // Decode a room's texture pages right after the room change instead of
    // letting the first draw calls stall on them. Disable with BUTTERSCOTCH_NO_PREFETCH=1.
    bool prefetchTextures = true;
    {
        const char* noPrefetch = getenv("BUTTERSCOTCH_NO_PREFETCH");
        if (noPrefetch != nullptr && noPrefetch[0] != '\0' && noPrefetch[0] != '0') {
            prefetchTextures = false;
        }
    }

    // Optional progress bar for the bake pass; see showBakeProgressBar.
    {
        const char* bakeBar = getenv("BUTTERSCOTCH_BAKE_PROGRESS_BAR");
        if (bakeBar != nullptr && bakeBar[0] != '\0' && bakeBar[0] != '0') {
            showBakeProgressBar = true;
        }
    }

    char* currentDataWinPath = safeStrdup(args.dataWinPath);    char** currentGameArgs = args.gameArgs;
    repeat(arrlen(args.gameArgs), i) {
        arrput(currentGameArgs, args.gameArgs[i]);
    }
    // The first argument will ALWAYS be the argv[0]
    arrins(currentGameArgs, 0, safeStrdup(argv0 != nullptr ? argv0 : ""));

    bool platformInitialized = false;
    int32_t inputFrameCount = 0;

    bool fastForwardActive = false;
    bool fastForwardTabPrev = false;
    bool showDebugOverlay = args.debug;
    while (true) {
        logInfo("Loading %s...\n", args.dataWinPath);

        DataWinParserOptions options = {0};
        options.parseGen8 = true;
        options.parseOptn = true;
        options.parseLang = true;
        options.parseExtn = true;
        options.parseSond = true;
        options.parseAgrp = true;
        options.parseSprt = true;
        options.parseBgnd = true;
        options.parsePath = true;
        options.parseScpt = true;
        options.parseGlob = true;
        options.parseShdr = true;
        options.parseFont = true;
        options.parseTmln = true;
        options.parseObjt = true;
        options.parseRoom = true;
        options.parseTpag = true;
        options.parseCode = true;
        options.parseVari = true;
        options.parseFunc = true;
        options.parseStrg = true;
        options.parseTxtr = true;
#ifdef PLATFORM_VITA
        do {
            char *texBinDir = safeStrdup(args.dataWinPath);
            bsGetDirname(texBinDir);
            if (strcmp(texBinDir, ".") == 0) {
                free(texBinDir);
                break;
            }
            size_t dirLen = strlen(texBinDir);
            bool needsSep = texBinDir[dirLen - 1] != '/' && texBinDir[dirLen - 1] != '\\' && texBinDir[dirLen - 1] != ':';
            const char *texBinName = "textures.bin";
            size_t texBinPathSize = dirLen + (needsSep ? 1 : 0) + strlen(texBinName) + 1;
            char *texBinPath = (char *)safeMalloc(texBinPathSize);
            snprintf(texBinPath, texBinPathSize, "%s%s%s", texBinDir, needsSep ? "/" : "", texBinName);
            free(texBinDir);
            FILE *texBinFile = fopen(texBinPath, "rb");
            free(texBinPath);
            if (!texBinFile)
                break;
            if (!VitaTextures_Init(texBinFile)) {
                logWarn("textures.bin found but failed to load!\n");
                break;
            }
            options.parseTxtr = false;
        } while(0);
#endif
#if defined(USE_SDL1_AUDIO) || defined(USE_MINIAUDIO) || defined(USE_OPENAL)
        if (!args.headless)
            options.parseAudo = true;
#endif
        options.skipLoadingPreciseMasksForNonPreciseSprites = true;
        options.loadType = args.loadType;
        options.autoBakeTextures = (args.renderer == SOFTWARE) && !args.disableAutoBakeTextures;
        options.lazyLoadRooms = args.lazyRooms;
        options.lazyLoadTextures = args.lazyTextures;
        options.lazyLoadAudio = args.lazyAudio;
        options.eagerlyLoadedRooms = args.eagerRooms;
        DataWin* dataWin = DataWin_parse(currentDataWinPath, options);

        Gen8* gen8 = &dataWin->gen8;
        logInfo("Loaded \"%s\" (%d) successfully! [WAD Version %u / GameMaker version %u.%u.%u.%u]\n", gen8->name, gen8->gameID, gen8->wadVersion, dataWin->detectedFormat.major, dataWin->detectedFormat.minor, dataWin->detectedFormat.release, dataWin->detectedFormat.build);

#ifdef HAVE_MALLINFO2
        {
            struct mallinfo2 mi = mallinfo2();
            logInfo("Memory after data.win parsing: used=%zu bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f);
        }
#endif

        // Build window title
        char windowTitle[256];
        snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

        // Initialize VM
        VMContext* vm = VM_create(dataWin);

        PerfLog_note(perfLog, "Loading %s", currentDataWinPath);
        Profiler_setEnabled(&vm->profiler, args.profilerFramesBetween > 0 || perfLog != nullptr);
#ifdef ENABLE_VM_OPCODE_PROFILER
        vm->opcodeProfilerEnabled = args.opcodeProfiler;
        if (vm->opcodeProfilerEnabled) {
            vm->opcodeVariantCounts = (uint64_t *)safeCalloc(256 * 256, sizeof(uint64_t));
            vm->opcodeRValueTypeCounts = (uint64_t *)safeCalloc(256 * 256, sizeof(uint64_t));
        }
#endif

        if (args.hasSeed)
            vm->hasFixedSeed = true;

        if (args.printRooms) {
            // Under --lazy-rooms we load each room for display and then free it again so the dump
            // reflects what each room contains without keeping all of them resident simultaneously.
            forEachIndexed(Room, room, idx, dataWin->room.rooms, dataWin->room.count) {
                if (!room->present) {
                    logInfo("[%d] <absent>\n", (int)idx);
                    continue;
                }
                bool loadedHere = false;
                if (!room->payloadLoaded) {
                    DataWin_loadRoomPayload(dataWin, (int32_t) idx);
                    loadedHere = true;
                }

                logInfo("[%d] %s ()\n", (int)idx, room->name);

                forEachIndexed(RoomGameObject, roomGameObject, idx2, room->gameObjects, room->gameObjectCount) {
                    if (roomGameObject->objectDefinition < 0 || (uint32_t) roomGameObject->objectDefinition >= dataWin->objt.count) {
                       logInfo("  [%d] <no object> (x=%d,y=%d)\n", (int)idx2, roomGameObject->x, roomGameObject->y);
                        continue;
                    }
                    GameObject* gameObject = &dataWin->objt.objects[roomGameObject->objectDefinition];
                    logInfo(
                        "  [%d] %s (x=%d,y=%d,persistent=%d,solid=%d,spriteId=%d,preCreateCode=%d,creationCode=%d)\n",
                        (int)idx2,
                        gameObject->name,
                        roomGameObject->x,
                        roomGameObject->y,
                        gameObject->persistent,
                        gameObject->solid,
                        gameObject->spriteId,
                        roomGameObject->preCreateCode,
                        roomGameObject->creationCode
                    );
                }

                if (loadedHere && !room->eagerlyLoaded) {
                    DataWin_freeRoomPayload(room);
                }
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (args.printObjects) {
            forEachIndexed(GameObject, obj, idx, dataWin->objt.objects, dataWin->objt.count) {
                uint32_t totalEvents = 0;
                repeat(OBJT_EVENT_TYPE_COUNT, e) {
                    totalEvents += obj->eventLists[e].eventCount;
                }
                logInfo("[%u] %s:\n", (unsigned int)idx, obj->name);
                if (obj->parentId >= 0 && (uint32_t) obj->parentId < dataWin->objt.count) {
                    logInfo("  Parent: %s (%d)\n", dataWin->objt.objects[obj->parentId].name, obj->parentId);
                } else {
                    logInfo("  Parent: none\n");
                }
                if (obj->spriteId >= 0 && (uint32_t) obj->spriteId < dataWin->sprt.count) {
                    logInfo("  Sprite: %s (%d)\n", dataWin->sprt.sprites[obj->spriteId].name, obj->spriteId);
                } else {
                    logInfo("  Sprite: none\n");
                }
                logInfo("  Solid: %d\n", obj->solid);
                logInfo("  Persistent: %d\n", obj->persistent);
                logInfo("  Visible: %d\n", obj->visible);
                logInfo("  Depth: %d\n", obj->depth);
                logInfo("  Events (%u):\n", totalEvents);
                {
                repeat(OBJT_EVENT_TYPE_COUNT, e) {
                    ObjectEventList* list = &obj->eventLists[e];
                    repeat(list->eventCount, eIdx) {
                        ObjectEvent* event = &list->events[eIdx];
                        const char* eventName = Runner_getEventName((int32_t) e, (int32_t) event->eventSubtype);
                        int32_t codeId = -1;
                        if (event->actionCount > 0) codeId = event->actions[0].codeId;
                        logInfo("    %s:\n", eventName);
                        logInfo("      Sub Type: %u\n", event->eventSubtype);
                        logInfo("      Code ID: %d\n", codeId);
                        logInfo("      Actions: %u\n", event->actionCount);
                    }
                }
                }
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (args.printShaders) {
            forEachIndexed(Shader, shader, idx, dataWin->shdr.shaders, dataWin->shdr.count) {
                logInfo("[%u] %s:\n", (unsigned int)idx, shader->name);
                logInfo("GLSL Vertex Shader:\n");
                char* glslVertex = collapseNewlines(shader->glsl_Vertex);
                logInfo("%s\n", glslVertex);
                free(glslVertex);

                logInfo("GLSL Fragment Shader:\n");
                char* glslFragment = collapseNewlines(shader->glsl_Fragment);
                logInfo("%s\n", glslFragment);
                free(glslFragment);

                logInfo("GLSL ES Vertex Shader:\n");
                char* glslESVertex = collapseNewlines(shader->glslES_Vertex);
                logInfo("%s\n", glslESVertex);
                free(glslESVertex);

                logInfo("GLSL ES Fragment Shader:\n");
                char* glslESFragment = collapseNewlines(shader->glslES_Fragment);
                logInfo("%s\n", glslESFragment);
                free(glslESFragment);
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (args.printDeclaredFunctions) {
            repeat(hmlen(vm->codeIndexByName), i) {
                logInfo("[%d] %s\n", vm->codeIndexByName[i].value, vm->codeIndexByName[i].key);
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (args.printUnknownFunctions) {
            uint32_t unimplementedCount = 0;
            logInfo("Unknown Functions:\n");
            repeat(dataWin->func.functionCount, i) {
                const char* name = dataWin->func.functions[i].name;
                if (name == nullptr)
                    continue;

                // Implemented as a user script/code entry?
                if (shgeti(vm->codeIndexByName, (char*) name) >= 0)
                    continue;

                // Implemented as a registered builtin?
                if (VM_findBuiltin(vm, name) != nullptr)
                    continue;

                logInfo("- %s\n", name);
                unimplementedCount++;
            }

            if (unimplementedCount == 0) {
                logInfo("All %u referenced functions are implemented! :3\n", dataWin->func.functionCount);
            } else {
                logInfo("%u unknown function(s) out of %u referenced\n", unimplementedCount, dataWin->func.functionCount);
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        if (shlen(args.disassemble) > 0) {
            VM_buildCrossReferences(vm);
            if (shgeti(args.disassemble, "*") >= 0) {
                repeat(dataWin->code.count, i) {
                    VM_disassemble(vm, (int32_t) i);
                }
            } else {
                for (ptrdiff_t i = 0; shlen(args.disassemble) > i; i++) {
                    const char* name = args.disassemble[i].key;
                    ptrdiff_t idx = shgeti(vm->codeIndexByName, (char*) name);
                    if (idx >= 0) {
                        VM_disassemble(vm, vm->codeIndexByName[idx].value);
                    } else {
                        logWarn("Script '%s' not found in funcMap\n", name);
                    }
                }
            }
            VM_free(vm);
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 0;
        }

        // Initialize the file system
        char* dataWinDir = safeStrdup(args.dataWinPath);
        bsGetDirname(dataWinDir);
        const char* savePath = args.saveFolder;
        char* sharedSaveDir = nullptr;
        if (savePath == nullptr) {
            sharedSaveDir = resolveSharedSaveDir(dataWinDir);
            savePath = sharedSaveDir != nullptr ? sharedSaveDir : dataWinDir;
            if (sharedSaveDir != nullptr) {
                logInfo("Using shared save folder: %s\n", sharedSaveDir);
            }
        }
        OverlayFileSystem* overlayFs = OverlayFileSystem_create(dataWinDir, savePath);
        free(sharedSaveDir);
        free(dataWinDir);

        gfx = args.renderer;

#ifndef ENABLE_LEGACY_GL
        if (gfx == LEGACY_GL) {
            logError("The legacy gl renderer is not available in this build!\n");
            return 0;
        }
#endif
#ifndef ENABLE_MODERN_GL
        if (gfx == MODERN_GL) {
            logError("The modern gl renderer is not available in this build!\n");
            return 0;
        }
#endif
#ifndef ENABLE_SW_RENDERER
        if (gfx == SOFTWARE) {
            logError("The software renderer is not available in this build!\n");
            return 0;
        }
#endif
#ifndef ENABLE_NOOP_RENDERER
        if (gfx == NOOP) {
            logError("The noop renderer is not available in this build!\n");
            return 0;
        }
#endif

#ifdef ENABLE_SCREENSHOTS
        if (gfx != MODERN_GL && hmlen(args.screenshotSurfacesFrames)) {
            logError("You can only use --screenshot-surfaces with the modern gl renderer!\n");
            return 0;
        }
#endif


        int32_t windowW, windowH;
        resolveWindowSize(&args, gen8->defaultWindowWidth, gen8->defaultWindowHeight, &windowW, &windowH);

        if (!platformInitialized) {
            if (!platformInit(windowW, windowH, windowTitle, args.headless)) {
                DataWin_free(dataWin);
                PreProcessedStuff_free();
                return 1;
            }

            // game_change path: reuse the existing window/GL context, just retitle and resize for the new game.
            platformSetWindowTitle(gen8->displayName);
#ifdef USE_GLAD
#if defined(USE_GLFW3) || defined(USE_GLFW2)
            if (gfx == LEGACY_GL || gfx == MODERN_GL || gfx == SOFTWARE) {
#else
            if (gfx == LEGACY_GL || gfx == MODERN_GL) {
#endif
                if (!platformInitGlad()) {
                    logError("Failed to initialize GLAD\n");
                    platformExit();
                    DataWin_free(dataWin);
                    PreProcessedStuff_free();
                    return 1;
                }
            }
#endif

            // Install the OpenGL debug message callback
#ifdef USE_OPENGL_DEBUG
            if (gfx == MODERN_GL)
                installGLDebugCallback();
#endif

            platformInitialized = true;
        } else {
            platformSetWindowSize(windowW, windowH);
        }

        // Initialize the renderer
        // NOTE: headless mode keeps rendering active (hidden window + normal renderer).
        // NOOP is a separate renderer that stubs all draw calls.
        Renderer* renderer = nullptr;
#ifdef ENABLE_SW_RENDERER
        if (gfx == SOFTWARE)
            renderer = SWRenderer_create();
#endif
#ifdef ENABLE_NOOP_RENDERER
        if (gfx == NOOP) {
            renderer = NoopRenderer_create();
        }
#endif
#ifdef ENABLE_LEGACY_GL
        if (gfx == LEGACY_GL) {
            renderer = GLLegacyRenderer_create();
            static GLuint hostfb = 0;
            hostFramebuffer = &hostfb;
        }
#endif
#ifdef ENABLE_MODERN_GL
        if (gfx == MODERN_GL) {
            renderer = GLRenderer_create();
            hostFramebuffer = &((GLModernRenderer *)renderer)->hostFramebuffer;
        }
#endif
        if (!renderer) {
            logError("Failed to initialize a renderer\n");
            platformExit();
            DataWin_free(dataWin);
            PreProcessedStuff_free();
            return 1;
        }
#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
        if (gfx == LEGACY_GL || gfx == MODERN_GL) {
            switch (args.glTextureFormat) {
                case GL_TEXTURE_FORMAT_RGBA4:
                    ((GLRenderer*)renderer)->textureFormat = GL_RGBA4;
                    break;
                case GL_TEXTURE_FORMAT_RGBA:
                    ((GLRenderer*)renderer)->textureFormat = GL_RGBA;
                    break;
                case GL_TEXTURE_FORMAT_COMPRESSED_RGBA:
#ifdef GL_COMPRESSED_RGBA
                    ((GLRenderer*)renderer)->textureFormat = GL_COMPRESSED_RGBA;
                    break;
#else
                    logError("Compressed textures are unavailable in this build\n");
                    platformExit();
                    DataWin_free(dataWin);
                    PreProcessedStuff_free();
                    return 1;
#endif
                default:
                    abort();
            }
            switch (args.glSurfaceFormat) {
                case GL_SURFACE_FORMAT_RGBA4:
                    ((GLRenderer*)renderer)->surfaceFormat = GL_RGBA4;
                    break;
                case GL_SURFACE_FORMAT_RGBA:
                    ((GLRenderer*)renderer)->surfaceFormat = GL_RGBA;
                    break;
                default:
                    abort();
            }
        }
#endif

        // Prepare every texture page before the game starts so the first session
        // does not pay BZip2 per room. Skipped when the sidecar is already complete,
        // and while headless (tests/CI).
        if (!args.headless && args.renderer == SOFTWARE && !args.disableAutoBakeTextures &&
            dataWin->sourcePath != nullptr && !TextureBaker_isComplete(dataWin->sourcePath)) {
            size_t sidecarLen = strlen(dataWin->sourcePath) + 5;
            char* sidecarPath = (char *) safeMalloc(sidecarLen);
            snprintf(sidecarPath, sidecarLen, "%s.bsq", dataWin->sourcePath);

            BakeProgressContext bakeCtx = { renderer, windowW, windowH, renderer != nullptr };
            int bakeResult = TextureBaker_bake(dataWin->sourcePath, sidecarPath, onTextureBakeProgress, &bakeCtx);
            free(sidecarPath);

            if (bakeResult == 0) {
                DataWin_refreshBakedTextures(dataWin);
            } else {
                logWarn("Texture pre-bake failed; continuing without it.\n");
            }
        }

        // Initialize the audio system
        AudioSystem* audioSystem = nullptr;
        if (args.headless) {
            audioSystem = (AudioSystem*) NoopAudioSystem_create();
        } else {
#if defined(USE_SDL1_AUDIO)
            audioSystem = (AudioSystem*) SdlAudioSystem_create(dataWin);
#elif defined(USE_MINIAUDIO)
            audioSystem = (AudioSystem*) MaAudioSystem_create(dataWin);
#elif defined(USE_OPENAL)
            audioSystem = (AudioSystem*) AlAudioSystem_create();
#else
            audioSystem = (AudioSystem*) NoopAudioSystem_create();
#endif
        }

        // Initialize the runner
        Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) overlayFs, audioSystem, args.seed);

        if (!args.lazyTextures) {
            repeat(runner->dataWin->txtr.count, i) {
#ifdef ENABLE_MODERN_GL
                if (gfx == MODERN_GL)
                    GLRenderer_ensureTextureLoaded((GLRenderer*) renderer, (int32_t) i);
#endif

#ifdef ENABLE_LEGACY_GL
                if (gfx == LEGACY_GL)
                    GLLegacyRenderer_ensureTextureLoaded((GLRenderer*) renderer, (int32_t) i);
#endif
            }
        }
        runner->debugMode = args.debug;
        runner->osType = args.osType;
        runner->setWindowSize = platformSetWindowSize;
        runner->getWindowSize = platformGetWindowSize;
        runner->setWindowTitle = platformSetWindowTitle;
        Runner_setGameArgs(runner, currentGameArgs, (int32_t) arrlen(currentGameArgs));
        platformInitFunctions(runner);

        // Set up input recording/playback (both can be active: playback then continue recording)
        if (args.playbackInputsPath != nullptr) {
            globalInputRecording = InputRecording_createPlayer(args.playbackInputsPath, args.recordInputsPath);
        } else if (args.recordInputsPath != nullptr) {
            globalInputRecording = InputRecording_createRecorder(args.recordInputsPath);
        }
        if (globalInputRecording != nullptr) {
            globalInputRecording->filterDebugKeys = args.debug;
#ifndef __wasi__
            installCrashHandlers();
#endif
        }
#ifdef ENABLE_VM_TRACING
        shcopyFromTo(args.varReadsToBeTraced, runner->vmContext->varReadsToBeTraced);
        shcopyFromTo(args.varWritesToBeTraced, runner->vmContext->varWritesToBeTraced);
        shcopyFromTo(args.functionCallsToBeTraced, runner->vmContext->functionCallsToBeTraced);
        shcopyFromTo(args.alarmsToBeTraced, runner->vmContext->alarmsToBeTraced);
        shcopyFromTo(args.instanceLifecyclesToBeTraced, runner->vmContext->instanceLifecyclesToBeTraced);
        shcopyFromTo(args.eventsToBeTraced, runner->vmContext->eventsToBeTraced);
        shcopyFromTo(args.collisionsToBeTraced, runner->vmContext->collisionsToBeTraced);
        shcopyFromTo(args.opcodesToBeTraced, runner->vmContext->opcodesToBeTraced);
        shcopyFromTo(args.stackToBeTraced, runner->vmContext->stackToBeTraced);
        shcopyFromTo(args.tilesToBeTraced, runner->vmContext->tilesToBeTraced);
        runner->vmContext->traceBytecodeAfterFrame = args.traceBytecodeAfterFrame;
#endif
#ifdef ENABLE_VM_STUB_LOGS
        runner->vmContext->alwaysLogStubbedFunctions = args.alwaysLogStubbedFunctions;
#endif
        runner->vmContext->alwaysLogUnknownFunctions = args.alwaysLogUnknownFunctions;
        runner->vmContext->traceEventInherited = args.traceEventInherited;

#ifdef USE_CRASH_SIGNAL_HANDLER
        struct sigaction sa = {0};
        sa.sa_handler = onCrashSignal;
        sigemptyset(&sa.sa_mask);
        struct sigaction prev;
        sigaction(SIGABRT, &sa, &prev);
        PreviousSignalActionEntry p;
        p.key = SIGABRT;
        p.value = prev;
        hmputs(previousSignalActions, p);
        sigaction(SIGSEGV, &sa, &prev);
        PreviousSignalActionEntry p2;
        p.key = SIGSEGV;
        p.value = prev;
        hmputs(previousSignalActions, p2);
#endif

        // Initialize the first room and fire Game Start / Room Start events
        Runner_initFirstRoom(runner);

        // Main loop
        bool debugShowCollisionMasks = false;
        size_t overlayCachedMemBytes = 0;
        uint64_t overlayLastMemCheck = 0;
        bool freeCamActive = false;
        bool actuallyShuttingDown = false;
        bool wasPaused = false;
        uint64_t lastFrameTime = nowNanos();
        uint64_t lastFrameStartTime = lastFrameTime; // for delta_time
        bool shouldWindowClose = false;
        // --perf-log state: RSS is sampled occasionally because reading it is not free.
        uint32_t perfCachedRssBytes = 0;
        int perfRssTick = 0;
        int perfFramesSinceReport = 0;
        bool perfRoomKnown = false;
        const Room* perfLastRoom = nullptr;
        // Room-change texture prefetch: decode the incoming room's pages once,
        // behind the loading bar, instead of stalling inside the first draw calls.
        const Room* prefetchLastRoom = nullptr;
        while (true) {
            if (runner->shouldExit || shouldWindowClose) {
                actuallyShuttingDown = true;
                break;
            }

            if (runner->pendingWorkingDirectory != nullptr && runner->pendingLaunchParameters != nullptr) {
                // Break from the game loop, we'll handle this later
                break;
            }

            uint64_t frameStartNow = nowNanos();
            runner->deltaTime = (int64_t)(frameStartNow - lastFrameStartTime) / 1000.0;
            lastFrameStartTime = frameStartNow;

            // Clear last frame's pressed/released state, then poll new input events
            RunnerKeyboard_beginFrame(runner->keyboard);
            RunnerGamepad_beginFrame(runner->gamepads);
            RunnerMouse_beginFrame(runner->mouse);
            if (platformHandleEvents()) {
                shouldWindowClose = true;
                continue;
            }
            
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F8)) {
                bool isPaused = Runner_isPaused(runner);
                Runner_setPaused(runner, !isPaused);
            }
            bool enteringPause = runner->paused && !wasPaused;
            bool shouldStep = !runner->paused;
            bool shouldRender = !runner->paused || enteringPause;
            if (runner->debugMode && runner->paused) {
                shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O');
                if (shouldStep) {
                    shouldRender = true;
                    enteringPause = false;
                    logDebug("Frame advance (frame %d)\n", runner->frameCount);
                }
            }

            uint64_t frameStartTime = 0;
            uint64_t perfAfterStep = 0;
            uint64_t perfAfterDraw = 0;

            if (shouldStep) {
                frameStartTime = nowNanos();
                if (args.traceFrames) {
                    logInfo("Frame %d (Start)\n", runner->frameCount);
                }

                // Process input recording/playback (must happen after platformHandleEvents, before Runner_step)
                InputRecording_processFrame(globalInputRecording, runner->keyboard, inputFrameCount++);
            }

            // Go to next room
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)) {
                DataWin* dw = runner->dataWin;
                if ((int32_t) dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
                    int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
                    runner->pendingRoom = nextIdx;
                    runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    logDebug("Going to next room -> %s\n", dw->room.rooms[nextIdx].name);
                }
            }

            // Go to previous room
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)) {
                DataWin* dw = runner->dataWin;
                if (runner->currentRoomOrderPosition > 0) {
                    int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
                    runner->pendingRoom = prevIdx;
                    runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    logDebug("Going to previous room -> %s\n", dw->room.rooms[prevIdx].name);
                }
            }

            // Dump runner state to console
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F12)) {
                logDebug("Dumping runner state at frame %d\n", runner->frameCount);
                Runner_dumpState(runner);
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F11)) {
                logDebug("Dumping runner state at frame %d\n", runner->frameCount);
                char* json = Runner_dumpStateJson(runner);

                if (json != nullptr) {
                    if (args.dumpJsonFilePattern != nullptr) {
                        char filename[512];
                        snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                        FILE* f = fopen(filename, "wb");

                        if (f != nullptr) {
                            size_t len = strlen(json);
                            if (fwrite(json, 1, len, f) != len) {
                                logWarn("Error: Could not write JSON dump to '%s'\n", filename);
                                fclose(f);
                            } else {
                                fputc('\n', f);
                                fclose(f);
                                logInfo("JSON dump saved: %s\n", filename);
                            }
                        } else {
                            logWarn("Could not write JSON dump to '%s'\n", filename);
                        }
                    } else {
                        logInfo("%s\n", json);
                    }

                    free(json);
                }
            }

            // Toggle the debug overlay
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F1)) {
                showDebugOverlay = !showDebugOverlay;
                shouldRender = true;
                logDebug("Debug overlay %s!\n", showDebugOverlay ? "enabled" : "disabled");
            }

            // Toggle the collision mask debug overlay
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F2)) {
                debugShowCollisionMasks = !debugShowCollisionMasks;
                shouldRender = true;
                logDebug("Collision mask overlay %s!\n", debugShowCollisionMasks ? "enabled" : "disabled");
            }

            // Enable free cam
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F3)) {
                runner->freeCamPanX = 0.0f;
                runner->freeCamPanY = 0.0f;
                runner->freeCamZoom = 1.0f;

                freeCamActive = !freeCamActive;
                shouldRender = freeCamActive;
                logDebug("Free cam %s!\n", freeCamActive ? "enabled" : "disabled");
            }

            if (freeCamActive) {
                if (RunnerKeyboard_check(runner->keyboard, VK_UP)) {
                    runner->freeCamPanY -= (float) (0.000005f * runner->deltaTime);
                }

                if (RunnerKeyboard_check(runner->keyboard, VK_DOWN)) {
                    runner->freeCamPanY += (float) (0.000005f * runner->deltaTime);
                }

                if (RunnerKeyboard_check(runner->keyboard, VK_LEFT)) {
                    runner->freeCamPanX -= (float) (0.000005f * runner->deltaTime);
                }

                if (RunnerKeyboard_check(runner->keyboard, VK_RIGHT)) {
                    runner->freeCamPanX += (float) (0.000005f * runner->deltaTime);
                }
            }

            // Reset global interact state because I HATE when I get stuck while moving through rooms
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10)) {
                int32_t interactVarId = shget(runner->vmContext->varNameMap, "interact");

                Instance_setSelfVar(runner->vmContext->globalScopeInstance, interactVarId, RValue_makeInt32(0));
                logInfo("Changed global.interact [%d] value!\n", interactVarId);
            }

            bool currentKeyDown[GML_KEY_COUNT];
            bool currentKeyPressed[GML_KEY_COUNT];
            bool currentKeyReleased[GML_KEY_COUNT];

            if (freeCamActive) {
                // THIS IS A HACK!! We don't want to pass keys to the runner, but we DO want to keep it so we can hold the arrow keys to move the camera
                memcpy(currentKeyDown, runner->keyboard->keyDown, sizeof(runner->keyboard->keyDown));
                memcpy(currentKeyPressed, runner->keyboard->keyPressed, sizeof(runner->keyboard->keyPressed));
                memcpy(currentKeyReleased, runner->keyboard->keyReleased, sizeof(runner->keyboard->keyReleased));

                memset(runner->keyboard->keyDown, 0, sizeof(runner->keyboard->keyDown));
                memset(runner->keyboard->keyPressed, 0, sizeof(runner->keyboard->keyPressed));
                memset(runner->keyboard->keyReleased, 0, sizeof(runner->keyboard->keyReleased));
            }

            // Run one game step (Begin Step, Keyboard, Alarms, Step, End Step, room transitions)
            if (shouldStep) {
                Runner_step(runner);
                perfAfterStep = nowNanos();
            }

            if (freeCamActive) {
                memcpy(runner->keyboard->keyDown, currentKeyDown, sizeof(runner->keyboard->keyDown));
                memcpy(runner->keyboard->keyPressed, currentKeyPressed, sizeof(runner->keyboard->keyPressed));
                memcpy(runner->keyboard->keyReleased, currentKeyReleased, sizeof(runner->keyboard->keyReleased));
            }

            if (shouldStep) {
                if (args.profilerFramesBetween > 0 && runner->frameCount > 0 && runner->frameCount % args.profilerFramesBetween == 0) {
                    char* profilerReport = Profiler_createReport(vm->profiler, 20, args.profilerFramesBetween);
                    if (profilerReport != nullptr) {
                        logInfo("%s\n", profilerReport);
                        free(profilerReport);
                    }
                    Profiler_reset(vm->profiler);
                }

                // Update audio system (gain fading, cleanup ended sounds)
                float dt = (float) (runner->deltaTime / 1000000.0);
                if (0.0f > dt) dt = 0.0f;
                if (dt > 0.1f) dt = 0.1f; // cap delta to avoid huge fades on lag spikes
                runner->audioSystem->vtable->update(runner->audioSystem, dt);
            }

            // Dump full runner state if this frame was requested
            if (hmget(args.dumpFrames, runner->frameCount)) {
                Runner_dumpState(runner);
            }

            // Dump runner state as JSON if this frame was requested
            if (hmget(args.dumpJsonFrames, runner->frameCount)) {
                char* json = Runner_dumpStateJson(runner);

                if (json != nullptr) {
                    if (args.dumpJsonFilePattern != nullptr) {
                        char filename[512];
                        snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                        FILE* f = fopen(filename, "wb");

                        if (f != nullptr) {
                            size_t len = strlen(json);
                            if (fwrite(json, 1, len, f) != len) {
                                logWarn("Error: Could not write JSON dump to '%s'\n", filename);
                                fclose(f);
                            } else {
                                fputc('\n', f);
                                fclose(f);
                                logInfo("JSON dump saved: %s\n", filename);
                            }
                        } else {
                            logWarn("Could not write JSON dump to '%s'\n", filename);
                        }
                    } else {
                        logInfo("%s\n", json);
                    }
                    free(json);
                }
            }

            // Query actual framebuffer size
            int32_t fbWidth, fbHeight;
            platformGetWindowSize(&fbWidth, &fbHeight);

            if (shouldRender) {
                // Clear the default framebuffer (window background) to black
#ifdef ENABLE_SW_RENDERER
                if (gfx == SOFTWARE && shouldRender)
                    SWRenderer_clearFrameBuffer(renderer, 0);
#endif
#if defined(ENABLE_LEGACY_GL) || defined(ENABLE_MODERN_GL)
                if ((gfx == LEGACY_GL || gfx == MODERN_GL) && shouldRender) {
                    glBindFramebuffer(GL_FRAMEBUFFER, *hostFramebuffer);
                    glClear(GL_COLOR_BUFFER_BIT);
                }
#endif

                if (!runner->appSurfaceEnabled) {
                    runner->applicationWidth = fbWidth;
                    runner->applicationHeight = fbHeight;
                    runner->usingAppSurface = false;
                } else {
                    if (runner->applicationWidth <= 0 || runner->applicationHeight <= 0) {
                        runner->applicationWidth = (int32_t) gen8->defaultWindowWidth;
                        runner->applicationHeight = (int32_t) gen8->defaultWindowHeight;
                    }
                    runner->usingAppSurface = true;
                }

                int32_t gameW = runner->applicationWidth;
                int32_t gameH = runner->applicationHeight;

                // Widescreen hack: render into a surface grown toward the requested aspect to fake a different aspect
                // ratio. The game's logical applicationWidth/Height is left untouched (so the reads above stay the real
                // size and this never compounds frame-to-frame); only the local gameW/gameH used for the projection/FBO
                // grow. A wider-than-native target grows width (reveal left/right); a taller one grows height (reveal
                // top/bottom). Runner_drawViews reads widescreenExtraWidth/Height to expand each view to match.
                runner->widescreenExtraWidth = 0;
                runner->widescreenExtraHeight = 0;
                if (args.widescreenAspect > 0.0f && runner->usingAppSurface && gameW > 0 && gameH > 0) {
                    float nativeAspect = (float) gameW / (float) gameH;
                    if (args.widescreenAspect > nativeAspect) {
                        int32_t targetW = (int32_t) ((float) gameH * args.widescreenAspect + 0.5f);
                        if (targetW > gameW) {
                            runner->widescreenExtraWidth = targetW - gameW;
                            gameW = targetW;
                        }
                    } else if (args.widescreenAspect < nativeAspect) {
                        int32_t targetH = (int32_t) ((float) gameW / args.widescreenAspect + 0.5f);
                        if (targetH > gameH) {
                            runner->widescreenExtraHeight = targetH - gameH;
                            gameH = targetH;
                        }
                    }
                }

                Runner_drawPre(runner, fbWidth, fbHeight);

                // Calculate viewport (letterboxing) in screen coordinates for mouse mapping
                int32_t winW, winH;
                platformGetScaledWindowSize(&winW, &winH);

                Runner_beginFrame(runner, gameW, gameH, winW, winH, fbWidth, fbHeight);

                double mx, my;
                platformGetMousePos(&mx, &my);
                Runner_updateMousePosition(runner, winW, winH, mx, my);

                Runner_drawViews(runner, gameW, gameH, debugShowCollisionMasks);
                renderer->vtable->endFrameInit(renderer);
                Runner_drawPost(runner, fbWidth, fbHeight);
                renderer->vtable->endFrameEnd(renderer);
                Runner_drawGUI(runner, fbWidth, fbHeight, gameW, gameH);
                perfAfterDraw = nowNanos();

                if (showDebugOverlay && renderer->vtable->drawTextUI != nullptr) {
                    renderer->vtable->beginGUI(renderer, fbWidth, fbHeight, 0, 0, fbWidth, fbHeight, RENDER_TARGET_HOST_FRAMEBUFFER);

                    int32_t savedHalign = renderer->drawHalign;
                    int32_t savedValign = renderer->drawValign;
                    renderer->drawHalign = 0;
                    renderer->drawValign = 0;

                    char fpsText[64];
                    snprintf(fpsText, sizeof(fpsText), "FPS: %.1f", runner->fps);

                    float text_height = 10.0f;
                    renderer->vtable->drawTextUI(renderer, fpsText, 10.0f, text_height, 0.5f, 0.5f, 0.0f, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 1.0f, -1.0f);

                    /*
                     * get_used_memory() is too slow to do every frame so we
                     * cache the result and only re-check twice a second.
                     */
                    if (overlayCachedMemBytes == 0 || frameStartNow - overlayLastMemCheck >= 500000000U) {
                        overlayCachedMemBytes = get_used_memory();
                        overlayLastMemCheck = frameStartNow;
                    }
                    if (overlayCachedMemBytes != 0) {
                        char memText[96];
                        snprintf(memText, sizeof(memText), "Memory: %zu bytes (%.1f MB)", overlayCachedMemBytes, overlayCachedMemBytes / 1024.0f / 1024.0f);

                        text_height += DEFAULT_FONT_UI_LINE_HEIGHT * 0.5f;
                        renderer->vtable->drawTextUI(renderer, memText, 10.0f, text_height, 0.5f, 0.5f, 0.0f, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 1.0f, -1.0f);
                    }

                    renderer->drawHalign = savedHalign;
                    renderer->drawValign = savedValign;

                    renderer->vtable->endGUI(renderer);
                }
            }

            if (runner->paused && enteringPause && !runner->debugMode) {
                int32_t winW = fbWidth;
                int32_t winH = fbHeight;

                renderer->vtable->beginGUI(renderer, winW, winH, 0, 0, winW, winH, RENDER_TARGET_HOST_FRAMEBUFFER);
                renderer->vtable->drawRectangle(renderer, 0.0f, 0.0f, (float) winW, (float) winH, 0x000000, 0.35f, false);

                // Keep the pause icon visually consistent across screen sizes/aspect ratios by scaling
                // from the shorter dimension instead of the full width, which changes on 4:3 vs 16:9.
                float middleX = 0.5f * (float) winW;
                float middleY = 0.5f * (float) winH;
                float smallerSide = (float) (winW < winH ? winW : winH);

                float barWidth = smallerSide * 0.025f;
                float barHeight = smallerSide * 0.1f;
                float gap = smallerSide * 0.028f;

                float rightBarLeft = middleX + gap * 0.5f;
                float leftBarRight = middleX - gap * 0.5f;

                renderer->vtable->drawRectangle(renderer, rightBarLeft, middleY - barHeight / 2.0f, rightBarLeft + barWidth, middleY + barHeight / 2.0f, 0xFFFFFF, 1.0f, false);
                renderer->vtable->drawRectangle(renderer, leftBarRight - barWidth, middleY - barHeight / 2.0f, leftBarRight, middleY + barHeight / 2.0f, 0xFFFFFF, 1.0f, false);

                renderer->vtable->endGUI(renderer);
            }

#ifdef ENABLE_SCREENSHOTS
            // Capture screenshot if this frame matches a requested frame
            bool shouldScreenshot = hmget(args.screenshotFrames, runner->frameCount);

            if (shouldScreenshot || RunnerKeyboard_checkPressed(runner->keyboard, VK_F5)) {
                captureScreenshot(0, args.screenshotPattern, runner->frameCount, fbWidth, fbHeight, true);
                glBindFramebuffer(GL_FRAMEBUFFER, *hostFramebuffer);
            }

            // Dump all surfaces if this frame matches a requested frame
            bool shouldDumpSurfaces = hmget(args.screenshotSurfacesFrames, runner->frameCount);

            if (shouldDumpSurfaces || RunnerKeyboard_checkPressed(runner->keyboard, VK_F6)) {
                GLRenderer* gl = (GLRenderer*) renderer;
                dumpAllSurfaces(gl, args.screenshotSurfacesPattern, runner->frameCount);
                glBindFramebuffer(GL_FRAMEBUFFER, *hostFramebuffer);
            }
#endif

            if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
                logInfo("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
                shouldWindowClose = true;
            }

            if (shouldStep && args.traceFrames) {
                double frameElapsedMs = (int64_t)(nowNanos() - frameStartTime) / 1000000.0;
                logInfo("Frame %d (End, %.2f ms)\n", runner->frameCount, frameElapsedMs);
            }

            // Only present a new frame when we actually rendered one. This includes the first paused frame so the
            // pause overlay can be presented once, and all later paused frames stay frozen without re-swapping.
            if (runner->pendingRoom == -1 && shouldRender)
                platformSwapBuffers();
            if (shouldStep) {
                Runner_handlePendingRoomChange(runner);

#ifdef ENABLE_SW_RENDERER
                if (prefetchTextures && !args.headless && gfx == SOFTWARE && renderer != nullptr &&
                    runner->currentRoom != prefetchLastRoom) {
                    prefetchLastRoom = runner->currentRoom;
                    prefetchRoomTextures(runner, renderer, fbWidth, fbHeight);
                }
#endif

                if (perfLog != nullptr && perfAfterDraw != 0) {
                    if (++perfRssTick >= 30) {
                        perfRssTick = 0;
                        perfCachedRssBytes = (uint32_t) get_used_memory();
                    }

                    PerfLogFrame perfFrame = {0};
                    perfFrame.frame = runner->frameCount;
                    const Room* perfRoom = runner->currentRoom;
                    perfFrame.room = perfRoom != nullptr ? perfRoom->name : nullptr;
                    perfFrame.instanceCount = (int) arrlen(runner->instances);
                    perfFrame.stepMs = (double) (perfAfterStep - frameStartTime) / 1000000.0;
                    perfFrame.drawMs = (double) (perfAfterDraw - perfAfterStep) / 1000000.0;
                    // deltaTime is the wall time since the previous frame start, in microseconds.
                    perfFrame.frameMs = (double) runner->deltaTime / 1000.0;
                    perfFrame.audioCacheBytes = (uint32_t) runner->dataWin->audoOwnedBytes;
                    perfFrame.rssBytes = perfCachedRssBytes;
#ifdef ENABLE_SW_RENDERER
                    if (gfx == SOFTWARE) {
                        SWRendererTextureStats texStats;
                        SWRenderer_getTextureStats(renderer, &texStats);
                        perfFrame.textureBytes = texStats.textureBytes;
                        perfFrame.textureBudgetBytes = texStats.budgetBytes;
                        perfFrame.textureLoads = texStats.loads;
                        perfFrame.textureEvictions = texStats.evictions;
                        perfFrame.drawsAttempted = texStats.drawsAttempted;
                        perfFrame.drawsCulled = texStats.drawsCulled;
                        perfFrame.pxWritten = texStats.pxWritten;
                        perfFrame.pxFastCopy = texStats.pxFastCopy;
                    }
#endif
                    if (!perfRoomKnown || perfRoom != perfLastRoom) {
                        perfRoomKnown = true;
                        perfLastRoom = perfRoom;
                        PerfLog_roomChange(perfLog, &perfFrame);
                    }
                    if (PerfLog_frame(perfLog, &perfFrame)) {
                        // This frame stuttered; record which GML scripts were running.
                        if (vm->profiler != nullptr) {
                            char* perfReport = Profiler_createReport(vm->profiler, 15, perfFramesSinceReport + 1);
                            if (perfReport != nullptr) {
                                PerfLog_note(perfLog, "slow frame %d (%.1f ms):", perfFrame.frame, perfFrame.frameMs);
                                PerfLog_scriptReport(perfLog, perfReport);
                                free(perfReport);
                            }
                            Profiler_reset(vm->profiler);
                        }
                        perfFramesSinceReport = 0;
                    } else {
                        perfFramesSinceReport++;
                    }
                }
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_BACKSPACE)) {
                size_t bytes_used = get_used_memory();
                if (bytes_used == 0)
                    logWarn("Unable to get memory usage\n");
                else
                    logInfo("Memory use right now: %zu bytes (%.1f MB)\n", bytes_used, bytes_used / 1024.0f / 1024.0f);
            }

            wasPaused = runner->paused;

            // Limit frame rate to room speed (skip in headless mode for max speed!!)
            double effectiveGameSpeed = Runner_getEffectiveGameSpeed(runner);
            if (!args.headless && effectiveGameSpeed > 0.0) {
                bool fastForwardTabNow = RunnerKeyboard_checkPressed(runner->keyboard, VK_TAB);
                if (args.fastForwardSpeed > 0.0 && fastForwardTabNow && !fastForwardTabPrev) {
                    fastForwardActive = !fastForwardActive;
                    lastFrameTime = nowNanos();
                }
                fastForwardTabPrev = fastForwardTabNow;
                double effectiveSpeed = (args.fastForwardSpeed > 0.0 && fastForwardActive) ? args.fastForwardSpeed : args.speedMultiplier;
                uint64_t targetFrameTime = 1000000000 / (effectiveGameSpeed * effectiveSpeed);
                uint64_t nextFrameTime = lastFrameTime + targetFrameTime;
                platformSleepUntil(nextFrameTime);
            }
            lastFrameTime = nowNanos();
        }

        saveInputRecording();

        // Snapshot any pending game_change request before we tear the runner down
        char* nextWorkingDirectory = runner->pendingWorkingDirectory;
        char* nextLaunchParameters = runner->pendingLaunchParameters;
        runner->pendingWorkingDirectory = nullptr;
        runner->pendingLaunchParameters = nullptr;

        // Cleanup
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = nullptr;
        renderer->vtable->destroy(renderer);

        // Keep the window + GL context alive across game_change so we don't spawn a new window
        if (actuallyShuttingDown) {
            platformExit();
            platformInitialized = false;
        }

        Runner_free(runner);
        OverlayFileSystem_destroy(overlayFs);
#ifdef ENABLE_VM_OPCODE_PROFILER
        VM_printOpcodeProfilerReport(vm);
#endif
        VM_free(vm);
        DataWin_free(dataWin);
        PreProcessedStuff_free();

        if (actuallyShuttingDown) {
            if (perfLog != nullptr) {
                PerfLog_summary(perfLog);
                PerfLog_close(perfLog);
                perfLog = nullptr;
            }
            free(currentDataWinPath);
            repeat(arrlen(currentGameArgs), i) {
                free(currentGameArgs[i]);
            }
            arrfree(currentGameArgs);
            logInfo("Bye! :3\n");
#ifdef _WIN32
            timeEndPeriod(1);
#endif
            return 0;
        }

        // game_change was called, so we need to restart the runner with the new data.win and launch parameters
        bool macosGameChange = (args.osType == OS_MACOSX);
        char* dataWinFilename = nullptr;

        if (nextWorkingDirectory != nullptr && nextLaunchParameters != nullptr) {
            char** newArguments = nullptr;
            newArguments = extractRunnerArguments(nextLaunchParameters);

            // Extract the data.win filename from "-game <file>" inside the new launch parameters
            if (!macosGameChange) {
                // After extraction, we now need to figure out where is the "-game" argument
                size_t length = arrlen(newArguments);
                repeat(length, i) {
                    if (strcmp(newArguments[i], "-game") == 0) {
                        // So we already know that the data.win file will be the NEXT one
                        if (length - 1 == i)
                            break; // Where's the value?? Bailing...

                        dataWinFilename = safeStrdup(newArguments[i + 1]);
                        break;
                    }
                }
            }

            // For some reason in the official runner, this value is just hardcoded to be game.ios.
            if (macosGameChange) {
                dataWinFilename = safeStrdup("game.ios");
            }

            if (dataWinFilename == nullptr) {
                logError("Runner: Launch parameters '%s' did not contain a '-game <file>' entry! Shutting down...\n", nextLaunchParameters);
                free(nextWorkingDirectory);
                free(nextLaunchParameters);
                free(currentDataWinPath);
                repeat(arrlen(newArguments), i) {
                    free(newArguments[i]);
                }
                arrfree(newArguments);
                {
                repeat(arrlen(currentGameArgs), i) {
                    free(currentGameArgs[i]);
                }
                }
                arrfree(currentGameArgs);
                return 1;
            }

            char* newPath = buildGameChangeTargetPath(currentDataWinPath, nextWorkingDirectory, dataWinFilename);
            if (newPath == nullptr) {
                logError("Runner: Failed to build target path for game_change! Shutting down...\n");
                free(nextWorkingDirectory);
                free(nextLaunchParameters);
                free(currentDataWinPath);
                repeat(arrlen(newArguments), i) {
                    free(newArguments[i]);
                }
                arrfree(newArguments);
                repeat(arrlen(currentGameArgs), j) {
                    free(currentGameArgs[j]);
                }
                arrfree(currentGameArgs);
                return 1;
            }

            free(currentDataWinPath);
            currentDataWinPath = newPath;
            args.dataWinPath = currentDataWinPath;

            // Rebuild the gameArgs
            // First, we'll remove ALL args except the first one (which is the argv[0])
            while (arrlen(currentGameArgs) > 1) {
                free(currentGameArgs[1]);
                arrdel(currentGameArgs, 1);
            }

            if (macosGameChange) {
                arrput(currentGameArgs, safeStrdup("-game"));
                arrput(currentGameArgs, safeStrdup(currentDataWinPath));
            } else {
                repeat(arrlen(newArguments), i) {
                    arrput(currentGameArgs, safeStrdup(newArguments[i]));
                }
            }

            free(dataWinFilename);
            free(nextWorkingDirectory);
            free(nextLaunchParameters);
            repeat(arrlen(newArguments), i) {
                free(newArguments[i]);
            }
            arrfree(newArguments);
        }
    }
}

void freeCommandLineArgs(CommandLineArgs* args) {
#ifdef ENABLE_SCREENSHOTS
    hmfree(args->screenshotFrames);
    hmfree(args->screenshotSurfacesFrames);
#endif
    hmfree(args->dumpFrames);
    hmfree(args->dumpJsonFrames);
#ifdef ENABLE_VM_TRACING
    shfree(args->varReadsToBeTraced);
    shfree(args->varWritesToBeTraced);
    shfree(args->functionCallsToBeTraced);
    shfree(args->alarmsToBeTraced);
    shfree(args->instanceLifecyclesToBeTraced);
    shfree(args->eventsToBeTraced);
    shfree(args->collisionsToBeTraced);
    shfree(args->opcodesToBeTraced);
    shfree(args->stackToBeTraced);
    shfree(args->tilesToBeTraced);
#endif
    shfree(args->disassemble);
    repeat(arrlen(args->gameArgs), i) free(args->gameArgs[i]);
    arrfree(args->gameArgs);
}
