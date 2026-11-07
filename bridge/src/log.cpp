#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_verbose = false;
bool g_console = true;

void emit(const char* level, const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s %s\n", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, msg);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_console) {
        fputs(line, stdout);
        fflush(stdout);
    }
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}
}  // namespace

void logInit(const wchar_t* filePath) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = filePath ? _wfsopen(filePath, L"a", _SH_DENYWR) : nullptr;
}

void logSetVerbose(bool on) { g_verbose = on; }
void logSetConsole(bool on) { g_console = on; }

#define LOG_IMPL(name, level)            \
    void name(const char* fmt, ...) {    \
        va_list ap;                      \
        va_start(ap, fmt);               \
        emit(level, fmt, ap);            \
        va_end(ap);                      \
    }
LOG_IMPL(logInfo, "INFO ")
LOG_IMPL(logWarn, "WARN ")
LOG_IMPL(logError, "ERROR")

void logVerbose(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    emit("DEBUG", fmt, ap);
    va_end(ap);
}
