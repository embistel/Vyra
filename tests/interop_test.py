"""Interoperability checks between vyra and other tools (stdlib only).

usage: python -I interop_test.py <path-to-vyra-cli> <work-dir>

* Vyra .zip archives are read back with Python's zipfile.
* When 7-Zip is installed, it must accept Vyra's .zip and .7z (Deflate/Store),
  and Vyra must extract archives created by 7-Zip (LZMA, LZMA2, BCJ, Deflate).
"""
import filecmp
import os
import random
import shutil
import subprocess
import sys
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
