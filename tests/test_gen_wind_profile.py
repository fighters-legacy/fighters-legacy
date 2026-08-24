# SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure-logic tests for tools/gen_wind_profile.py (#489). No NetCDF, no network."""
import math

import pytest

from conftest import load_tool

gwp = load_tool("gen_wind_profile", "tools", "gen_wind_profile.py")


def test_season_months():
    assert gwp.season_months("summer") == [6, 7, 8]
    assert gwp.season_months("Winter") == [12, 1, 2]
    assert gwp.season_months("annual") == list(range(1, 13))
    with pytest.raises(ValueError):
        gwp.season_months("monsoon")


def test_uv_to_met_cardinal_winds():
    # A westerly blows toward +east (u>0, v=0) and is FROM 270 deg.
    speed, heading = gwp.uv_to_met(10.0, 0.0)
    assert speed == pytest.approx(10.0)
    assert heading == pytest.approx(270.0)
    # A northerly blows toward -north / +south (v<0) and is FROM 0/360.
    speed, heading = gwp.uv_to_met(0.0, -5.0)
    assert speed == pytest.approx(5.0)
    assert heading % 360 == pytest.approx(0.0)
    # A southerly (v>0, blowing north) is FROM 180.
    _, heading = gwp.uv_to_met(0.0, 5.0)
    assert heading == pytest.approx(180.0)
    # An easterly (u<0, blowing west) is FROM 90.
    _, heading = gwp.uv_to_met(-3.0, 0.0)
    assert heading == pytest.approx(90.0)


def test_isa_pressure_to_alt_monotonic():
    # Sea level ~0 m; lower pressure -> higher altitude.
    assert gwp.isa_pressure_to_alt_m(101325.0) == pytest.approx(0.0, abs=1.0)
    a500 = gwp.isa_pressure_to_alt_m(50000.0)  # ~500 hPa ~ 5.5 km
    a250 = gwp.isa_pressure_to_alt_m(25000.0)  # ~250 hPa ~ 10 km
    assert 5000 < a500 < 6000
    assert a250 > a500
    assert gwp.isa_pressure_to_alt_m(0.0) == 0.0


def test_bbox_mask_basic_and_antimeridian():
    lats = [30.0, 35.0, 40.0]
    lons = [-120.0, -118.0, -116.0]
    m = gwp.bbox_mask(lats, lons, 34, -119, 38, -117)
    # Only lat 35 (index 1) and lon -118 (index 1) is inside.
    assert m[1][1] is True
    assert m[0][0] is False
    assert m[2][2] is False
    # Antimeridian box lon_min=170, lon_max=-170 selects both 175 and -175.
    m2 = gwp.bbox_mask([0.0], [175.0, 0.0, -175.0], -10, 170, 10, -170)
    assert m2[0][0] is True   # 175
    assert m2[0][1] is False  # 0
    assert m2[0][2] is True   # -175


def test_select_knots_caps_and_keeps_endpoints():
    levels = [(float(i * 1000), float(i), 0.0) for i in range(12)]  # 0..11 km
    knots = gwp.select_knots(levels, max_knots=5)
    assert len(knots) <= 5
    assert knots[0][0] == pytest.approx(0.0)      # lowest kept
    assert knots[-1][0] == pytest.approx(11000.0)  # highest kept
    # Ascending.
    alts = [k[0] for k in knots]
    assert alts == sorted(alts)


def test_select_knots_passthrough_when_few():
    levels = [(0.0, 1.0, 0.0), (2000.0, 2.0, 0.0)]
    assert gwp.select_knots(levels, max_knots=8) == levels


def test_render_profile_toml_roundtrips_with_tomllib():
    tomllib = pytest.importorskip("tomllib")
    knots = [(0.0, 10.0, 0.0), (5000.0, 0.0, 20.0)]
    text = gwp.render_profile_toml(knots, source="MERRA-2", theater="nevada", season="summer")
    parsed = tomllib.loads(text)
    assert parsed["wind"]["source"] == "MERRA-2"
    assert parsed["wind"]["season"] == "summer"
    profile = parsed["wind"]["profile"]
    assert len(profile) == 2
    assert profile[0]["altitude_m"] == pytest.approx(0.0)
    # Surface knot u=10 (westerly) -> FROM 270.
    assert profile[0]["heading_deg"] == pytest.approx(270.0)
    assert profile[0]["speed_ms"] == pytest.approx(10.0)
