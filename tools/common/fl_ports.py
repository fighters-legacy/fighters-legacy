# SPDX-FileCopyrightText: Contributors to Fighters Legacy
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ephemeral-port selection for the tools that launch an fl-server subprocess (#1056).

fl-server binds **UDP** under every transport it has -- enet6 and GameNetworkingSockets alike -- so
a TCP probe proves nothing about the port it is handed: the two protocols have separate port spaces,
and a port that is free for TCP can be busy for UDP. That is what `mission_harness_ci_smoke` hit --
`enet_host_create()` failing on a port the harness had just declared free, on a PR whose whole diff
was one line of YAML.

Probing UDP removes that mismatch. It does **not** close the race: the probe socket is shut before
the server starts, so anything else on the machine can take the port in the gap. Choosing a port
out-of-process cannot be made collision-proof, and the previous docstring claiming it was is why
nobody re-read the code. So the contract here is the honest one -- probe with the right protocol,
and RETRY on a bind failure.

Pure logic, no fl-server dependency; unit-tested in tests/test_fl_ports.py.
"""
from __future__ import annotations

import socket

# fl-server's own failure lines (server/fl-server/main.cpp). `LocalServer` matches the same two
# strings for the single-player launch path -- keep the lists in step if either side gains a case.
_BIND_FAILURE_MARKERS = ("bind failed", "network init failed")

DEFAULT_ATTEMPTS = 3


class BindFailure(RuntimeError):
    """Raised by a `with_free_port` attempt callback when the server could not bind the port.

    Distinct from any other launch failure on purpose: this is the one outcome worth retrying on a
    fresh port. Everything else propagates on the first try, because retrying a genuinely broken
    server three times only makes the CI log three times longer.
    """


def free_udp_port(host: str = "127.0.0.1") -> int:
    """Ask the OS for a free UDP port -- fl-server binds UDP, so probing TCP proves nothing.

    The port is only free at the moment of the probe; see `with_free_port` for the retry that makes
    that acceptable.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind((host, 0))
        return s.getsockname()[1]


def looks_like_bind_failure(text: str) -> bool:
    """True when fl-server's output says it failed to bind (as opposed to failing some other way)."""
    if not text:
        return False
    lowered = text.lower()
    return any(marker in lowered for marker in _BIND_FAILURE_MARKERS)


def with_free_port(attempt, attempts: int = DEFAULT_ATTEMPTS, host: str = "127.0.0.1"):
    """Call `attempt(port)` on a freshly probed UDP port, retrying while it raises `BindFailure`.

    Returns whatever `attempt` returns. The final `BindFailure` propagates once the attempts are
    spent -- a port collision that survives three independent OS-assigned ports is a real problem
    (something is holding the whole ephemeral range), not a flake to swallow.
    """
    if attempts < 1:
        raise ValueError(f"attempts must be >= 1, got {attempts}")
    for remaining in range(attempts - 1, -1, -1):
        try:
            return attempt(free_udp_port(host))
        except BindFailure as e:
            if remaining == 0:
                raise BindFailure(
                    f"bind failed on {attempts} independently probed free UDP port(s): {e}") from e
