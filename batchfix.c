// Lets battle draw items with a non-zero per-item variant value batch like everything else.
// The four pass loops (FUN_117b0400 and three siblings at +0x17B0F80, +0x17B1250, +0x17B1520) keep the
// previous item's variant value in state+0x14 (item +0xEC in the first loop, +0x3C / +0x40 / +0x3C in
// the others). Their "same material" test also demands state+0x14 == 0, and state+0x14 is cleared
// before every item, so each item with a non-zero value (most fighting soldiers) gets a flush plus two
// full material setups and is drawn alone: melee went from ~60 items per batch to ~2, about 20,000
// batches a frame (captures/melee5).
// Patch, per loop:
//   cmp [esi+14h],0 / jnz diff   ->  state+0x14 == 0 or item+field == state+0x14 ? same : diff
//                                    (the same material was checked just before)
//   mov [esi+14h],0              ->  only when the material state is set up again (AL != 0)
// Equal non-zero values now stay in one batch; any other case takes the original path with the original
// values. The batch still ends on texture, mesh, bone, size or flag changes, as for zero-valued items.
#include <windows.h>
#include <string.h>

struct site {
    unsigned cmp_rva;    // cmp dword [esi+14h],0 ; jnz +0Dh
    unsigned reset_rva;  // mov dword [esi+14h],0
    unsigned load_rva;   // the load of the item pointer into the register used below
    unsigned char load[3];
    unsigned char item_modrm;  // mov edx,[reg+disp32]: 0x90 = eax, 0x95 = ebp
    unsigned field;            // the item field the loop keeps in state+0x14 (its own later load checked)
    unsigned field_rva;
    unsigned char field_load[6];
    unsigned field_load_len;
};
static const struct site SITES[4] = {
    // FUN_117b0400: item = mov eax,[eax+ebx*4]; field +0xEC (mov edi,[edx+0ECh])
    {0x17B0466, 0x17B04BF, 0x17B0456, {0x8B, 0x04, 0x98}, 0x90, 0xEC, 0x17B05A3, {0x8B, 0xBA, 0xEC, 0x00, 0x00, 0x00}, 6},
    // loops at +0x17B0F80 / +0x17B1250: item = mov eax,[eax+ebp*4]; fields +0x3C / +0x40 (mov eax,[eax+3Ch/40h])
    {0x17B0FD9, 0x17B1032, 0x17B0FCB, {0x8B, 0x04, 0xA8}, 0x90, 0x3C, 0x17B1117, {0x8B, 0x40, 0x3C}, 3},
    {0x17B12A9, 0x17B1302, 0x17B129B, {0x8B, 0x04, 0xA8}, 0x90, 0x40, 0x17B13E7, {0x8B, 0x40, 0x40}, 3},
    // FUN_117b1520: item = mov ebp,[eax+ebp*4]; field +0x3C (mov eax,[ebp+3Ch])
    {0x17B1579, 0x17B15D2, 0x17B156B, {0x8B, 0x2C, 0xA8}, 0x95, 0x3C, 0x17B16AD, {0x8B, 0x45, 0x3C}, 3},
};
static const unsigned char CMP_BYTES[6] = {0x83, 0x7E, 0x14, 0x00, 0x75, 0x0D};
static const unsigned char SAME_BYTES[6] = {0x32, 0xC0, 0x88, 0x44, 0x24, 0x00};  // xor al,al; mov [esp+?],al
static const unsigned char DIFF_BYTES[4] = {0x8B, 0xCA, 0xB0, 0x01};               // mov ecx,edx; mov al,1
static const unsigned char RESET_BYTES[7] = {0xC7, 0x46, 0x14, 0x00, 0x00, 0x00, 0x00};
static const unsigned char TEST_AL[2] = {0x84, 0xC0};

static void put_rel(unsigned char *at, const unsigned char *target) {  // at = the 4 offset bytes
    DWORD rel = (DWORD)(target - (at + 4));
    memcpy(at, &rel, 4);
}

static int site_matches(const unsigned char *b, const struct site *s) {
    const unsigned char *c = b + s->cmp_rva, *r = b + s->reset_rva;
    return memcmp(b + s->load_rva, s->load, 3) == 0 && memcmp(b + s->field_rva, s->field_load, s->field_load_len) == 0 &&
           memcmp(c, CMP_BYTES, 6) == 0 &&
           memcmp(c + 6, SAME_BYTES, 5) == 0 && memcmp(c + 0x13, DIFF_BYTES, 4) == 0 &&
           memcmp(r, RESET_BYTES, 7) == 0 && memcmp(r + 7, TEST_AL, 2) == 0;
}

// Writes the two stubs for one site into code (64 bytes: the test at 0, the reset at 32).
static void build_stubs(unsigned char *code, unsigned char *c, unsigned char *r, unsigned char modrm, unsigned field) {
    unsigned char *p = code;
    // cmp dword [esi+14h],0; je same (the original test); then
    // mov edx,[item+field]; cmp edx,[esi+14h]; mov edx,ecx (EDX == ECX here); jne diff; jmp same
    *p++ = 0x83; *p++ = 0x7E; *p++ = 0x14; *p++ = 0x00;
    *p++ = 0x0F; *p++ = 0x84; put_rel(p, c + 6); p += 4;
    *p++ = 0x8B; *p++ = modrm; memcpy(p, &field, 4); p += 4;
    *p++ = 0x3B; *p++ = 0x56; *p++ = 0x14;
    *p++ = 0x8B; *p++ = 0xD1;
    *p++ = 0x0F; *p++ = 0x85; put_rel(p, c + 0x13); p += 4;
    *p++ = 0xE9; put_rel(p, c + 6); p += 4;
    unsigned char *s2 = code + 32;
    p = s2;
    // test al,al; jz +7; mov dword [esi+14h],0; jmp back (the game tests AL again right after)
    *p++ = 0x84; *p++ = 0xC0; *p++ = 0x74; *p++ = 0x07;
    memcpy(p, RESET_BYTES, 7); p += 7;
    *p++ = 0xE9; put_rel(p, r + 7);
}

static void jump_to(unsigned char *at, int len, unsigned char *target) {
    DWORD old;
    VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    put_rel(at + 1, target);
    memset(at + 5, 0x90, len - 5);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
}

// Returns the number of pass loops patched (0 if any site differs: then nothing is changed).
int install_batchfix(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *b = (unsigned char *)m;
    for (int i = 0; i < 4; i++)
        if (!site_matches(b, &SITES[i])) {
            log("batch fix: pass loop %d at +0x%X differs from this game version - not installed", i, SITES[i].cmp_rva);
            return 0;
        }
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return 0;
    for (int i = 0; i < 4; i++) {
        unsigned char *c = b + SITES[i].cmp_rva, *r = b + SITES[i].reset_rva, *st = code + 64 * i;
        build_stubs(st, c, r, SITES[i].item_modrm, SITES[i].field);
        jump_to(c, 6, st);
        jump_to(r, 7, st + 32);
    }
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    log("batch fix installed: draw items with equal variant values now share batches (4 pass loops)");
    return 4;
}
