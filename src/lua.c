#include <errno.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "./lua.h"

#include "./config.h"
#include "luvi/lenv.c"
#include "luvi/lminiz.c"
#include "luvi/luvi.c"
#include "luvi/luvi.h"

#include <luv/luv.h>
#include <uv.h>

#include "util.h"

/* the following headers are auto-generated by xxd */
#include "logdconfig.lua.h"
#include "logdinit.lua.h"
#include "luvibundle.lua.h"
#include "luvipath.lua.h"

#define ON_LOG_INTERNAL "__logd_on_log"

static int lua_load_init_modules(lua_t* l)
{
	char* logdinit_lua_buf = NULL;
	char* luvibundle_lua_buf = NULL;
	char* luvipath_lua_buf = NULL;
	char* logdconfig_lua_buf = NULL;
	int ret = 0;

	if ((logdinit_lua_buf = calloc(1, logdinit_lua_len + 1)) == NULL ||
	  (logdconfig_lua_buf = calloc(1, logdconfig_lua_len + 1)) == NULL ||
	  (luvibundle_lua_buf = calloc(1, luvibundle_lua_len + 1)) == NULL ||
	  (luvipath_lua_buf = calloc(1, luvipath_lua_len + 1)) == NULL) {
		errno = ENOMEM;
		ret = 1;
		goto exit;
	}

	memcpy(logdinit_lua_buf, logdinit_lua, logdinit_lua_len);
	memcpy(luvibundle_lua_buf, luvibundle_lua, luvibundle_lua_len);
	memcpy(luvipath_lua_buf, luvipath_lua, luvipath_lua_len);
	memcpy(logdconfig_lua_buf, logdconfig_lua, logdconfig_lua_len);

	if ((ret = luaL_dostring(l->state, luvipath_lua_buf)) ||
	  (ret = luaL_dostring(l->state, logdconfig_lua_buf)) ||
	  (ret = luaL_dostring(l->state, luvibundle_lua_buf)) ||
	  (ret = luaL_dostring(l->state, logdinit_lua_buf))) {
		fprintf(
		  stderr, "Couldn't load script: %s\n", lua_tostring(l->state, -1));
		errno = EINVAL;
		goto exit;
	}

exit:
	if (logdinit_lua_buf)
		free(logdinit_lua_buf);
	if (luvibundle_lua_buf)
		free(luvibundle_lua_buf);
	if (luvipath_lua_buf)
		free(luvipath_lua_buf);
	if (logdconfig_lua_buf)
		free(logdconfig_lua_buf);

	return ret;
}

static int lua_load_libs(lua_t* l, uv_loop_t* loop)
{
	luaopen_logd(l->state);

	if (luaopen_luv_loop(l->state, loop) < 0)
		return 1;

	lua_getglobal(l->state, "package");
	lua_getfield(l->state, -1, "preload");
	lua_remove(l->state, -2);

#ifdef LOGD_WITH_ZLIB
	lua_pushcfunction(l->state, luaopen_zlib);
	lua_setfield(l->state, -2, "zlib");
#endif

#ifdef LOGD_WITH_LPEG
	lua_pushcfunction(l->state, luaopen_lpeg);
	lua_setfield(l->state, -2, "lpeg");
#endif

#ifdef LOGD_WITH_OPENSSL
	lua_pushcfunction(l->state, luaopen_openssl);
	lua_setfield(l->state, -2, "openssl");
#endif

	/* used by luvibundle.lua */
	lua_pushcfunction(l->state, luaopen_miniz);
	lua_setfield(l->state, -2, "miniz");

	/* used for version checking */
	lua_pushcfunction(l->state, luaopen_luvi);
	lua_setfield(l->state, -2, "luvi");

	lua_pushcfunction(l->state, luaopen_env);
	lua_setfield(l->state, -2, "env");

	lua_pop(l->state, 1);

	return 0;
}

char* make_run_str(lua_State* state, const char* script)
{
	char* run_str = NULL;

	int want = snprintf(NULL, 0, "return require('logdinit')('%s')", script);
	if ((run_str = malloc(want + 1)) == NULL) {
		perror("malloc");
		errno = ENOMEM;
		goto exit;
	}
	sprintf(run_str, "return require('logdinit')('%s')", script);

exit:
	return run_str;
}

static void lua_push_on_error(lua_State* state)
{
	lua_getglobal(state, LUA_NAME_LOGD_MODULE);
	lua_getfield(state, -1, LUA_NAME_ON_ERROR);
}

static void lua_push_on_exit(lua_State* state)
{
	lua_getglobal(state, LUA_NAME_LOGD_MODULE);
	lua_getfield(state, -1, LUA_NAME_ON_EXIT);
}

lua_t* lua_create(uv_loop_t* loop, const char* script)
{
	lua_t* l = NULL;

	if ((l = calloc(1, sizeof(lua_t))) == NULL) {
		perror("calloc");
		goto error;
	}

	if (lua_init(l, loop, script) == -1) {
		goto error;
	}

	return l;

error:
	if (l)
		free(l);
	return NULL;
}

int lua_init(lua_t* l, uv_loop_t* loop, const char* script)
{
	char* run_str = NULL;
	int lerrno;

	l->loop = loop;
	l->state = luaL_newstate();
	if (l->state == NULL) {
		errno = ENOMEM;
		perror("luaL_newstate");
		goto error;
	}

	luaL_openlibs(l->state);

	if (lua_load_libs(l, l->loop) != 0) {
		perror("lua_load_libs");
		goto error;
	}

	if (lua_load_init_modules(l) != 0) {
		perror("lua_load_init_modules");
		goto error;
	}

	if ((run_str = make_run_str(l->state, script)) == NULL) {
		goto error;
	}

	/* Load the init.lua builtin script */
	if (luaL_dostring(l->state, run_str)) {
		fprintf(
		  stderr, "Couldn't load script: %s\n", lua_tostring(l->state, -1));
		errno = EINVAL;
		goto error;
	}

	lua_getglobal(l->state, LUA_NAME_LOGD_MODULE);
	lua_getfield(l->state, -1, LUA_NAME_ON_LOG);
	if (!lua_isfunction(l->state, -1)) {
		fprintf(stderr,
		  "Couldn not find '" LUA_NAME_LOGD_MODULE "." LUA_NAME_ON_LOG
		  "' function in loaded script\n");
		errno = EINVAL;
		goto error;
	}

	lua_setglobal(l->state, ON_LOG_INTERNAL);
	/* pop logd module */
	lua_pop(l->state, 1);

	free(run_str);
	return 0;

error:
	lerrno = errno;
	if (run_str)
		free(run_str);
	if (l->state)
		lua_close(l->state);
	errno = lerrno;
	return -1;
}

void lua_call_on_log(lua_t* l, const char* data, log_t* log)
{
	lua_getglobal(l->state, ON_LOG_INTERNAL);
	DEBUG_ASSERT(lua_isfunction(l->state, -1));

	lua_pushlightuserdata(l->state, log);
	lua_pushstring(l->state, data);

	lua_call(l->state, 2, 0);
}

bool lua_on_error_defined(lua_t* l)
{
	bool ret;

	lua_push_on_error(l->state);
	ret = lua_isfunction(l->state, -1);
	lua_pop(l->state, 2);

	return ret;
}

void lua_call_on_error(
  lua_t* l, const char* err, log_t* partial, const char* data, const char* at)
{
	lua_push_on_error(l->state);
	DEBUG_ASSERT(lua_isfunction(l->state, -1));

	lua_pushstring(l->state, err);
	lua_pushlightuserdata(l->state, partial);
	lua_pushstring(l->state, at);
	lua_pushstring(l->state, data);

	lua_call(l->state, 4, 0);
	lua_pop(l->state, 1); // logd module
}

bool lua_on_exit_defined(lua_t* l)
{
	bool ret;

	lua_push_on_exit(l->state);
	ret = lua_isfunction(l->state, -1);
	lua_pop(l->state, 2);

	return ret;
}

void lua_call_on_exit(lua_t* l, enum exit_reason reason, const char* reason_str)
{
	lua_push_on_exit(l->state);
	DEBUG_ASSERT(lua_isfunction(l->state, -1));

	lua_pushinteger(l->state, reason);
	lua_pushstring(l->state, reason_str);
	lua_call(l->state, 2, 0);
	lua_pop(l->state, 1); // logd module
}

void lua_free(lua_t* l)
{
	if (l) {
		lua_close(l->state);
		free(l);
	}
}
