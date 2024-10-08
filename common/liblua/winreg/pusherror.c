#include <ctype.h>
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>

#include <dbug.h>

#include "pusherror.h"

/* Push nil, followed by the Windows error message corresponding to
 * the error number, or a string giving the error value in decimal if
 * no error message is found.  If nresults is -2, always push nil and
 * the error message and return 2 even if error is NO_ERROR.  If
 * nresults is -1 and error is NO_ERROR, then push true and return 1.
 * Otherwise, if error is NO_ERROR, return nresults.
 */
int
windows_pusherror(lua_State *L, DWORD error, int nresults)
{
	DBUG_ENTER("windows_pusherror");
	if (error != NO_ERROR || nresults == -2) {
		char buffer[1024];
		size_t len, res;

		len = sprintf(buffer, "%lu (0x%lX): ", error, error);

		res = FormatMessage(
			FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
			0, error, 0, buffer + len, sizeof buffer - len, 0);
		if (res) {
			len += res;
			while (len > 0 && isspace(buffer[len - 1]))
				len--;
		}
		else
			len += sprintf(buffer + len, "<error string not available>");

		lua_pushnil(L);
		lua_pushlstring(L, buffer, len);
		nresults = 2;
	}
	else if (nresults < 0) {
		lua_pushboolean(L, 1);
		nresults = 1;
	}
	DBUG_T_RETURN(int, nresults);
}
