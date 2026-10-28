#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "gettime.h"
#include "utils.h"

// In the platform main.c
void platformLog(const logType type, const char *format, va_list va);

// Stop mirroring once the file reaches this size so a long session cannot fill
// the SD card. The perf log (--perf-log) is tiny by comparison.
#define LOG_FILE_MAX_BYTES ((long) (8 * 1024 * 1024))

static FILE* logFileHandle = NULL;
static char* logFilePath = NULL;
static uint64_t logFileStartNanos = 0;
static bool logFileLimitHit = false;

bool logSetFile(const char* path) {
	logCloseFile();
	if (path == NULL || path[0] == '\0')
		return false;

	FILE* f = fopen(path, "wb");
	if (f == NULL)
		return false;

	logFileHandle = f;
	logFileStartNanos = nowNanos();
	logFileLimitHit = false;
	logFilePath = safeStrdup(path);
	fprintf(f, "[%9.3f] Butterscotch log started\n", 0.0);
	fflush(f);
	return true;
}

void logCloseFile(void) {
	if (logFileHandle != NULL) {
		fflush(logFileHandle);
		fclose(logFileHandle);
		logFileHandle = NULL;
	}
	if (logFilePath != NULL) {
		free(logFilePath);
		logFilePath = NULL;
	}
}

bool logIsFileOpen(void) {
	return logFileHandle != NULL;
}

const char* logGetFilePath(void) {
	return logFilePath;
}

// Writes one formatted message to the mirror, tagging every physical line with
// the elapsed time and the severity prefix. Does nothing when no file is open.
static void logFileEmit(logType type, const char* fmt, va_list va) {
	if (logFileHandle == NULL || logFileLimitHit)
		return;

	const char* prefix = "";
	switch (type) {
		case LOG_TYPE_WARNING:
			prefix = "Warning: ";
			break;
		case LOG_TYPE_ERROR:
			prefix = "Error: ";
			break;
		case LOG_TYPE_DEBUG:
			prefix = "Debug: ";
			break;
		default:
			break;
	}

	char stackBuf[1024];
	char* buf = stackBuf;
	va_list copy;
	va_copy(copy, va);
	int needed = vsnprintf(stackBuf, sizeof(stackBuf), fmt, copy);
	va_end(copy);
	if (needed < 0)
		return;
	if ((size_t) needed >= sizeof(stackBuf)) {
		buf = (char*) malloc((size_t) needed + 1);
		if (buf == NULL)
			return;
		va_copy(copy, va);
		vsnprintf(buf, (size_t) needed + 1, fmt, copy);
		va_end(copy);
	}

	double elapsedSec = (double) (nowNanos() - logFileStartNanos) / 1000000000.0;

	const char* line = buf;
	while (true) {
		const char* end = strchr(line, '\n');
		size_t lineLen = end != NULL ? (size_t) (end - line) : strlen(line);
		fprintf(logFileHandle, "[%9.3f] %s", elapsedSec, prefix);
		fwrite(line, 1, lineLen, logFileHandle);
		fputc('\n', logFileHandle);
		if (end == NULL || end[1] == '\0')
			break;
		line = end + 1;
	}

	if (buf != stackBuf)
		free(buf);

	if (ftell(logFileHandle) >= LOG_FILE_MAX_BYTES) {
		logFileLimitHit = true;
		fprintf(logFileHandle, "[%9.3f] log size limit reached; further output is not mirrored\n", elapsedSec);
	}
	fflush(logFileHandle);
}

// Example impl:
// void platformLog(const logType type, const char *format, va_list va) {
//     FILE *out = stderr;
//     switch (type) {
//         case LOG_TYPE_NORMAL:
//             out = stdout;
//             fputs(ANSI_COLOUR_CODE_RESET, out);
//             break;
//         case LOG_TYPE_WARNING:
//             fputs(ANSI_COLOUR_CODE_BOLD_YELLOW"Warning: ", out);
//             break;
//         case LOG_TYPE_ERROR:
//             fputs(ANSI_COLOUR_CODE_BOLD_RED"Error: ", out);
//             break;
// 		case LOG_TYPE_DEBUG:
//             fputs(ANSI_COLOUR_CODE_BOLD_PURPLE"Debug: ", out);
//             break;
//     }
//     vfprintf(out, format, va);
// 	   fputs(ANSI_COLOUR_CODE_RESET, out);
// }

void logInfo(const char* fmt, ...) {
	va_list va, fileVa;

	va_start(va, fmt);
	va_copy(fileVa, va);
	platformLog(LOG_TYPE_NORMAL, fmt, va);
	logFileEmit(LOG_TYPE_NORMAL, fmt, fileVa);
	va_end(fileVa);
	va_end(va);
}

void vLogInfo(const char* fmt, va_list va) {
	va_list fileVa;

	va_copy(fileVa, va);
	platformLog(LOG_TYPE_NORMAL, fmt, va);
	logFileEmit(LOG_TYPE_NORMAL, fmt, fileVa);
	va_end(fileVa);
}

void logWarn(const char* fmt, ...) {
	va_list va, fileVa;

	va_start(va, fmt);
	va_copy(fileVa, va);
	platformLog(LOG_TYPE_WARNING, fmt, va);
	logFileEmit(LOG_TYPE_WARNING, fmt, fileVa);
	va_end(fileVa);
	va_end(va);
}

void vLogWarn(const char* fmt, va_list va) {
	va_list fileVa;

	va_copy(fileVa, va);
	platformLog(LOG_TYPE_WARNING, fmt, va);
	logFileEmit(LOG_TYPE_WARNING, fmt, fileVa);
	va_end(fileVa);
}

void logError(const char* fmt, ...) {
	va_list va, fileVa;

	va_start(va, fmt);
	va_copy(fileVa, va);
	platformLog(LOG_TYPE_ERROR, fmt, va);
	logFileEmit(LOG_TYPE_ERROR, fmt, fileVa);
	va_end(fileVa);
	va_end(va);
}

void vLogError(const char* fmt, va_list va) {
	va_list fileVa;

	va_copy(fileVa, va);
	platformLog(LOG_TYPE_ERROR, fmt, va);
	logFileEmit(LOG_TYPE_ERROR, fmt, fileVa);
	va_end(fileVa);
}

void logDebug(const char* fmt, ...) {
	va_list va, fileVa;

	va_start(va, fmt);
	va_copy(fileVa, va);
	platformLog(LOG_TYPE_DEBUG, fmt, va);
	logFileEmit(LOG_TYPE_DEBUG, fmt, fileVa);
	va_end(fileVa);
	va_end(va);
}

void vLogDebug(const char* fmt, va_list va) {
	va_list fileVa;

	va_copy(fileVa, va);
	platformLog(LOG_TYPE_DEBUG, fmt, va);
	logFileEmit(LOG_TYPE_DEBUG, fmt, fileVa);
	va_end(fileVa);
}
