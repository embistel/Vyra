# 개발자 안내서

## 구조

| 경로 | 내용 |
|---|---|
| `src/core` | Qt를 쓰지 않는 C++17 코어: 백엔드, 압축 파이프라인, zip/7z/gz/lz4 리더·라이터, 스트리밍 디코더 |
| `src/core/gpubackend.cu` | nvCOMP 배치 백엔드 (Deflate / LZ4 / Zstd, CUDA) |
| `src/cli` | `vyra-cli` (7-Zip `7z` 명령과 비슷한 a / x / t / l / info / bench, 파일별 gz / lz4) |
| `src/app` | Qt Widgets GUI, 진행률 창, 압축 대화상자, 다중 선택 수집기 |
| `src/platform` | Windows 탐색기 통합 (HKCU 레지스트리) |
| `tests` | `core_test.cpp`(왕복 테스트), `interop_test.py`(7-Zip·Python 호환, 엄격한 LZ4 프레임 디코더), `backend_bench.cpp` |
| `scripts` | 빌드, 패키징, 아이콘 생성, 한국어 번역 채우기 |

## 백엔드 인터페이스

`core/backend.h`의 `Backend`는 독립 청크 묶음을 한 번에 압축/해제합니다.

```cpp
virtual void compress(Method, int level, const uint8_t *input, const std::vector<Span> &chunks, ChunkOutput &out);
virtual void compressChecked(...);  // + 입력 범위 CRC32 (CPU는 청크마다 CRC와 압축을 한 번에)
virtual bool compressFramed(...);   // + 컨테이너 프레이밍과 CRC를 백엔드 안에서 (GPU)
virtual std::size_t prepareCompress(Method, int level, size_t chunk, size_t batch); // 작업 전 할당
virtual int concurrency() const;    // 동시에 받을 수 있는 호출 수 (GPU 슬롯 수)
virtual bool registerHost(void *, size_t); // 이미 쓴 메모리를 page-lock (GPU)
```

- `CpuBackend`: 스레드 풀에서 청크별로 zlib-ng / LZ4 / Zstd. Deflate 청크는 `Z_SYNC_FLUSH`로 끝냅니다 (`DeflateShape::SyncFlushed`). 출력은 최악 크기 슬롯에 그대로 두고(`ChunkOutput::sizes`) 이어 붙이지 않습니다. 소비하는 쪽(프레이밍, 라이터)이 어차피 복사하기 때문입니다.
- `GpuBackend`: 아래 "GPU 파이프라인" 참조. 프로세스당 장치별로 하나만 만들어 작업 사이에 CUDA 컨텍스트, 장치 버퍼, pinned 블록을 재사용합니다.
- **Intel/AMD 내장 GPU 백엔드 추가 방법**: 같은 인터페이스를 Vulkan compute 또는 oneAPI/OpenCL로 구현하고 `selectBackend()`에 연결하면 파이프라인·형식 코드는 그대로 씁니다. 출력은 청크마다 표준 raw Deflate / LZ4 block / Zstd frame이면 됩니다.

## 압축 파이프라인 (`core/archive.cpp`)

1. **스캔**: 입력 경로를 순회해 항목 목록과 전체 크기를 만듭니다. 출력 파일 자신은 건너뜁니다.
2. **엔진 선택 (Auto)**: 256 MiB 미만 작업은 CPU(CUDA 시작 0.5초가 더 큼). 그 밖에는 가장 큰 파일의 앞 1 MiB를 CPU 1스레드로 압축해 `속도 × 스레드 수 × 0.7`을 GPU 경로의 실제 처리량(약 2 GiB/s, 시작·전송·기록 포함)과 비교합니다. GPU를 쓰면 CUDA 초기화를 백그라운드 스레드에서 바로 시작합니다.
3. **스트림 배정**: zip은 비어 있지 않은 파일마다 스트림 하나, 7z는 솔리드 블록 크기까지 파일을 모아 폴더 하나, gz / lz4는 파일마다 출력 파일 하나. `compressEachFile`은 각 파일 옆에 `<파일>.gz|.lz4`를 정하고, 출력이 이미 있거나 이미 `.gz`/`.tgz`/`.lz4`인 파일은 경고로 남기고 건너뜁니다(`planSingleFileOutputs`). `createArchive`에 gz / lz4를 주면 지정한 경로에 파일 하나만 받습니다.
4. **리더 스레드**: 배치(GPU 64 MiB, CPU 32 MiB)에 파일 조각을 배치하고 청크(기본 64 KiB)를 자릅니다. 읽기는 I/O 풀(8스레드)에서 4 MiB 조각으로 병렬 수행하며, 리더와 I/O 스레드는 압축보다 높은 우선순위로 돕니다(읽기가 대개 병목).
   - 입력 버퍼는 일반 페이지라 GPU 준비 전에도 읽기를 시작합니다(최대 1.5 GiB 선행). GPU가 준비되면 리더가 버퍼를 **채우기 직전에** `cudaHostRegister`합니다. 쓰는 중인 메모리를 등록하면 업로드된 데이터가 깨질 수 있습니다(실제로 겪은 버그).
   - CPU 작업에서 한 파일의 연속 구간인 배치는 일부(60%)를 복사 대신 파일 매핑으로 처리합니다. 매핑은 프로세스 페이지 테이블에서, 복사는 파일 캐시에서 직렬화되므로 섞을 때 가장 빠릅니다. 매핑한 페이지는 I/O 스레드가 미리 건드리고(`PrefetchVirtualMemory`는 작업 집합에 매핑하지 않아 효과가 없었음), 해제는 별도 스레드가 합니다(32 MiB당 약 10 ms).
5. **코덱 워커**: GPU는 슬롯 수만큼, CPU는 2개. GPU 작업에서는 CPU 워커가 즉시 시작해 CUDA 초기화 동안 압축하고(하이브리드), GPU 워커가 준비되면 합류합니다. 엔진이 섞여도 출력은 유효합니다(LZ4 블록 독립, Deflate 바이트 정렬 비종결 블록, Zstd 다중 프레임).
6. **후처리 단계**: CPU 배치의 Deflate 결합 / LZ4 프레이밍. GPU 배치는 이미 장치에서 프레이밍되어 그대로 지나갑니다.
7. **라이터**: 배치 순서를 복원(sequence 번호)해 zip 로컬 헤더/데이터 또는 7z 팩 스트림을 기록합니다. 출력은 직접 I/O(`FILE_FLAG_NO_BUFFERING`, 16 MiB 정렬 블록을 쓰기 스레드 4개가 병렬 기록)로 씁니다. NVMe에서 캐시 경유 쓰기(0.85 GB/s)의 약 3배입니다. 헤더 패치(`writeAt`)는 닫을 때 일반 핸들로 적용합니다. 임시 파일 `*.vyra-partial`에 쓴 뒤 이름을 바꾸므로 실패·취소 시 기존 파일이 손상되지 않습니다. gz / lz4는 `writerLoopPerFile`이 파일마다 같은 방식(임시 파일 → 헤더, 데이터, 트레일러 → 이름 변경, 수정 시각 복사)으로 씁니다. 취소·실패 시 이미 끝난 출력은 남고(gzip과 같음), 쓰던 파일의 임시 파일은 지웁니다.

CLI는 작업과 출력이 끝나면 `TerminateProcess`로 바로 끝냅니다. CUDA 컨텍스트, page-locked 메모리, DLL 정리를 건너뛰어 실행당 수백 ms를 아낍니다(`VYRA_CLEAN_EXIT`이면 정상 종료, 프로파일러용).

## GPU 파이프라인 (`core/gpubackend.cu`)

- **슬롯**: 배치 하나에 필요한 장치 버퍼(입력, nvCOMP 출력, 패킹 결과, 임시, 메타데이터)와 pinned 메타데이터 블록, 이벤트. 장치 여유 메모리에 맞춰 최대 4개를 쓰고, 하나도 안 들어가면 Auto는 CPU로 갑니다.
- **스트림 3개**: 업로드, 압축, 다운로드. 코덱 스레드마다 슬롯 하나를 빌려 업로드 → 압축(nvCOMP) → CRC32(`nvcompBatchedCRC32Async`) → Deflate 끝 비트 찾기 → 크기·오프셋 prefix sum → 패킹/프레이밍을 모두 장치에서 실행합니다. 호스트는 결과 블록 한 번만 받습니다.
- **패킹**: 결과가 호출자의 pinned 출력 버퍼에 들어가면 패킹 커널이 다운로드 스트림에서 PCIe로 직접 씁니다(zero-copy). 압축 스트림은 그동안 다음 배치를 처리하고, 복사 엔진 대기열(업로드 뒤 줄서기)도 거치지 않습니다.
- **Deflate 끝 비트**: nvCOMP 청크는 정확히 `ceil(끝비트/8)` 바이트입니다. 동적 블록 헤더에서 EOB 부호를 구한 뒤 마지막 바이트의 후보 8곳 중 "EOB 부호 + 0 패딩"에 맞는 곳이 정확히 하나면 그 위치를 씁니다. 아니면 전체 허프만 순회. `VYRA_VERIFY_DEFLATE_SCAN=1`이면 모든 결과를 전체 순회로 교차 검증합니다(테스트는 항상 켬).
- 입력 버퍼는 업로드가 끝나는 즉시 호출자에게 돌려줍니다(`CompressExtras::inputReleased`).

GPU 관련 실측 메모 (Windows, GeForce = WDDM 드라이버, HAGS 꺼짐):

- `cudaFree`/`cudaFreeHost`는 장치 전체를 동기화합니다. 작업 전에 `prepareCompress`로 모두 할당하고, 실행 중 할당은 0이어야 합니다(`VYRA_PROFILE`이 개수를 출력).
- WDDM은 명령을 모아서 제출합니다. 큐에 넣은 직후 `cudaStreamQuery`로 즉시 제출시킵니다.
- `cudaSetDevice()`가 다른 스레드의 `cudaStreamSynchronize()`가 끝날 때까지 대기합니다. 장치 선택은 스레드당 한 번만 하고, 대기는 blocking-sync 이벤트로 합니다.
- CUDA 시작(cuInit + 컨텍스트) 약 0.45초, `cudaMallocHost` 384 MiB 약 0.2초, 이미 쓴 384 MiB의 `cudaHostRegister`는 7 ms. 파일 매핑 뷰는 등록할 수 없습니다.
- 작은 배치(64 MiB = 청크 1,024개)에서는 nvCOMP LZ4 커널이 GPU를 다 채우지 못합니다(5 GB/s, 256 MiB에서는 7.8 GB/s). 대신 pinned 메모리와 시작·종료 비용이 줄어 실제 실행에서는 64 MiB가 낫습니다.

버퍼(`ByteBuffer`, `HostBuffer`)와 배치 객체는 재사용하며 0으로 채우지 않습니다. 수백 MiB 단위에서는 초기화와 페이지 폴트가 눈에 띄게 느리기 때문입니다.

### Deflate 결합 (`core/deflatestream.cpp`)

nvCOMP의 청크는 각각 완결된 raw Deflate 스트림입니다. ZIP 항목과 7z Deflate 폴더는 하나의 스트림이어야 하므로:

1. `scanDeflate()`가 출력을 만들지 않고 블록을 따라가며(허프만 테이블 + 긴 코드 fallback) 마지막 블록의 `BFINAL` 비트 위치와 EOB 끝 비트 위치를 찾습니다.
2. 스트림의 마지막 청크가 아니면 `BFINAL`을 지우고, 끝 바이트의 패딩 비트를 0으로 만든 뒤 빈 stored 블록을 붙입니다. 헤더 3비트가 남은 패딩에 들어가면 `00 00 FF FF`, 아니면 `00` 한 바이트를 더 붙입니다.
3. CPU 백엔드 청크는 이미 sync-flush로 끝나므로 스트림의 마지막에만 최종 빈 고정 블록 `03 00`을 붙입니다.

배치 경계에서 백엔드가 달라도(예: 스트림 앞부분은 GPU, 뒷부분은 CPU) 결과는 유효합니다.

### LZ4 / Zstd

- LZ4: 스트림 시작 청크 앞에 프레임 헤더(`FLG=0x60` 독립 블록, BD=64 KiB), 블록마다 4바이트 크기(압축이 안 되면 최상위 비트를 세우고 원본 저장), 스트림 끝에 EndMark. `.lz4` 파일은 라이터가 콘텐츠 크기를 담은 헤더(`FLG=0x68`, BD는 청크 크기에 맞는 64 KiB / 256 KiB / 1 MiB / 4 MiB)를 직접 씁니다.
- Zstd: nvCOMP/libzstd가 청크마다 완결된 프레임(콘텐츠 크기 포함)을 만듭니다.
- 해제 시 `FrameParser`가 프레임/블록 경계를 나눠 배치 단위로 병렬 해제합니다. LZ4 헤더 체크섬은 직접 확인합니다. 연결 블록 LZ4, 블록/콘텐츠 체크섬이 있는 LZ4 프레임(`lz4` 도구는 기본으로 콘텐츠 체크섬을 씀), 콘텐츠 크기가 없는 Zstd 프레임을 만나면 그 지점부터 스트리밍 디코더(liblz4 / libzstd, 체크섬 검증)로 전환합니다.

## 형식 세부

- **ZIP**: UTF-8 이름(플래그 11), 크기를 아는 상태로 로컬 헤더를 쓰고 데이터 후에 CRC/크기를 덮어써서 데이터 디스크립터를 쓰지 않습니다. 4 GiB에 가까운 파일은 로컬 헤더에 ZIP64 extra를 미리 둡니다. 중앙 디렉터리에 NTFS 시간(0x000A). 읽을 때는 OEM 코드 페이지 이름(CP949 등), Info-ZIP 유니코드 경로(0x7075), ZIP64를 지원합니다.
- **7z**: 헤더는 압축하지 않은 일반 헤더(kHeader)로 씁니다. 폴더당 코더 1개, SubStreamsInfo에 파일별 크기·CRC, FilesInfo에 이름·수정 시각·속성·빈 스트림/빈 파일 비트. 읽을 때는 압축 헤더(kEncodedHeader), LZMA/LZMA2, BCJ 체인을 지원합니다.
- 메서드 ID: Copy `00`, Deflate `040108`, LZMA `030101`, LZMA2 `21`, BCJ `03030103`, LZ4 `04F71104`, Zstd `04F71101`. LZ4/Zstd 속성은 7-Zip-zstd 규약의 5바이트 `{ver major, ver minor, level, 0, 0}`.
- **gz** (RFC 1952): 멤버 하나. 헤더 `1F 8B 08`, FLG = FEXTRA(+ 이름이 ASCII면 FNAME), MTIME, XFL 0, OS 11(NTFS), extra 하위 필드 `Vy`(길이 8, 64비트 원본 크기. 4 GiB를 넘는 파일도 목록에 정확한 크기). 본문은 결합된 Deflate 스트림(빈 파일은 `03 00`), 끝에 CRC32와 ISIZE. 읽을 때는 여러 멤버(`cat a.gz b.gz`)와 끝의 0 패딩을 허용하고 멤버마다 CRC32·크기를 검증합니다. 헤더의 이름이 압축 파일 자신의 이름과 같으면(Python tarfile이 만든 `x.tgz`) 압축 파일 이름에서 확장자를 뗀 이름(`.tgz`는 `.tar`)을 씁니다. 목록의 크기·CRC는 단일 멤버 파일 기준입니다.
- **lz4**: LZ4 프레임 하나(독립 블록, 콘텐츠 크기, 체크섬 없음). 이름은 저장하지 않으므로 압축 파일 이름에서 `.lz4`를 뗀 이름으로 풉니다.

## 해제

- ZIP: 항목별로 스레드 풀에서 병렬(큰 항목 먼저). Store / Deflate / Zstd(93).
- 7z: 작은 폴더는 병렬, 큰 폴더는 하나씩 처리하면서 내부를 배치 병렬 해제. GPU 백엔드는 잠금 프록시로 직렬화합니다.
- gz: zlib-ng inflate로 직렬(멤버 경계를 모르면 나눌 수 없음). lz4: 256 MiB 이상이면 GPU(없으면 CPU) 배치 병렬, 작으면 CPU 병렬.
- 경로 안전성: `..` 포함 항목은 건너뛰고, 드라이브 문자·Windows 금지 문자는 `_`로 바꿉니다 (`safeRelativePath`).

## 탐색기 통합

`HKCU\Software\Classes` 아래:

- `*\shell\Vyra`, `Directory\shell\Vyra`: `MUIVerb`, `Icon`, `SubCommands=""` + `shell\10compress|20add7z|30addzip|40addgz|50addlz4\command`
- `SystemFileAssociations\.7z|.zip|.gz|.tgz|.lz4\shell\VyraExtract`: 열기/여기에 풀기/폴더에 풀기/압축 풀기.../테스트
- `.7z|.zip|.gz|.tgz|.lz4\OpenWithProgids\Vyra.Archive`, `Vyra.Archive`, `Applications\Vyra.exe` ("연결 프로그램" 목록에만 추가, 기본 앱은 바꾸지 않음)
- `*\shell\Vyra`의 `VyraLayout` 값이 메뉴 구성의 버전입니다(현재 2). 실행한 exe가 등록된 exe와 같고 값이 다르면(이전 버전 설치) 다시 설치합니다. 작업을 맡는 프로세스(GUI, 또는 다중 선택 수집이 끝난 수집자)만 하므로, 다중 선택으로 프로세스가 여러 개 떠도 한 번만 씁니다. 다른 위치의 Vyra(예: 빌드 폴더)는 남의 통합을 바꾸지 않습니다. 동사·형식을 바꾸면 `kLayout`을 올리세요.

정적 동사는 선택한 파일마다 프로세스를 하나씩 띄웁니다. `singleinstance.cpp`의 수집기가 첫 프로세스를 수집자로 정하고(QLockFile) 나머지 경로를 QLocalSocket으로 받은 뒤 600 ms 동안 새 요청이 없으면 하나의 작업으로 실행합니다.

## 진단 도구

- `VYRA_PROFILE=1`: 프로세스 시작 기준 시각표, 단계별 시간(읽기/압축/후처리/기록), Auto 판단, GPU 슬롯·호출 통계를 stderr에 출력. `VYRA_PROFILE=2`는 배치별·GPU 호출별 타임라인도 출력
- `VYRA_AUTO=cpu|gpu`: Auto 엔진 판단을 고정, `VYRA_HYBRID=0..3`: 하이브리드 모드(0 끔, 1 GPU 준비 전까지만 CPU, 2 기본, 3 테스트용 혼합)
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
