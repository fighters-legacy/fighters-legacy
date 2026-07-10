<!--
SPDX-FileCopyrightText: 2026 MKZ Systems LLC
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Disabled fuzz harnesses

Harnesses parked here are **not built and not run** by any automation. The per-PR smoke
enumerates `fuzz/fuzz_*.cpp` (a non-recursive glob) and the CMake registration lives in
`fuzz/CMakeLists.txt`; neither reaches this subdirectory. A harness lands here when it targets a
real attack surface but a **third-party dependency** has memory-safety defects on malformed input
that we can't fix in-tree and that would keep the smoke permanently red.

A parked harness must keep its `.cpp` here unchanged, get a section in this README documenting the
reproducers and the re-enable checklist, and have a tracking issue for the hardening work.

**None currently parked.** (`fuzz_ogg` lived here while the OGG decode path was backed by
stb_vorbis, a trusted-input decoder that crashes on malformed streams; #723 replaced it with
libvorbis and the harness moved back to `fuzz/`.)
