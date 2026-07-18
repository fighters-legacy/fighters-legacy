#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate an altitude wind-profile TOML from a gridded NetCDF wind dataset (#489).

The engine consumes a per-theater wind profile — world-frame wind at a handful of altitudes — that
`WorldBroadcaster` interpolates per entity (see `engine/weather/WindProfile.h`). This tool derives
one from a NetCDF file of U/V wind components on pressure levels, taking a seasonal median over a
bounding box.

SOURCE-AGNOSTIC BY DESIGN: variable names are flags, so a NASA MERRA-2 `M2I3NPASM` file (needs an
EarthData account) AND a NOAA GFS/NOMADS file converted to NetCDF (anonymous, via `wgrib2`) both
work. The tool NEVER fetches — it consumes a local file. Recipes:

  * MERRA-2 (EarthData): download M2I3NPASM (3-hourly, pressure levels) for the theater/season, then
      gen_wind_profile.py in.nc4 --u U --v V --lon lon --lat lat --plev lev \\
        --bbox 34 -120 38 -116 --season summer --out theater.toml
  * NOAA GFS (anonymous): grab a GFS grib2 from NOMADS, convert with
      `wgrib2 gfs.grib2 -netcdf gfs.nc`, then point --u/--v at the UGRD/VGRD variable names.

Pure-logic helpers (season_months, bbox_mask, uv_to_met, isa_pressure_to_alt_m, select_knots,
render_profile_toml) are unit-tested in tests/test_gen_wind_profile.py without NetCDF or a network.
"""
import argparse
import math
import sys

# NetCDF is optional so --help and the pure-logic unit tests work without the system package.
try:
    import netCDF4  # type: ignore

    _HAVE_NETCDF = True
except ImportError:
    _HAVE_NETCDF = False

try:
    import numpy as np  # type: ignore

    _HAVE_NUMPY = True
except ImportError:
    _HAVE_NUMPY = False

# ESA/engine altitude knot cap (EnvironmentState::kWindProfileMaxKnots).
MAX_KNOTS = 8

SEASONS = {
    "winter": [12, 1, 2],
    "spring": [3, 4, 5],
    "summer": [6, 7, 8],
    "autumn": [9, 10, 11],
    "annual": list(range(1, 13)),
}


def season_months(season: str) -> list[int]:
    """Calendar months (1-12) for a named season. Raises on an unknown name."""
    key = season.lower()
    if key not in SEASONS:
        raise ValueError(f"unknown season '{season}' (expected one of {sorted(SEASONS)})")
    return list(SEASONS[key])


def bbox_mask(lats, lons, lat_min, lon_min, lat_max, lon_max):
    """Boolean (lat, lon) grid mask for cells inside the bounding box. Longitudes are normalised to
    [-180, 180) so a box that straddles the antimeridian (lon_min > lon_max) still selects correctly."""

    def norm(x):
        return ((x + 180.0) % 360.0) - 180.0

    lo, hi = norm(lon_min), norm(lon_max)
    out = []
    for la in lats:
        row = []
        in_lat = lat_min <= la <= lat_max
        for lo_v in lons:
            lonn = norm(lo_v)
            if lo <= hi:
                in_lon = lo <= lonn <= hi
            else:  # wraps the antimeridian
                in_lon = lonn >= lo or lonn <= hi
            row.append(in_lat and in_lon)
        out.append(row)
    return out


def uv_to_met(u: float, v: float) -> tuple[float, float]:
    """(u, v) wind components (m/s, eastward/northward) -> (speed m/s, heading-FROM degrees).

    Meteorological convention: heading is the direction the wind blows FROM, so a westerly (blowing
    toward +east, u>0) is 270 deg. heading = atan2(-u, -v) in compass degrees [0, 360)."""
    speed = math.hypot(u, v)
    heading = math.degrees(math.atan2(-u, -v)) % 360.0
    return speed, heading


def isa_pressure_to_alt_m(pressure_pa: float) -> float:
    """ISA barometric altitude (m) for a pressure (Pa) — the fallback when a NetCDF gives pressure
    levels but no geopotential height. Troposphere formula (valid to ~11 km, adequate for knots)."""
    p0 = 101325.0
    t0 = 288.15
    lapse = 0.0065
    g = 9.80665
    r = 287.05
    if pressure_pa <= 0:
        return 0.0
    return (t0 / lapse) * (1.0 - (pressure_pa / p0) ** (r * lapse / g))


def select_knots(levels: list[tuple[float, float, float]], max_knots: int = MAX_KNOTS):
    """Reduce (alt_m, u, v) samples to at most `max_knots`, ascending by altitude, always keeping the
    lowest and highest, evenly sampling the middle. Deduplicates by altitude."""
    seen = {}
    for alt, u, v in levels:
        seen[round(alt, 1)] = (alt, u, v)
    ordered = sorted(seen.values(), key=lambda k: k[0])
    if len(ordered) <= max_knots:
        return ordered
    picks = [ordered[0]]
    inner = max_knots - 2
    n = len(ordered)
    for i in range(1, inner + 1):
        idx = round(i * (n - 1) / (inner + 1))
        picks.append(ordered[idx])
    picks.append(ordered[-1])
    # Dedup preserving order.
    out, last_alt = [], None
    for k in picks:
        if last_alt is None or abs(k[0] - last_alt) > 0.5:
            out.append(k)
            last_alt = k[0]
    return out


def render_profile_toml(knots, source: str, theater: str, season: str) -> str:
    """Render a [wind] profile TOML the server loads. Each knot: altitude_m, speed_ms, heading_deg."""
    lines = [
        "# Generated by tools/gen_wind_profile.py (#489). Altitude wind profile.",
        "[wind]",
        f'source = "{source}"',
        f'theater = "{theater}"',
        f'season = "{season}"',
        "",
    ]
    for alt, u, v in knots:
        speed, heading = uv_to_met(u, v)
        lines.append("[[wind.profile]]")
        lines.append(f"altitude_m = {alt:.1f}")
        lines.append(f"speed_ms = {speed:.2f}")
        lines.append(f"heading_deg = {heading:.1f}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def _seasonal_median_uv(nc, args, months):  # pragma: no cover - needs NetCDF + numpy
    """Median U/V per pressure level over the bbox + season. Returns [(alt_m, u, v), ...]."""
    lats = nc.variables[args.lat][:]
    lons = nc.variables[args.lon][:]
    plev = nc.variables[args.plev][:]
    u_all = nc.variables[args.u][:]
    v_all = nc.variables[args.v][:]
    times = nc.variables[args.time]
    dates = netCDF4.num2date(times[:], getattr(times, "units", "hours since 2000-01-01"))
    tmask = np.array([d.month in months for d in dates])
    mask = np.array(bbox_mask(list(lats), list(lons), args.bbox[0], args.bbox[1], args.bbox[2], args.bbox[3]))
    out = []
    for li, p in enumerate(plev):
        # p may be hPa; convert to Pa for the ISA fallback.
        p_pa = float(p) * (100.0 if float(p) < 2000.0 else 1.0)
        alt = isa_pressure_to_alt_m(p_pa)
        u_sel = u_all[tmask][:, li][:, mask]
        v_sel = v_all[tmask][:, li][:, mask]
        out.append((alt, float(np.nanmedian(u_sel)), float(np.nanmedian(v_sel))))
    return out


def main(argv=None):  # pragma: no cover - CLI wrapper over tested helpers
    ap = argparse.ArgumentParser(description="Generate an altitude wind-profile TOML from NetCDF wind data (#489).")
    ap.add_argument("input", help="input NetCDF file (MERRA-2 M2I3NPASM or GFS-converted)")
    ap.add_argument("--out", required=True, help="output profile TOML path")
    ap.add_argument("--u", default="U", help="eastward-wind variable name")
    ap.add_argument("--v", default="V", help="northward-wind variable name")
    ap.add_argument("--lat", default="lat")
    ap.add_argument("--lon", default="lon")
    ap.add_argument("--plev", default="lev", help="pressure-level variable name")
    ap.add_argument("--time", default="time")
    ap.add_argument("--bbox", nargs=4, type=float, metavar=("LATMIN", "LONMIN", "LATMAX", "LONMAX"), required=True)
    ap.add_argument("--season", default="annual")
    ap.add_argument("--theater", default="theater")
    ap.add_argument("--source", default="netcdf")
    ap.add_argument("--max-knots", type=int, default=MAX_KNOTS)
    args = ap.parse_args(argv)

    months = season_months(args.season)
    if not (_HAVE_NETCDF and _HAVE_NUMPY):
        print("error: netCDF4 + numpy are required to read the dataset "
              "(pip install netCDF4 numpy / apt install python3-netcdf4 python3-numpy)", file=sys.stderr)
        return 2

    nc = netCDF4.Dataset(args.input)
    levels = _seasonal_median_uv(nc, args, months)
    knots = select_knots(levels, args.max_knots)
    toml = render_profile_toml(knots, args.source, args.theater, args.season)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(toml)
    print(f"wrote {args.out} with {len(knots)} knots ({args.season}, {len(levels)} levels sampled)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
