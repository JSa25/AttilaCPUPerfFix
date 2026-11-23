// scriptext64: script extender, ported from heapfix/scriptext.c: loose Lua scripts and a Lua console through the
// game's own Lua 5.1 (linked into the game file; its functions are at the addresses the symbol table names and are
// checked by their first bytes). Opt-in: nothing happens unless the attila64 folder has a lua_ext\ folder.
// The game makes one Lua thread per script environment in UTILITYDLL::LUA::ScriptManager::CreateThread(State &out,
// State *parent) (0x2e03d20: it stores the thread in out's first field) and drops it in ReleaseThread(State &)
// (0x2e04200). x86 read the manager's list every frame; here both functions are hooked at their entry instead, so the
// list's layout is not needed.
// Every frame (d3d64.c's per-frame point), on the thread the environments were made on and only there:
//  - an environment seen for 2 frames in a row gets ext_print() and runs every lua_ext\*.lua, in name order;
//  - lines typed in the console (Ctrl+` toggles its window) run in the selected environment, the newest by default.
// Output goes to the console and to lua_ext\lua_ext.log.
// This file is generated: tools of the port keep the console and script-running code of heapfix/scriptext.c word for
// word (scratch gen_scriptext64.py: scriptext64_head.inc + that code + scriptext64_tail.inc). Edit the .inc files.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "perffix64.h"

#ifndef SV
#define SV __attribute__((sysv_abi))   // the test builds real Lua with the Windows convention and defines SV empty
#endif
typedef struct lua_State lua_State;
typedef SV int (*lua_CFunction)(lua_State *L);
#define LUA_GLOBALSINDEX (-10002)
#define LUA_MULTRET (-1)

static SV int (*l_gettop)(lua_State *L);
static SV void (*l_settop)(lua_State *L, int idx);
static SV void (*l_pushvalue)(lua_State *L, int idx);
static SV void (*l_pushstring)(lua_State *L, const char *s);
static SV void (*l_getfield)(lua_State *L, int idx, const char *k);
static SV void (*l_setfield)(lua_State *L, int idx, const char *k);
static SV void (*l_pushcclosure)(lua_State *L, lua_CFunction f, int n);
static SV int (*l_pcall)(lua_State *L, int nargs, int nresults, int errfunc);
static SV const char *(*l_tolstring)(lua_State *L, int idx, size_t *len);
static SV int (*l_loadbuffer)(lua_State *L, const char *buf, size_t n, const char *name);
static SV int (*l_type)(lua_State *L, int idx);
static SV int (*l_status)(lua_State *L);

static const struct { uint32_t va; uint8_t b[8]; void **fn; } API[] = {
    {0x355fa60, {0x48, 0x8b, 0x47, 0x10, 0x48, 0x2b, 0x47, 0x18}, (void **)&l_gettop},
    {0x355fa70, {0x85, 0xf6, 0x78, 0x5c, 0x48, 0x8b, 0x4f, 0x10}, (void **)&l_settop},
    {0x355fe20, {0x85, 0xf6, 0x7e, 0x21, 0xff, 0xce, 0x48, 0x63}, (void **)&l_pushvalue},
    {0x3561310, {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x50}, (void **)&l_pushstring},
    {0x3561680, {0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec}, (void **)&l_getfield},
    {0x3561cb0, {0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec}, (void **)&l_setfield},
    {0x3561490, {0x55, 0x41, 0x56, 0x53, 0x89, 0xd5, 0x49, 0x89}, (void **)&l_pushcclosure},
    {0x35622e0, {0x55, 0x53, 0x48, 0x83, 0xec, 0x18, 0x89, 0xd5}, (void **)&l_pcall},
    {0x3560b20, {0x41, 0x57, 0x41, 0x56, 0x53, 0x49, 0x89, 0xd6}, (void **)&l_tolstring},
    {0x3563fe0, {0x48, 0x83, 0xec, 0x18, 0x48, 0x89, 0x74, 0x24}, (void **)&l_loadbuffer},
    {0x355fef0, {0x85, 0xf6, 0x7e, 0x21, 0xff, 0xce, 0x48, 0x63}, (void **)&l_type},
    {0x3562560, {0x0f, 0xb6, 0x47, 0x0a, 0xc3, 0x66, 0x66, 0x2e}, (void **)&l_status},
};
#define SE_CREATE 0x2e03d20u          // ScriptManager::CreateThread: push rbp ; push r15 ; push r14 (5 bytes)
#define SE_RELEASE 0x2e04200u         // ScriptManager::ReleaseThread: push rbp ; push r14 ; push rbx ; mov rcx, [rip+m_RegisteredStates]
static const uint8_t SE_CREATE_B[10] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xf6};
static const uint8_t SE_RELEASE_B[11] = {0x55, 0x41, 0x56, 0x53, 0x48, 0x8b, 0x0d, 0x7d, 0x44, 0x38, 0x02};

static wchar_t g_dir[MAX_PATH];   // <attila64>\lua_ext
static FILE *g_log;

#define MAXENV 256
static struct { void *env; lua_State *T; unsigned id, frames, loaded; } g_env[MAXENV];
static unsigned g_nenv, g_next_id = 1, g_sel;   // g_sel: the console's environment id, 0 = the newest
static DWORD g_lua_thread;        // the thread the environments are made on: the only one that runs Lua here
static CRITICAL_SECTION g_envlock;

