# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Loading a repo script as an importable module, in one place (#1265).

The tools here are SCRIPTS, not an installed package, so anything wanting to reuse one has to load
it by path. Twenty-three places did: 21 pytest files, `code_stats.py` (which imports docs_drift so
the release notes and the drift gate count the same things), and `gpu_contention/driver.py`.

The two tool-side copies had already micro-diverged — one registered the module in `sys.modules`
before executing it and one did not — and that difference is not cosmetic:

  * `@dataclass` resolves its annotations through `sys.modules`, so a module loaded by path alone
    cannot define one. `docs_drift.py` does, which is why `code_stats.py`'s copy grew the
    registration and the other did not need it yet.
  * a script that cross-imports another script by name (gen_terrain_color -> gen_terrain_tiles)
    resolves through `sys.modules` too, and without registration it would load a SECOND copy.

Registering is the careful behaviour, so it is what this does, always.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from types import ModuleType


def load_script_module(module_name: str, path: str | Path, *, reload: bool = False) -> ModuleType:
    """Load the script at `path` as `module_name` and return it.

    The module is registered in `sys.modules` BEFORE it executes, which is what makes dataclass
    annotations and cross-script imports resolve.

    ⚠ ALREADY-LOADED WINS, like a real `import`. If `module_name` is in `sys.modules` this returns
    that module instead of executing a second copy — because a second copy is not a harmless
    duplicate. `gen_terrain_color` re-exports `gen_terrain_tiles.bbox_tiles`, and
    `tests/test_gen_terrain_color.py` asserts the re-export IS the shared function; loading a second
    `gen_terrain_tiles` breaks that identity, and would equally break `isinstance` against any class
    a tool defines. Pass `reload=True` for the rare caller that genuinely wants a fresh execution.
    """
    if not reload:
        cached = sys.modules.get(module_name)
        if cached is not None:
            return cached
    spec = importlib.util.spec_from_file_location(module_name, str(path))
    if spec is None or spec.loader is None:  # pragma: no cover - packaging edge
        raise ImportError(f"cannot load {path} as {module_name}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module
