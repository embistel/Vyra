# 개발자 안내서

## 구조

| 경로 | 내용 |
|---|---|
| `src/core` | Qt를 쓰지 않는 C++17 코어: 백엔드, 압축 파이프라인, zip/7z 리더·라이터, 스트리밍 디코더 |
| `src/core/gpubackend.cu` | nvCOMP 배치 백엔드 (Deflate / LZ4 / Zstd, CUDA) |
| `src/cli` | `vyra-cli` (7-Zip `7z` 명령과 비슷한 a / x / t / l / info / bench) |
| `src/app` | Qt Widgets GUI, 진행률 창, 압축 대화상자, 다중 선택 수집기 |
| `src/platform` | Windows 탐색기 통합 (HKCU 레지스트리) |
| `tests` | `core_test.cpp`(왕복 테스트), `interop_test.py`(7-Zip·Python 호환), `backend_bench.cpp` |
| `scripts` | 빌드, 패키징, 아이콘 생성, 한국어 번역 채우기 |

## 백엔드 인터페이스

`core/backend.h`의 `Backend`는 독립 청크 묶음을 한 번에 압축/해제합니다.

```cpp
virtual void compress(Method, int level, const uint8_t *input, const std::vector<Span> &chunks, ChunkOutput &out);
virtual void decompress(Method, const uint8_t *input, const std::vector<Span> &chunks,
                        uint8_t *output, const std::vector<Span> &outputs, std::vector<size_t> &actualSizes);
virtual void *allocateHost(size_t);  // GPU 백엔드는 pinned 메모리
```

- `CpuBackend`: 스레드 풀에서 청크별로 zlib-ng / LZ4 / Zstd. Deflate 청크는 `Z_SYNC_FLUSH`로 끝냅니다 (`DeflateShape::SyncFlushed`).
- `GpuBackend`: 입력 업로드 → nvCOMP 배치 압축 → 크기 다운로드 → GPU에서 패킹 → 압축 바이트만 다운로드. Deflate 청크는 `BFINAL` 블록으로 끝납니다 (`DeflateShape::FinalBlock`).
- **Intel/AMD 내장 GPU 백엔드 추가 방법**: 같은 인터페이스를 Vulkan compute 또는 oneAPI/OpenCL로 구현하고 `selectBackend()`에 연결하면 파이프라인·형식 코드는 그대로 씁니다. 출력은 청크마다 표준 raw Deflate / LZ4 block / Zstd frame이면 됩니다.

## 압축 파이프라인 (`core/archive.cpp`)

1. **스캔**: 입력 경로를 순회해 항목 목록과 전체 크기를 만듭니다. 출력 파일 자신은 건너뜁니다.
2. **스트림 배정**: zip은 비어 있지 않은 파일마다 스트림 하나, 7z는 솔리드 블록 크기까지 파일을 모아 폴더 하나.
3. **리더 스레드**: 128 MiB 배치 버퍼(pinned)에 파일 조각을 배치하고 청크(기본 64 KiB)를 자릅니다. 읽기 자체는 배치를 넘길 때 I/O 풀(8스레드)에서 병렬로 수행합니다. `FILE_FLAG_SEQUENTIAL_SCAN`은 파일 캐시를 버리게 만들어 쓰지 않습니다.
4. **코덱 워커**: GPU는 CUDA 스트림 2개(워커 2개)로 배치를 겹쳐 처리하고, 동시에 CPU에서 CRC32를 계산합니다. 이후 Deflate 결합 또는 LZ4 프레이밍.
5. **라이터**: 배치 순서를 복원(sequence 번호)해 zip 로컬 헤더/데이터 또는 7z 팩 스트림을 기록하고, 끝에 중앙 디렉터리 / 7z 헤더를 씁니다. 임시 파일 `*.vyra-partial`에 쓴 뒤 이름을 바꾸므로 실패·취소 시 기존 파일이 손상되지 않습니다.

버퍼(`ByteBuffer`, `HostBuffer`)와 배치 객체는 재사용하며 0으로 채우지 않습니다. 수백 MiB 단위에서는 초기화와 페이지 폴트가 눈에 띄게 느리기 때문입니다.

### Deflate 결합 (`core/deflatestream.cpp`)

nvCOMP의 청크는 각각 완결된 raw Deflate 스트림입니다. ZIP 항목과 7z Deflate 폴더는 하나의 스트림이어야 하므로:

1. `scanDeflate()`가 출력을 만들지 않고 블록을 따라가며(허프만 테이블 + 긴 코드 fallback) 마지막 블록의 `BFINAL` 비트 위치와 EOB 끝 비트 위치를 찾습니다.
2. 스트림의 마지막 청크가 아니면 `BFINAL`을 지우고, 끝 바이트의 패딩 비트를 0으로 만든 뒤 빈 stored 블록을 붙입니다. 헤더 3비트가 남은 패딩에 들어가면 `00 00 FF FF`, 아니면 `00` 한 바이트를 더 붙입니다.
3. CPU 백엔드 청크는 이미 sync-flush로 끝나므로 스트림의 마지막에만 최종 빈 고정 블록 `03 00`을 붙입니다.

배치 경계에서 백엔드가 달라도(예: 스트림 앞부분은 GPU, 뒷부분은 CPU) 결과는 유효합니다.

### LZ4 / Zstd

- LZ4: 스트림 시작 청크 앞에 프레임 헤더(`FLG=0x60` 독립 블록, BD=64 KiB), 블록마다 4바이트 크기(압축이 안 되면 최상위 비트를 세우고 원본 저장), 스트림 끝에 EndMark.
- Zstd: nvCOMP/libzstd가 청크마다 완결된 프레임(콘텐츠 크기 포함)을 만듭니다.
- 해제 시 `FrameParser`가 프레임/블록 경계를 나눠 배치 단위로 병렬 해제합니다. 연결 블록 LZ4나 콘텐츠 크기가 없는 Zstd 프레임을 만나면 그 지점부터 스트리밍 디코더로 전환합니다.

## 형식 세부

- **ZIP**: UTF-8 이름(플래그 11), 크기를 아는 상태로 로컬 헤더를 쓰고 데이터 후에 CRC/크기를 덮어써서 데이터 디스크립터를 쓰지 않습니다. 4 GiB에 가까운 파일은 로컬 헤더에 ZIP64 extra를 미리 둡니다. 중앙 디렉터리에 NTFS 시간(0x000A). 읽을 때는 OEM 코드 페이지 이름(CP949 등), Info-ZIP 유니코드 경로(0x7075), ZIP64를 지원합니다.
- **7z**: 헤더는 압축하지 않은 일반 헤더(kHeader)로 씁니다. 폴더당 코더 1개, SubStreamsInfo에 파일별 크기·CRC, FilesInfo에 이름·수정 시각·속성·빈 스트림/빈 파일 비트. 읽을 때는 압축 헤더(kEncodedHeader), LZMA/LZMA2, BCJ 체인을 지원합니다.
- 메서드 ID: Copy `00`, Deflate `040108`, LZMA `030101`, LZMA2 `21`, BCJ `03030103`, LZ4 `04F71104`, Zstd `04F71101`. LZ4/Zstd 속성은 7-Zip-zstd 규약의 5바이트 `{ver major, ver minor, level, 0, 0}`.

## 해제

- ZIP: 항목별로 스레드 풀에서 병렬(큰 항목 먼저). Store / Deflate / Zstd(93).
- 7z: 작은 폴더는 병렬, 큰 폴더는 하나씩 처리하면서 내부를 배치 병렬 해제. GPU 백엔드는 잠금 프록시로 직렬화합니다.
- 경로 안전성: `..` 포함 항목은 건너뛰고, 드라이브 문자·Windows 금지 문자는 `_`로 바꿉니다 (`safeRelativePath`).

## 탐색기 통합

`HKCU\Software\Classes` 아래:

- `*\shell\Vyra`, `Directory\shell\Vyra`: `MUIVerb`, `Icon`, `SubCommands=""` + `shell\10compress|20add7z|30addzip\command`
- `SystemFileAssociations\.7z|.zip\shell\VyraExtract`: 열기/여기에 풀기/폴더에 풀기/압축 풀기.../테스트
- `.7z|.zip\OpenWithProgids\Vyra.Archive`, `Vyra.Archive`, `Applications\Vyra.exe` ("연결 프로그램" 목록에만 추가, 기본 앱은 바꾸지 않음)

정적 동사는 선택한 파일마다 프로세스를 하나씩 띄웁니다. `singleinstance.cpp`의 수집기가 첫 프로세스를 수집자로 정하고(QLockFile) 나머지 경로를 QLocalSocket으로 받은 뒤 600 ms 동안 새 요청이 없으면 하나의 작업으로 실행합니다.

## 진단 도구

- `VYRA_PROFILE=1`: 단계별 시간(읽기/CRC/압축/후처리/기록)과 GPU 업로드/커널/다운로드 시간을 stderr에 출력
- `vyra-cli bench <경로>`: 파일 캐시에서 읽고 출력은 버려 파이프라인만 측정 (AUTO/GPU/CPU)
- `vyra_backend_bench <파일> [스레드] [MiB]`: I/O 없이 백엔드만 측정
- `VYRA_SCREENSHOT=<폴더>`: 메인 창과 압축 대화상자를 PNG로 저장하고 종료. `-platform offscreen`과 `QT_QPA_FONTDIR=C:\Windows\Fonts`를 함께 쓰면 화면에 창을 띄우지 않습니다 (`platforms\qoffscreen.dll` 필요).

## 번역

UI 원문은 영어(`tr()`), 한국어는 `translations/app_ko.ts`.

```powershell
cmake --build build --target Vyra_lupdate     # 소스에서 문자열 추출
python scripts/fill_ko_translations.py         # 사전으로 번역 채우기 (누락 시 목록 출력)
cmake --build build                            # app_ko.qm 생성
```

언어 설정 `auto`(기본)는 Windows 표시 언어가 한국어면 한국어를 씁니다. Qt 기본 대화상자 버튼은 `qtbase_ko.qm`(패키지의 `translations\`)으로 번역됩니다.

## 호환성 원칙

- C++17, Qt 6.5+ 공개 API, CUDA 12/13 + nvCOMP 5.x
- 코어와 CLI는 CUDA 없이도 빌드(`VYRA_WITH_CUDA=OFF`)되며 Linux 경로도 고려해 작성
- nvCOMP DLL은 지연 로드: DLL·드라이버·GPU가 없으면 CPU 백엔드로 자동 전환
