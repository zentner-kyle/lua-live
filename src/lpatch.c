#include <pthread.h>
#include <zmq.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lstate.h"

#define LUA_LIVE_PATCH_TABLE_NAME "__lua_live_patch_table"
#define LUA_LIVE_PATCH_VERSIONS_TABLE_NAME "__lua_live_patch_versions_table"
#define LUA_LIVE_THREAD_STATE_NAME "__lua_live_thread_state"

typedef struct lua_live_thread_state {
  lua_State *main_state;
  pthread_t thread;
  pthread_mutex_t run_mutex;
  pthread_mutex_t resume_mutex;
  pthread_cond_t resume_cond;
  int patch_mode;
} lua_live_thread_state;

static LClosure * lookup_closure(lua_State * L, const char * name) {
  lua_getglobal(L, LUA_LIVE_PATCH_TABLE_NAME);
  lua_pushstring(L, name);
  lua_gettable(L, -2);
  LClosure * c = lua_tolclosure(L, -1);
  lua_pop(L, 2); /* Pop the closure and the table off. */
  return c;
}

static void set_closure(lua_State *L, const char * name, LClosure * c) {
  lua_getglobal(L, LUA_LIVE_PATCH_TABLE_NAME);
  lua_pushstring(L, name);
  lua_pushlclosure(L, c);
  lua_settable(L, -3);
  lua_pop(L, 1); /* Pop the table off. */
}

static void save_closure(lua_State *L, const char * name, LClosure * c) {
  lua_getglobal(L, LUA_LIVE_PATCH_VERSIONS_TABLE_NAME);
  lua_pushstring(L, name);
  lua_gettable(L, -2);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1); /* Remove the nil. */
    lua_pushstring(L, name);
    lua_newtable(L);
    lua_settable(L, -3);
    lua_pushstring(L, name);
    lua_gettable(L, -2);
  }
  lua_pushinteger(L, luaL_len(L, -1) + 1);
  lua_pushlclosure(L, c);
  lua_settable(L, -3);
  lua_pop(L, 2); /* Pop the two tables off. */
}

static void patch_prototype(Proto *old_proto, Proto *new_proto) {
  if (old_proto->numparams != new_proto->numparams ||
      old_proto->is_vararg != new_proto->is_vararg ||
      old_proto->sizeupvalues != new_proto->sizeupvalues) {
    printf("Patching of function prototype failed.\n");
    return;
  }
  old_proto->sizek = new_proto->sizek;
  old_proto->sizecode = new_proto->sizecode;
  old_proto->sizelineinfo = new_proto->sizelineinfo;
  old_proto->sizep = new_proto->sizep;
  old_proto->sizelocvars = new_proto->sizelocvars;
  old_proto->linedefined = new_proto->linedefined;
  old_proto->lastlinedefined = new_proto->lastlinedefined;
  old_proto->k = new_proto->k;
  old_proto->code = new_proto->code;
  old_proto->p = new_proto->p;
  old_proto->lineinfo = new_proto->lineinfo;
  old_proto->locvars = new_proto->locvars;
  old_proto->upvalues = new_proto->upvalues;
  old_proto->source = new_proto->source;
}

static int lua_patch(lua_State * L) {
  const char * name = luaL_checkstring(L, 1);
  LClosure * new_closure = lua_tolclosure(L, 2);
  lua_pop(L, 2);
  if (!name || !new_closure) {
    lua_pushstring(L, "Missing argument to patch function.\n");
    return lua_error(L);
  }
  save_closure(L, name, new_closure);
  LClosure * old_closure = lookup_closure(L, name);
  if (old_closure) {
    patch_prototype(old_closure->p, new_closure->p);
    lua_pushlclosure(L, old_closure);
  } else {
    set_closure(L, name, new_closure);
    lua_pushlclosure(L, new_closure);
  }
  return 1;
}

static int lua_start(lua_State *L) {
  lua_getglobal(L, LUA_LIVE_THREAD_STATE_NAME);
  lua_live_thread_state *state = (lua_live_thread_state *) lua_topointer(L, -1);
  lua_pop(L, 1);
  if (state->patch_mode) {
    /* Do nothing. */
    lua_pop(L, 1);
  } else {
    lua_call(L, 0, 0);
  }
  return 0;
}

static const luaL_Reg live_funcs[] = {
  {"patch", lua_patch},
  {"start", lua_start},
  {NULL, NULL}
};

static const char * lua_live_address = "tcp://127.0.0.1:5555";

static const int lua_live_max_source_size_mb = 8;

void pause_lua_hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  lua_getglobal(L, LUA_LIVE_THREAD_STATE_NAME);
  lua_live_thread_state *state = (lua_live_thread_state *) lua_topointer(L, -1);
  lua_pop(L, 1);
  pthread_mutex_unlock(&state->run_mutex);
  pthread_cond_wait(&state->resume_cond, &state->resume_mutex);
  pthread_mutex_lock(&state->run_mutex);
}

void pause_lua(lua_live_thread_state *state) {
  lua_Hook hook = lua_gethook(state->main_state);
  int mask = lua_gethookmask(state->main_state);
  int count = lua_gethookcount(state->main_state);
  lua_sethook(state->main_state, pause_lua_hook, LUA_MASKCOUNT, 1);
  pthread_mutex_lock(&state->run_mutex);
  lua_sethook(state->main_state, hook, mask, count);
}

void resume_lua(lua_live_thread_state *state) {
  pthread_mutex_unlock(&state->run_mutex);
  pthread_cond_signal(&state->resume_cond);
}

static void *lua_live_listener_thread(void *state_) {
  lua_live_thread_state *state = (lua_live_thread_state *)state_;
  void * context = zmq_ctx_new();
  void * responder = zmq_socket(context, ZMQ_REP);
  int rc = zmq_bind(responder, lua_live_address);
  int packet_size = lua_live_max_source_size_mb * 1024 * 1024;
  char *c_buf = malloc(packet_size);
  int ret;
  char *msg;
  size_t msg_size;
  const char * traceback;
  const char * format;
  if (rc != 0) {
    printf("Error listening on address '%s'.\n", lua_live_address);
    exit(0);
  }
  if (c_buf == NULL) {
    printf("Could not allocate buffer for source code.\n");
    exit(0);
  }
  while (1) {
    memset(c_buf, 0, packet_size);
    zmq_recv(responder, c_buf, packet_size - 1, 0);
    state->patch_mode = 1;
    pause_lua(state);
    lua_State * t = lua_newthread(state->main_state);
    ret = luaL_loadstring(t, c_buf);
    if (ret != LUA_OK) {
      printf("Error loading patch:\n");
    } else {
      ret = lua_resume(t, NULL, 0);
    }
    if (ret == LUA_OK) {
      msg = "{\"result\":\"Successfully patched.\"}";
      zmq_send(responder, msg, strlen(msg), 0);
    } else if (ret == LUA_YIELD) {
      printf("Top-level coroutine yielded!\n");
      msg = "{\"error\":\"Top level coroutine yielded!\"}";
      zmq_send(responder, msg, strlen(msg), 0);
      exit(0);
    } else {
      /* Something broke. */
      luaL_traceback(state->main_state, t, NULL, 0);
      traceback = lua_tostring(state->main_state, -1);
      format = "{\"error\": \"%s\"}";
      msg_size = strlen(format) + strlen(traceback) + 1;
      msg = malloc(msg_size);
      if (!msg) {
        printf("Could not allocate error message.\n");
        exit(0);
      }
      snprintf(msg, msg_size, traceback);
      zmq_send(responder, msg, msg_size, 0);
      /* traceback freed by lua? */
      /* msg freed by zmq. */
    }
    resume_lua(state);
  }
  return NULL;
}

int lua_open_live(lua_State *L) {
  luaL_newlibtable(L, live_funcs);
  luaL_setfuncs(L, live_funcs, 0);
  return 1;
}


int lua_live_init(lua_State *L) {
  lua_live_thread_state *state = malloc(sizeof(lua_live_thread_state));
  int error = 0;
  lua_newtable(L);
  lua_setglobal(L, LUA_LIVE_PATCH_TABLE_NAME);
  lua_newtable(L);
  lua_setglobal(L, LUA_LIVE_PATCH_VERSIONS_TABLE_NAME);
  if (state == NULL) {
    printf("Could not allocate state.\n");
    exit(0);
  }
  state->main_state = L;
  error |= pthread_mutex_init(&state->run_mutex, NULL);
  error |= pthread_mutex_init(&state->resume_mutex, NULL);
  error |= pthread_cond_init(&state->resume_cond, NULL);
  error |= pthread_create(&state->thread, NULL, &lua_live_listener_thread, (void *) state);
  if (error) {
    printf("Error initializing lua-live thread.\n");
    exit(0);
  }
  lua_pushlightuserdata(L, (void *) state);
  lua_setglobal(L, LUA_LIVE_THREAD_STATE_NAME);

  luaL_requiref(L, "live", lua_open_live, 1);
  return 1;
}
