#pragma once

//#include "_ansi.h"
//int	snprintf (char *__restrict, unsigned int size, const char *__restrict, ...) _ATTRIBUTE ((__format__ (__printf__, 3, 4)));

#ifdef MNGR_DEBUG

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
