#pragma once

#ifdef MNGR_DEBUG

#define LOG_FILE_NAME "\\emu80.log"

#ifdef __cplusplus
extern "C" {
#endif
void logMsg(char* msg);
#ifdef __cplusplus
}
#endif

#define lprintf(...) { char tmp[256]; snprintf(tmp, 256, __VA_ARGS__); logMsg(tmp); }

#else

#define lprintf(...)

#endif
