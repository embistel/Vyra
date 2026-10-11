# Vyra

**GPU accelerated archiver built on NVIDIA nvCOMP.** Vyra creates 7z and zip archives on the GPU while keeping the output in **standard formats**, so other PCs open them with 7-Zip, Windows Explorer and friends. The UI follows [LMDB Archiver](https://github.com/embistel/LMDBArchiver) (Qt Widgets), and it integrates with the Explorer context menu like 7-Zip.

[한국어](README.ko.md)

![Main window](docs/images/main-window.png)

## Features

- **GPU compression (nvCOMP 5.3)**: Deflate, LZ4 and Zstd in batches of 64 KiB chunks. Without a GPU the same formats are produced on the CPU (zlib-ng / LZ4 / Zstd on all cores).
- **Standard output**
  | Created as | Opens in |
  |---|---|
  | `.zip` (Deflate / Store) | Windows Explorer, 7-Zip, any unzip tool |
  | `.7z` Deflate / Store | **official 7-Zip**, NanaZip, … |
  | `.7z` LZ4 / Zstd | NanaZip, 7-Zip-zstd, official 7-Zip + [codec plugins](https://github.com/mcmilk/7-Zip-zstd/releases) (`Codecs\lz4.dll`, `zstd.dll`) |

  > The official 7-Zip has no built-in LZ4/Zstd. When "any PC with 7-Zip" must open the archive, choose **7z + Deflate** or **zip** (the compress dialog explains this).
- **Extract / test**: Vyra archives plus ordinary 7-Zip archives (LZMA, LZMA2, BCJ, compressed headers) and zip files (Deflate, Zstd). LZ4/Zstd 7z blocks are decoded in parallel on the GPU or CPU. Every CRC is verified.
- **Explorer integration (no admin rights)**: files/folders → *Vyra ▸ Compress… / Fast compress to .7z (GPU) / Compress to .zip*; `.7z`/`.zip` → *Vyra Extract ▸ Open / Extract Here / Extract to Folder / Extract… / Test*. A multi-selection becomes one archive.
- **Smart Extract Here**: straight into the folder when the archive has one top-level item, otherwise into a folder named after the archive.
- Drag and drop in/out, progress window with speed / ETA / ratio / engine, English and Korean UI, command-line tool `vyra-cli`.

![Compress dialog](docs/images/compress-dialog.png)

## How it works

```text
parallel file reads ─▶ 128 MiB batches ─▶ GPU: upload → nvCOMP chunk compression → pack on GPU → download ─▶ ordered writer
                                          └ CPU: per-chunk CRC32 (concurrently)
```

- **Deflate (zip / 7z)**: nvCOMP emits a complete stream per 64 KiB chunk, ending in a `BFINAL` block. Vyra finds the end bit of each chunk's final block, clears `BFINAL` and byte-aligns with an empty stored block (a sync-flush marker, `00 00 FF FF`). The result is **one ordinary Deflate stream** that every inflater accepts (the pigz technique).
- **LZ4 (7z)**: the raw LZ4 blocks from nvCOMP are wrapped into a standard **LZ4 frame** (independent 64 KiB blocks); incompressible blocks are stored uncompressed.
- **Zstd (7z)**: a sequence of complete Zstd frames, one per chunk (multi-frame streams are part of the Zstd format).
- 7z method ids are the ones registered by 7-Zip-zstd (LZ4 `04F71104`, Zstd `04F71101`).
- Large inputs are split into solid blocks (default 4 GiB); zip switches to ZIP64 automatically.

## Performance (this version, measured)

Test PC: Xeon E5-2698 v3 (16 cores / 32 threads, 2014), RTX 3090 on PCIe 3.0 x16, Windows 10.
Data: 4.0 GiB of Qt `.lib` files read from the file cache, output discarded (`vyra-cli bench`, so the disk is not measured).

| Method (level 1) | GPU (nvCOMP) | CPU 32 threads | CPU 8 threads | Size GPU / CPU |
|---|---|---|---|---|
| 7z LZ4 | 1.74 GiB/s | 2.13 GiB/s | 1.64 GiB/s | 31.1% / 27.6% |
| 7z Zstd | 1.71 GiB/s | 2.39 GiB/s | 1.47 GiB/s | 17.8% / 18.2% |
| zip Deflate | 1.66 GiB/s | 2.00 GiB/s | 1.15 GiB/s | 25.6% / 24.4% |

- Writing real archives, this PC's SSD (about 0.7–1 GiB/s) is the limit first.
- Honest status: on a many-core CPU like this one the first GPU pipeline is **on par with 8 CPU threads and a bit slower than 32**. The GPU backend alone reaches about 3.0 GiB/s for LZ4; the gap is PCIe 3.0 transfers and synchronisation between batches (see the roadmap). The GPU path uses very little CPU.
- Zstd runs at a constant speed on the GPU. Reaching a similar ratio on the CPU (level 4 → zstd 12) gives 0.41 GiB/s, so **the GPU is about 4× faster** there.
- nvCOMP's high-ratio Deflate modes (level 3+) are slower than zlib-ng, so the *automatic* engine uses the CPU for them.

## Usage

### GUI
Run `Vyra.exe`, drop files/folders or press **Compress**. Drop an archive or press **Open** to browse and extract. Install, refresh or remove the Explorer menu from **Tools ▸ Windows Explorer Integration...** (on Windows 11 it is under "Show more options").

### CLI
```powershell
vyra-cli a backup.7z D:\Photos D:\Docs          # 7z + LZ4 (GPU)
vyra-cli a backup.7z D:\Data -m zstd             # 7z + Zstd
vyra-cli a share.zip D:\Data                     # zip + Deflate (opens everywhere)
vyra-cli a backup.7z D:\Data -m deflate          # 7z the official 7-Zip opens
vyra-cli x backup.7z -o D:\Restore               # extract (list paths for a subset)
vyra-cli t backup.7z                             # test integrity
vyra-cli l backup.7z                             # list
vyra-cli info                                    # GPU status
vyra-cli bench D:\Data                           # throughput without disk writes
```
Options: `-m lz4|zstd|deflate|store`, `-l 0-5`, `--gpu`/`--cpu`, `--chunk 64k`, `--batch 128m`, `--solid 4g|off`, `-t <threads>`, `--keep` (keep existing files).

## Build

Requirements: Visual Studio 2022 (C++), CUDA Toolkit 12.x/13.x, Qt 6.5+ (`msvc2022_64`), Python 3 (installs nvCOMP, runs tests), CMake 3.24+. GPU: NVIDIA Volta (7.0) or newer.

```powershell
.\scripts\build.ps1 -InstallDependencies -Test   # installs nvCOMP 5.3 into .deps, builds, tests
.\scripts\build.ps1 -Test                        # later builds
.\scripts\build.ps1 -NoCuda -Test                # CPU-only build, no CUDA needed
.\scripts\package.ps1                            # out\Vyra-<version>-win64.zip (Qt, nvCOMP, VC runtime)
```

zlib-ng, LZ4, Zstandard and the LZMA SDK are fetched by CMake and linked statically. `nvcomp64_5.dll` is delay-loaded: without it (or without an NVIDIA driver) Vyra runs in CPU mode.

Tests: `vyra_tests` (format × method × GPU/CPU × batch-size round trips, Deflate joining, path safety, cancellation) and `interop_test.py` (Python zipfile and the official 7-Zip, both directions). See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Roadmap

- GPU pipeline: LZ4 framing and Deflate boundary search as GPU kernels, fewer syncs per batch, overlapped downloads
- **Intel / AMD integrated GPU acceleration**: a Vulkan compute or oneAPI/OpenCL backend behind the `Backend` interface
- MSI installer (WiX, like LMDB Archiver), Windows 11 top-level menu (IExplorerCommand)
- GPU-parallel extraction of Vyra Deflate archives (chunk index), editing archives (add/delete), AES-256 encryption, split volumes

## License

Vyra is MIT licensed. Bundled third-party components (notably the NVIDIA nvCOMP binary, under NVIDIA's license) are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
