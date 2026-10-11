# Third-party notices

Vyra itself is released under the MIT License (see `LICENSE`). It uses the
components below. Their licenses apply to those components only; the MIT
License does not extend to them.

| Component | Use | License | How it is included |
|---|---|---|---|
| [NVIDIA nvCOMP](https://developer.nvidia.com/nvcomp) 5.3 | GPU compression | NVIDIA Software License Agreement (`licenses/nvCOMP-LICENSE.txt`, `licenses/nvCOMP-NOTICE.txt` in the package) | `nvcomp64_5.dll`, redistributed in binary form as part of Vyra, not in this repository |
| NVIDIA CUDA runtime | GPU runtime | NVIDIA CUDA EULA | statically linked `cudart_static` |
| [Qt](https://www.qt.io/) 6 | GUI | LGPL v3 | dynamically linked DLLs, unmodified |
| [zlib-ng](https://github.com/zlib-ng/zlib-ng) 2.2.4 | Deflate on the CPU, CRC32 | zlib License | static |
| [LZ4](https://github.com/lz4/lz4) 1.10.0 | LZ4 on the CPU | BSD 2-Clause | static |
| [Zstandard](https://github.com/facebook/zstd) 1.5.7 | Zstd on the CPU | BSD 3-Clause | static |
| [LZMA SDK](https://www.7-zip.org/sdk.html) 24.09 (`LzmaDec`, `Lzma2Dec`, `Bra`, `Bra86`, `CpuArch`) | reading ordinary 7z archives | public domain | static |

## NVIDIA nvCOMP

The nvCOMP library is distributed by NVIDIA under its own license, which
allows redistributing the binary as part of an application with material
additional functionality. It is not covered by Vyra's MIT License and may only
be used through Vyra. Building from source downloads it from PyPI
(`nvidia-libnvcomp-cu13==5.3.0.16`) into the git-ignored `.deps` folder.

## Qt

Qt is used under the GNU Lesser General Public License v3. The Qt DLLs are
shipped unmodified and can be replaced by the user. Qt source code is
available from https://download.qt.io/.

## zlib-ng

Copyright (C) 1995-2024 Jean-loup Gailly, Mark Adler and the zlib-ng
contributors. Provided 'as-is', without any express or implied warranty.

## LZ4

Copyright (c) 2011-2020, Yann Collet. All rights reserved. Redistribution and
use in source and binary forms, with or without modification, are permitted
under the BSD 2-Clause License.

## Zstandard

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.
Licensed under the BSD 3-Clause License.

## LZMA SDK

Igor Pavlov. The decoder sources used by Vyra are placed in the public domain.
