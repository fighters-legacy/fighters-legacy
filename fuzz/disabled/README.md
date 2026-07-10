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

## `fuzz_ogg.cpp` — OGG Vorbis decode (stb_vorbis)

**Why disabled.** Content-pack audio is attacker-controlled, so `fl::decodeOgg` / `fl::openOggStream`
(both thin wrappers over vendored `stb_vorbis`) are a genuine attack surface. But `stb_vorbis` is a
*trusted-input* decoder and is not memory-safe on malformed streams. A 60-second smoke run reproduces:

- **SEGV during cleanup** — `stb_vorbis.c: setup_free` ← `vorbis_deinit` ← `stb_vorbis_open_memory`
  ← `stb_vorbis_decode_memory` (a null/garbage pointer dereferenced while tearing down a
  partially-initialized decoder on a malformed stream).
- **Integer-overflow-driven wild allocation** — `stb_vorbis.c: setup_malloc` ← `start_decoder`
  computes a near-`SIZE_MAX` size from an attacker-controlled count.
- **Temp-buffer leak** — `stb_vorbis.c: setup_temp_malloc` ← `start_decoder` on the setup-failure path.

None of these are fighters-legacy bugs and none are resolvable by sanitizer configuration
(`allocator_may_return_null=1` merely converts the wild allocation into a null return that
`stb_vorbis`'s own cleanup then SEGVs on).

**Follow-up.** Hardening the untrusted-audio path — sandboxing the decode, adding an OGG structural
pre-validator, or replacing `stb_vorbis` for content-pack audio — is tracked as **#723**. An upstream
report to the `stb` project (github.com/nothings/stb) accompanies it: the SEGV in
`vorbis_deinit`/`setup_free` on malformed input is the highest-value one to file, with a minimized
reproducer produced via `-minimize_crash=1`.

**Re-enabling** (once the decode path is hardened or sandboxed):

1. Move `fuzz_ogg.cpp` back to `fuzz/`.
2. In `fuzz/CMakeLists.txt`, add `fl_add_fuzzer(fuzz_ogg)` + `target_link_libraries(fuzz_ogg PRIVATE engine-audio)`.
3. In `fuzz/mint_seeds.cpp`, re-add `#include "ogg_fixture.h"` and a `mintOggSeeds()` section writing
   `fl::kMinimalOgg` to `fuzz/corpus/fuzz_ogg/seed-silence.bin` (the fixture already lives in
   `tests/ogg_fixture.h`), and call it from `main()`.
4. Rebuild the fuzz preset and confirm `ctest --preset fuzz -R fuzz_ogg` is clean.
