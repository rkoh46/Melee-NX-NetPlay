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

## Status (2026-11-10)

On: state.c, tev.c, tobj.c, mobj.c, dobj.c and pobj.c run natively (134 hooked functions). The CPU
matches play identically to the translation (same fingerprints on Battlefield 2p and 4p and Mute
City; Fountain of Dreams identical frame for frame after loading one frame later, which is disc
timing). Battlefield 10.1 -> 9.6 ms, Mute City 18.2 -> 16.2, Fountain of Dreams 12.9 -> 11.4,
4-player Battlefield 17.0 -> 16.4. The divergence came from header-inline memcpy/memset/memmove
being compiled and hooked over the game's own; C library routines now stay the game's. Found and
fixed on the way:

* globals must be externalised before optimisation (a `static` the file never reads was deleted;
  const tables could be folded although Slippi patches data at run time);
* statics resolve per file through splits.txt (eight files have a `static default_class`);
* function statics are found by content in main.dol, or kept as host constants when MWCC folded them;
* Melee reads uninitialised stack in places (HSD_TExpSetReg's `GXColor reg[8]`, GXInitTlutObj's
  unused register bits): native frames leave different leftovers, which only changes channels and
  bits the GPU ignores; `gml_verify` masks those.

* Slippi's Gecko codes patch instructions inside some of these functions (TObj's texture
  animation id widened from a byte to a word at +0x70 in four functions, a call removed in
  GObj_SetupGXLinkMaxSorted, a hook in CObjLoad). Native C would not see those patches, so
  `tools/gml/gecko_patched.py` lists every function a code touches and the GML build drops their
  bodies before optimisation (calls go to the patched translation); recomp.py refuses a build that
  hooks one anyway. The first native builds ran the four TObj functions unpatched.
* by-value struct arguments reuse one scratch area per call (they had accumulated past the frame);
* finding a divergence: frame_trace=1 logs GX counters every frame (diff two builds' logs), then
  gml_verify_frames=A:B compares every native call in that window (audio buffers excluded: audio
  callbacks run inside the translation's loops, not inside native code).

36 files (all of HSD's scene graph, joints, animation, lights, cameras, GObj; 610 hooked functions)
since 2026-11-10: per-frame GX traces equal the pure translation for the whole match (1490 frames
in a row on Battlefield; the only difference is one menu frame drawn while the disc loads). Match
averages against the pure translation: Battlefield 10.1 -> 8.3 ms, Mute City 17.8 -> 13.3 (late
frames 1254 -> 17), Fountain of Dreams 12.3 -> 10.2, 4-player Battlefield 17.0 -> 12.6 (981 -> 8).
The match fingerprint counts from the frame the match scene starts on the retrace clock, so a build
that is faster or slower can shift it by a frame; compare per-frame traces aligned by content.

Next: the fighter, item and stage code (ft, it, gr, lb).
