#ifndef LOGD_LUA_MODULE_H
#define LOGD_LUA_MODULE_H

#include "lua/lua.h"

#define LUA_NAME_ON_LOG "on_log"
#define LUA_NAME_LOGD_MODULE "logd"

int luaopen_logd(lua_State* L);

#endif