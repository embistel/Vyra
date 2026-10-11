#include "core/fileio.h"

#include <cerrno>
#include <cstring>

#include <algorithm>

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

OutFile::~OutFile()
{
    if (m_file) std::fclose(m_file);
}

void OutFile::create(const fs::path &path)
{
    if (m_file) close();
    m_path = path;
    m_position = 0;
    m_file = openFile(path, true);
    if (!m_file) fail("cannot create file", path);
    std::setvbuf(m_file, nullptr, _IOFBF, kStdioBuffer);
}

void OutFile::write(const void *data, std::size_t size)
{
    if (size && std::fwrite(data, 1, size, m_file) != size) fail("write failed", m_path);
    m_position += size;
}

void OutFile::writeAt(std::uint64_t offset, const void *data, std::size_t size)
{
    if (seek64(m_file, offset) != 0 || std::fwrite(data, 1, size, m_file) != size) fail("write failed", m_path);
    if (seek64(m_file, m_position) != 0) fail("seek failed", m_path);
}

void OutFile::close()
{
    if (!m_file) return;
    const bool ok = std::fflush(m_file) == 0;
    const bool closed = std::fclose(m_file) == 0;
    m_file = nullptr;
    if (!ok || !closed) fail("write failed", m_path);
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
