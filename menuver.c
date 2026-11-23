// Main menu: "Engine Fix <version>" in the top right, under the game's own version text, drawn by the panel
// (fgpanel.c label_redraw) on the game's frames while the game's front end (its menus) is up. Written into the game's
// own "version_number" box first, that box was too narrow and its font too soft at 4K (two in-game tries, 2026-11-22).
// "The menus are up" is the front end's own pointer, UIDLL::FRONT_END_UI::s_ui (set by its constructor +0x13CEEA0,
// cleared by its destructor +0x13CFA10), as Sol Invictus's 64-bit Attila (MIT) does for its Discord button. Its
// address is read from the main menu's OnCreate (+0x13C5D0D: mov ecx,[s_ui]) after that code is checked byte for
// byte, so it is right on any load address and on the GOG build (same code there).
#include <windows.h>
#include <string.h>
#include "version.h"

extern int (*g_fgp_menu_label)(void);   // fgpanel.c
static void *const volatile *g_s_ui;

static int menus_up(void) { return *g_s_ui != NULL; }

int install_menuver(HMODULE m, void (*log)(const char *fmt, ...)) {
    // mov ecx,[s_ui]; mov esi,eax; lea eax,[esp+28h]; mov ecx,[ecx+16Ch]; mov ecx,[ecx+48h]; add ecx,48h
    static const unsigned char before[2] = {0x8B, 0x0D}, after[22] = {0x8B, 0xF0, 0x8D, 0x44, 0x24, 0x28, 0x8B, 0x89, 0x6C, 0x01, 0x00,
                                                                       0x00, 0x8B, 0x49, 0x48, 0x83, 0xC1, 0x48, 0x51, 0x50, 0xE8};
    const unsigned char *site = (const unsigned char *)m + 0x13C5D0D;
    unsigned addr;
    memcpy(&addr, site + 2, 4);
    MEMORY_BASIC_INFORMATION mi;
    if (memcmp(site, before, 2) || memcmp(site + 6, after, 21) || !VirtualQuery((void *)(UINT_PTR)addr, &mi, sizeof mi) ||
        mi.State != MEM_COMMIT || mi.AllocationBase != (void *)m) {
        log("main menu label: the game's code differs here - left out");
        return 0;
    }
    g_s_ui = (void *const volatile *)(UINT_PTR)addr;
    g_fgp_menu_label = menus_up;
    log("main menu: \"Engine Fix " MOD_VERSION_STR "\" in the top right while the menus are up");
    return 1;
}
