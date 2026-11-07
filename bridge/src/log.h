#pragma once

void logInit(const wchar_t* filePath);  // nullptr = console only
void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);
void logVerbose(const char* fmt, ...);
void logSetVerbose(bool on);
void logSetConsole(bool on);  // write to stdout (off for the windowless build)
