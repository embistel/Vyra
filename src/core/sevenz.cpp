#include "core/sevenz.h"
#include "core/bytes.h"
#include "core/crc32.h"
#include "core/streamdecoder.h"

#include <algorithm>
#include <cstring>

namespace vyra {
namespace {

const std::uint8_t kSignature[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};

enum Id : std::uint8_t {
    kEnd = 0x00, kHeader = 0x01, kArchiveProperties = 0x02, kAdditionalStreamsInfo = 0x03,
    kMainStreamsInfo = 0x04, kFilesInfo = 0x05, kPackInfo = 0x06, kUnPackInfo = 0x07,
    kSubStreamsInfo = 0x08, kSize = 0x09, kCRC = 0x0A, kFolder = 0x0B, kCodersUnPackSize = 0x0C,
    kNumUnPackStream = 0x0D, kEmptyStream = 0x0E, kEmptyFile = 0x0F, kAnti = 0x10, kName = 0x11,
    kCTime = 0x12, kATime = 0x13, kMTime = 0x14, kWinAttributes = 0x15, kComment = 0x16,
    kEncodedHeader = 0x17, kStartPos = 0x18, kDummy = 0x19
};

// ---- UTF-8 <-> UTF-16 ----
void appendUtf16(std::vector<std::uint8_t> &out, const std::string &s)
{
    std::size_t i = 0;
    auto put = [&](std::uint32_t u) { out.push_back(std::uint8_t(u)); out.push_back(std::uint8_t(u >> 8)); };
    while (i < s.size()) {
        std::uint32_t c = std::uint8_t(s[i]);
        int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        if (extra) c &= (0x3Fu >> extra);
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) c = (c << 6) | (std::uint8_t(s[i]) & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            put(0xD800 + (c >> 10));
            put(0xDC00 + (c & 0x3FF));
        } else {
            put(c);
        }
    }
    put(0);
}

std::string utf16ToUtf8(const std::uint16_t *p, std::size_t n)
{
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t c = p[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (p[++i] - 0xDC00);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

// ---- 7z variable-length numbers ----
void writeNumber(std::vector<std::uint8_t> &v, std::uint64_t value)
{
    std::uint8_t first = 0, mask = 0x80;
    int i = 0;
    for (; i < 8; ++i) {
        if (value < (std::uint64_t(1) << (7 * (i + 1)))) {
            first |= std::uint8_t(value >> (8 * i));
            break;
        }
        first |= mask;
        mask >>= 1;
    }
    v.push_back(first);
    for (; i > 0; --i) {
        v.push_back(std::uint8_t(value));
        value >>= 8;
    }
}

void writeBits(std::vector<std::uint8_t> &v, const std::vector<bool> &bits)
{
    std::uint8_t b = 0, mask = 0x80;
    for (bool bit : bits) {
        if (bit) b |= mask;
        mask >>= 1;
        if (!mask) { v.push_back(b); b = 0; mask = 0x80; }
    }
    if (mask != 0x80) v.push_back(b);
}

class HeaderReader {
public:
    HeaderReader(const std::uint8_t *p, std::size_t n) : m_p(p), m_end(p + n) {}
    std::uint8_t byte()
    {
        need(1);
        return *m_p++;
    }
    std::uint64_t number()
    {
        const std::uint8_t first = byte();
        std::uint8_t mask = 0x80;
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            if ((first & mask) == 0) {
                const std::uint64_t high = first & (mask - 1u);
                return value | (high << (8 * i));
            }
            value |= std::uint64_t(byte()) << (8 * i);
            mask >>= 1;
        }
        return value;
    }
    std::uint64_t count(std::uint64_t limit = 1u << 26)
    {
        const std::uint64_t n = number();
        if (n > limit) throw Error("corrupt 7z header (count out of range)");
        return n;
    }
    std::uint32_t u32() { need(4); const std::uint32_t v = getLE32(m_p); m_p += 4; return v; }
    std::uint64_t u64() { need(8); const std::uint64_t v = getLE64(m_p); m_p += 8; return v; }
    const std::uint8_t *bytes(std::size_t n) { need(n); const std::uint8_t *p = m_p; m_p += n; return p; }
    void skip(std::uint64_t n) { need(std::size_t(n)); m_p += n; }
    std::vector<bool> bits(std::size_t n)
    {
        std::vector<bool> out(n);
        std::uint8_t b = 0, mask = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!mask) { b = byte(); mask = 0x80; }
            out[i] = (b & mask) != 0;
            mask >>= 1;
        }
        return out;
    }
    std::vector<bool> definedBits(std::size_t n)
    {
        if (byte()) return std::vector<bool>(n, true);
        return bits(n);
    }
    void expect(std::uint8_t id)
    {
        if (byte() != id) throw Error("corrupt 7z header (unexpected property)");
    }
    std::size_t remaining() const { return std::size_t(m_end - m_p); }

private:
    void need(std::size_t n) const
    {
        if (std::size_t(m_end - m_p) < n) throw Error("corrupt 7z header (truncated)");
    }
    const std::uint8_t *m_p;
    const std::uint8_t *m_end;
};

struct StreamsInfo {
    std::uint64_t packPos = 0;
    std::vector<std::uint64_t> packSizes;
    std::vector<SevenZFolder> folders;
    std::vector<std::uint64_t> substreamSizes;
    std::vector<bool> substreamCrcDefined;
    std::vector<std::uint32_t> substreamCrcs;
};

void readDigests(HeaderReader &r, std::size_t n, std::vector<bool> &defined, std::vector<std::uint32_t> &crcs)
{
    defined = r.definedBits(n);
    crcs.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        if (defined[i]) crcs[i] = r.u32();
}

void readPackInfo(HeaderReader &r, StreamsInfo &info)
{
    info.packPos = r.number();
    const std::uint64_t n = r.count();
    info.packSizes.assign(std::size_t(n), 0);
    for (;;) {
        const std::uint8_t id = r.byte();
        if (id == kEnd) break;
        if (id == kSize) {
            for (auto &s : info.packSizes) s = r.number();
        } else if (id == kCRC) {
            std::vector<bool> d;
            std::vector<std::uint32_t> c;
            readDigests(r, std::size_t(n), d, c);
        } else {
            throw Error("corrupt 7z header (pack info)");
        }
    }
}

void readFolder(HeaderReader &r, SevenZFolder &f)
{
    const std::uint64_t numCoders = r.count(64);
    std::uint64_t totalIn = 0, totalOut = 0;
    for (std::uint64_t i = 0; i < numCoders; ++i) {
        const std::uint8_t flag = r.byte();
        if (flag & 0x80) throw Error("unsupported 7z coder definition");
        SevenZCoder c;
        const std::uint8_t *id = r.bytes(flag & 0x0F);
        c.id.assign(id, id + (flag & 0x0F));
        c.method = sevenZMethodFromId(c.id);
        if (flag & 0x10) {
            c.inStreams = r.count(64);
            c.outStreams = r.count(64);
        }
        if (flag & 0x20) {
            const std::uint64_t size = r.count(1 << 16);
            const std::uint8_t *p = r.bytes(std::size_t(size));
            c.properties.assign(p, p + size);
        }
        totalIn += c.inStreams;
        totalOut += c.outStreams;
        f.coders.push_back(std::move(c));
    }
    if (totalOut == 0) throw Error("corrupt 7z folder");
    for (std::uint64_t i = 0; i + 1 < totalOut; ++i) {
        const std::uint64_t in = r.number(), out = r.number();
        f.bindPairs.emplace_back(in, out);
    }
    if (totalIn < f.bindPairs.size()) throw Error("corrupt 7z folder");
    const std::uint64_t numPacked = totalIn - f.bindPairs.size();
    if (numPacked == 1) {
        for (std::uint64_t i = 0; i < totalIn; ++i) {
            const bool bound = std::any_of(f.bindPairs.begin(), f.bindPairs.end(), [&](auto &b) { return b.first == i; });
            if (!bound) { f.packedStreams.push_back(i); break; }
        }
    } else {
        for (std::uint64_t i = 0; i < numPacked; ++i) f.packedStreams.push_back(r.number());
    }
    f.unpackSizes.assign(std::size_t(totalOut), 0);
}

void readUnpackInfo(HeaderReader &r, StreamsInfo &info)
{
    r.expect(kFolder);
    const std::uint64_t n = r.count();
    if (r.byte() != 0) throw Error("unsupported 7z header (external folders)");
    info.folders.resize(std::size_t(n));
    for (auto &f : info.folders) readFolder(r, f);
    r.expect(kCodersUnPackSize);
    for (auto &f : info.folders)
        for (auto &s : f.unpackSizes) s = r.number();
    for (;;) {
        const std::uint8_t id = r.byte();
        if (id == kEnd) break;
        if (id == kCRC) {
            std::vector<bool> d;
            std::vector<std::uint32_t> c;
            readDigests(r, std::size_t(n), d, c);
            for (std::size_t i = 0; i < info.folders.size(); ++i) {
                info.folders[i].hasCrc = d[i];
                info.folders[i].crc = c[i];
            }
        } else {
            throw Error("corrupt 7z header (unpack info)");
        }
    }
}

void readSubStreamsInfo(HeaderReader &r, StreamsInfo &info)
{
    for (auto &f : info.folders) f.substreams = 1;
    std::uint8_t id = r.byte();
    if (id == kNumUnPackStream) {
        for (auto &f : info.folders) f.substreams = r.count();
        id = r.byte();
    }
    const bool haveSizes = id == kSize;
    for (auto &f : info.folders) {
        if (f.substreams == 0) continue;
        std::uint64_t sum = 0;
        for (std::uint64_t k = 1; k < f.substreams; ++k) {
            const std::uint64_t s = haveSizes ? r.number() : 0;
            info.substreamSizes.push_back(s);
            sum += s;
        }
        if (sum > f.unpackSize()) throw Error("corrupt 7z header (substream sizes)");
        info.substreamSizes.push_back(f.unpackSize() - sum);
    }
    if (haveSizes) id = r.byte();
    std::size_t digestCount = 0;
    for (auto &f : info.folders)
        if (!(f.substreams == 1 && f.hasCrc)) digestCount += std::size_t(f.substreams);
    // Default: substream CRC = folder CRC for single-stream folders.
    for (auto &f : info.folders) {
        for (std::uint64_t k = 0; k < f.substreams; ++k) {
            const bool single = f.substreams == 1 && f.hasCrc;
            info.substreamCrcDefined.push_back(single);
            info.substreamCrcs.push_back(single ? f.crc : 0);
        }
    }
    while (id != kEnd) {
        if (id == kCRC) {
            std::vector<bool> d;
            std::vector<std::uint32_t> c;
            readDigests(r, digestCount, d, c);
            std::size_t k = 0, s = 0;
            for (auto &f : info.folders) {
                if (f.substreams == 1 && f.hasCrc) { ++s; continue; }
                for (std::uint64_t j = 0; j < f.substreams; ++j, ++s, ++k) {
                    info.substreamCrcDefined[s] = d[k];
                    info.substreamCrcs[s] = c[k];
                }
            }
        } else {
            r.skip(r.number());
        }
        id = r.byte();
    }
}

void readStreamsInfo(HeaderReader &r, StreamsInfo &info)
{
    bool haveSub = false;
    for (;;) {
        const std::uint8_t id = r.byte();
        if (id == kEnd) break;
        if (id == kPackInfo) readPackInfo(r, info);
        else if (id == kUnPackInfo) readUnpackInfo(r, info);
        else if (id == kSubStreamsInfo) { readSubStreamsInfo(r, info); haveSub = true; }
        else throw Error("corrupt 7z header (streams info)");
    }
    if (!haveSub) {
        for (auto &f : info.folders) {
            f.substreams = 1;
            info.substreamSizes.push_back(f.unpackSize());
            info.substreamCrcDefined.push_back(f.hasCrc);
            info.substreamCrcs.push_back(f.crc);
        }
    }
    // Absolute pack stream offsets, in folder order.
    std::uint64_t offset = 32 + info.packPos;
    std::size_t pack = 0;
    for (auto &f : info.folders) {
        for (std::size_t k = 0; k < f.packedStreams.size(); ++k) {
            if (pack >= info.packSizes.size()) throw Error("corrupt 7z header (pack streams)");
            f.packOffsets.push_back(offset);
            f.packSizes.push_back(info.packSizes[pack]);
            offset += info.packSizes[pack++];
        }
    }
}

} // namespace

std::vector<std::uint8_t> sevenZMethodId(Method method)
{
    switch (method) {
    case Method::Store: return {0x00};
    case Method::Deflate: return {0x04, 0x01, 0x08};
    case Method::LZ4: return {0x04, 0xF7, 0x11, 0x04};
    case Method::Zstd: return {0x04, 0xF7, 0x11, 0x01};
    case Method::LZMA: return {0x03, 0x01, 0x01};
    case Method::LZMA2: return {0x21};
    case Method::BCJ: return {0x03, 0x03, 0x01, 0x03};
    default: throw Error("method has no 7z id");
    }
}

Method sevenZMethodFromId(const std::vector<std::uint8_t> &id)
{
    for (Method m : {Method::Store, Method::Deflate, Method::LZ4, Method::Zstd, Method::LZMA, Method::LZMA2, Method::BCJ})
        if (sevenZMethodId(m) == id) return m;
    return Method::Other;
}

std::vector<std::uint8_t> sevenZMethodProperties(Method method, int level)
{
    // 7-Zip-zstd coder props: {version major, version minor, level, 0, 0}.
    if (method == Method::LZ4) return {1, 10, std::uint8_t(std::clamp(level, 1, 12)), 0, 0};
    if (method == Method::Zstd) return {1, 5, std::uint8_t(std::clamp(level, 1, 22)), 0, 0};
    return {};
}

std::uint64_t SevenZFolder::unpackSize() const
{
    for (std::size_t i = 0; i < unpackSizes.size(); ++i) {
        const bool bound = std::any_of(bindPairs.begin(), bindPairs.end(), [&](auto &b) { return b.second == i; });
        if (!bound) return unpackSizes[i];
    }
    return 0;
}

std::vector<std::size_t> SevenZFolder::decodeChain() const
{
    std::vector<std::size_t> chain;
    for (const auto &c : coders)
        if (c.inStreams != 1 || c.outStreams != 1) return {};
    if (packedStreams.size() != 1) return {};
    // With 1-in/1-out coders, coder i owns in-stream i and out-stream i.
    std::size_t out = 0;
    for (; out < unpackSizes.size(); ++out) {
        const bool bound = std::any_of(bindPairs.begin(), bindPairs.end(), [&](auto &b) { return b.second == out; });
        if (!bound) break;
    }
    if (out >= coders.size()) return {};
    std::size_t coder = out;
    for (std::size_t guard = 0; guard <= coders.size(); ++guard) {
        chain.push_back(coder);
        auto it = std::find_if(bindPairs.begin(), bindPairs.end(), [&](auto &b) { return b.first == coder; });
        if (it == bindPairs.end()) {
            if (packedStreams[0] != coder) return {};
            return chain;
        }
        coder = std::size_t(it->second);
        if (coder >= coders.size()) return {};
    }
    return {};
}

bool sevenZFolderSupported(const SevenZFolder &folder)
{
    const auto chain = folder.decodeChain();
    if (chain.empty()) return false;
    for (std::size_t k = 0; k < chain.size(); ++k) {
        const Method m = folder.coders[chain[k]].method;
        const bool last = k + 1 == chain.size();
        if (last ? (!canDecode(m) || m == Method::BCJ) : m != Method::BCJ) return false;
    }
    return true;
}

std::vector<std::uint8_t> decodeFolderToMemory(InFile &file, const SevenZFolder &folder)
{
    if (!sevenZFolderSupported(folder)) throw Error("unsupported 7z coder chain");
    const auto chain = folder.decodeChain();
    const SevenZCoder &base = folder.coders[chain.back()];
    std::unique_ptr<StreamDecoder> decoder =
        createStreamDecoder({base.method, base.properties, folder.unpackSizes[chain.back()]}, nullptr, 0);
    for (std::size_t k = chain.size() - 1; k-- > 0;) decoder = createBcjDecoder(std::move(decoder));
    std::vector<std::uint8_t> packed(std::size_t(folder.packSizes[0]));
    file.seek(folder.packOffsets[0]);
    file.readExact(packed.data(), packed.size());
    std::vector<std::uint8_t> out;
    out.reserve(std::size_t(folder.unpackSize()));
    decoder->feed(packed.data(), packed.size(), true, [&](const std::uint8_t *p, std::size_t n) { out.insert(out.end(), p, p + n); });
    if (out.size() != folder.unpackSize()) throw Error("corrupt 7z data (size mismatch)");
    if (folder.hasCrc && crc32(out.data(), out.size()) != folder.crc) throw Error("corrupt 7z data (CRC mismatch)");
    return out;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void SevenZWriter::begin()
{
    const std::uint8_t placeholder[32] = {};
    m_out.write(placeholder, sizeof(placeholder));
}

void SevenZWriter::finish(const std::vector<SevenZFileInfo> &files, const std::vector<SevenZFolderInfo> &folders)
{
    const std::uint64_t headerOffset = m_out.tell();
    std::vector<std::uint8_t> h;
    h.push_back(kHeader);
    if (!folders.empty()) {
        h.push_back(kMainStreamsInfo);
        h.push_back(kPackInfo);
        writeNumber(h, 0);
        writeNumber(h, folders.size());
        h.push_back(kSize);
        for (const auto &f : folders) writeNumber(h, f.packSize);
        h.push_back(kEnd);

        h.push_back(kUnPackInfo);
        h.push_back(kFolder);
        writeNumber(h, folders.size());
        h.push_back(0);
        for (const auto &f : folders) {
            const auto id = sevenZMethodId(f.method);
            writeNumber(h, 1);
            h.push_back(std::uint8_t(id.size() | (f.properties.empty() ? 0 : 0x20)));
            h.insert(h.end(), id.begin(), id.end());
            if (!f.properties.empty()) {
                writeNumber(h, f.properties.size());
                h.insert(h.end(), f.properties.begin(), f.properties.end());
            }
        }
        h.push_back(kCodersUnPackSize);
        for (const auto &f : folders) writeNumber(h, f.unpackSize);
        h.push_back(kEnd);

        h.push_back(kSubStreamsInfo);
        h.push_back(kNumUnPackStream);
        for (const auto &f : folders) writeNumber(h, f.streamCount);
        h.push_back(kSize);
        std::size_t fileIndex = 0;
        std::vector<std::uint32_t> crcs;
        for (const auto &f : folders) {
            for (std::uint64_t k = 0; k < f.streamCount; ++k) {
                while (fileIndex < files.size() && !files[fileIndex].hasStream) ++fileIndex;
                if (fileIndex >= files.size()) throw Error("7z writer: stream/file mismatch");
                if (k + 1 < f.streamCount) writeNumber(h, files[fileIndex].size);
                crcs.push_back(files[fileIndex].crc);
                ++fileIndex;
            }
        }
        h.push_back(kCRC);
        h.push_back(1); // all defined
        for (std::uint32_t c : crcs) {
            std::uint8_t b[4];
            setLE32(b, c);
            h.insert(h.end(), b, b + 4);
        }
        h.push_back(kEnd);
        h.push_back(kEnd);
    }

    if (!files.empty()) {
        h.push_back(kFilesInfo);
        writeNumber(h, files.size());
        std::vector<bool> emptyStream, emptyFile;
        bool anyEmpty = false, anyEmptyFile = false;
        for (const auto &f : files) {
            emptyStream.push_back(!f.hasStream);
            if (!f.hasStream) {
                anyEmpty = true;
                emptyFile.push_back(!f.directory);
                anyEmptyFile |= !f.directory;
            }
        }
        if (anyEmpty) {
            std::vector<std::uint8_t> bits;
            writeBits(bits, emptyStream);
            h.push_back(kEmptyStream);
            writeNumber(h, bits.size());
            h.insert(h.end(), bits.begin(), bits.end());
            if (anyEmptyFile) {
                bits.clear();
                writeBits(bits, emptyFile);
                h.push_back(kEmptyFile);
                writeNumber(h, bits.size());
                h.insert(h.end(), bits.begin(), bits.end());
            }
        }
        std::vector<std::uint8_t> names;
        names.push_back(0); // not external
        for (const auto &f : files) appendUtf16(names, f.name);
        h.push_back(kName);
        writeNumber(h, names.size());
        h.insert(h.end(), names.begin(), names.end());

        std::vector<std::uint8_t> times = {1, 0}; // all defined, not external
        for (const auto &f : files) {
            std::uint8_t b[8];
            setLE64(b, f.mtime);
            times.insert(times.end(), b, b + 8);
        }
        h.push_back(kMTime);
        writeNumber(h, times.size());
        h.insert(h.end(), times.begin(), times.end());

        std::vector<std::uint8_t> attrs = {1, 0};
        for (const auto &f : files) {
            std::uint8_t b[4];
            setLE32(b, f.attributes | (f.directory ? 0x10u : 0u));
            attrs.insert(attrs.end(), b, b + 4);
        }
        h.push_back(kWinAttributes);
        writeNumber(h, attrs.size());
        h.insert(h.end(), attrs.begin(), attrs.end());
        h.push_back(kEnd);
    }
    h.push_back(kEnd);
    m_out.write(h.data(), h.size());

    std::uint8_t start[32] = {};
    std::memcpy(start, kSignature, 6);
    start[6] = 0;
    start[7] = 4;
    setLE64(start + 12, headerOffset - 32);
    setLE64(start + 20, h.size());
    setLE32(start + 28, crc32(h.data(), h.size()));
    setLE32(start + 8, crc32(start + 12, 20));
    m_out.writeAt(0, start, 32);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
void SevenZReader::open(const fs::path &path)
{
    m_path = path;
    m_items.clear();
    m_folders.clear();
    InFile file(path);
    std::uint8_t start[32];
    if (file.size() < 32) throw Error("not a 7z archive");
    file.readExact(start, 32);
    if (std::memcmp(start, kSignature, 6) != 0) throw Error("not a 7z archive");
    if (crc32(start + 12, 20) != getLE32(start + 8)) throw Error("corrupt 7z archive (start header CRC)");
    const std::uint64_t nextOffset = getLE64(start + 12);
    const std::uint64_t nextSize = getLE64(start + 20);
    if (nextSize == 0) return; // empty archive
    if (nextOffset + nextSize + 32 > file.size() || nextSize > (1ull << 31))
        throw Error("corrupt or truncated 7z archive");
    std::vector<std::uint8_t> header(static_cast<std::size_t>(nextSize));
    file.seek(32 + nextOffset);
    file.readExact(header.data(), header.size());
    if (crc32(header.data(), header.size()) != getLE32(start + 28)) throw Error("corrupt 7z archive (header CRC)");

    // Encoded (compressed) headers, possibly nested.
    for (int depth = 0; depth < 4 && !header.empty() && header[0] == kEncodedHeader; ++depth) {
        HeaderReader r(header.data() + 1, header.size() - 1);
        StreamsInfo info;
        readStreamsInfo(r, info);
        if (info.folders.empty()) throw Error("corrupt 7z archive (encoded header)");
        header = decodeFolderToMemory(file, info.folders[0]);
    }
    HeaderReader r(header.data(), header.size());
    r.expect(kHeader);
    StreamsInfo info;
    std::uint8_t id = r.byte();
    if (id == kArchiveProperties) {
        for (;;) {
            const std::uint64_t type = r.number();
            if (type == 0) break;
            r.skip(r.number());
        }
        id = r.byte();
    }
    if (id == kAdditionalStreamsInfo) {
        StreamsInfo extra;
        readStreamsInfo(r, extra);
        id = r.byte();
    }
    if (id == kMainStreamsInfo) {
        readStreamsInfo(r, info);
        id = r.byte();
    }
    m_folders = info.folders;

    std::size_t numFiles = 0;
    std::vector<bool> emptyStream, emptyFile, anti;
    std::vector<std::string> names;
    std::vector<bool> mtimeDefined, attrDefined;
    std::vector<std::uint64_t> mtimes;
    std::vector<std::uint32_t> attrs;
    if (id == kFilesInfo) {
        numFiles = std::size_t(r.count());
        for (;;) {
            const std::uint64_t type = r.number();
            if (type == kEnd) break;
            const std::uint64_t size = r.number();
            if (size > r.remaining()) throw Error("corrupt 7z header (file property)");
            HeaderReader p(r.bytes(std::size_t(size)), std::size_t(size));
            std::size_t numEmpty = std::size_t(std::count(emptyStream.begin(), emptyStream.end(), true));
            switch (type) {
            case kEmptyStream: emptyStream = p.bits(numFiles); break;
            case kEmptyFile: emptyFile = p.bits(numEmpty); break;
            case kAnti: anti = p.bits(numEmpty); break;
            case kName: {
                if (p.byte() != 0) throw Error("unsupported 7z header (external names)");
                const std::size_t chars = p.remaining() / 2;
                const std::uint8_t *raw = p.bytes(chars * 2);
                std::vector<std::uint16_t> u16(chars);
                for (std::size_t i = 0; i < chars; ++i) u16[i] = getLE16(raw + 2 * i);
                std::size_t s = 0;
                for (std::size_t i = 0; i < chars && names.size() < numFiles; ++i) {
                    if (u16[i] == 0) { names.push_back(utf16ToUtf8(u16.data() + s, i - s)); s = i + 1; }
                }
                break;
            }
            case kMTime: {
                mtimeDefined = p.definedBits(numFiles);
                if (p.byte() != 0) throw Error("unsupported 7z header (external times)");
                mtimes.assign(numFiles, 0);
                for (std::size_t i = 0; i < numFiles; ++i)
                    if (mtimeDefined[i]) mtimes[i] = p.u64();
                break;
            }
            case kWinAttributes: {
                attrDefined = p.definedBits(numFiles);
                if (p.byte() != 0) throw Error("unsupported 7z header (external attributes)");
                attrs.assign(numFiles, 0);
                for (std::size_t i = 0; i < numFiles; ++i)
                    if (attrDefined[i]) attrs[i] = p.u32();
                break;
            }
            default:
                break; // kCTime, kATime, kComment, kStartPos, kDummy ...
            }
        }
    }
    if (emptyStream.empty()) emptyStream.assign(numFiles, false);
    if (names.size() < numFiles) names.resize(numFiles);

    std::size_t folderIndex = 0, substreamInFolder = 0, substream = 0, emptyIndex = 0;
    std::uint64_t offsetInFolder = 0;
    m_items.reserve(numFiles);
    for (std::size_t i = 0; i < numFiles; ++i) {
        SevenZItem item;
        std::string name = names[i];
        std::replace(name.begin(), name.end(), '\\', '/');
        while (!name.empty() && name.back() == '/') name.pop_back();
        item.path = name;
        if (!mtimes.empty() && mtimeDefined[i]) item.mtime = mtimes[i];
        if (!attrs.empty() && attrDefined[i]) {
            item.attributes = attrs[i];
            item.hasAttributes = true;
        }
        if (emptyStream[i]) {
            const bool isFile = emptyIndex < emptyFile.size() && emptyFile[emptyIndex];
            const bool isAnti = emptyIndex < anti.size() && anti[emptyIndex];
            ++emptyIndex;
            if (isAnti) continue;
            item.directory = !isFile;
            if (item.hasAttributes && (item.attributes & 0x10)) item.directory = true;
            item.method = Method::Store;
        } else {
            while (folderIndex < m_folders.size() && substreamInFolder >= m_folders[folderIndex].substreams) {
                ++folderIndex;
                substreamInFolder = 0;
                offsetInFolder = 0;
            }
            if (folderIndex >= m_folders.size() || substream >= info.substreamSizes.size())
                throw Error("corrupt 7z header (more files than streams)");
            const SevenZFolder &f = m_folders[folderIndex];
            item.folder = int(folderIndex);
            item.offsetInFolder = offsetInFolder;
            item.size = info.substreamSizes[substream];
            item.hasCrc = info.substreamCrcDefined[substream];
            item.crc = info.substreamCrcs[substream];
            const auto chain = f.decodeChain();
            item.method = chain.empty() ? Method::Other : f.coders[chain.back()].method;
            item.supported = sevenZFolderSupported(f);
            if (f.substreams == 1 && !f.packSizes.empty()) item.packedSize = f.packSizes[0];
            offsetInFolder += item.size;
            ++substream;
            ++substreamInFolder;
        }
        if (!item.path.empty()) m_items.push_back(std::move(item));
    }
}

} // namespace vyra
