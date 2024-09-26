#ifndef pusherror_h
#define pusherror_h

#include <lua.h>
#include <windows.h>

int windows_pusherror(lua_State *L, DWORD error, int nresults);

#endif/*pusherror_h*/
