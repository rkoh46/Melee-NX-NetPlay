#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Start after stdout/stderr have been redirected. Before init, writes are synchronous.
void melee_nx_log_init(void);
double melee_nx_log_now_ms(void);
void melee_nx_log_write(const char* bytes, size_t size);
void melee_nx_log_write_critical(const char* bytes, size_t size);
// Drains accepted records and commits them. Used for errors, panic and fast exit.
void melee_nx_log_flush(void);
// Joins the writer; used by host regression tests and orderly teardown.
void melee_nx_log_shutdown(void);
// Asynchronous file output on the same writer thread: bytes are queued in order per FILE and
// written (and flushed about once a second) off the calling thread. melee_nx_log_flush drains them;
// close a file with melee_nx_file_close, which drains first. Before init, writes are synchronous.
void melee_nx_file_write(void* file, const void* bytes, size_t size);
void melee_nx_file_close(void* file);
// Files the writer thread owns entirely: open (creating parent directories), writes, an in-place
// patch (seek, write, back to the end) and close are queued in order and none of them blocks the
// caller. The handle is valid from open until close; a failed open turns the rest into no-ops.
void* melee_nx_afile_open(const char* path, const char* mode);
void melee_nx_afile_write(void* file, const void* bytes, size_t size);
void melee_nx_afile_patch(void* file, long offset, const void* bytes, size_t size);
void melee_nx_afile_close(void* file);
#ifdef __cplusplus
}
#endif
