#include "log_switch.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace {
constexpr size_t Capacity = 256 * 1024;
// During a match (melee_nx_log_set_quiet) the writer touches the SD card every 3 s, or when the buffer
// is half full, instead of every 100 ms with a sync each second: SD traffic kept the console's system
// core busy and the game's socket calls waited behind it (online freezes, Nov 2026).
std::atomic<bool> quiet{false};
size_t wake_bytes() { return quiet.load(std::memory_order_relaxed) ? Capacity / 2 : 16384; }
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wake = PTHREAD_COND_INITIALIZER, flushed = PTHREAD_COND_INITIALIZER;
pthread_t worker;
char buffers[2][Capacity];
size_t used = 0;
unsigned pending = 0;
bool running = false, stopping = false;
uint64_t requested = 0, completed = 0, dropped = 0;
// A file the writer thread opens, patches and closes itself (melee_nx_afile_*): the simulation
// thread never touches the SD card for it, not even to create it.
struct AsyncFile { std::string path, mode; FILE* f = nullptr; };
enum ChunkKind { Write, Open, Patch, Close };
struct FileChunk { FILE* file; std::string bytes; AsyncFile* af = nullptr; ChunkKind kind = Write; long offset = 0; };
std::vector<FileChunk> fileQueue;        // guarded by `mutex`; drained by the writer in order
std::vector<FILE*> dirtyFiles;           // writer thread only: files with unflushed bytes
pthread_once_t clockOnce = PTHREAD_ONCE_INIT;
uint64_t clockStart = 0;

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
void init_clock() { clockStart = now_ns(); }
bool write_all(const char* data, size_t length) {
    while (length) {
        const ssize_t n = write(STDOUT_FILENO, data, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        length -= size_t(n);
    }
    return true;
}
void* writer(void*) {
#if defined(__SWITCH__)
    // I/O work should not preempt game/audio/FIFO work. No compiler-core pin.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x32);
#endif
    uint64_t lastSync = now_ns(), lastReport = lastSync;
    uint64_t writeNs = 0, syncNs = 0, bytes = 0, errors = 0;
    bool dirty = false;
    for (;;) {
        pthread_mutex_lock(&mutex);
        if (used < wake_bytes() && requested == completed && !stopping) {
            timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            if (quiet.load(std::memory_order_relaxed)) deadline.tv_sec += 3;
            else deadline.tv_nsec += 100000000;
            if (deadline.tv_nsec >= 1000000000) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&wake, &mutex, &deadline);
        }
        const size_t length = used;
        const unsigned ready = pending;
        pending ^= 1;
        used = 0;
        const uint64_t request = requested, lost = dropped;
        dropped = 0;
        const bool stop = stopping;
        std::vector<FileChunk> chunks;
        chunks.swap(fileQueue);
        pthread_mutex_unlock(&mutex);

        for (const FileChunk& chunk : chunks) {
            if (chunk.af) {
                AsyncFile* af = chunk.af;
                const auto start = now_ns();
                if (chunk.kind == Open) {
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(af->path).parent_path(), ec);
                    af->f = std::fopen(af->path.c_str(), af->mode.c_str());
                    if (!af->f) ++errors;
                } else if (af->f && chunk.kind == Write) {
                    if (std::fwrite(chunk.bytes.data(), 1, chunk.bytes.size(), af->f) != chunk.bytes.size()) ++errors;
                    bytes += chunk.bytes.size();
                } else if (af->f && chunk.kind == Patch) {
                    std::fflush(af->f);
                    if (std::fseek(af->f, chunk.offset, SEEK_SET) != 0 ||
                        std::fwrite(chunk.bytes.data(), 1, chunk.bytes.size(), af->f) != chunk.bytes.size()) ++errors;
                    std::fseek(af->f, 0, SEEK_END);
                } else if (chunk.kind == Close) {
                    for (size_t i = 0; i < dirtyFiles.size(); ++i)
                        if (dirtyFiles[i] == af->f) { dirtyFiles.erase(dirtyFiles.begin() + long(i)); break; }
                    if (af->f && std::fclose(af->f) != 0) ++errors;
                    delete af;
                    writeNs += now_ns() - start;
                    continue;
                }
                writeNs += now_ns() - start;
                if (af->f && chunk.kind != Open) {
                    bool known = false;
                    for (FILE* f : dirtyFiles) known = known || f == af->f;
                    if (!known) dirtyFiles.push_back(af->f);
                    dirty = true;
                }
                continue;
            }
            if (!chunk.file) continue;
            const auto start = now_ns();
            if (std::fwrite(chunk.bytes.data(), 1, chunk.bytes.size(), chunk.file) != chunk.bytes.size()) ++errors;
            writeNs += now_ns() - start;
            bytes += chunk.bytes.size();
            bool known = false;
            for (FILE* f : dirtyFiles) known = known || f == chunk.file;
            if (!known) dirtyFiles.push_back(chunk.file);
            dirty = true;
        }

        if (length) {
            const auto start = now_ns();
            if (!write_all(buffers[ready], length))
                ++errors;
            writeNs += now_ns() - start;
            bytes += length;
            dirty = true;
        }
        auto now = now_ns();
        if (lost || now - lastReport >= 2000000000 || stop) {
            char line[256];
            const int n = std::snprintf(
                line, sizeof(line),
                "[%9.3f] LOGIO bytes %llu dropped_records %llu errors %llu write_ms %.3f flush_ms %.3f\n",
                melee_nx_log_now_ms(), (unsigned long long)bytes, (unsigned long long)lost,
                (unsigned long long)errors, writeNs / 1e6, syncNs / 1e6);
            if (n > 0)
                write_all(line, size_t(n));
            bytes = writeNs = syncNs = errors = 0;
            dirty = true;
            lastReport = now;
        }
        const uint64_t syncEvery = quiet.load(std::memory_order_relaxed) ? 30000000000ull : 1000000000ull;
        if (request != completed || stop || (dirty && now - lastSync >= syncEvery)) {
            const auto start = now_ns();
            if (fsync(STDOUT_FILENO) != 0)
                ++errors;
            for (FILE* f : dirtyFiles) if (std::fflush(f) != 0) ++errors;
            dirtyFiles.clear();
            syncNs += now_ns() - start;
            lastSync = now_ns();
            dirty = false;
        }
        pthread_mutex_lock(&mutex);
        completed = request;
        pthread_cond_broadcast(&flushed);
        pthread_mutex_unlock(&mutex);
        if (stop)
            break;
    }
    return nullptr;
}
} // namespace

extern "C" double melee_nx_log_now_ms(void) {
    pthread_once(&clockOnce, init_clock);
    return (now_ns() - clockStart) / 1e6;
}
extern "C" void melee_nx_log_set_quiet(int on) {
    const bool was = quiet.exchange(on != 0, std::memory_order_relaxed);
    if (was && !on) {   // match over: write what was held now
        pthread_mutex_lock(&mutex);
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&mutex);
    }
}
extern "C" void melee_nx_log_init(void) {
    pthread_mutex_lock(&mutex);
    if (!running) {
        stopping = false;
        running = pthread_create(&worker, nullptr, writer, nullptr) == 0;
        if (!running)
            write_all("[LOGIO] writer unavailable; synchronous fallback\n",
                      sizeof("[LOGIO] writer unavailable; synchronous fallback\n") - 1);
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" void melee_nx_log_write(const char* data, size_t size) {
    if (!data || !size)
        return;
    pthread_mutex_lock(&mutex);
    if (!running) {
        write_all(data, size);
    } else if (stopping || size > Capacity - used) {
        // Drop whole records, never block producers on a slow SD card. The
        // writer reports losses explicitly; no unbounded allocation or queue.
        ++dropped;
    } else {
        std::memcpy(buffers[pending] + used, data, size);
        used += size;
        if (used >= wake_bytes())
            pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" void melee_nx_log_flush(void) {
    pthread_mutex_lock(&mutex);
    if (running && !stopping) {
        const uint64_t target = ++requested;
        pthread_cond_signal(&wake);
        while (completed < target)
            pthread_cond_wait(&flushed, &mutex);
    } else if (!running) {
        fsync(STDOUT_FILENO);
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" void melee_nx_log_write_critical(const char* data, size_t size) {
    if (!data || !size)
        return;
    // Critical records may block for space. Routine telemetry never does.
    // Split only if a caller supplies more than the entire buffer capacity.
    while (size) {
        const size_t chunk = size < Capacity ? size : Capacity;
        pthread_mutex_lock(&mutex);
        while (running && !stopping && chunk > Capacity - used) {
            const auto target = ++requested;
            pthread_cond_signal(&wake);
            while (completed < target)
                pthread_cond_wait(&flushed, &mutex);
        }
        if (!running)
            write_all(data, chunk);
        else if (!stopping) {
            std::memcpy(buffers[pending] + used, data, chunk);
            used += chunk;
        }
        pthread_mutex_unlock(&mutex);
        data += chunk;
        size -= chunk;
    }
    melee_nx_log_flush();
}
extern "C" void melee_nx_log_shutdown(void) {
    melee_nx_log_flush();
    pthread_mutex_lock(&mutex);
    const bool join = running;
    stopping = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&mutex);
    if (join)
        pthread_join(worker, nullptr);
    pthread_mutex_lock(&mutex);
    running = false;
    pthread_mutex_unlock(&mutex);
}

extern "C" void melee_nx_file_write(void* file, const void* bytes, size_t size) {
    if (!file || !size) return;
    if (!running) {   // before the writer exists (or after shutdown): synchronous, as log writes are
        std::fwrite(bytes, 1, size, static_cast<FILE*>(file));
        return;
    }
    pthread_mutex_lock(&mutex);
    // Coalesce with the previous chunk for the same file: replay records arrive several times a
    // frame, and one string append is far cheaper than a queue node each. No wake-up either: the
    // writer runs every 100 ms (or with the next log line), which is soon enough for a file, and
    // a signal per record was a syscall per record on the simulation thread.
    if (!fileQueue.empty() && !fileQueue.back().af && fileQueue.back().file == file && fileQueue.back().bytes.size() < (1u << 16))
        fileQueue.back().bytes.append(static_cast<const char*>(bytes), size);
    else
        fileQueue.push_back({static_cast<FILE*>(file), std::string(static_cast<const char*>(bytes), size)});
    pthread_mutex_unlock(&mutex);
}

extern "C" void melee_nx_file_close(void* file) {
    if (!file) return;
    if (running) melee_nx_log_flush();   // every queued chunk for this file is written and flushed
    std::fclose(static_cast<FILE*>(file));
}

// ---- writer-owned files ----------------------------------------------------------------------
namespace {
void afile_sync(AsyncFile* af, ChunkKind kind, const void* bytes, size_t size, long offset) {
    // No writer thread (tests, or after shutdown): do the operation here, in order.
    if (kind == Open) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(af->path).parent_path(), ec);
        af->f = std::fopen(af->path.c_str(), af->mode.c_str());
    } else if (kind == Write) {
        if (af->f) std::fwrite(bytes, 1, size, af->f);
    } else if (kind == Patch) {
        if (af->f) { std::fflush(af->f); std::fseek(af->f, offset, SEEK_SET); std::fwrite(bytes, 1, size, af->f); std::fseek(af->f, 0, SEEK_END); }
    } else {
        if (af->f) std::fclose(af->f);
        delete af;
    }
}
void afile_queue(AsyncFile* af, ChunkKind kind, const void* bytes, size_t size, long offset) {
    pthread_mutex_lock(&mutex);
    if (!running || stopping) {
        pthread_mutex_unlock(&mutex);
        afile_sync(af, kind, bytes, size, offset);
        return;
    }
    if (kind == Write && !fileQueue.empty() && fileQueue.back().af == af && fileQueue.back().kind == Write &&
        fileQueue.back().bytes.size() < (1u << 16)) {
        fileQueue.back().bytes.append(static_cast<const char*>(bytes), size);
    } else {
        FileChunk c{nullptr, bytes ? std::string(static_cast<const char*>(bytes), size) : std::string(), af, kind, offset};
        fileQueue.push_back(std::move(c));
        if (kind == Close) pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&mutex);
}
}  // namespace

extern "C" void* melee_nx_afile_open(const char* path, const char* mode) {
    AsyncFile* af = new AsyncFile{path, mode, nullptr};
    afile_queue(af, Open, nullptr, 0, 0);
    return af;
}
extern "C" void melee_nx_afile_write(void* file, const void* bytes, size_t size) {
    if (file && size) afile_queue(static_cast<AsyncFile*>(file), Write, bytes, size, 0);
}
extern "C" void melee_nx_afile_patch(void* file, long offset, const void* bytes, size_t size) {
    if (file && size) afile_queue(static_cast<AsyncFile*>(file), Patch, bytes, size, offset);
}
extern "C" void melee_nx_afile_close(void* file) {
    if (file) afile_queue(static_cast<AsyncFile*>(file), Close, nullptr, 0, 0);
}
