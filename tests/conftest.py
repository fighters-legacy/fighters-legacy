# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared pytest bootstrap: one way to load a repo script under test (#1265).

Sixteen test files opened with their own four-line `spec_from_file_location` preamble, in five
spellings, and only three of them registered the module in `sys.modules` — the detail that decides
whether a dataclass in the module under test can resolve its annotations. One helper, so a new tool
test is one line and cannot get that wrong.

pytest collects this file for any test under tests/ regardless of how it is invoked, including CI's
per-file `python3 -m pytest tests/test_X.py`, and the default import mode puts tests/ on sys.path so
test modules can `from conftest import load_tool`.

The five tests that use `sys.path.insert` + a real `import` are deliberately NOT converted: they
exercise genuine import semantics (a script importing a sibling script by name), which is the thing
this helper stands in for rather than reproduces.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
from types import ModuleType

REPO_ROOT = Path(__file__).resolve().parent.parent


def _bootstrap_pyload():
    """Load tools/common/pyload.py by path — the ONE remaining by-path load in the test suite.

    Explicitly by path rather than `from tools.common.pyload import ...`, which would silently
    depend on the repo root being on sys.path and so on how pytest was invoked. This is the
    chicken-and-egg the helper cannot solve for itself; everything after it is one line.
    """
    path = REPO_ROOT / "tools" / "common" / "pyload.py"
    spec = importlib.util.spec_from_file_location("fl_pyload", path)
    if spec is None or spec.loader is None:  # pragma: no cover - packaging edge
        raise ImportError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_load_script_module = _bootstrap_pyload().load_script_module


def load_tool(module_name: str, *relpath: str, reload: bool = False) -> ModuleType:
    """Load a repo script by repo-relative path and return it as a module.

    `load_tool("docs_drift", "tools", "docs_drift.py")` — the name is what the module is registered
    as, which is also what a sibling script's `import` of it will find. An already-loaded name is
    returned as-is (see pyload); `reload=True` forces a fresh execution.
    """
    return _load_script_module(module_name, REPO_ROOT.joinpath(*relpath), reload=reload)
