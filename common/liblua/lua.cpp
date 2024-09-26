extern "C"
{
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
int main()
{
   lua_State * S = luaL_newstate();
   luaL_openlibs(S);
   luaL_loadfile(S,"lua_script.lua");
   lua_call(S,0,0);
   lua_close(S);
}
