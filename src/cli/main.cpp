// vyra-cli — command-line front end, modelled on 7-Zip's "7z" command.
#include "core/archive.h"
#include "core/backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace vyra;

namespace {

bool stderrIsTty()
{
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

std::string formatBytes(double bytes)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int u = 0;
    while (bytes >= 1024 && u < 4) { bytes /= 1024; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u ? "%.2f %s" : "%.0f %s", bytes, units[u]);
    return buf;
}

std::uint64_t parseSize(const std::string &text)
{
    if (text.empty()) throw Error("missing size value");
    double value = std::stod(text);
    const char suffix = char(std::tolower(static_cast<unsigned char>(text.back())));
    if (suffix == 'k') value *= 1024;
    else if (suffix == 'm') value *= 1024.0 * 1024;
    else if (suffix == 'g') value *= 1024.0 * 1024 * 1024;
    return std::uint64_t(value);
}

Method parseMethod(const std::string &name)
{
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (n == "lz4") return Method::LZ4;
    if (n == "zstd") return Method::Zstd;
    if (n == "deflate") return Method::Deflate;
    if (n == "store" || n == "copy") return Method::Store;
    throw Error("unknown method: " + name + " (lz4, zstd, deflate, store)");
}

void usage()
{
    std::cout <<
        "Vyra " VYRA_VERSION " — GPU accelerated zip/7z archiver (nvCOMP)\n\n"
        "Usage:\n"
        "  vyra-cli a <archive.7z|archive.zip> <files/folders...> [options]   create\n"
        "  vyra-cli x <archive> [-o <dir>] [paths...] [options]              extract with paths\n"
        "  vyra-cli t <archive> [options]                                    test integrity\n"
        "  vyra-cli l <archive>                                              list\n"
        "  vyra-cli info                                                     GPU / backend status\n"
        "  vyra-cli bench <files/folders...> [options]                       throughput without disk writes\n\n"
        "Options:\n"
        "  -m <lz4|zstd|deflate|store>  method (7z default: lz4, zip: deflate)\n"
        "  -l <0-5>                     level: 0 fastest .. 5 best (default 1)\n"
        "  --gpu | --cpu                force backend (default: GPU when available)\n"
        "  --device <n>                 CUDA device index\n"
        "  --chunk <size>               chunk size, e.g. 64k (Deflate max 64k)\n"
        "  --batch <size>               GPU batch size, e.g. 128m\n"
        "  --solid <size|off>           7z solid block size (default 4g)\n"
        "  -t <n>                       CPU threads\n"
        "  -o <dir>                     output folder for x\n"
        "  --keep                       x: do not overwrite existing files\n\n"
        "Compatibility:\n"
        "  .zip (Deflate)  -> Windows Explorer, 7-Zip, any unzip\n"
        "  .7z  Deflate    -> official 7-Zip\n"
        "  .7z  LZ4 / Zstd -> 7-Zip-zstd, NanaZip (official 7-Zip cannot decode these)\n";
}

class ConsoleProgress {
public:
    explicit ConsoleProgress(const char *verb) : m_verb(verb), m_tty(stderrIsTty()) {}
    bool operator()(const Progress &p)
    {
        if (!m_tty) return true;
        const double speed = p.elapsedSeconds > 0 ? double(p.bytesDone) / p.elapsedSeconds : 0;
        const int percent = p.bytesTotal ? int(100.0 * double(p.bytesDone) / double(p.bytesTotal)) : 0;
        std::string item = p.currentItem;
        if (item.size() > 40) item = "..." + item.substr(item.size() - 37);
        std::fprintf(stderr, "\r%s %3d%%  %s / %s  %s/s  %s%-40s", m_verb, percent, formatBytes(double(p.bytesDone)).c_str(),
                     formatBytes(double(p.bytesTotal)).c_str(), formatBytes(speed).c_str(), "", item.c_str());
        std::fflush(stderr);
        m_dirty = true;
        return true;
    }
    ~ConsoleProgress()
    {
        if (m_dirty) std::fprintf(stderr, "\r%-120s\r", "");
    }

private:
    const char *m_verb;
    bool m_tty;
    bool m_dirty = false;
};

int printWarnings(const OperationResult &r)
{
    for (const std::string &w : r.warnings) std::cerr << "WARNING: " << w << '\n';
    return r.warnings.empty() ? 0 : 2;
}

int run(std::vector<std::string> args)
{
    if (args.empty() || args[0] == "-h" || args[0] == "--help") { usage(); return args.empty() ? 1 : 0; }
    const std::string command = args[0];
    std::vector<std::string> positional;
    CompressOptions copt;
    ExtractOptions xopt;
    bool methodGiven = false;
    std::string outDir;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string &a = args[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) throw Error("missing value for " + a);
            return args[++i];
        };
        if (a == "-m") { copt.method = parseMethod(value()); methodGiven = true; }
        else if (a == "-l") copt.level = std::stoi(value());
        else if (a == "--gpu") copt.backend = xopt.backend = BackendKind::Gpu;
        else if (a == "--cpu") copt.backend = xopt.backend = BackendKind::Cpu;
        else if (a == "--device") copt.gpuDevice = xopt.gpuDevice = std::stoi(value());
        else if (a == "--chunk") copt.chunkSize = std::size_t(parseSize(value()));
        else if (a == "--batch") copt.batchSize = xopt.batchSize = std::size_t(parseSize(value()));
        else if (a == "--solid") { const std::string v = value(); copt.solidBlockSize = v == "off" ? 0 : parseSize(v); }
        else if (a == "-t") copt.threads = xopt.threads = std::stoi(value());
        else if (a == "-o") outDir = value();
        else if (a.rfind("-o", 0) == 0 && a.size() > 2) outDir = a.substr(2);
        else if (a == "--keep") xopt.overwrite = false;
        else if (!a.empty() && a[0] == '-' && a.size() > 1) throw Error("unknown option: " + a);
        else positional.push_back(a);
    }

    if (command == "info") {
        const GpuStatus gpu = queryGpu();
        std::cout << "Vyra " << VYRA_VERSION << "\nGPU: " << (gpu.available ? "available — " : "unavailable — ") << gpu.description << '\n';
        return 0;
    }
    if (command == "bench") {
        // Encode from the file cache and discard the output: measures the
        // pipeline itself rather than the disk.
        if (positional.empty()) throw Error("bench needs input files/folders");
        std::vector<fs::path> inputs;
        for (const auto &p : positional) inputs.push_back(pathFromUtf8(p));
        const std::vector<std::pair<Format, Method>> runs = {
            {Format::SevenZip, Method::LZ4}, {Format::SevenZip, Method::Zstd}, {Format::Zip, Method::Deflate}};
        auto label = [](BackendKind k) { return k == BackendKind::Auto ? "AUTO" : k == BackendKind::Gpu ? "GPU" : "CPU"; };
        for (BackendKind kind : {BackendKind::Auto, BackendKind::Gpu, BackendKind::Cpu}) {
            if (copt.backend != BackendKind::Auto && copt.backend != kind) continue;
            for (const auto &[format, method] : runs) {
                if (methodGiven && method != copt.method) continue;
                CompressOptions o = copt;
                o.format = format;
                o.method = method;
                o.backend = kind;
                o.discardOutput = true;
                try {
                    createArchive(pathFromUtf8("bench.tmp"), inputs, o); // warm-up: file cache, CUDA context
                    const OperationResult r = createArchive(pathFromUtf8("bench.tmp"), inputs, o);
                    std::printf("%-4s %-8s %-7s %8.2f GiB/s  ratio %5.1f%%  (%s)\n", label(kind),
                                methodName(method), formatName(format),
                                r.seconds > 0 ? double(r.inputBytes) / r.seconds / double(1u << 30) : 0,
                                r.inputBytes ? 100.0 * double(r.outputBytes) / double(r.inputBytes) : 0, r.backend.c_str());
                } catch (const std::exception &e) {
                    std::printf("%-4s %-8s skipped: %s\n", label(kind), methodName(method), e.what());
                }
            }
        }
        return 0;
    }
    if (positional.empty()) throw Error("missing archive path");
    const fs::path archive = pathFromUtf8(positional[0]);

    if (command == "a") {
        if (positional.size() < 2) throw Error("nothing to add");
        std::string ext = utf8FromPath(archive.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        copt.format = ext == ".zip" ? Format::Zip : Format::SevenZip;
        if (!methodGiven) copt.method = copt.format == Format::Zip ? Method::Deflate : Method::LZ4;
        std::vector<fs::path> inputs;
        for (std::size_t i = 1; i < positional.size(); ++i) inputs.push_back(pathFromUtf8(positional[i]));
        ConsoleProgress progress("Compressing");
        const OperationResult r = createArchive(archive, inputs, copt, std::ref(progress));
        const double ratio = r.inputBytes ? 100.0 * double(r.outputBytes) / double(r.inputBytes) : 0;
        std::printf("%s  %llu files  %s -> %s (%.1f%%)  %.2f s  %s/s\n  method %s, %s\n", utf8FromPath(archive).c_str(),
                    static_cast<unsigned long long>(r.files), formatBytes(double(r.inputBytes)).c_str(),
                    formatBytes(double(r.outputBytes)).c_str(), ratio, r.seconds,
                    formatBytes(r.seconds > 0 ? double(r.inputBytes) / r.seconds : 0).c_str(), methodName(copt.method),
                    r.backend.c_str());
        return printWarnings(r);
    }
    if (command == "x" || command == "t") {
        xopt.testOnly = command == "t";
        for (std::size_t i = 1; i < positional.size(); ++i) xopt.selection.push_back(positional[i]);
        const fs::path dest = outDir.empty() ? fs::current_path() : pathFromUtf8(outDir);
        ConsoleProgress progress(xopt.testOnly ? "Testing" : "Extracting");
        const OperationResult r = extractArchive(archive, dest, xopt, std::ref(progress));
        std::printf("%s  %llu files  %s  %.2f s  %s/s  (%s)\n", xopt.testOnly ? (r.warnings.empty() ? "OK" : "ERRORS") : "Extracted",
                    static_cast<unsigned long long>(r.files), formatBytes(double(r.inputBytes)).c_str(), r.seconds,
                    formatBytes(r.seconds > 0 ? double(r.inputBytes) / r.seconds : 0).c_str(), r.backend.c_str());
        return printWarnings(r);
    }
    if (command == "l") {
        const ArchiveListing listing = listArchive(archive);
        std::printf("Format: %s   Methods: %s%s   Size: %s\n\n", formatName(listing.format), listing.methods.c_str(),
                    listing.solid ? " (solid)" : "", formatBytes(double(listing.archiveSize)).c_str());
        std::printf("%14s %14s  %-8s %s\n", "Size", "Packed", "CRC", "Name");
        std::uint64_t total = 0;
        for (const Entry &e : listing.entries) {
            char crc[16] = "";
            if (e.hasCrc && !e.directory) std::snprintf(crc, sizeof(crc), "%08X", e.crc);
            std::printf("%14llu %14s  %-8s %s%s\n", static_cast<unsigned long long>(e.size),
                        e.packedSize ? std::to_string(e.packedSize).c_str() : "", crc, e.path.c_str(), e.directory ? "/" : "");
            total += e.size;
        }
        std::printf("\n%zu entries, %s\n", listing.entries.size(), formatBytes(double(total)).c_str());
        return 0;
    }
    throw Error("unknown command: " + command);
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(fs::path(argv[i]).u8string());
#else
int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    try {
        return run(args);
    } catch (const Cancelled &) {
        std::cerr << "Cancelled.\n";
        return 3;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
