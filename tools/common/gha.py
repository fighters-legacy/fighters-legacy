# SPDX-FileCopyrightText: Contributors to Fighters Legacy
# SPDX-License-Identifier: GPL-3.0-or-later
"""GitHub Actions helpers shared by the CI gates (#1242).

The one step-summary writer. Its contract is a measurement-harness rule: a summary that cannot be
written must never change a gate's verdict, so the append is guarded and a failure is a stderr
note — never an exception into the caller. coverage_gate and scale_gate carried diverged private
copies of this; scale_gate's raised on an unwritable path, flipping the gate result over a
reporting problem.
"""

from __future__ import annotations

import os
import sys


def append_step_summary(text: str, *, echo: bool = False) -> None:
    """Append ``text`` to ``$GITHUB_STEP_SUMMARY`` when set; with ``echo``, print it to stdout too."""
    if echo:
        print(text)
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(text + "\n")
    except OSError as exc:  # a summary that cannot be written must not change the verdict
        print(f"note: could not write GITHUB_STEP_SUMMARY: {exc}", file=sys.stderr)
