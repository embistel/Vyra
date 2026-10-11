#include "core/zip.h"
#include "core/bytes.h"
#include "core/crc32.h"

#include <algorithm>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vyra {
namespace {

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEndSig = 0x06054b50;
constexpr std::uint32_t kEnd64Sig = 0x06064b50;
constexpr std::uint32_t kLocator64Sig = 0x07064b50;
constexpr std::uint32_t k32Max = 0xFFFFFFFFu;
// Reserve a ZIP64 local header when the stored size might not fit in 32 bits.
constexpr std::uint64_t kZip64LocalThreshold = 0xF0000000ull;

std::uint16_t methodCode(Method m) { return m == Method::Deflate ? 8 : 0; }

std::string nativeToUtf8(const std::string &raw)
{
#ifdef _WIN32
    // Legacy ZIP tools (Explorer, ALZip, ...) store names in the OEM code page.
    const int wlen = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), int(raw.size()), nullptr, 0);
    if (wlen <= 0) return raw;
    std::wstring wide(std::size_t(wlen), L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, raw.data(), int(raw.size()), wide.data(), wlen);
    return fs::path(wide).u8string();
#else
    return raw;
#endif
}

} // namespace

std::uint32_t fileTimeToDos(std::uint64_t fileTime)
{
    if (fileTime <= kFileTimeUnixEpoch) return (0u << 25) | (1u << 21) | (1u << 16); // 1980-01-01
    const std::time_t t = std::time_t((fileTime - kFileTimeUnixEpoch) / 10000000ULL);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    if (tm.tm_year < 80) return (1u << 21) | (1u << 16);
    return (std::uint32_t(tm.tm_year - 80) << 25) | (std::uint32_t(tm.tm_mon + 1) << 21) | (std::uint32_t(tm.tm_mday) << 16) |
           (std::uint32_t(tm.tm_hour) << 11) | (std::uint32_t(tm.tm_min) << 5) | std::uint32_t(tm.tm_sec / 2);
}

std::uint64_t dosToFileTime(std::uint32_t dos)
{
    std::tm tm{};
    tm.tm_year = int((dos >> 25) & 0x7F) + 80;
    tm.tm_mon = int((dos >> 21) & 0x0F) - 1;
    tm.tm_mday = int((dos >> 16) & 0x1F);
    tm.tm_hour = int((dos >> 11) & 0x1F);
    tm.tm_min = int((dos >> 5) & 0x3F);
    tm.tm_sec = int(dos & 0x1F) * 2;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t < 0) return 0;
    return kFileTimeUnixEpoch + std::uint64_t(t) * 10000000ULL;
}

Method zipMethodFromCode(std::uint16_t code)
{
    switch (code) {
    case 0: return Method::Store;
    case 8: return Method::Deflate;
    case 14: return Method::LZMA;
    case 93: return Method::Zstd;
    default: return Method::Other;
    }
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void ZipWriter::beginEntry(const EntryInfo &info)
{
    if (m_open) throw Error("zip writer: previous entry still open");
    Record r;
    r.info = info;
    if (r.info.directory && (r.info.name.empty() || r.info.name.back() != '/')) r.info.name += '/';
    if (r.info.directory) r.info.method = Method::Store;
    r.localOffset = m_out.tell();
    r.zip64Local = info.size >= kZip64LocalThreshold;
    const std::uint32_t dos = fileTimeToDos(info.mtime);
    ByteWriter w;
    w.u32(kLocalSig);
    w.u16(r.zip64Local ? 45 : 20);
    w.u16(0x0800); // UTF-8 names
    w.u16(methodCode(r.info.method));
    w.u16(std::uint16_t(dos));
    w.u16(std::uint16_t(dos >> 16));
    w.u32(0); // crc, patched
    w.u32(r.zip64Local ? k32Max : 0);
    w.u32(r.zip64Local ? k32Max : 0);
    w.u16(std::uint16_t(r.info.name.size()));
    w.u16(r.zip64Local ? 20 : 0);
    w.raw(r.info.name.data(), r.info.name.size());
    if (r.zip64Local) {
        w.u16(0x0001);
        w.u16(16);
        w.u64(0);
        w.u64(0);
    }
    m_out.write(w.bytes.data(), w.size());
    m_records.push_back(std::move(r));
    m_open = true;
}

void ZipWriter::writeData(const void *data, std::size_t size)
{
    m_out.write(data, size);
}

void ZipWriter::endEntry(std::uint32_t crc)
{
    if (!m_open) throw Error("zip writer: no open entry");
    Record &r = m_records.back();
    const std::uint64_t dataStart = r.localOffset + 30 + r.info.name.size() + (r.zip64Local ? 20 : 0);
    r.compressedSize = m_out.tell() - dataStart;
    r.crc = crc;
    std::uint8_t patch[8];
    setLE32(patch, crc);
    m_out.writeAt(r.localOffset + 14, patch, 4);
    if (r.zip64Local) {
        std::uint8_t sizes[16];
        setLE64(sizes, r.info.size);
        setLE64(sizes + 8, r.compressedSize);
        m_out.writeAt(r.localOffset + 30 + r.info.name.size() + 4, sizes, 16);
    } else {
        if (r.compressedSize >= k32Max || r.info.size >= k32Max) throw Error("zip writer: entry unexpectedly exceeds 4 GiB");
        setLE32(patch, std::uint32_t(r.compressedSize));
        setLE32(patch + 4, std::uint32_t(r.info.size));
        m_out.writeAt(r.localOffset + 18, patch, 8);
    }
    m_open = false;
}

void ZipWriter::finish()
{
    if (m_open) throw Error("zip writer: entry still open");
    const std::uint64_t cdOffset = m_out.tell();
    ByteWriter w;
    for (const Record &r : m_records) {
        const bool bigU = r.info.size >= k32Max;
        const bool bigC = r.compressedSize >= k32Max;
        const bool bigO = r.localOffset >= k32Max;
        const bool zip64 = bigU || bigC || bigO;
        const std::uint32_t dos = fileTimeToDos(r.info.mtime);
        ByteWriter extra;
        if (zip64) {
            extra.u16(0x0001);
            extra.u16(std::uint16_t(8 * (int(bigU) + int(bigC) + int(bigO))));
            if (bigU) extra.u64(r.info.size);
            if (bigC) extra.u64(r.compressedSize);
            if (bigO) extra.u64(r.localOffset);
        }
        if (r.info.mtime) {
            extra.u16(0x000a); // NTFS: exact timestamps
            extra.u16(32);
            extra.u32(0);
            extra.u16(0x0001);
            extra.u16(24);
            extra.u64(r.info.mtime);
            extra.u64(r.info.mtime);
            extra.u64(r.info.mtime);
        }
        std::uint32_t attrs = r.info.attributes & 0xFFFF;
        if (r.info.directory) attrs |= 0x10;
        w.u32(kCentralSig);
        w.u16(0x003F); // made by: MS-DOS/Windows attributes, spec 6.3
        w.u16((zip64 || r.zip64Local) ? 45 : 20);
        w.u16(0x0800);
        w.u16(methodCode(r.info.method));
        w.u16(std::uint16_t(dos));
        w.u16(std::uint16_t(dos >> 16));
        w.u32(r.crc);
        w.u32(bigC ? k32Max : std::uint32_t(r.compressedSize));
        w.u32(bigU ? k32Max : std::uint32_t(r.info.size));
        w.u16(std::uint16_t(r.info.name.size()));
        w.u16(std::uint16_t(extra.size()));
        w.u16(0);
        w.u16(0);
        w.u16(0);
        w.u32(attrs);
        w.u32(bigO ? k32Max : std::uint32_t(r.localOffset));
        w.raw(r.info.name.data(), r.info.name.size());
        w.raw(extra.bytes.data(), extra.size());
        if (w.size() >= (8u << 20)) {
            m_out.write(w.bytes.data(), w.size());
            w.bytes.clear();
        }
    }
    m_out.write(w.bytes.data(), w.size());
    w.bytes.clear();
    const std::uint64_t cdSize = m_out.tell() - cdOffset;
    const std::uint64_t count = m_records.size();
    const bool zip64 = count >= 0xFFFF || cdSize >= k32Max || cdOffset >= k32Max;
    if (zip64) {
        const std::uint64_t end64 = m_out.tell();
        w.u32(kEnd64Sig);
        w.u64(44);
        w.u16(0x003F);
        w.u16(45);
        w.u32(0);
        w.u32(0);
        w.u64(count);
        w.u64(count);
        w.u64(cdSize);
        w.u64(cdOffset);
        w.u32(kLocator64Sig);
        w.u32(0);
        w.u64(end64);
        w.u32(1);
    }
    w.u32(kEndSig);
    w.u16(0);
    w.u16(0);
    w.u16(std::uint16_t(std::min<std::uint64_t>(count, 0xFFFF)));
    w.u16(std::uint16_t(std::min<std::uint64_t>(count, 0xFFFF)));
    w.u32(cdSize >= k32Max ? k32Max : std::uint32_t(cdSize));
    w.u32(cdOffset >= k32Max ? k32Max : std::uint32_t(cdOffset));
    w.u16(0);
    m_out.write(w.bytes.data(), w.size());
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
void ZipReader::open(const fs::path &path)
{
    m_path = path;
    m_entries.clear();
    InFile file(path);
    const std::uint64_t size = file.size();
    if (size < 22) throw Error("not a zip archive");
    const std::size_t tailSize = std::size_t(std::min<std::uint64_t>(size, 65535 + 22 + 20));
    std::vector<std::uint8_t> tail(tailSize);
    file.seek(size - tailSize);
    file.readExact(tail.data(), tailSize);
    std::ptrdiff_t eocd = -1;
    for (std::ptrdiff_t i = std::ptrdiff_t(tailSize) - 22; i >= 0; --i) {
        if (getLE32(tail.data() + i) == kEndSig) { eocd = i; break; }
    }
    if (eocd < 0) throw Error("not a zip archive (end of central directory not found)");
    const std::uint8_t *e = tail.data() + eocd;
    std::uint64_t count = getLE16(e + 10);
    std::uint64_t cdSize = getLE32(e + 12);
    std::uint64_t cdOffset = getLE32(e + 16);
    const std::uint64_t eocdPos = size - tailSize + std::uint64_t(eocd);
    const bool hasLocator = eocd >= 20 && getLE32(e - 20) == kLocator64Sig;
    if (hasLocator) {
        const std::uint64_t end64 = getLE64(e - 20 + 8);
        std::uint8_t rec[56];
        file.seek(end64);
        file.readExact(rec, sizeof(rec));
        if (getLE32(rec) != kEnd64Sig) throw Error("corrupt ZIP64 end record");
        count = getLE64(rec + 32);
        cdSize = getLE64(rec + 40);
        cdOffset = getLE64(rec + 48);
    }
    // Self-extracting or prefixed archives: offsets are relative to a shifted base.
    std::uint64_t base = 0;
    if (cdOffset + cdSize < eocdPos && !hasLocator) base = eocdPos - (cdOffset + cdSize);
    if (cdOffset + base + cdSize > size) throw Error("corrupt zip archive (central directory out of range)");
    std::vector<std::uint8_t> cd(static_cast<std::size_t>(cdSize));
    file.seek(cdOffset + base);
    file.readExact(cd.data(), cd.size());
    m_entries.reserve(std::size_t(count));
    std::size_t p = 0;
    while (p + 46 <= cd.size() && getLE32(cd.data() + p) == kCentralSig) {
        const std::uint8_t *h = cd.data() + p;
        ZipEntry z;
        const std::uint16_t madeBy = getLE16(h + 4);
        z.flags = getLE16(h + 8);
        z.methodCode = getLE16(h + 10);
        const std::uint32_t dos = std::uint32_t(getLE16(h + 12)) | (std::uint32_t(getLE16(h + 14)) << 16);
        z.crc = getLE32(h + 16);
        z.hasCrc = true;
        std::uint64_t csize = getLE32(h + 20);
        std::uint64_t usize = getLE32(h + 24);
        const std::uint16_t nameLen = getLE16(h + 28), extraLen = getLE16(h + 30), commentLen = getLE16(h + 32);
        const std::uint32_t external = getLE32(h + 38);
        std::uint64_t offset = getLE32(h + 42);
        if (p + 46 + nameLen + extraLen + commentLen > cd.size()) throw Error("corrupt zip central directory");
        std::string rawName(reinterpret_cast<const char *>(h + 46), nameLen);
        std::string name = (z.flags & 0x0800) ? rawName : nativeToUtf8(rawName);
        const std::uint8_t *x = h + 46 + nameLen;
        const std::uint8_t *xEnd = x + extraLen;
        z.mtime = dosToFileTime(dos);
        while (x + 4 <= xEnd) {
            const std::uint16_t tag = getLE16(x), len = getLE16(x + 2);
            const std::uint8_t *d = x + 4;
            if (d + len > xEnd) break;
            if (tag == 0x0001) {
                const std::uint8_t *q = d;
                if (usize == k32Max && q + 8 <= d + len) { usize = getLE64(q); q += 8; }
                if (csize == k32Max && q + 8 <= d + len) { csize = getLE64(q); q += 8; }
                if (offset == k32Max && q + 8 <= d + len) { offset = getLE64(q); q += 8; }
            } else if (tag == 0x000a && len >= 32) {
                if (getLE16(d + 4) == 0x0001 && getLE16(d + 6) >= 24) z.mtime = getLE64(d + 8);
            } else if (tag == 0x5455 && len >= 5 && (d[0] & 1)) {
                z.mtime = kFileTimeUnixEpoch + std::uint64_t(getLE32(d + 1)) * 10000000ULL;
            } else if (tag == 0x7075 && len >= 5 && d[0] == 1 && !(z.flags & 0x0800)) {
                if (getLE32(d + 1) == crc32(rawName.data(), rawName.size()))
                    name.assign(reinterpret_cast<const char *>(d + 5), len - 5);
            }
            x = d + len;
        }
        std::replace(name.begin(), name.end(), '\\', '/');
        const int host = madeBy >> 8;
        z.directory = !name.empty() && name.back() == '/';
        if ((host == 0 || host == 10 || host == 14) && (external & 0x10)) z.directory = true;
        if (host == 3 && ((external >> 16) & 0170000) == 0040000) z.directory = true;
        while (!name.empty() && name.back() == '/') name.pop_back();
        z.path = name;
        if (host == 0 || host == 10 || host == 14 || host == 11) {
            z.attributes = external & 0xFFFF;
            z.hasAttributes = true;
        } else if (host == 3) {
            const std::uint32_t mode = external >> 16;
            z.attributes = (z.directory ? 0x10u : 0x20u) | 0x8000u | (mode << 16);
            z.hasAttributes = mode != 0;
        }
        z.size = usize;
        z.packedSize = csize;
        z.localHeaderOffset = offset + base;
        z.method = zipMethodFromCode(z.methodCode);
        z.encrypted = (z.flags & 1) != 0;
        z.supported = !z.encrypted && (z.directory || z.method == Method::Store || z.method == Method::Deflate || z.method == Method::Zstd);
        if (!z.path.empty()) m_entries.push_back(std::move(z));
        p += 46 + nameLen + extraLen + commentLen;
    }
}

std::uint64_t ZipReader::dataOffset(InFile &file, const ZipEntry &entry) const
{
    std::uint8_t h[30];
    file.seek(entry.localHeaderOffset);
    file.readExact(h, 30);
    if (getLE32(h) != kLocalSig) throw Error("corrupt zip archive (bad local header): " + entry.path);
    return entry.localHeaderOffset + 30 + getLE16(h + 26) + getLE16(h + 28);
}

} // namespace vyra
