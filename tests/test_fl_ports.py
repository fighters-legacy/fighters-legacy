# SPDX-FileCopyrightText: Contributors to Fighters Legacy
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/common/fl_ports.py — the shared free-port probe + bind retry (#1056)."""

from __future__ import annotations

import socket
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "common"))
from fl_ports import (  # noqa: E402
    BindFailure,
    free_udp_port,
    looks_like_bind_failure,
    with_free_port,
)


# --- free_udp_port -----------------------------------------------------------------------------

def test_free_udp_port_returns_a_bindable_udp_port():
    """The whole point of #1056: the port must be free in the UDP space, which is what fl-server binds."""
    port = free_udp_port()
    assert 1 <= port <= 65535
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", port))  # would raise if the probe had reported a busy UDP port


def test_free_udp_port_releases_the_socket():
    """Probing must not hold the port -- the server is what binds it a moment later."""
    port = free_udp_port()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as a:
        a.bind(("127.0.0.1", port))
        assert a.getsockname()[1] == port


# --- looks_like_bind_failure -------------------------------------------------------------------

def test_recognises_fl_servers_enet_bind_failure():
    assert looks_like_bind_failure("[ERROR] bind failed: enet_host_create() failed")


def test_recognises_network_init_failure():
    assert looks_like_bind_failure("[ERROR] network init failed")


def test_case_insensitive():
    assert looks_like_bind_failure("BIND FAILED: whatever")


def test_ordinary_output_is_not_a_bind_failure():
    assert not looks_like_bind_failure("[INFO ] fl-server 0.0.1 starting\n[INFO ] transport: enet6 6.1.3")


def test_empty_output_is_not_a_bind_failure():
    assert not looks_like_bind_failure("")


# --- with_free_port ----------------------------------------------------------------------------

def test_returns_the_attempts_value_on_first_success():
    seen = []

    def attempt(port):
        seen.append(port)
        return "recorded"

    assert with_free_port(attempt) == "recorded"
    assert len(seen) == 1


def test_retries_on_bind_failure_with_a_fresh_port():
    seen = []

    def attempt(port):
        seen.append(port)
        if len(seen) < 3:
            raise BindFailure("bind failed")
        return "ok"

    assert with_free_port(attempt, attempts=3) == "ok"
    assert len(seen) == 3


def test_gives_up_after_the_attempt_budget():
    calls = []

    def attempt(port):
        calls.append(port)
        raise BindFailure("bind failed: enet_host_create() failed")

    with pytest.raises(BindFailure) as excinfo:
        with_free_port(attempt, attempts=2)
    assert len(calls) == 2
    assert "2 independently probed" in str(excinfo.value)


def test_other_failures_are_not_retried():
    """A genuinely broken server must fail once, not three times as slowly."""
    calls = []

    def attempt(port):
        calls.append(port)
        raise RuntimeError("fl-server exited 3")

    with pytest.raises(RuntimeError):
        with_free_port(attempt)
    assert len(calls) == 1


def test_rejects_a_nonsense_attempt_budget():
    with pytest.raises(ValueError):
        with_free_port(lambda port: None, attempts=0)
