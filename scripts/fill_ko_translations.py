"""Fills translations/app_ko.ts with the Korean UI strings.

Workflow after changing UI text:
  cmake --build build --target Vyra_lupdate   # refresh app_ko.ts from sources
  python scripts/fill_ko_translations.py       # fill / update translations
  cmake --build build                          # lrelease -> app_ko.qm
Strings missing here stay "unfinished" and fall back to English.
"""
import html
import re
import sys
from pathlib import Path

TS = Path(__file__).resolve().parent.parent / "translations" / "app_ko.ts"

KO = {
    # Actions / App
    "… and %1 more": "… 외 %1개",
    "Finished with %1 problem(s).": "%1개의 문제와 함께 완료되었습니다.",
    "Files already exist": "파일이 이미 있습니다",
    "%1 file(s) already exist in\n%2": "다음 위치에 이미 %1개의 파일이 있습니다.\n%2",
    "Replace all": "모두 덮어쓰기",
    "Keep existing": "기존 파일 유지",
    "Compressing": "압축 중",
    "Extracting": "압축 푸는 중",
    "Testing": "테스트 중",
    "No errors found.\n\n%1\n%2 files · %3 · %4 s\n%5": "오류가 없습니다.\n\n%1\n파일 %2개 · %3 · %4초\n%5",
    "Extract %1 to": "%1 압축 풀 위치",
    "GPU accelerated 7z / zip archiver": "GPU 가속 7z / zip 압축 프로그램",
    "Archive to open, or paths for a verb": "열 압축 파일 또는 명령에 전달할 경로",
    "Show the compression dialog for the paths": "경로에 대한 압축 대화상자 표시",
    "Compress the paths to <name>.7z": "경로를 <이름>.7z로 압축",
    "Compress the paths to <name>.zip": "경로를 <이름>.zip으로 압축",
    "Extract next to the archive": "압축 파일 옆에 풀기",
    "Extract into a folder named after the archive": "압축 파일 이름의 폴더에 풀기",
    "Choose where to extract": "풀 위치 선택",
    "Test archive integrity": "압축 파일 무결성 검사",
    "Install Windows Explorer integration": "Windows 탐색기 통합 설치",
    "Remove Windows Explorer integration": "Windows 탐색기 통합 제거",
    # ArchiveModel
    "Name": "이름",
    "Size": "크기",
    "Packed": "압축 크기",
    "Modified": "수정한 날짜",
    "Method": "방식",
    "CRC": "CRC",
    " (encrypted)": " (암호화됨)",
    " (unsupported)": " (지원 안 함)",
    # CompressDialog
    "Add to Archive": "압축 파일에 추가",
    "Create a GPU accelerated archive": "GPU 가속 압축 파일 만들기",
    "%1 items": "항목 %1개",
    "Source: %1": "원본: %1",
    "Archive:": "압축 파일:",
    "Format:": "형식:",
    "Method:": "압축 방식:",
    "0 · Fastest": "0 · 가장 빠르게",
    "1 · Fast (recommended)": "1 · 빠르게 (권장)",
    "2 · Normal": "2 · 보통",
    "3 · Normal+": "3 · 보통+",
    "4 · Good": "4 · 높음",
    "5 · Best": "5 · 최고",
    "Level:": "압축 수준:",
    "Automatic (GPU when available)": "자동 (가능하면 GPU)",
    "GPU (NVIDIA nvCOMP)": "GPU (NVIDIA nvCOMP)",
    "CPU (all cores)": "CPU (모든 코어)",
    "Engine:": "엔진:",
    "Off (one block per file)": "끔 (파일마다 블록)",
    "Solid block:": "솔리드 블록:",
    "Independent compression unit. Larger chunks compress Zstd/LZ4 slightly better; Deflate always uses 64 KiB.":
        "독립적으로 압축되는 단위입니다. 클수록 Zstd/LZ4 압축률이 조금 좋아지며, Deflate는 항상 64 KiB를 사용합니다.",
    "Chunk size:": "청크 크기:",
    "GPU ready: %1": "GPU 준비됨: %1",
    "GPU unavailable (%1) — the CPU engine will be used.": "GPU를 사용할 수 없습니다 (%1) — CPU 엔진을 사용합니다.",
    "Compress": "압축",
    "LZ4 — fastest": "LZ4 — 가장 빠름",
    "Zstd — fast, smaller": "Zstd — 빠르고 더 작게",
    "Deflate — opens in official 7-Zip": "Deflate — 공식 7-Zip에서 열림",
    "Store — no compression": "저장 — 압축 안 함",
    "Deflate — opens everywhere": "Deflate — 어디서나 열림",
    "Standard ZIP: opens in Windows Explorer, 7-Zip and every other unzip tool.":
        "표준 ZIP: Windows 탐색기, 7-Zip 등 모든 압축 프로그램에서 열 수 있습니다.",
    "Standard 7z: opens in the official 7-Zip, NanaZip and other 7z tools.":
        "표준 7z: 공식 7-Zip, NanaZip 등 7z를 지원하는 프로그램에서 열 수 있습니다.",
    "7z with %1: opens in NanaZip and 7-Zip-zstd. The official 7-Zip needs the lz4/zstd codec plugin. Pick Deflate for the widest 7z compatibility.":
        "%1 방식 7z: NanaZip과 7-Zip-zstd에서 열 수 있습니다. 공식 7-Zip은 lz4/zstd 코덱 플러그인이 필요합니다. 가장 넓은 호환성이 필요하면 Deflate를 선택하세요.",
    "ZIP archive (*.zip)": "ZIP 압축 파일 (*.zip)",
    "7z archive (*.7z)": "7z 압축 파일 (*.7z)",
    "Archive to create": "만들 압축 파일",
    "%1 already exists. Replace it?": "%1 파일이 이미 있습니다. 바꾸시겠습니까?",
    # DropView
    "Drop files or folders here to compress them\nor drop a .7z / .zip archive to open it.":
        "파일이나 폴더를 여기에 놓으면 압축합니다.\n.7z / .zip 파일을 놓으면 엽니다.",
    # MainWindow
    "Vyra — GPU accelerated archiver": "Vyra — GPU 가속 압축 프로그램",
    "Compress files to 7z / zip at GPU speed, or open an archive.": "GPU 속도로 7z / zip 압축을 만들거나 압축 파일을 여세요.",
    "Search archive...": "압축 파일 내 검색...",
    "&File": "파일(&F)",
    "&Compress Files...": "파일 압축(&C)...",
    "Compress &Folder...": "폴더 압축(&F)...",
    "&Open Archive...": "압축 파일 열기(&O)...",
    "Open": "열기",
    "E&xit": "끝내기(&X)",
    "&Archive": "압축 파일(&A)",
    "&Extract Selected...": "선택 항목 풀기(&E)...",
    "Extract": "풀기",
    "Extract &All...": "모두 풀기(&A)...",
    "Extract All": "모두 풀기",
    "&Test Archive": "압축 파일 테스트(&T)",
    "Test": "테스트",
    "&Copy Selected to Explorer": "선택 항목을 탐색기로 복사(&C)",
    "Open Selected": "선택 항목 열기",
    "&Properties": "속성(&P)",
    "Info": "정보",
    "Refresh": "새로 고침",
    "&Tools": "도구(&T)",
    "Windows Explorer Integration...": "Windows 탐색기 통합...",
    "Language": "언어",
    "&Help": "도움말(&H)",
    "About Vyra": "Vyra 정보",
    "Main Toolbar": "기본 도구 모음",
    "Drag files here to compress, or open a .7z / .zip archive.": "파일을 끌어다 놓아 압축하거나 .7z / .zip 파일을 여세요.",
    "Detecting GPU…": "GPU 확인 중…",
    "Compression runs on the GPU with NVIDIA nvCOMP.": "NVIDIA nvCOMP로 GPU에서 압축합니다.",
    "CPU mode": "CPU 모드",
    "GPU unavailable: %1": "GPU 사용 불가: %1",
    "Cannot open %1:\n%2": "%1 파일을 열 수 없습니다:\n%2",
    "%1 files · %2 → %3 (%4%) · %5 %6": "파일 %1개 · %2 → %3 (%4%) · %5 %6",
    "Created %1": "%1 생성됨",
    "Files to compress": "압축할 파일",
    "Folder to compress": "압축할 폴더",
    "Open Archive": "압축 파일 열기",
    "Archives (*.7z *.zip);;All files (*.*)": "압축 파일 (*.7z *.zip);;모든 파일 (*.*)",
    "Could not create a temporary export folder.": "임시 내보내기 폴더를 만들 수 없습니다.",
    "Copied %1 item(s). Paste in Explorer.": "항목 %1개를 복사했습니다. 탐색기에 붙여넣으세요.",
    "Could not open the file with the associated program.": "연결된 프로그램으로 파일을 열 수 없습니다.",
    "Folder to extract the selection to": "선택 항목을 풀 폴더",
    "Extracted the selection.": "선택 항목을 풀었습니다.",
    "Folder to extract everything to": "전체를 풀 폴더",
    "Extracted to %1": "%1에 풀었습니다",
    "Archive Properties": "압축 파일 속성",
    "<b>%1</b><br><br>Format: %2<br>Methods: %3%4<br>Files: %5 · Folders: %6<br>Original size: %7<br>Archive size: %8 (%9%)":
        "<b>%1</b><br><br>형식: %2<br>방식: %3%4<br>파일: %5개 · 폴더: %6개<br>원본 크기: %7<br>압축 크기: %8 (%9%)",
    " (solid)": " (솔리드)",
    "Windows Explorer Integration": "Windows 탐색기 통합",
    "Explorer integration is installed system-wide by the installer.\nTo change or remove it, use Windows Installed apps.":
        "탐색기 통합이 설치 프로그램에 의해 시스템 전체에 설치되어 있습니다.\n변경하거나 제거하려면 Windows의 설치된 앱을 사용하세요.",
    "Explorer integration is installed.\n\nYes: refresh it (e.g. after moving Vyra or changing the language)\nNo: remove it":
        "탐색기 통합이 설치되어 있습니다.\n\n예: 다시 등록 (Vyra를 옮겼거나 언어를 바꾼 경우)\n아니요: 제거",
    "Done": "완료",
    "Explorer integration updated.": "탐색기 통합을 갱신했습니다.",
    "Explorer integration removed.": "탐색기 통합을 제거했습니다.",
    "Add Vyra to the Explorer right-click menu for the current user?\n\n• Files and folders: Compress…, Fast compress to .7z, Compress to .zip\n• .7z / .zip files: Open, Extract Here, Extract to Folder, Test\n\nNo administrator rights are needed. On Windows 11 the entries appear under \"Show more options\".":
        "현재 사용자의 탐색기 오른쪽 클릭 메뉴에 Vyra를 추가할까요?\n\n• 파일·폴더: 압축하기…, 7z로 빠르게 압축, ZIP으로 압축\n• .7z / .zip 파일: 열기, 여기에 풀기, 폴더를 만들어 풀기, 테스트\n\n관리자 권한이 필요 없습니다. Windows 11에서는 \"더 많은 옵션 표시\" 아래에 나타납니다.",
    "Explorer integration installed.": "탐색기 통합을 설치했습니다.",
    "The language will change after restarting the app. Restart now?": "앱을 다시 시작하면 언어가 바뀝니다. 지금 다시 시작할까요?",
    "<h2>Vyra %1</h2><p>GPU accelerated 7z / zip archiver built on NVIDIA nvCOMP.</p><p>Engine: %2</p><p>Qt %3 · zlib-ng · LZ4 · Zstandard · LZMA SDK</p><p>MIT License</p>":
        "<h2>Vyra %1</h2><p>NVIDIA nvCOMP 기반 GPU 가속 7z / zip 압축 프로그램</p><p>엔진: %2</p><p>Qt %3 · zlib-ng · LZ4 · Zstandard · LZMA SDK</p><p>MIT 라이선스</p>",
    # ProgressDialog
    "Scanning files…": "파일 확인 중…",
    "Compressing…": "압축 중…",
    "Extracting…": "압축 푸는 중…",
    "Testing…": "테스트 중…",
    "Finishing…": "마무리 중…",
    "Processed:": "처리량:",
    "Speed:": "속도:",
    "Elapsed / remaining:": "경과 / 남은 시간:",
    "Ratio:": "압축률:",
    "Cancel": "취소",
    "Cancelling…": "취소하는 중…",
    "%1 files · %2": "파일 %1개 · %2",
    "%1 / %2 · %3 / %4 files": "%1 / %2 · 파일 %3 / %4개",
}


def main() -> int:
    text = TS.read_text(encoding="utf-8")
    missing = []

    def fill(match: re.Match) -> str:
        source = html.unescape(match.group(2))
        ko = KO.get(source)
        if ko is None:
            missing.append(source)
            return match.group(0)
        return f"{match.group(1)}<translation>{html.escape(ko, quote=False)}</translation>"

    pattern = re.compile(r"(<source>(.*?)</source>(?:\s*<comment>.*?</comment>)?\s*)<translation[^>]*?(?:/>|>.*?</translation>)", re.S)
    text = pattern.sub(fill, text)
    TS.write_text(text, encoding="utf-8", newline="\n")
    for m in missing:
        print("untranslated:", repr(m))
    print(f"filled {len(KO)} entries, {len(missing)} missing")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
