#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <assert.h>
#include <stdlib.h>

#include "ejoy2dgame.h"
#include "fault.h"
#include "shader.h"
#include "texture.h"
#include "ppm.h"
#include "spritepack.h"
#include "sprite.h"
#include "lmatrix.h"
#include "label.h"
#include "particle.h"
#include "lrenderbuffer.h"
#include "lgeometry.h"

//#define LOGIC_FRAME 30

#define EJOY_INIT "EJOY2D_INIT"
#define EJOY_UPDATE "EJOY2D_UPDATE"
#define EJOY_DRAWFRAME "EJOY2D_DRAWFRAME"
#define EJOY_TOUCH "EJOY2D_TOUCH"
#define EJOY_GESTURE "EJOY2D_GESTURE"
#define EJOY_MESSAGE "EJOY2D_MESSAGE"
#define EJOY_HANDLE_ERROR "EJOY2D_HANDLE_ERROR"
#define EJOY_RESUME "EJOY2D_RESUME"
#define EJOY_PAUSE "EJOY2D_PAUSE"
#define EJOY_VPUPDATE "EJOY2D_VPUPDATE"

#define TRACEBACK_FUNCTION 1
#define UPDATE_FUNCTION 2
#define DRAWFRAME_FUNCTION 3
#define VP_UPDATE_FUNCTION 4
#define TOP_FUNCTION 4

static int LOGIC_FRAME = 30;
static int VIEWPORT_FRAME = 30;

static int
_panic(lua_State *L) {
	const char * err = lua_tostring(L,-1);
	fault("%s", err);
	return 0;
}

static int
linject(lua_State *L) {
	static const char * ejoy_callback[] = {
		EJOY_INIT,
		EJOY_UPDATE,
		EJOY_DRAWFRAME,
        EJOY_VPUPDATE,
		EJOY_TOUCH,
		EJOY_GESTURE,
		EJOY_MESSAGE,
		EJOY_HANDLE_ERROR,
		EJOY_RESUME,
		EJOY_PAUSE,
	};
	int i;
	for (i=0;i<sizeof(ejoy_callback)/sizeof(ejoy_callback[0]);i++) {
		lua_getfield(L, lua_upvalueindex(1), ejoy_callback[i]);
		if (!lua_isfunction(L,-1)) {
			return luaL_error(L, "%s is not found", ejoy_callback[i]);
		}
		lua_setfield(L, LUA_REGISTRYINDEX, ejoy_callback[i]);
	}
	return 0;
}

static int
ejoy2d_framework(lua_State *L) {
	luaL_Reg l[] = {
		{ "inject", linject },
		{ NULL, NULL },
	};
	luaL_newlibtable(L, l);
	lua_pushvalue(L,-1);
	luaL_setfuncs(L,l,1);
	return 1;
}

static void
checkluaversion(lua_State *L) {
	const lua_Number *v = lua_version(L);
	if (v != lua_version(NULL))
		fault("multiple Lua VMs detected");
	else if (*v != LUA_VERSION_NUM) {
		fault("Lua version mismatch: app. needs %f, Lua core provides %f",
			LUA_VERSION_NUM, *v);
	}
}
#if __ANDROID__
#define OS_STRING "ANDROID"
#else
#ifdef __MACOSX
#define OS_STRING "MAC"
#else
#define STR_VALUE(arg)	#arg
#define _OS_STRING(name) STR_VALUE(name)
#define OS_STRING _OS_STRING(EJOY2D_OS)
#endif
#endif

lua_State *
ejoy2d_lua_init() {
	lua_State *L = luaL_newstate();
	
	lua_atpanic(L, _panic);
	luaL_openlibs(L);
	return L;
}

void
ejoy2d_init(lua_State *L) {
	checkluaversion(L);
    
	lua_pushliteral(L, OS_STRING);
	lua_setglobal(L , "OS");
    
#ifdef _EJOY_VER_
    lua_pushinteger(L, _EJOY_VER_);
    lua_setglobal(L , "_EJOY_VER_");
#endif
    
	luaL_requiref(L, "ejoy2d.shader.c", ejoy2d_shader, 0);
	luaL_requiref(L, "ejoy2d.framework", ejoy2d_framework, 0);
	luaL_requiref(L, "ejoy2d.ppm", ejoy2d_ppm, 0);
	luaL_requiref(L, "ejoy2d.spritepack.c", ejoy2d_spritepack, 0);
	luaL_requiref(L, "ejoy2d.sprite.c", ejoy2d_sprite, 0);
	luaL_requiref(L, "ejoy2d.renderbuffer", ejoy2d_renderbuffer, 0);
	luaL_requiref(L, "ejoy2d.matrix.c", ejoy2d_matrix, 0);
	luaL_requiref(L, "ejoy2d.particle.c", ejoy2d_particle, 0);
	luaL_requiref(L, "ejoy2d.geometry.c", ejoy2d_geometry, 0);

	lua_settop(L,0);

	shader_init();
	label_load();
}

struct game *
ejoy2d_game() {
	struct game *G = (struct game *)malloc(sizeof(*G));
	lua_State *L = ejoy2d_lua_init();

	G->L = L;
	G->real_time = 0;
	G->logic_time = 0;
    G->vp_real_time = 0;
    G->vp_logic_time = 0;
    G->update_count = 0;
    
	ejoy2d_init(L);

	return G;
}

void
ejoy2d_close_lua(struct game *G) {
	if (G) {
		if (G->L) {
			lua_close(G->L);
			G->L = NULL;
		}
		free(G);
	}
}

void
ejoy2d_game_exit(struct game *G) {
	ejoy2d_close_lua(G);
	label_unload();
	texture_exit();
	shader_unload();
}

lua_State *
ejoy2d_game_lua(struct game *G) {
	return G->L;
}

static int
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else if (!lua_isnoneornil(L, 1)) {
	if (!luaL_callmeta(L, 1, "__tostring"))
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

void
ejoy2d_game_logicframe(int frame) {
	LOGIC_FRAME = frame;
}

void
ejoy2d_game_vpframe(int frame) {
    VIEWPORT_FRAME = frame;
}

void
ejoy2d_game_start(struct game *G) {
	lua_State *L = G->L;
	lua_getfield(L, LUA_REGISTRYINDEX, EJOY_INIT);
	lua_call(L, 0, 0);
	assert(lua_gettop(L) == 0);
	lua_pushcfunction(L, traceback);
	lua_getfield(L,LUA_REGISTRYINDEX, EJOY_UPDATE);
	lua_getfield(L,LUA_REGISTRYINDEX, EJOY_DRAWFRAME);
	lua_getfield(L,LUA_REGISTRYINDEX, EJOY_VPUPDATE);
	lua_getfield(L,LUA_REGISTRYINDEX, EJOY_MESSAGE);
  lua_getfield(L,LUA_REGISTRYINDEX, EJOY_RESUME);
	lua_getfield(L, LUA_REGISTRYINDEX, EJOY_PAUSE);
}


void 
ejoy2d_handle_error(lua_State *L, const char *err_type, const char *msg) {
	lua_getfield(L, LUA_REGISTRYINDEX, EJOY_HANDLE_ERROR);
	lua_pushstring(L, err_type);
	lua_pushstring(L, msg);
	int err = lua_pcall(L, 2, 0, 0);
	switch(err) {
	case LUA_OK:
		break;
	case LUA_ERRRUN:
		fault("!LUA_ERRRUN : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		fault("!LUA_ERRMEM : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRERR:
		fault("!LUA_ERRERR : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRGCMM:
		fault("!LUA_ERRGCMM : %s\n", lua_tostring(L,-1));
		break;
	default:
		fault("!Unknown Lua error: %d\n", err);
		break;
	}
}

static int
call(lua_State *L, int n, int r) {
	int err = lua_pcall(L, n, r, TRACEBACK_FUNCTION);
	switch(err) {
	case LUA_OK:
		break;
	case LUA_ERRRUN:
		ejoy2d_handle_error(L, "LUA_ERRRUN", lua_tostring(L,-1));
		fault("!LUA_ERRRUN : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		ejoy2d_handle_error(L, "LUA_ERRMEM", lua_tostring(L,-1));
		fault("!LUA_ERRMEM : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRERR:
		ejoy2d_handle_error(L, "LUA_ERRERR", lua_tostring(L,-1));
		fault("!LUA_ERRERR : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRGCMM:
		ejoy2d_handle_error(L, "LUA_ERRGCMM", lua_tostring(L,-1));
		fault("!LUA_ERRGCMM : %s\n", lua_tostring(L,-1));
		break;
	default:
		ejoy2d_handle_error(L, "UnknownError", "Unknown");
		fault("!Unknown Lua error: %d\n", err);
		break;
	}
	return err;
}

void
ejoy2d_call_lua(lua_State *L, int n, int r) {
  call(L, n, r);
	lua_settop(L, TOP_FUNCTION);
}

static void
logic_frame(lua_State *L) {
	lua_pushvalue(L, UPDATE_FUNCTION);
	call(L, 0, 0);
	lua_settop(L, TOP_FUNCTION);
}

static void
viewport_frame(lua_State *L, float time) {
    lua_pushvalue(L, VP_UPDATE_FUNCTION);
    lua_pushnumber(L, time);
    call(L, 1, 0);
    lua_settop(L, TOP_FUNCTION);
}

static void 
ejoy2d_viewport_frame(struct game *G, float time) {
    float dt = 1.0f/VIEWPORT_FRAME;
    if (G->vp_logic_time == 0) {
        G->vp_real_time = dt;
    } else {
        G->vp_real_time += time;
    }
    
    while (G->vp_logic_time < G->vp_real_time) {
        G->vp_logic_time += dt;
        viewport_frame(G->L, dt);
    }
}

void
ejoy2d_game_update(struct game *G, float time) {
	if (G->logic_time == 0) {
		G->real_time = 1.0f/LOGIC_FRAME;
        G->update_count = 0;
	} else {
		G->real_time += time;
        G->update_count += 1;
        G->cur_fps = 1/time;
	}

	while (G->logic_time < G->real_time) {
		G->logic_time += 1.0f/LOGIC_FRAME;
        logic_frame(G->L);
	}

    ejoy2d_viewport_frame(G, time);
}

void
ejoy2d_game_drawframe(struct game *G) {
	reset_drawcall_count();
	lua_pushvalue(G->L, DRAWFRAME_FUNCTION);
	call(G->L, 0, 0);
	lua_settop(G->L, TOP_FUNCTION);
	shader_flush();
	label_flush();
    G->last_draw_call = drawcall_count();
    G->last_obj_count = object_count();
}

int
ejoy2d_game_touch(struct game *G, int id, float x, float y, int status) {
    int disable_gesture = 0;
	lua_getfield(G->L, LUA_REGISTRYINDEX, EJOY_TOUCH);
	lua_pushnumber(G->L, x);
	lua_pushnumber(G->L, y);
	lua_pushinteger(G->L, status+1);
	lua_pushinteger(G->L, id);
	int err = call(G->L, 4, 1);
  if (err == LUA_OK) {
      disable_gesture = lua_toboolean(G->L, -1);
  }
  lua_settop(G->L, TOP_FUNCTION);
  return disable_gesture;
}

void
ejoy2d_game_gesture(struct game *G, int type,
                    double x1, double y1,double x2,double y2, int s) {
    lua_getfield(G->L, LUA_REGISTRYINDEX, EJOY_GESTURE);
    lua_pushnumber(G->L, type);
    lua_pushnumber(G->L, x1);
    lua_pushnumber(G->L, y1);
    lua_pushnumber(G->L, x2);
    lua_pushnumber(G->L, y2);
    lua_pushinteger(G->L, s);
    call(G->L, 6, 0);
    lua_settop(G->L, TOP_FUNCTION);
}

void
ejoy2d_game_message(struct game* G,int id_, const char* state, const char* data, lua_Number n) {
    lua_State *L = G->L;
    lua_getfield(L, LUA_REGISTRYINDEX, EJOY_MESSAGE);
    lua_pushnumber(L, id_);
    
    if (state != NULL) {
        lua_pushstring(L, state);
    } else {
        lua_pushnil(L);
    }
    
    if (data != NULL) {
        lua_pushstring(L, data);
    } else {
        lua_pushnil(L);
    }
    
    lua_pushnumber(L, n);
    call(L, 4, 0);
    lua_settop(L, TOP_FUNCTION);
}

void
ejoy2d_game_resume(struct game* G){
    lua_State *L = G->L;
    lua_getfield(L, LUA_REGISTRYINDEX, EJOY_RESUME);
    call(L, 0, 0);
    lua_settop(L, TOP_FUNCTION);
}

void
ejoy2d_game_pause(struct game* G) {
	lua_State *L = G->L;
	lua_getfield(L, LUA_REGISTRYINDEX, EJOY_PAUSE);
	call(L, 0, 0);
	lua_settop(L, TOP_FUNCTION);
}
