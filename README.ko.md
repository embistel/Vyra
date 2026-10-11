# Vyra

**NVIDIA nvCOMP 기반 GPU 가속 압축 프로그램.** 7z / zip 압축 파일을 GPU로 만들고, 결과물은 **표준 형식 그대로**라서 다른 PC에서도 7-Zip·Windows 탐색기 등으로 바로 풀 수 있습니다. UI는 [LMDB Archiver](https://github.com/embistel/LMDBArchiver)와 같은 Qt Widgets 스타일이며, 7-Zip처럼 탐색기 오른쪽 클릭 메뉴와 통합됩니다.

[English](README.md)

![메인 창](docs/images/main-window.ko.png)

## 주요 기능

- **GPU 압축 (nvCOMP 5.3)**: Deflate · LZ4 · Zstd를 64 KiB 청크 단위로 GPU에서 일괄 압축. GPU가 없으면 같은 형식을 CPU(zlib-ng / LZ4 / Zstd, 전 코어)로 만듭니다.
- **표준 형식 출력**
  | 만든 형식 | 열 수 있는 프로그램 |
  |---|---|
  | `.zip` (Deflate / 저장) | Windows 탐색기, 7-Zip, 반디집 등 모든 ZIP 프로그램 |
  | `.7z` Deflate / 저장 | **공식 7-Zip**, NanaZip 등 |
  | `.7z` LZ4 / Zstd | NanaZip, 7-Zip-zstd, 공식 7-Zip + [코덱 플러그인](https://github.com/mcmilk/7-Zip-zstd/releases)(`Codecs\lz4.dll`, `zstd.dll`) |

  > 공식 7-Zip은 LZ4/Zstd 방식을 기본 내장하지 않습니다. "어느 PC에서나 7-Zip으로" 풀려야 한다면 **7z + Deflate** 또는 **zip**을 고르세요 (압축 대화상자에 호환성 안내가 표시됩니다).
- **해제/테스트**: Vyra가 만든 파일뿐 아니라 일반 7-Zip 압축 파일(LZMA, LZMA2, BCJ, 압축된 헤더), 일반 ZIP(Deflate, Zstd)도 풀 수 있습니다. LZ4/Zstd 7z는 블록 단위 병렬(GPU 또는 CPU)로 풉니다. 모든 파일의 CRC를 검증합니다.
- **탐색기 통합 (관리자 권한 불필요)**: 파일·폴더 우클릭 → *Vyra ▸ 압축하기… / 7z로 빠르게 압축 (GPU) / ZIP으로 압축*, `.7z`·`.zip` 우클릭 → *Vyra 압축 풀기 ▸ Vyra로 열기 / 여기에 풀기 / 폴더를 만들어 풀기 / 압축 풀기… / 테스트*. 여러 파일을 선택해도 하나의 압축 파일로 묶습니다.
- **스마트 "여기에 풀기"**: 최상위 항목이 하나면 그 자리에, 여러 개면 압축 파일 이름의 폴더를 만들어 풉니다.
- 드래그 앤 드롭(넣으면 압축/열기, 끌어내면 추출), 진행률 창(속도·남은 시간·압축률·엔진), 한국어/영어 UI, CLI(`vyra-cli`).

![압축 대화상자](docs/images/compress-dialog.ko.png)

## 동작 원리

```text
파일 읽기(병렬 I/O) ─▶ 128 MiB 배치 ─▶ GPU: 업로드 → nvCOMP 청크 압축 → GPU에서 패킹 → 다운로드 ─▶ 순서대로 기록
                                       └ CPU: 청크별 CRC32 (동시에)
```

- **Deflate (zip / 7z)**: nvCOMP는 64 KiB 청크마다 `BFINAL=1`인 완결된 Deflate 스트림을 냅니다. Vyra는 각 청크의 마지막 블록 끝 비트 위치를 찾아 `BFINAL`을 지우고 빈 stored 블록(sync-flush 마커, `00 00 FF FF`)으로 바이트 정렬해 이어 붙입니다. 결과는 **하나의 정상적인 Deflate 스트림**이라 어떤 압축 해제기도 그대로 풉니다 (pigz와 같은 방식).
- **LZ4 (7z)**: nvCOMP가 만든 raw LZ4 블록을 표준 **LZ4 Frame**(독립 블록, 64 KiB 블록)으로 감쌉니다. 압축이 안 되는 블록은 비압축 블록으로 저장합니다.
- **Zstd (7z)**: 청크마다 완결된 Zstd 프레임이 이어진 스트림(Zstd 표준이 허용하는 다중 프레임)입니다.
- 7z 메서드 ID는 7-Zip-zstd가 등록한 값(LZ4 `04F71104`, Zstd `04F71101`)을 사용합니다.
- 큰 입력은 솔리드 블록(기본 4 GiB) 단위로 나눠 7z 폴더를 만들고, zip은 ZIP64를 자동 사용합니다.

## 성능 (현재 버전 실측)

테스트 PC: Xeon E5-2698 v3 (16코어/32스레드, 2014년), RTX 3090 (PCIe 3.0 x16), Windows 10.
데이터: Qt `.lib` 파일 4.0 GiB, 파일 캐시에서 읽고 출력은 버림(`vyra-cli bench`, 디스크 영향 제외).

| 방식 (수준 1) | GPU (nvCOMP) | CPU 32스레드 | CPU 8스레드 | 압축 후 크기 GPU / CPU |
|---|---|---|---|---|
| 7z LZ4 | 1.74 GiB/s | 2.13 GiB/s | 1.64 GiB/s | 31.1% / 27.6% |
| 7z Zstd | 1.71 GiB/s | 2.39 GiB/s | 1.47 GiB/s | 17.8% / 18.2% |
| zip Deflate | 1.66 GiB/s | 2.00 GiB/s | 1.15 GiB/s | 25.6% / 24.4% |

- 실제 파일로 쓰는 경우에는 이 PC의 SSD 쓰기 속도(약 0.7–1 GiB/s)가 먼저 한계에 닿습니다.
- 솔직한 현황: 이 PC처럼 코어가 많은 CPU에서는 1차 버전의 GPU 경로가 **8스레드 CPU와 비슷하고 32스레드 CPU보다 약간 느립니다**. GPU만 단독 측정하면 LZ4 3.0 GiB/s 수준이며, 차이는 PCIe 3.0 전송과 배치 사이 동기화 때문입니다(아래 로드맵 참고). GPU 경로는 CPU 사용량이 매우 적다는 장점이 있습니다.
- Zstd는 GPU가 수준과 무관하게 일정한 속도를 냅니다. 비슷한 압축률을 CPU로 내면(수준 4 → zstd 12) 0.41 GiB/s로, **GPU가 약 4배 빠릅니다**.
- nvCOMP의 고압축 Deflate(수준 3 이상)는 CPU(zlib-ng)보다 느려서, *자동* 엔진은 이 경우 CPU를 사용합니다.

## 사용법

### GUI
`Vyra.exe` 실행 → 파일/폴더를 끌어다 놓거나 **압축**. 압축 파일을 끌어다 놓거나 **열기**로 내용을 보고 풀 수 있습니다. 탐색기 메뉴는 **도구 ▸ Windows 탐색기 통합...** 에서 설치/갱신/제거합니다 (Windows 11은 "더 많은 옵션 표시" 아래).

### CLI
```powershell
vyra-cli a backup.7z D:\Photos D:\Docs          # 7z + LZ4 (GPU)
vyra-cli a backup.7z D:\Data -m zstd             # 7z + Zstd
vyra-cli a share.zip D:\Data                     # zip + Deflate (어디서나 열림)
vyra-cli a backup.7z D:\Data -m deflate          # 공식 7-Zip 호환 7z
vyra-cli x backup.7z -o D:\Restore               # 풀기 (일부만: 경로 나열)
vyra-cli t backup.7z                             # 무결성 테스트
vyra-cli l backup.7z                             # 목록
vyra-cli info                                    # GPU 상태
vyra-cli bench D:\Data                           # 디스크 제외 처리량 측정
```
옵션: `-m lz4|zstd|deflate|store`, `-l 0-5`, `--gpu`/`--cpu`, `--chunk 64k`, `--batch 128m`, `--solid 4g|off`, `-t <스레드>`, `--keep`(기존 파일 유지).

## 빌드

필요: Visual Studio 2022 (C++), CUDA Toolkit 12.x/13.x, Qt 6.5+ (`msvc2022_64`), Python 3 (nvCOMP 설치·테스트용), CMake 3.24+. NVIDIA GPU는 Volta(7.0) 이상.

```powershell
.\scripts\build.ps1 -InstallDependencies -Test   # nvCOMP 5.3을 .deps에 설치하고 빌드 + 테스트
.\scripts\build.ps1 -Test                        # 이후 빌드
.\scripts\build.ps1 -NoCuda -Test                # GPU 없이 CPU 전용 빌드
.\scripts\package.ps1                            # out\Vyra-<버전>-win64.zip (Qt, nvCOMP, VC 런타임 포함)
```

zlib-ng · LZ4 · Zstandard · LZMA SDK는 CMake가 자동으로 받아 정적으로 링크합니다. `nvcomp64_5.dll`은 지연 로드되므로 DLL이나 NVIDIA 드라이버가 없어도 프로그램은 CPU 모드로 실행됩니다.

테스트: `vyra_tests`(형식 × 방식 × GPU/CPU × 배치 크기 왕복, Deflate 결합, 경로 안전성, 취소)와 `interop_test.py`(Python zipfile, 공식 7-Zip과 양방향 호환). 자세한 구조는 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## 로드맵

- GPU 파이프라인 최적화: LZ4 프레이밍·Deflate 경계 탐색을 GPU 커널로, 배치 간 동기화 제거, 다운로드 겹치기
- **Intel / AMD 내장 그래픽 가속**: `Backend` 인터페이스에 Vulkan compute 또는 oneAPI/OpenCL 백엔드 추가
- MSI 설치 프로그램(WiX, LMDB Archiver와 동일한 구성), Windows 11 최상위 메뉴(IExplorerCommand)
- Vyra가 만든 Deflate 압축의 GPU 병렬 해제(청크 인덱스), 압축 파일 편집(추가/삭제), 암호화(AES-256), 분할 압축

## 라이선스

Vyra는 MIT 라이선스입니다. 함께 배포되는 서드파티 구성 요소(특히 NVIDIA nvCOMP 바이너리는 NVIDIA 라이선스)는 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)를 참고하세요.
