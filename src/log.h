#ifndef _BS_LOG_H
#define _BS_LOG_H

#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>

typedef enum {
	LOG_TYPE_NORMAL=0,
	LOG_TYPE_WARNING=1,
	LOG_TYPE_ERROR=2,
	LOG_TYPE_DEBUG=3
} logType;

#define ANSI_COLOUR_CODE_RESET "\033[0m"
#define ANSI_COLOUR_CODE_BOLD_YELLOW "\033[1;33m"
#define ANSI_COLOUR_CODE_BOLD_RED "\033[1;31m"
#define ANSI_COLOUR_CODE_BOLD_PURPLE "\033[1;35m"

void logInfo(const char* fmt, ...);
void vLogInfo(const char* fmt, va_list va);

void logWarn(const char* fmt, ...);
void vLogWarn(const char* fmt, va_list va);

void logError(const char* fmt, ...);
void vLogError(const char* fmt, va_list va);

void logDebug(const char* fmt, ...);
void vLogDebug(const char* fmt, va_list va);

// ===[ File mirror ]===
// Mirrors every log line into a plain-text file so a session running on a
// handheld (where stdout goes nowhere) can be copied off the SD card and
// shared. Each physical line gets an elapsed-seconds prefix so it can be lined
// up with --perf-log output. Returns false when the file could not be opened.
bool logSetFile(const char* path);
void logCloseFile(void);
bool logIsFileOpen(void);
const char* logGetFilePath(void);

#endif /* _BS_LOG_H */
