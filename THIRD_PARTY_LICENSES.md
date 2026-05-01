# Third-Party Licenses

This software is distributed with the following third-party components.

---

## FFmpeg

- **License:** GNU Lesser General Public License v2.1 or later (LGPL-2.1+)
- **Source:** <https://ffmpeg.org/>
- **Build:** [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) (LGPL shared variant)

The distribution package includes the following FFmpeg shared libraries:

- `avcodec-62.dll`
- `avformat-62.dll`
- `avutil-60.dll`
- `swscale-9.dll`
- `swresample-6.dll`
- `avfilter-11.dll`
- `avdevice-62.dll`

These DLLs are built **without** GPL-only components (no libx264, libx265, etc.).  
The FFmpeg source code is available at https://ffmpeg.org/download.html.  
A copy of the LGPL-2.1 license is included in the FFmpeg distribution (`LICENSE.txt`).

Under LGPL-2.1, you may use and redistribute these libraries provided you:

1. Preserve the copyright notices and license text.
2. Make the LGPL library source available (or point to the upstream source).
3. Allow users to replace the LGPL library with a modified version.

---

## spdlog

- **License:** MIT
- **Source:** <https://github.com/gabime/spdlog>

---

## nlohmann/json

- **License:** MIT  
- **Source:** <https://github.com/nlohmann/json>

---

## winpthread (mingw-w64)

- **License:** BSD 2-Clause
- **Source:** <https://sourceforge.net/projects/mingw-w64/>

The distribution package includes `libwinpthread-1.dll`, which is the POSIX
threads implementation for Windows provided by the mingw-w64 project.

This DLL is bundled to ensure the application starts correctly on machines
that do not have the MinGW-w64 runtime installed.

Copyright (c) 2011, Kaz Kojima and others (winpthread)  
Copyright (c) 2009, 2010 by Mingw-w64 project

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
