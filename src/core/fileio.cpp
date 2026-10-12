#include "core/fileio.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace vyra {
namespace {
constexpr std::size_t kStdioBuffer = 1 << 20;

std::FILE *openFile(const fs::path &path, bool write)
{
#ifdef _WIN32
    return _wfopen(path.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

int seek64(std::FILE *file, std::uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET);
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

[[noreturn]] void fail(const char *what, const fs::path &path)
{
    throw Error(std::string(what) + ": " + utf8FromPath(path) + " (" + systemErrorText() + ")");
}
} // namespace

std::string systemErrorText()
{
#ifdef _WIN32
    const DWORD code = GetLastError();
    if (code) {
        wchar_t *buffer = nullptr;
        const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        std::string text;
        if (len && buffer) {
            std::wstring wide(buffer, len);
            while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r' || wide.back() == L' ')) wide.pop_back();
            text = fs::path(wide).u8string();
        }
        if (buffer) LocalFree(buffer);
        if (!text.empty()) return text;
    }
#endif
    return std::strerror(errno);
}

InFile::~InFile() { close(); }

void InFile::open(const fs::path &path)
{
    close();
    m_path = path;
    m_file = openFile(path, false);
    if (!m_file) fail("cannot open file", path);
    std::setvbuf(m_file, nullptr, _IOFBF, kStdioBuffer);
#ifdef _WIN32
    const __int64 length = _filelengthi64(_fileno(m_file));
    m_size = length < 0 ? 0 : std::uint64_t(length);
#else
    struct stat s {};
    m_size = ::fstat(fileno(m_file), &s) == 0 ? std::uint64_t(s.st_size) : 0;
#endif
}

void InFile::close()
{
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
}

void InFile::seek(std::uint64_t offset)
{
    if (seek64(m_file, offset) != 0) fail("seek failed", m_path);
}

std::uint64_t InFile::tell() const
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(_ftelli64(m_file));
#else
    return static_cast<std::uint64_t>(ftello(m_file));
#endif
}

void InFile::readExact(void *data, std::size_t size)
{
    if (readSome(data, size) != size) throw Error("unexpected end of file: " + utf8FromPath(m_path));
}

std::size_t InFile::readSome(void *data, std::size_t size)
{
    auto *p = static_cast<std::uint8_t *>(data);
    std::size_t total = 0;
    while (total < size) {
        const std::size_t n = std::fread(p + total, 1, size - total, m_file);
        if (n == 0) {
            if (std::ferror(m_file)) fail("read error", m_path);
            break;
        }
        total += n;
    }
    return total;
}

#ifdef _WIN32
// Unbuffered (FILE_FLAG_NO_BUFFERING) sequential writer: data is gathered in
// sector-aligned blocks that worker threads write at their offsets, each
// through its own handle, so several requests are in flight at once.
struct OutFile::Direct {
    static constexpr std::size_t kBlock = std::size_t(16) << 20;
    static constexpr int kBlocks = 6, kWorkers = 4;

    struct Job { int block; std::uint64_t offset; std::size_t size; };
    struct Patch { std::uint64_t offset; std::vector<std::uint8_t> bytes; };

    fs::path path;
    HANDLE main = INVALID_HANDLE_VALUE;
    std::size_t alignment = 4096;
    std::uint8_t *memory = nullptr;
    int current = -1;           // block being filled
    std::size_t fill = 0;       // bytes in the current block
    std::uint64_t blockOffset = 0;
    std::vector<int> freeBlocks;
    std::deque<Job> jobs;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::thread> workers;
    bool stopping = false;
    DWORD error = 0;
    std::vector<Patch> patches;

    ~Direct()
    {
        stopWorkers();
        if (main != INVALID_HANDLE_VALUE) CloseHandle(main);
    }

    static std::unique_ptr<Direct> open(const fs::path &path)
    {
        auto d = std::make_unique<Direct>();
        d->path = path;
        d->main = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                              FILE_FLAG_NO_BUFFERING, nullptr);
        if (d->main == INVALID_HANDLE_VALUE) return nullptr;
        FILE_STORAGE_INFO storage{};
        if (GetFileInformationByHandleEx(d->main, FileStorageInfo, &storage, sizeof(storage)))
            d->alignment = std::max<std::size_t>(d->alignment, storage.PhysicalBytesPerSectorForPerformance);
        if (d->alignment > (std::size_t(1) << 20) || kBlock % d->alignment) return nullptr;
        d->memory = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, kBlock * kBlocks, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!d->memory) return nullptr;
        for (int i = 0; i < kBlocks; ++i) d->freeBlocks.push_back(i);
        for (int i = 0; i < kWorkers; ++i) d->workers.emplace_back([p = d.get()] { p->work(); });
        return d;
    }

    void work()
    {
        HANDLE own = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_NO_BUFFERING, nullptr);
        std::unique_lock<std::mutex> lock(mutex);
        if (own == INVALID_HANDLE_VALUE && !error) error = GetLastError();
        for (;;) {
            changed.wait(lock, [&] { return stopping || !jobs.empty(); });
            if (jobs.empty()) break;
            const Job job = jobs.front();
            jobs.pop_front();
            lock.unlock();
            bool ok = own != INVALID_HANDLE_VALUE;
            for (std::size_t done = 0; ok && done < job.size;) {
                OVERLAPPED o{};
                const std::uint64_t at = job.offset + done;
                o.Offset = DWORD(at);
                o.OffsetHigh = DWORD(at >> 32);
                DWORD written = 0;
                ok = WriteFile(own, memory + std::size_t(job.block) * kBlock + done, DWORD(job.size - done), &written, &o) && written;
                done += written;
            }
            const DWORD code = ok ? 0 : GetLastError();
            lock.lock();
            if (!ok && !error) error = code ? code : ERROR_WRITE_FAULT;
            freeBlocks.push_back(job.block);
            changed.notify_all();
        }
        if (own != INVALID_HANDLE_VALUE) CloseHandle(own);
    }

    void check(const char *what)
    {
        if (error) {
            SetLastError(error);
            fail(what, path);
        }
    }

    // Hands the current block (padded to the sector size) to the workers.
    void submit()
    {
        if (current < 0 || fill == 0) return;
        const std::size_t padded = (fill + alignment - 1) / alignment * alignment;
        std::memset(memory + std::size_t(current) * kBlock + fill, 0, padded - fill);
        std::lock_guard<std::mutex> lock(mutex);
        jobs.push_back({current, blockOffset, padded});
        changed.notify_all();
        blockOffset += fill;
        current = -1;
        fill = 0;
    }

    void write(const std::uint8_t *data, std::size_t size)
    {
        while (size) {
            if (current < 0) {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return !freeBlocks.empty() || error; });
                check("write failed");
                current = freeBlocks.back();
                freeBlocks.pop_back();
            }
            const std::size_t n = std::min(size, kBlock - fill);
            std::memcpy(memory + std::size_t(current) * kBlock + fill, data, n);
            fill += n;
            data += n;
            size -= n;
            if (fill == kBlock) submit();
        }
    }

    // Writes out everything, trims the sector padding, applies the patches.
    void finish(std::uint64_t length)
    {
        // A final partial block is written whole (padded) only if it starts
        // sector aligned, which every block does: blocks are kBlock apart.
        submit();
        stopWorkers();
        check("write failed");
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = LONGLONG(length);
        if (!SetFileInformationByHandle(main, FileEndOfFileInfo, &eof, sizeof(eof))) fail("write failed", path);
        CloseHandle(main);
        main = INVALID_HANDLE_VALUE;
        if (patches.empty()) return;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) fail("cannot reopen file", path);
        for (const Patch &p : patches) {
            OVERLAPPED o{};
            o.Offset = DWORD(p.offset);
            o.OffsetHigh = DWORD(p.offset >> 32);
            DWORD written = 0;
            if (!WriteFile(h, p.bytes.data(), DWORD(p.bytes.size()), &written, &o) || written != p.bytes.size()) {
                const DWORD code = GetLastError();
                CloseHandle(h);
                SetLastError(code);
                fail("write failed", path);
            }
        }
        CloseHandle(h);
    }

    // Lets the workers finish the queued blocks, then releases them.
    void stopWorkers()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            changed.notify_all();
        }
        for (auto &t : workers) t.join();
        workers.clear();
        if (memory) VirtualFree(memory, 0, MEM_RELEASE);
        memory = nullptr;
    }
};
#else
struct OutFile::Direct {
    static std::unique_ptr<Direct> open(const fs::path &) { return nullptr; }
    void write(const std::uint8_t *, std::size_t) {}
    void finish(std::uint64_t) {}
    struct Patch { std::uint64_t offset; std::vector<std::uint8_t> bytes; };
    std::vector<Patch> patches;
};
#endif

OutFile::OutFile() = default;

OutFile::OutFile(const fs::path &path) { create(path); }

OutFile::~OutFile()
{
    if (m_file) std::fclose(m_file);
}

void OutFile::create(const fs::path &path, bool direct)
{
    if (isOpen()) close();
    m_path = path;
    m_position = 0;
    if (direct) {
        m_direct = Direct::open(path);
        if (m_direct) return;
    }
    m_file = openFile(path, true);
    if (!m_file) fail("cannot create file", path);
    std::setvbuf(m_file, nullptr, _IOFBF, kStdioBuffer);
}

void OutFile::write(const void *data, std::size_t size)
{
    if (m_direct) {
        m_direct->write(static_cast<const std::uint8_t *>(data), size);
    } else if (size && std::fwrite(data, 1, size, m_file) != size) {
        fail("write failed", m_path);
    }
    m_position += size;
}

void OutFile::writeAt(std::uint64_t offset, const void *data, std::size_t size)
{
    if (m_direct) {
        const auto *p = static_cast<const std::uint8_t *>(data);
        m_direct->patches.push_back({offset, std::vector<std::uint8_t>(p, p + size)});
        return;
    }
    if (seek64(m_file, offset) != 0 || std::fwrite(data, 1, size, m_file) != size) fail("write failed", m_path);
    if (seek64(m_file, m_position) != 0) fail("seek failed", m_path);
}

void OutFile::close()
{
    if (m_direct) {
        auto direct = std::move(m_direct);
        direct->finish(m_position);
        return;
    }
    if (!m_file) return;
    const bool ok = std::fflush(m_file) == 0;
    const bool closed = std::fclose(m_file) == 0;
    m_file = nullptr;
    if (!ok || !closed) fail("write failed", m_path);
}

FileView mapFileRange(const fs::path &path, std::uint64_t offset, std::size_t size)
{
    FileView view;
#ifdef _WIN32
    if (size == 0) return view;
    const std::wstring root = path.root_path().wstring();
    if (root.empty() || GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return view;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return view;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || std::uint64_t(length.QuadPart) < offset + size) {
        CloseHandle(file);
        return view;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        return view;
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const std::uint64_t aligned = offset / info.dwAllocationGranularity * info.dwAllocationGranularity;
    void *base = MapViewOfFile(mapping, FILE_MAP_READ, DWORD(aligned >> 32), DWORD(aligned), SIZE_T(offset - aligned + size));
    CloseHandle(mapping); // the view keeps the section alive
    if (!base) {
        CloseHandle(file);
        return view;
    }
    view.base = base;
    view.file = file;
    view.data = static_cast<const std::uint8_t *>(base) + (offset - aligned);
#else
    (void)path; (void)offset; (void)size;
#endif
    return view;
}

void unmapFileView(FileView &view)
{
#ifdef _WIN32
    if (view.base) UnmapViewOfFile(view.base);
    if (view.file) CloseHandle(static_cast<HANDLE>(view.file));
#endif
    view = FileView{};
}

void *allocatePages(std::size_t bytes)
{
#ifdef _WIN32
    void *p = VirtualAlloc(nullptr, bytes ? bytes : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) throw Error("out of memory (" + std::to_string(bytes >> 20) + " MiB)");
    return p;
#else
    return ::operator new(bytes ? bytes : 1);
#endif
}

void freePages(void *pointer)
{
    if (!pointer) return;
#ifdef _WIN32
    VirtualFree(pointer, 0, MEM_RELEASE);
#else
    ::operator delete(pointer);
#endif
}

double processUptime()
{
#ifdef _WIN32
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    auto ticks = [](const FILETIME &f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(ticks(now) - ticks(created)) / 1e7;
#else
    return 0;
#endif
}

void profileMark(const char *what)
{
    static const bool enabled = std::getenv("VYRA_PROFILE") != nullptr;
    if (enabled) std::fprintf(stderr, "[time] %-28s %7.3f s\n", what, processUptime());
}

std::size_t readFileRange(const fs::path &path, std::uint64_t offset, void *data, std::size_t size)
{
    auto *p = static_cast<std::uint8_t *>(data);
    std::size_t total = 0;
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) fail("cannot open file", path);
    while (total < size) {
        OVERLAPPED ov{};
        const std::uint64_t at = offset + total;
        ov.Offset = DWORD(at);
        ov.OffsetHigh = DWORD(at >> 32);
        DWORD got = 0;
        const DWORD want = DWORD(std::min<std::size_t>(size - total, 1u << 30));
        if (!ReadFile(h, p + total, want, &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            CloseHandle(h);
            fail("read error", path);
        }
        if (got == 0) break;
        total += got;
    }
    CloseHandle(h);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) fail("cannot open file", path);
    while (total < size) {
        const ssize_t got = ::pread(fd, p + total, size - total, off_t(offset + total));
        if (got < 0) { ::close(fd); fail("read error", path); }
        if (got == 0) break;
        total += std::size_t(got);
    }
    ::close(fd);
#endif
    return total;
}

FileStat statPath(const fs::path &path)
{
    FileStat st;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return st;
    st.exists = true;
    st.attributes = data.dwFileAttributes;
    st.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st.size = st.directory ? 0 : (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    st.mtime = (std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
#else
    struct stat s {};
    if (::stat(path.c_str(), &s) != 0) return st;
    st.exists = true;
    st.directory = S_ISDIR(s.st_mode);
    st.size = st.directory ? 0 : std::uint64_t(s.st_size);
    st.mtime = kFileTimeUnixEpoch + std::uint64_t(s.st_mtime) * 10000000ULL;
    st.attributes = (st.directory ? 0x10u : 0x20u) | 0x8000u | (std::uint32_t(s.st_mode & 0xFFFF) << 16);
#endif
    return st;
}

void applyFileMetadata(const fs::path &path, std::uint64_t mtime, std::uint32_t attributes, bool hasAttributes)
{
#ifdef _WIN32
    const bool directory = hasAttributes && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    if (mtime) {
        HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME ft;
            ft.dwLowDateTime = DWORD(mtime);
            ft.dwHighDateTime = DWORD(mtime >> 32);
            SetFileTime(h, nullptr, nullptr, &ft);
            CloseHandle(h);
        }
    }
    if (hasAttributes) {
        const DWORD keep = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
        const DWORD value = attributes & keep;
        if (value && value != FILE_ATTRIBUTE_ARCHIVE) SetFileAttributesW(path.c_str(), value);
    }
#else
    if (mtime > kFileTimeUnixEpoch) {
        struct timeval tv[2];
        tv[0].tv_sec = tv[1].tv_sec = time_t((mtime - kFileTimeUnixEpoch) / 10000000ULL);
        tv[0].tv_usec = tv[1].tv_usec = suseconds_t(((mtime - kFileTimeUnixEpoch) % 10000000ULL) / 10);
        ::utimes(path.c_str(), tv);
    }
    if (hasAttributes && (attributes & 0x8000u)) ::chmod(path.c_str(), mode_t((attributes >> 16) & 07777));
#endif
}

} // namespace vyra
