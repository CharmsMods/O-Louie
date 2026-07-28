# Third-Party Notices

This file identifies third-party software used by or alongside O'Louie and records publication and packaging follow-up. It is not a project license and does not grant rights to O'Louie itself.

## Dear ImGui

- Project: Dear ImGui
- Version: `1.92.8`
- Upstream: <https://github.com/ocornut/imgui>
- Files used: core Dear ImGui sources plus the official Win32 and DirectX 11 backends
- License: MIT

Dear ImGui is downloaded into the ignored `_deps/imgui` development tree and compiled into O'Louie. Source or binary distributions that include it must retain its copyright and permission notice. The pinned upstream notice is reproduced below so it can be carried into future release packaging:

```text
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The acquisition URL, archive hash, and local layout are recorded in `docs/dear_imgui_provenance.md`.

## FFmpeg

- Project: FFmpeg
- Pinned helper version: `8.1.2`
- Upstream: <https://ffmpeg.org/>
- Libraries linked when enabled: `libavformat`, `libavcodec`, `libavutil`, and `libswresample`
- Linkage: dynamic Windows DLLs
- Upstream licensing references:
  - <https://ffmpeg.org/legal.html>
  - <https://ffmpeg.org/doxygen/trunk/md_LICENSE.html>
  - <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

FFmpeg is not vendored in the public source tree. `tools/BuildFfmpegLgplFromSource.ps1` pins official source and is configured to disable GPL, nonfree, and version3 components while building shared libraries. CMake can also consume an externally supplied FFmpeg root, so the actual license and enabled-component inventory must be verified from the exact binaries selected for each release; directory shape and library names are not a license audit.

The current repository does not produce a release package. Before distributing any FFmpeg-enabled binary, review the complete upstream FFmpeg legal checklist and the exact source tree's `LICENSE.md` and `COPYING.*` files. At minimum, the release process still needs to:

- preserve the applicable FFmpeg copyright and license notices;
- retain the exact configure flags, source version, modifications, and build instructions;
- provide the exact corresponding FFmpeg source in the manner required by the applicable license and upstream checklist;
- confirm that replacement of the dynamically linked libraries is not obstructed;
- review every enabled external library and optimization for terms that can change the resulting FFmpeg license; and
- ensure download pages, installer/EULA text, and any about/legal UI carry the notices required for that distribution.

This notice is deliberately not presented as proof of LGPL compliance. Patent, codec, trademark, and jurisdiction-specific distribution questions are outside this repository audit and should receive qualified legal review before release.

### Development-only FFmpeg tools

The long-session verification flow can use an independently installed `ffprobe` executable. It is an inspection tool only and is not linked into O'Louie. Do not copy that separate FFmpeg distribution into an O'Louie release without reviewing its own build configuration and license obligations.

## Microsoft Platform Dependencies

O'Louie links Windows SDK and operating-system libraries for Win32, Direct3D 11/DXGI, Windows Graphics Capture, Media Foundation, WASAPI, and related APIs. They are not vendored in this repository. If future packaging adds Microsoft redistributables or other separately shipped runtime components, their redistribution terms and required notices must be reviewed at that time.

## Release Packaging Gate

Before the first binary release:

1. Generate an inventory from the actual packaged files rather than from intended dependency settings.
2. Include this notice and the exact third-party license files required by that inventory.
3. Archive the corresponding dependency sources, hashes, configure flags, modifications, and build instructions.
4. Re-run the license review whenever dependency versions, FFmpeg configure flags, or packaged binaries change.
