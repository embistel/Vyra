"""Interoperability checks between vyra and other tools (stdlib only).

usage: python -I interop_test.py <path-to-vyra-cli> <work-dir>

* Vyra .zip archives are read back with Python's zipfile.
* When 7-Zip is installed, it must accept Vyra's .zip and .7z (Deflate/Store),
  and Vyra must extract archives created by 7-Zip (LZMA, LZMA2, BCJ, Deflate).
* Vyra .gz files are read by Python's gzip (and 7-Zip); Vyra .lz4 files by
  the strict LZ4 frame decoder below. Vyra reads gzip / tar.gz files written
  by Python and LZ4 frames with linked blocks, checksums and several frames.
"""
import filecmp
import gzip
import io
import os
import random
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

VYRA = Path(sys.argv[1])
WORK = Path(sys.argv[2])
SEVEN_ZIP = next((p for p in [Path(os.environ.get("SEVENZIP", "")),
                              Path(r"C:\Program Files\7-Zip\7z.exe"),
                              Path(r"C:\Program Files (x86)\7-Zip\7z.exe")] if p.is_file()), None)
failures = []


def run(*args, ok=True):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if ok and result.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, args))} -> {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result


def check(name, condition, detail=""):
    print(("ok   " if condition else "FAIL ") + name)
    if not condition:
        failures.append(f"{name} {detail}")


def same_tree(a: Path, b: Path) -> bool:
    cmp = filecmp.dircmp(a, b)
    if cmp.left_only or cmp.right_only or cmp.diff_files or cmp.funny_files:
        return False
    _, mismatch, errors = filecmp.cmpfiles(a, b, cmp.common_files, shallow=False)
    if mismatch or errors:
        return False
    return all(same_tree(a / d, b / d) for d in cmp.common_dirs)


# ---- LZ4 frame format (lz4.org "LZ4 Frame Format Description"), stdlib only ----
M32 = 0xFFFFFFFF
P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393


def rotl(x, r):
    return ((x << r) | (x >> (32 - r))) & M32


def xxh32(data, seed=0):
    n, i = len(data), 0
    if n >= 16:
        v = [(seed + P1 + P2) & M32, (seed + P2) & M32, seed & M32, (seed - P1) & M32]
        while i + 16 <= n:
            for k in range(4):
                v[k] = rotl((v[k] + int.from_bytes(data[i:i + 4], "little") * P2) & M32, 13) * P1 & M32
                i += 4
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & M32
    else:
        h = (seed + P5) & M32
    h = (h + n) & M32
    while i + 4 <= n:
        h = rotl((h + int.from_bytes(data[i:i + 4], "little") * P3) & M32, 17) * P4 & M32
        i += 4
    while i < n:
        h = rotl((h + data[i] * P5) & M32, 11) * P1 & M32
        i += 1
    h ^= h >> 15
    h = h * P2 & M32
    h ^= h >> 13
    h = h * P3 & M32
    return h ^ (h >> 16)


def lz4_block(src, out, floor):
    """Decodes one LZ4 block onto `out`; matches may not reach below `floor`."""
    i, n = 0, len(src)
    while True:
        token = src[i]
        i += 1
        length = token >> 4
        if length == 15:
            while True:
                i += 1
                length += src[i - 1]
                if src[i - 1] != 255:
                    break
        if i + length > n:
            raise ValueError("literals beyond the block")
        out += src[i:i + length]
        i += length
        if i == n:
            return  # the last sequence holds literals only
        offset = src[i] | src[i + 1] << 8
        i += 2
        length = token & 15
        if length == 15:
            while True:
                i += 1
                length += src[i - 1]
                if src[i - 1] != 255:
                    break
        length += 4
        start = len(out) - offset
        if offset == 0 or start < floor:
            raise ValueError("match outside the allowed window")
        if offset >= length:
            out += out[start:start + length]
        else:
            pattern = bytes(out[start:])
            out += (pattern * (length // offset + 1))[:length]


def lz4_decode(data):
    """Strict frame decoder: header checksum, block size limit, block
    independence, block / content checksums and content size are enforced.
    Returns (content, [(FLG, block maximum) per frame])."""
    out, frames, pos = bytearray(), [], 0
    while pos < len(data):
        magic = int.from_bytes(data[pos:pos + 4], "little")
        if magic & 0xFFFFFFF0 == 0x184D2A50:  # skippable frame
            pos += 8 + int.from_bytes(data[pos + 4:pos + 8], "little")
            continue
        if magic != 0x184D2204:
            raise ValueError("not an LZ4 frame")
        flg, bd = data[pos + 4], data[pos + 5]
        if flg >> 6 != 1 or flg & 0x02 or bd & 0x8F or bd >> 4 < 4:
            raise ValueError("invalid frame descriptor")
        end = pos + 6 + (8 if flg & 0x08 else 0) + (4 if flg & 0x01 else 0)
        if data[end] != xxh32(data[pos + 4:end]) >> 8 & 0xFF:
            raise ValueError("header checksum")
        size = int.from_bytes(data[pos + 6:pos + 14], "little") if flg & 0x08 else None
        block_max = 1 << (8 + 2 * (bd >> 4))
        pos, start = end + 1, len(out)
        while True:
            word = int.from_bytes(data[pos:pos + 4], "little")
            pos += 4
            if word == 0:
                break
            n = word & 0x7FFFFFFF
            if n > block_max:
                raise ValueError("block larger than the frame's maximum")
            block = data[pos:pos + n]
            pos += n
            if flg & 0x10:
                if int.from_bytes(data[pos:pos + 4], "little") != xxh32(block):
                    raise ValueError("block checksum")
                pos += 4
            before = len(out)
            if word >> 31:
                out += block
            else:
                lz4_block(block, out, before if flg & 0x20 else start)
            if len(out) - before > block_max:
                raise ValueError("block decodes beyond the frame's maximum")
        if flg & 0x04:
            if int.from_bytes(data[pos:pos + 4], "little") != xxh32(bytes(out[start:])):
                raise ValueError("content checksum")
            pos += 4
        if size is not None and len(out) - start != size:
            raise ValueError("content size")
        frames.append((flg, block_max))
    return bytes(out), frames


def lz4_sequence(literals, offset=0, match=0):
    """One LZ4 sequence; the block's last sequence has literals only."""
    def extra(value):
        out = bytearray()
        while value >= 255:
            out.append(255)
            value -= 255
        out.append(value)
        return bytes(out)
    ml = match - 4 if match else 0
    out = bytearray([min(len(literals), 15) << 4 | min(ml, 15)])
    if len(literals) >= 15:
        out += extra(len(literals) - 15)
    out += literals
    if match:
        out += offset.to_bytes(2, "little")
        if ml >= 15:
            out += extra(ml - 15)
    return bytes(out)


def lz4_frame(blocks, block_id=4, linked=False, block_checksum=False, content_checksum=False, size=None):
    """Frame from (encoded block, raw flag, decoded content) triples."""
    flg = 0x40 | (0 if linked else 0x20) | (0x10 if block_checksum else 0) | (0x04 if content_checksum else 0)
    desc = bytes([flg | (0x08 if size is not None else 0), block_id << 4])
    if size is not None:
        desc += size.to_bytes(8, "little")
    out = bytearray(b"\x04\x22\x4d\x18" + desc + bytes([xxh32(desc) >> 8 & 0xFF]))
    for encoded, raw, _ in blocks:
        out += (len(encoded) | (0x80000000 if raw else 0)).to_bytes(4, "little") + encoded
        if block_checksum:
            out += xxh32(encoded).to_bytes(4, "little")
    out += bytes(4)
    if content_checksum:
        out += xxh32(b"".join(b[2] for b in blocks)).to_bytes(4, "little")
    return bytes(out)


def gzip_header(path: Path):
    """(FNAME, size from Vyra's 'Vy' extra subfield) of a gzip file."""
    data = path.read_bytes()
    flags, pos, name, size = data[3], 10, None, None
    if flags & 0x04:
        xlen = int.from_bytes(data[10:12], "little")
        extra, pos = data[12:12 + xlen], 12 + xlen
        while len(extra) >= 4:
            sub = int.from_bytes(extra[2:4], "little")
            if extra[:2] == b"Vy" and sub == 8:
                size = int.from_bytes(extra[4:12], "little")
            extra = extra[4 + sub:]
    if flags & 0x08:
        name = data[pos:data.index(b"\0", pos)].decode("latin-1")
    return name, size


def build_fixture(root: Path):
    rng = random.Random(42)
    words = [b"vyra ", b"gpu ", b"archive ", b"interop ", b"\n"]
    (root / "dir" / "nested").mkdir(parents=True)
    (root / "empty").mkdir()
    (root / "text.txt").write_bytes(b"".join(rng.choice(words) for _ in range(200000)))
    (root / "random.bin").write_bytes(rng.randbytes(150000))
    (root / "dir" / "nested" / "small.txt").write_bytes(b"hello vyra\n" * 10)
    (root / "dir" / "empty.txt").write_bytes(b"")
    (root / "한글.txt").write_text("한글 파일 이름 테스트\n" * 100, encoding="utf-8")


def files_of(root: Path):
    return sorted(p for p in root.rglob("*") if p.is_file())


def single_file_formats(fixture: Path):
    check("xxh32 reference values", xxh32(b"") == 0x02CC5D05 and xxh32(b"abc") == 0x32D153FF
          and xxh32(b"Nobody inspects the spammish repetition") == 0xE2293B2F)

    # 1. Vyra writes .gz / .lz4 next to every file; others read them.
    for fmt in ("gz", "lz4"):
        tree = WORK / f"each-{fmt}"
        shutil.copytree(fixture, tree)
        originals = files_of(tree)
        run(VYRA, fmt, tree)
        good = 0
        for original in originals:
            packed = original.with_name(original.name + "." + fmt)
            try:
                if fmt == "gz":
                    content = gzip.decompress(packed.read_bytes())
                    name, size = gzip_header(packed)
                    ascii_name = original.name.isascii()
                    header_ok = size == original.stat().st_size and (name == original.name if ascii_name else name is None)
                else:
                    content, frames = lz4_decode(packed.read_bytes())
                    header_ok = frames == [(0x68, 64 << 10)]  # independent blocks, content size, 64 KiB
                good += content == original.read_bytes() and header_ok
            except Exception as e:  # noqa: BLE001
                print(f"  {packed.name}: {e}")
        check(f"vyra {fmt} each file: {len(originals)} files decoded by "
              f"{'python gzip' if fmt == 'gz' else 'the strict LZ4 decoder'}", good == len(originals))
        if fmt == "gz" and SEVEN_ZIP:
            tested = sum(run(SEVEN_ZIP, "t", f"{p}.gz", ok=False).returncode == 0 for p in originals)
            check(f"7-Zip tests vyra .gz ({len(originals)} files)", tested == len(originals))

    # Explicit output with each LZ4 block size; 512 KiB chunks need the 1 MiB maximum.
    text = fixture / "text.txt"
    for chunk, block_max in (("256k", 256 << 10), ("512k", 1 << 20), ("4m", 4 << 20)):
        packed = WORK / f"chunk-{chunk}.lz4"
        run(VYRA, "a", packed, text, "--chunk", chunk)
        try:
            content, frames = lz4_decode(packed.read_bytes())
            check(f"vyra lz4 chunk {chunk}: block maximum {block_max >> 10} KiB",
                  content == text.read_bytes() and frames == [(0x68, block_max)], str(frames))
        except Exception as e:  # noqa: BLE001
            check(f"vyra lz4 chunk {chunk}", False, str(e))

    # Concatenated Vyra gzip files are one stream of both contents.
    a, b = WORK / "cat-a.gz", WORK / "cat-b.gz"
    run(VYRA, "a", a, text)
    run(VYRA, "a", b, fixture / "random.bin")
    both = WORK / "cat-ab.gz"
    both.write_bytes(a.read_bytes() + b.read_bytes())
    expected = text.read_bytes() + (fixture / "random.bin").read_bytes()
    check("python gzip reads concatenated vyra .gz", gzip.decompress(both.read_bytes()) == expected)
    out = WORK / "vyra-out-cat"
    result = run(VYRA, "x", both, "-o", out, ok=False)
    check("vyra reads concatenated vyra .gz", result.returncode == 0 and (out / "text.txt").read_bytes() == expected,
          result.stderr)

    # 2. Others write, Vyra reads.
    content = text.read_bytes()
    cases = {}
    buffer = io.BytesIO()
    with gzip.GzipFile(filename="named.txt", mode="wb", fileobj=buffer, mtime=1700000000) as g:
        g.write(content)
    cases["python.txt.gz"] = (buffer.getvalue(), "named.txt", content)
    cases["members.bin.gz"] = (gzip.compress(content[:100000]) + gzip.compress(content[100000:]),
                               "members.bin", content)
    # LZ4: linked blocks with a match into the previous block, both checksums.
    first = content[:65536]
    second = first[1:20001]
    linked = [(first, True, first), (lz4_sequence(b"", 65535, len(second) - 5) + lz4_sequence(second[-5:]), False, second)]
    cases["linked.bin.lz4"] = (lz4_frame(linked, linked=True, content_checksum=True, block_checksum=True,
                                         size=len(first) + len(second)), "linked.bin", first + second)
    # Two frames (no content size, independent blocks) around a skippable frame.
    # Literal-only blocks: 60000 bytes stay within the 64 KiB block maximum.
    blocks = [(lz4_sequence(content[i:i + 60000]), False, content[i:i + 60000]) for i in range(0, 200000, 60000)]
    plain = lz4_frame(blocks[:2]) + b"\x50\x2a\x4d\x18" + (5).to_bytes(4, "little") + b"skip!" + lz4_frame(blocks[2:])
    cases["frames.bin.lz4"] = (plain, "frames.bin", b"".join(b[2] for b in blocks))
    for name, (data, expected_name, expected) in cases.items():
        packed = WORK / name
        packed.write_bytes(data)
        out = WORK / f"vyra-out-{name}"
        result = run(VYRA, "x", packed, "-o", out, ok=False)
        produced = out / expected_name
        check(f"vyra extracts {name}", result.returncode == 0 and produced.is_file() and produced.read_bytes() == expected,
              result.stderr)
    for name in ("linked.bin.lz4", "frames.bin.lz4"):  # the test frames themselves are valid
        try:
            check(f"strict decoder accepts {name}", lz4_decode(cases[name][0])[0] == cases[name][2])
        except Exception as e:  # noqa: BLE001
            check(f"strict decoder accepts {name}", False, str(e))

    # A damaged byte under the content checksum must be reported.
    damaged = bytearray(cases["linked.bin.lz4"][0])
    damaged[30] ^= 0x55  # inside the first (stored) block
    bad = WORK / "damaged.bin.lz4"
    bad.write_bytes(bytes(damaged))
    check("vyra test reports a damaged .lz4", run(VYRA, "t", bad, ok=False).returncode != 0)

    # tar.gz written by Python's tarfile (which stores "x.tgz" as the name).
    tgz = WORK / "tarball" / "x.tgz"
    tgz.parent.mkdir()
    with tarfile.open(tgz, "w:gz") as t:
        t.add(text, "text.txt")
    result = run(VYRA, "x", tgz, "-o", tgz.parent, ok=False)
    tar = tgz.parent / "x.tar"
    ok = result.returncode == 0 and tar.is_file()
    if ok:
        with tarfile.open(tar) as t:
            ok = t.extractfile("text.txt").read() == content
    check("vyra extracts python tar.gz to x.tar", ok, result.stderr)


def main():
    if WORK.exists():
        shutil.rmtree(WORK)
    fixture = WORK / "fixture"
    fixture.mkdir(parents=True)
    build_fixture(fixture)

    # 1. Vyra writes, others read.
    zip_path = WORK / "vyra.zip"
    run(VYRA, "a", zip_path, fixture, "--cpu")
    with zipfile.ZipFile(zip_path) as z:
        check("python zipfile testzip", z.testzip() is None)
        out = WORK / "pyzip-out"
        z.extractall(out)
    check("python zipfile content", same_tree(fixture, out / "fixture"))

    if SEVEN_ZIP:
        for fmt, method in [("zip", "deflate"), ("zip", "store"), ("7z", "deflate"), ("7z", "store")]:
            archive = WORK / f"vyra-{method}.{fmt}"
            run(VYRA, "a", archive, fixture, "-m", method)
            check(f"7-Zip tests vyra {fmt}/{method}", run(SEVEN_ZIP, "t", archive, ok=False).returncode == 0)
            out = WORK / f"7z-out-{method}-{fmt}"
            run(SEVEN_ZIP, "x", f"-o{out}", "-y", archive)
            check(f"7-Zip extracts vyra {fmt}/{method}", same_tree(fixture, out / "fixture"))

        # 2. Others write, Vyra reads.
        for name, args in [("lzma2", ["-t7z", "-m0=LZMA2"]), ("lzma", ["-t7z", "-m0=LZMA"]),
                           ("bcj-lzma", ["-t7z", "-m0=BCJ", "-m1=LZMA"]), ("7z-deflate", ["-t7z", "-m0=Deflate"]),
                           ("zip-deflate", ["-tzip", "-mm=Deflate"]), ("solid-mt", ["-t7z", "-mx=5", "-ms=on", "-mmt=4"])]:
            ext = "zip" if name.startswith("zip") else "7z"
            archive = WORK / f"7zip-{name}.{ext}"
            run(SEVEN_ZIP, "a", *args, archive, fixture)
            out = WORK / f"vyra-out-{name}"
            result = run(VYRA, "x", archive, "-o", out, ok=False)
            check(f"vyra extracts 7-Zip {name}", result.returncode == 0 and same_tree(fixture, out / "fixture"),
                  result.stderr)
    else:
        print("7-Zip not found; 7-Zip interop checks skipped")

    single_file_formats(fixture)

    # Python-written zip read by Vyra.
    pyzip = WORK / "python.zip"
    with zipfile.ZipFile(pyzip, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(fixture.rglob("*")):
            z.write(path, path.relative_to(WORK).as_posix())
    out = WORK / "vyra-out-python"
    result = run(VYRA, "x", pyzip, "-o", out, ok=False)
    check("vyra extracts python zipfile", result.returncode == 0 and same_tree(fixture, out / "fixture"), result.stderr)

    if failures:
        print("\n".join(failures))
        return 1
    print("all interop tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
