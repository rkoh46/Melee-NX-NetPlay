/* Host side of the native GX (tools/generate_gx_native.py): the write-gather pipe and the two
   __GXData callbacks, which run guest code. Included by every generated GX file, in C. */
#ifndef GXN_GLUE_H
#define GXN_GLUE_H
#ifdef __cplusplus
extern "C" {
#endif
void gxn_write_u8(unsigned char v);
void gxn_write_u16(unsigned short v);
void gxn_write_u32(unsigned int v);
void gxn_write_f32(float v);
/* GXTexRegion* gx->texRegionCallback(GXTexObj*, GXTexMapID) and GXTlutRegion* gx->tlutRegionCallback(u32):
   guest functions; the glue calls them and returns host pointers to the region they return. */
void* gxn_tex_region_callback(void* obj, int id);
void* gxn_tlut_region_callback(unsigned long idx);
/* The guest-matrix-to-pipe copies (GXTransform's asm); the argument is the guest address. */
void gxn_WriteMTXPS4x3(const void* guest_mtx);
void gxn_WriteMTXPS3x3from3x4(const void* guest_mtx);
void gxn_WriteMTXPS3x3(const void* guest_mtx);
void gxn_WriteMTXPS4x2(const void* guest_mtx);
#define __cntlzw(x) ((x) ? __builtin_clz(x) : 32)
#ifdef __cplusplus
}
#endif
#endif
