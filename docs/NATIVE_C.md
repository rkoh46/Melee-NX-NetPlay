# Native decomp C on the console

Most of the game is translated PowerPC (port/recomp). Where the decompilation (doldecomp/melee,
pinned in ref/melee) has byte-matching C, that C can run instead, compiled for the A57. It is the
same program: a matching file compiles to Melee's exact instructions, so its semantics are the
game's. Two techniques, both generated at build time (nothing from the decomp is committed):

## 1. GCC with big-endian structs (done: native GX)

`tools/generate_gx_native.py`. Works for code with few pointer fields: GCC's
`#pragma scalar_storage_order big-endian` swaps every struct field; a shadow include tree keeps
MWCC's 32-bit `long`; the few pointer fields become 32-bit integers. 86 GX functions, verified
byte-exact (`gxn_verify=1`) over 5.5 million calls.

## 2. Guest-memory lowering of GameCube IR (in progress: tools/gml)

For code that chases pointers (the HSD scene graph, fighters, items):

1. `clang -target powerpc-unknown-eabi -O2 -ffp-contract=on` compiles the decomp file to LLVM IR
   with the GameCube's exact data layout (32-bit pointers, big-endian, MWCC struct layouts).
   `-ffp-contract=on` contracts `a*b+c` within an expression, which is MWCC's `-fp_contract on`.
2. The GuestMemLower pass (an opt plugin) rewrites the module for the host:
   * every pointer becomes an i32 guest address (GEPs become integer arithmetic in the PowerPC
     layout's offsets);
   * loads and stores become guest-RAM accesses with a byte swap (`ram + (addr & 0x01FFFFFF)`),
     stores to the write-gather pipe (0xCC008000) go to the host FIFO;
   * globals and function addresses become their guest addresses (symbols.txt), so the native code
     uses the game's own data;
   * allocas move to the guest stack (r1), so addresses of locals are guest addresses too;
   * calls to functions compiled in the same module stay direct; other calls go to the translated
     game through `gml_call` with the PowerPC EABI (r3..r10, f1..f8); indirect calls dispatch on
     the guest address (native if it is one of ours).
3. The module is retargeted to aarch64 and compiled; generated HLE wrappers enter it from guest
   registers.

Exactness is checked, not assumed: a sampled verify mode runs a native entry and the original
translation from the same state and compares guest RAM and the pipe. Functions whose float code
does not match (MWCC contraction choices) stay translated. Anything whose results feed game state
(joint matrices read by hitboxes, physics) must match exactly or stays translated, because Slippi
peers run the original code.

## Status (2026-11-09)

The pipeline works end to end: state.c, tev.c, tobj.c, mobj.c, dobj.c and pobj.c lower into 182
native functions (137 hooked) and the game runs on them, Battlefield 10.2 -> 9.4 ms. It is off
(`port/recomp/gml_files.txt` is empty) because the match diverges from the translation and stock
icons draw with the wrong palette. Found and fixed on the way:

* globals must be externalised before optimisation (a `static` the file never reads was deleted;
  const tables could be folded although Slippi patches data at run time);
* statics resolve per file through splits.txt (eight files have a `static default_class`);
* function statics are found by content in main.dol, or kept as host constants when MWCC folded them;
* Melee reads uninitialised stack in places (HSD_TExpSetReg's `GXColor reg[8]`, GXInitTlutObj's
  unused register bits): native frames leave different leftovers, which only changes channels and
  bits the GPU ignores; `gml_verify` masks those.

Next: a verifier that checks every call (not one in 512) by recording only the bytes each side
writes, to find the first call that diverges; and matching the original frame sizes so translated
callees see the same stack.
