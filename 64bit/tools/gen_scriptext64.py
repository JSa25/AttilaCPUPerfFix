import os, sys
root = sys.argv[1]
# the dev tree (heapfix/, x64Review/perffix64/) or the public one (32bit/, 64bit/); this file is in <tree>/tools
P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
h32 = root + '/heapfix' if os.path.isdir(root + '/heapfix') else root + '/32bit'
src = open(h32 + '/scriptext.c', encoding='utf-8').read()
# the portable middle of the x86 file: out/outf ... command(); the x86 list walk is dropped
i = src.index('static void out(const char *s) {'); j = src.index('static int g_key_down;')
mid = src[i:j]
k1 = mid.index('// The environment list as it is now'); k2 = mid.index('static lua_State *selected(void) {')
mid = mid[:k1] + mid[k2:]
for f in ('tostr_fn', 'ext_print', 'has_fn', 'setup_fn'):
    a = 'static int %s(lua_State *L)' % f
    assert mid.count(a) == 1, f
    mid = mid.replace(a, 'static SV int %s(lua_State *L)' % f)
a = 'g_has = TOP_TYPE(L) != 0;'; assert mid.count(a) == 1; mid = mid.replace(a, 'g_has = l_type(L, -1) != 0;')
a = 'if (L_STATUS(T)) {'; assert mid.count(a) == 1; mid = mid.replace(a, 'if (l_status(T)) {')
mid = mid.replace('swprintf(pat, MAX_PATH, ', '_snwprintf(pat, MAX_PATH, ').replace('swprintf(path, MAX_PATH, ', '_snwprintf(path, MAX_PATH, ')
assert 'TOP_TYPE' not in mid and 'L_STATUS' not in mid and 'swprintf(p' not in mid.replace('_snwprintf(p', '')
head = open(P + '/src/scriptext64_head.inc', encoding='utf-8').read()
tail = open(P + '/src/scriptext64_tail.inc', encoding='utf-8').read()
open(P + '/src/scriptext64.c', 'w', encoding='utf-8', newline='\n').write(head + mid + tail)
print('written', len(head + mid + tail))
