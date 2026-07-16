# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Generate minimal glTF 2.0 binary (.glb) byte arrays for BuiltinGeometry.cpp.

Default: prints a C++ snippet with static const uint8_t[] definitions:
  kTetrahedronGlb — legacy directional wedge (~10 m, +X forward); removed with the
    SceneRenderer shape switch (#886) together with kDamagedWedgeGlb + the face glbs
  kFloorPlaneGlb  — flat 4 km x 4 km quad at Y=0, normal (0,1,0)
  the per-category placeholder shapes (#886), one per BuiltinShape:
    kUnknownGlb        — spiky 6-pointed jack/caltrop error beacon (bug states only)
    kAircraftGlb       — winged planform (~12 m, +X forward, ground-contact origin)
    kMissileGlb        — slender finned dart (~3.6 m, CENTER origin — projectiles are point masses)
    kBombGlb           — stubby finned dart (~2.4 m, center origin)
    kRocketGlb         — small slim dart (~1.6 m, center origin)
    kGroundVehicleGlb  — boxy hull + turret + gun (~7 m, ground origin)
    kNavalVesselGlb    — hull + bow + superstructure (~64 m, waterline origin)
    kStructureGlb      — stepped bunker block (~18 m, ground origin)
  k*DamagedGlb wreck variants (slumped/crushed) for the persistent categories
  (aircraft / ground vehicle / naval vessel / structure); projectiles despawn on
  death and the unknown marker is a bug state, so neither gets a wreck.

All shapes are single-node / single-mesh / SINGLE-PRIMITIVE glbs — the engine's
VkResourceManager::createMesh reads only meshes[0].primitives[0], so every part
(hull, fins, turret) must be baked into one triangle list. Flat shading: per-face
normals DERIVED from the CCW-from-outside winding, so the validate-mesh winding
check passes by construction; the assert_outward() checks here are what actually
catch a mis-wound face (per convex piece, at generation time). Fins/plates get
real thickness — the opaque pipeline is single-sided.

Run (regenerate C arrays):  python3 tools/gen_builtin_glb.py
Run (export .glb files):    python3 tools/gen_builtin_glb.py --export-dir /tmp/builtin
  Writes every builtin_*.glb for inspection in Blender (File > Import > glTF 2.0)
  and for the CI validate-mesh gate. Use these to confirm the engine's winding /
  normal convention matches Blender's glTF export (CCW front faces, +Y up).

NOTE: this is a Python file -- do NOT run clang-format on it (it will mangle the
comments and SPDX headers). After regenerating BuiltinGeometry.cpp, clang-format
only that .cpp output, never this script.
"""

import argparse
import json
import struct
import math
import os


def pack_vec3(x, y, z):
    return struct.pack('<fff', x, y, z)


def align4(n):
    return (n + 3) & ~3


def make_glb(vertices_bin: bytes, accessors_json: dict, meshes_json: dict) -> bytes:
    """
    Build a minimal .glb from raw vertex binary and glTF accessor/mesh descriptions.
    vertices_bin: raw binary buffer (positions + normals interleaved or sequential)
    accessors_json: list of accessor dicts
    meshes_json: list of mesh dicts (with 'primitives')
    """
    raise NotImplementedError("Use make_glb_full instead")


def make_glb_full(bin_data: bytes, gltf_json: dict) -> bytes:
    """Assemble a complete .glb from a binary payload and a glTF JSON object."""
    json_str = json.dumps(gltf_json, separators=(',', ':'))
    json_bytes = json_str.encode('utf-8')
    # Pad JSON to 4-byte alignment with spaces (as per glTF spec)
    json_pad = (4 - len(json_bytes) % 4) % 4
    json_bytes += b' ' * json_pad

    # Pad binary to 4-byte alignment with zeros (as per glTF spec)
    bin_pad = (4 - len(bin_data) % 4) % 4
    bin_data_padded = bin_data + b'\x00' * bin_pad

    json_chunk_len = len(json_bytes)
    bin_chunk_len = len(bin_data_padded)

    total = 12 + 8 + json_chunk_len + 8 + bin_chunk_len

    header = struct.pack('<III', 0x46546C67, 2, total)           # magic glTF, version 2, total length
    json_chunk_hdr = struct.pack('<II', json_chunk_len, 0x4E4F534A)  # length, type JSON
    bin_chunk_hdr = struct.pack('<II', bin_chunk_len, 0x004E4942)    # length, type BIN\0

    return header + json_chunk_hdr + json_bytes + bin_chunk_hdr + bin_data_padded


def tetra_vertices():
    """
    Directional wedge/dart placeholder, ~10 m long, pointing in +X (direction of travel).
    Topologically a tetrahedron (4 vertices, 4 faces) oriented so it reads like a vehicle:
      - FLAT BOTTOM on the ground   (the BL/BR/F triangle, all at y=0)
      - FLAT VERTICAL BACK at -X     (the BL/BR/BT triangle, all at x=-d)
      - single NOSE vertex F at +X   (the "front" / direction of travel)
      - top slopes from the back-top (BT) down to the nose
    Origin is the ground-contact point (lowest verts at y=0), the standard vehicle convention
    (origin at the gear line) so the physics floor clamps the origin straight to the terrain.
    Returns (BL, BR, BT, F).
    """
    d = 2.5  # back face plane at x = -d
    L = 7.5  # nose at x = +L  (total length d + L = 10 m)
    w = 2.5  # half-width (5 m span)
    h = 3.0  # back-top height
    BL = (-d, 0.0, -w)  # back-bottom-left
    BR = (-d, 0.0, w)   # back-bottom-right
    BT = (-d, h, 0.0)   # back-top
    F = (L, 0.0, 0.0)   # front nose (ground level)
    return BL, BR, BT, F


def damaged_wedge_vertices():
    """
    The DAMAGED-variant placeholder (#864): a slumped, foreshortened wedge that reads as a wreck
    next to the clean one, so the `damageLevel > 0` mesh swap has something distinct to show with no
    content pack. Same topology (4 verts / 4 faces, flat bottom preserved so it still sits on the
    ground), but a short collapsed nose, a low bent back-top, and an asymmetric tilt.
    Returns (BL, BR, BT, F).
    """
    d = 2.5   # back face plane at x = -d (unchanged, so the footprint matches the clean wedge)
    L = 3.5   # collapsed short nose (vs 7.5 clean)
    w = 2.5   # half-width unchanged
    BL = (-d, 0.0, -w)      # back-bottom-left  (on the ground)
    BR = (-d, 0.0, w)       # back-bottom-right (on the ground)
    BT = (-d + 0.8, 1.3, -0.7)  # back-top: slumped and bent to one side
    F = (L, 0.0, 0.0)       # nose stub at ground level
    return BL, BR, BT, F


def tetra_faces(verts=None):
    """4 faces wound CCW-from-outside (outward normals). See tetra_vertices() for the shape.

    Outward normals are required by the engine's opaque pipeline (frontFace=CW after the Vulkan
    Y-flip + cull BACK): the outside renders, the inside is culled. Face order matters because the
    forward shader's debug face-colouring keys off the face normal:
      0 bottom (-Y) = red, 1 back (-X) = green, 2 right (+Z) = blue, 3 left (-Z) = yellow.
    """
    BL, BR, BT, F = verts if verts is not None else tetra_vertices()
    return [
        (BL, F, BR),   # 0 bottom -> outward normal -Y
        (BL, BR, BT),  # 1 back   -> outward normal -X
        (BR, F, BT),   # 2 right  -> outward normal +Z (up/forward)
        (BL, BT, F),   # 3 left   -> outward normal -Z (up/forward)
    ]


def build_tetrahedron(verts=None) -> bytes:
    """
    Directional wedge placeholder (see tetra_vertices/tetra_faces), ~10 m long, +X forward.
    4 faces × 3 vertices = 12 vertices, each with POSITION (vec3) + NORMAL (vec3).
    Vertex stride = 24 bytes. Total binary = 12 * 24 = 288 bytes.
    Per-face normals (flat shading): each triangle has its own 3 identical normals.
    `verts` overrides the vertex set (used for the damaged variant); default is the clean wedge.
    """
    def norm(a, b, c):
        """Face normal from 3 vertices (a, b, c) — CCW winding."""
        ab = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
        ac = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
        nx = ab[1]*ac[2] - ab[2]*ac[1]
        ny = ab[2]*ac[0] - ab[0]*ac[2]
        nz = ab[0]*ac[1] - ab[1]*ac[0]
        length = math.sqrt(nx*nx + ny*ny + nz*nz)
        return (nx/length, ny/length, nz/length)

    faces = tetra_faces(verts)

    pos_bin = b''
    norm_bin = b''
    pos_min = [float('inf')] * 3
    pos_max = [float('-inf')] * 3

    # tetra_faces() lists each face CCW-from-outside (the glTF 2.0 standard, what Blender exports):
    # norm(a,b,c) is the OUTWARD normal and the winding cross-product agrees with it. The engine's
    # opaque pipeline front-faces standard CCW geometry (frontFace=CCW + the projection Y-flip), so
    # this renders solid from outside.
    for face in faces:
        a, b, c = face
        n = norm(a, b, c)  # outward normal (== winding cross-product direction)
        for v in (a, b, c):
            pos_bin += pack_vec3(*v)
            norm_bin += pack_vec3(*n)
            for i in range(3):
                pos_min[i] = min(pos_min[i], v[i])
                pos_max[i] = max(pos_max[i], v[i])

    # Non-interleaved: [positions 144B][normals 144B] = 288 bytes.
    # No byteStride — sequential layout avoids the stride×count byteLength issue.
    bin_data = pos_bin + norm_bin
    assert len(bin_data) == 12 * 24  # 12 verts × (vec3 pos + vec3 norm)

    byte_len = len(bin_data)

    gltf = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "builtin_entity", "mesh": 0}],
        "meshes": [{
            "name": "builtin_entity",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "mode": 4  # TRIANGLES
            }]
        }],
        "accessors": [
            {
                "bufferView": 0,
                "byteOffset": 0,
                "componentType": 5126,  # FLOAT
                "count": 12,
                "type": "VEC3",
                "min": [round(pos_min[i], 6) for i in range(3)],
                "max": [round(pos_max[i], 6) for i in range(3)],
            },
            {
                "bufferView": 1,
                "byteOffset": 0,
                "componentType": 5126,
                "count": 12,
                "type": "VEC3",
            },
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0,   "byteLength": 144, "target": 34962},  # positions
            {"buffer": 0, "byteOffset": 144, "byteLength": 144, "target": 34962},  # normals
        ],
        "buffers": [{"byteLength": byte_len}],
    }

    return make_glb_full(bin_data, gltf)


def build_floor_plane() -> bytes:
    """
    Flat 4 km × 4 km quad at Y=0, centered at origin, normal (0,1,0).
    4 vertices, 6 indices (2 triangles). POSITION (vec3) + NORMAL (vec3).
    """
    half = 2000.0  # 2 km half-extent = 4 km total

    # 4 corner vertices (Y=0), normal = (0,1,0)
    positions = [
        (-half, 0.0,  half),  # v0: NW
        ( half, 0.0,  half),  # v1: NE
        ( half, 0.0, -half),  # v2: SE
        (-half, 0.0, -half),  # v3: SW
    ]
    normal = (0.0, 1.0, 0.0)

    # 2 triangles wound CCW when viewed from above (glTF standard), so the winding cross-product
    # agrees with the stored +Y normal; the engine front-faces this (frontFace=CCW + projection
    # Y-flip), rendering the top surface.
    indices = [0, 1, 2, 0, 2, 3]

    # Build binary: positions then normals then indices
    pos_bin = b''
    for p in positions:
        pos_bin += pack_vec3(*p)

    norm_bin = b''
    for _ in positions:
        norm_bin += pack_vec3(*normal)

    idx_bin = b''
    for i in indices:
        idx_bin += struct.pack('<H', i)

    # Layout: [positions 48B][normals 48B][indices 12B] = 108 bytes
    # Pad indices section to 4 bytes (12 bytes is already aligned)
    bin_data = pos_bin + norm_bin + idx_bin
    assert len(bin_data) == 4*12 + 4*12 + 6*2  # 48+48+12 = 108

    byte_len = len(bin_data)

    gltf = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "builtin_floor", "mesh": 0}],
        "meshes": [{
            "name": "builtin_floor",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "indices": 2,
                "mode": 4
            }]
        }],
        "accessors": [
            {
                "bufferView": 0,
                "byteOffset": 0,
                "componentType": 5126,
                "count": 4,
                "type": "VEC3",
                "min": [-half, 0.0, -half],
                "max": [ half, 0.0,  half],
            },
            {
                "bufferView": 1,
                "byteOffset": 0,
                "componentType": 5126,
                "count": 4,
                "type": "VEC3",
            },
            {
                "bufferView": 2,
                "byteOffset": 0,
                "componentType": 5123,  # UNSIGNED_SHORT
                "count": 6,
                "type": "SCALAR",
            },
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0,  "byteLength": 48, "target": 34962},   # positions
            {"buffer": 0, "byteOffset": 48, "byteLength": 48, "target": 34962},   # normals
            {"buffer": 0, "byteOffset": 96, "byteLength": 12, "target": 34963},   # indices (ELEMENT_ARRAY_BUFFER)
        ],
        "buffers": [{"byteLength": byte_len}],
    }

    return make_glb_full(bin_data, gltf)


# ─── per-category placeholder shapes (#886) ──────────────────────────────────
#
# Composable flat-shaded solids. Every helper returns a list of triangles
# ((a, b, c) vertex tuples) wound CCW-from-outside; build_flat_glb() derives the
# per-face normals from that winding. Each convex piece is checked by
# assert_outward() at generation time — the only stage that can catch a
# mis-wound face, because derived normals always agree with their winding.

def _face_normal(a, b, c):
    ab = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
    ac = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
    n = (ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0])
    length = math.sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2])
    assert length > 1e-9, f"degenerate triangle: {a} {b} {c}"
    return (n[0]/length, n[1]/length, n[2]/length)


def assert_outward(tris):
    """Every face normal must point away from the piece centroid (pieces are convex)."""
    verts = [v for t in tris for v in t]
    cx = sum(v[0] for v in verts) / len(verts)
    cy = sum(v[1] for v in verts) / len(verts)
    cz = sum(v[2] for v in verts) / len(verts)
    for a, b, c in tris:
        n = _face_normal(a, b, c)
        fx = (a[0]+b[0]+c[0])/3.0 - cx
        fy = (a[1]+b[1]+c[1])/3.0 - cy
        fz = (a[2]+b[2]+c[2])/3.0 - cz
        assert n[0]*fx + n[1]*fy + n[2]*fz > 0, \
            f"face wound inward (normal {n} vs centroid dir ({fx},{fy},{fz}))"
    return tris


def quad(a, b, c, d):
    """Two triangles for a planar quad whose corners are CCW viewed from outside."""
    return [(a, b, c), (a, c, d)]


def box(cx, cy, cz, hx, hy, hz):
    """Axis-aligned box (12 triangles, CCW-from-outside)."""
    x0, x1 = cx-hx, cx+hx
    y0, y1 = cy-hy, cy+hy
    z0, z1 = cz-hz, cz+hz
    tris = []
    tris += quad((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1))  # +X
    tris += quad((x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (x0, y0, z0))  # -X
    tris += quad((x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0))  # +Y
    tris += quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))  # -Y
    tris += quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))  # +Z
    tris += quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0))  # -Z
    return assert_outward(tris)


def bipyramid(axis, tip_pos, tip_neg, r1, r2, mid=0.0):
    """Elongated square bipyramid along `axis` ('x'|'y'|'z'): a dart/spindle.
    Tips at axis = tip_pos / tip_neg, square cross-section ring (r1 x r2) at axis = mid.
    Built along +X then rotated onto the axis (rotations, not axis swaps — a swap of
    two axes has det -1 and would silently flip every winding inside-out)."""
    n = (tip_pos, 0.0, 0.0)
    t = (tip_neg, 0.0, 0.0)
    a = (mid, r1, 0.0)
    b = (mid, 0.0, r2)
    c = (mid, -r1, 0.0)
    d = (mid, 0.0, -r2)
    tris = [(n, a, b), (n, b, c), (n, c, d), (n, d, a),
            (t, b, a), (t, c, b), (t, d, c), (t, a, d)]
    if axis == 'y':
        tris = [tuple((-v[1], v[0], v[2]) for v in tri) for tri in tris]  # Rz(+90): +X -> +Y
    elif axis == 'z':
        tris = [tuple((-v[2], v[1], v[0]) for v in tri) for tri in tris]  # Ry(-90): +X -> +Z
    else:
        assert axis == 'x'
    return assert_outward(tris)


def spike(apex, c0, c1, c2, c3):
    """Open 4-sided pyramid: apex over the quad c0..c3 (corners CCW viewed from the
    apex side, same order quad() would take for a face pointing at the apex). The base
    is omitted — the piece is glued over an existing solid face that covers the hole."""
    tris = [(apex, c0, c1), (apex, c1, c2), (apex, c2, c3), (apex, c3, c0)]
    # No assert_outward: an open pyramid's centroid test is meaningless for the (absent)
    # base; the four sides are validated by the composite's visual review instead.
    return tris


def prism_y(corners_xz, y0, y1):
    """Vertical prism over a CONVEX polygon footprint. corners_xz = [(x, z), ...] in any
    consistent order (auto-corrected so the top face winds CCW viewed from +Y)."""
    # Shoelace in (x, z): the +Y-normal winding has NEGATIVE signed area in this
    # right-handed Y-up basis (verified numerically against quad()'s convention).
    area2 = 0.0
    m = len(corners_xz)
    for i in range(m):
        x0_, z0_ = corners_xz[i]
        x1_, z1_ = corners_xz[(i + 1) % m]
        area2 += x0_*z1_ - x1_*z0_
    if area2 > 0:
        corners_xz = list(reversed(corners_xz))
    top = [(x, y1, z) for x, z in corners_xz]
    bot = [(x, y0, z) for x, z in corners_xz]
    tris = []
    for i in range(1, m - 1):  # top + bottom fans
        tris.append((top[0], top[i], top[i+1]))
        tris.append((bot[0], bot[i+1], bot[i]))
    for i in range(m):  # sides
        j = (i + 1) % m
        tris += quad(bot[i], bot[j], top[j], top[i])
    return assert_outward(tris)


def translate(tris, dx, dy, dz):
    return [tuple((v[0]+dx, v[1]+dy, v[2]+dz) for v in tri) for tri in tris]


def slump(tris):
    """Wreck transform (#886): crush vertically + shear sideways. The matrix has
    det = 0.45 > 0, so it preserves orientation — the CCW winding survives and the
    normals are re-derived from the transformed triangles."""
    def s(v):
        x, y, z = v
        return (x + 0.35*y, y*0.45, z + 0.15*y)
    return [tuple(s(v) for v in tri) for tri in tris]


def unknown_tris():
    """Spiky 6-pointed jack/caltrop — the error beacon. Deliberately jarring and
    unlike any vehicle: it renders ONLY in bug states (a typeIndex missing from the
    client registry, the never-spawned Effect category, an unmapped ordinal)."""
    tris = []
    tris += bipyramid('x', 2.2, -2.2, 0.5, 0.5)
    tris += bipyramid('y', 2.2, -2.2, 0.5, 0.5)
    tris += bipyramid('z', 2.2, -2.2, 0.5, 0.5)
    return translate(tris, 0.0, 2.2, 0.0)  # bottom spike touches the ground at origin


def aircraft_tris():
    """Winged planform (~12 m, +X forward, ground-contact origin): spindle fuselage,
    swept main wings, tailplane, vertical fin."""
    tris = []
    tris += translate(bipyramid('x', 7.0, -5.0, 0.9, 0.8, 1.5), 0.0, 1.3, 0.0)  # fuselage
    for side in (1.0, -1.0):  # swept main wings (thin prisms)
        foot = [(1.5, 0.7*side), (-1.0, 4.8*side), (-2.4, 4.8*side), (-2.4, 0.7*side)]
        tris += prism_y(foot, 1.05, 1.3)
    tris += box(-4.2, 1.3, 0.0, 0.7, 0.08, 1.8)  # tailplane
    tris += box(-4.2, 2.4, 0.0, 0.7, 1.1, 0.07)  # vertical fin
    return tris


def missile_tris():
    """Slender square-section dart with 4 tail fins (~3.6 m, CENTER origin)."""
    tris = bipyramid('x', 1.8, -1.8, 0.18, 0.18, 0.5)
    tris += box(-1.45, 0.25, 0.0, 0.28, 0.25, 0.03)   # fin +Y
    tris += box(-1.45, -0.25, 0.0, 0.28, 0.25, 0.03)  # fin -Y
    tris += box(-1.45, 0.0, 0.25, 0.28, 0.03, 0.25)   # fin +Z
    tris += box(-1.45, 0.0, -0.25, 0.28, 0.03, 0.25)  # fin -Z
    return tris


def bomb_tris():
    """Stubby fat dart with short tail fins (~2.4 m, center origin)."""
    tris = bipyramid('x', 1.2, -1.2, 0.34, 0.34, 0.35)
    tris += box(-1.0, 0.3, 0.0, 0.2, 0.18, 0.025)
    tris += box(-1.0, -0.3, 0.0, 0.2, 0.18, 0.025)
    tris += box(-1.0, 0.0, 0.3, 0.2, 0.025, 0.18)
    tris += box(-1.0, 0.0, -0.3, 0.2, 0.025, 0.18)
    return tris


def rocket_tris():
    """Small slim unfinned dart (~1.6 m, center origin)."""
    return bipyramid('x', 0.8, -0.8, 0.09, 0.09, 0.3)


def ground_vehicle_tris():
    """Boxy hull + turret + gun barrel (~7 m, ground origin, gun forward +X)."""
    tris = []
    tris += box(0.0, 0.95, 0.0, 3.4, 0.95, 1.5)   # hull, y 0..1.9
    tris += box(0.4, 2.35, 0.0, 1.3, 0.45, 1.0)   # turret, y 1.9..2.8
    tris += box(2.9, 2.4, 0.0, 1.6, 0.07, 0.07)   # gun barrel
    return tris


def naval_vessel_tris():
    """Hull + pointed bow + superstructure + bridge (~64 m, waterline origin, bow +X)."""
    tris = []
    tris += box(-5.0, 3.0, 0.0, 25.0, 3.0, 5.0)   # hull, x -30..20, y 0..6
    tris += spike((34.0, 3.0, 0.0),               # bow over the hull's +X face
                  (20.0, 0.0, -5.0), (20.0, 6.0, -5.0), (20.0, 6.0, 5.0), (20.0, 0.0, 5.0))
    tris += box(-2.0, 8.0, 0.0, 8.0, 2.0, 3.0)    # superstructure, y 6..10
    tris += box(4.0, 11.0, 0.0, 2.5, 1.0, 2.0)    # bridge, y 10..12
    return tris


def structure_tris():
    """Stepped bunker block (~18 m, ground origin)."""
    tris = []
    tris += box(0.0, 3.5, 0.0, 9.0, 3.5, 7.0)     # base, y 0..7
    tris += box(0.0, 8.25, 0.0, 5.5, 1.25, 4.5)   # upper step, y 7..9.5
    return tris


def build_flat_glb(node_name: str, tris) -> bytes:
    """Single-node / single-mesh / SINGLE-PRIMITIVE flat-shaded .glb from a triangle
    list. Non-indexed, non-interleaved layout: [positions][normals], 3 verts per
    triangle, per-face normals derived from the CCW-from-outside winding (the same
    convention as the legacy wedge builder). node_name must be lowercase_underscored
    and must NOT end in `_b` (the validate-mesh damage-node suffix)."""
    pos_bin = b''
    norm_bin = b''
    pos_min = [float('inf')] * 3
    pos_max = [float('-inf')] * 3

    for a, b, c in tris:
        n = _face_normal(a, b, c)
        for v in (a, b, c):
            pos_bin += pack_vec3(*v)
            norm_bin += pack_vec3(*n)
            for i in range(3):
                pos_min[i] = min(pos_min[i], v[i])
                pos_max[i] = max(pos_max[i], v[i])

    count = 3 * len(tris)
    block = count * 12  # 3 floats per vec3
    bin_data = pos_bin + norm_bin
    assert len(bin_data) == 2 * block

    gltf = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": node_name, "mesh": 0}],
        "meshes": [{
            "name": node_name,
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "mode": 4  # TRIANGLES
            }]
        }],
        "accessors": [
            {
                "bufferView": 0,
                "byteOffset": 0,
                "componentType": 5126,  # FLOAT
                "count": count,
                "type": "VEC3",
                "min": [round(pos_min[i], 6) for i in range(3)],
                "max": [round(pos_max[i], 6) for i in range(3)],
            },
            {
                "bufferView": 1,
                "byteOffset": 0,
                "componentType": 5126,
                "count": count,
                "type": "VEC3",
            },
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0,     "byteLength": block, "target": 34962},  # positions
            {"buffer": 0, "byteOffset": block, "byteLength": block, "target": 34962},  # normals
        ],
        "buffers": [{"byteLength": len(bin_data)}],
    }

    return make_glb_full(bin_data, gltf)


# (node_name, C array name, triangle builder, has wreck variant) — order matches the
# BuiltinShape enum in engine/render/BuiltinShape.h (Unknown first).
SHAPES = [
    ("builtin_unknown",        "kUnknownGlb",       unknown_tris,        False),
    ("builtin_aircraft",       "kAircraftGlb",      aircraft_tris,       True),
    ("builtin_missile",        "kMissileGlb",       missile_tris,        False),
    ("builtin_bomb",           "kBombGlb",          bomb_tris,           False),
    ("builtin_rocket",         "kRocketGlb",        rocket_tris,         False),
    ("builtin_ground_vehicle", "kGroundVehicleGlb", ground_vehicle_tris, True),
    ("builtin_naval_vessel",   "kNavalVesselGlb",   naval_vessel_tris,   True),
    ("builtin_structure",      "kStructureGlb",     structure_tris,      True),
]


def shape_glbs():
    """Yield (file_stem, array_name, glb_bytes) for every shape + wreck variant."""
    for node_name, array_name, builder, has_wreck in SHAPES:
        tris = builder()
        yield node_name, array_name, build_flat_glb(node_name, tris)
        if has_wreck:
            yield (node_name + "_damaged",
                   array_name.replace("Glb", "DamagedGlb"),
                   build_flat_glb(node_name + "_damaged", slump(tris)))


def build_tetrahedron_face(vertices) -> bytes:
    """Build a single triangle as a minimal .glb (one face of the tetrahedron)."""
    a, b, c = vertices

    def cross3(u, v):
        return (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])

    def norm3(n):
        length = math.sqrt(sum(x*x for x in n))
        return tuple(x/length for x in n)

    ab = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
    ac = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
    n = norm3(cross3(ab, ac))  # outward normal (winding cross-product, standard CCW)

    pos_bin = pack_vec3(*a) + pack_vec3(*b) + pack_vec3(*c)
    norm_bin = pack_vec3(*n) * 3

    pos_min = [min(v[i] for v in (a, b, c)) for i in range(3)]
    pos_max = [max(v[i] for v in (a, b, c)) for i in range(3)]

    bin_data = pos_bin + norm_bin
    assert len(bin_data) == 3 * 24

    gltf = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "builtin_entity_face", "mesh": 0}],
        "meshes": [{"name": "builtin_entity_face",
                    "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "mode": 4}]}],
        "accessors": [
            {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 3,
             "type": "VEC3",
             "min": [round(pos_min[i], 6) for i in range(3)],
             "max": [round(pos_max[i], 6) for i in range(3)]},
            {"bufferView": 1, "byteOffset": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0,  "byteLength": 36, "target": 34962},
            {"buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962},
        ],
        "buffers": [{"byteLength": len(bin_data)}],
    }
    return make_glb_full(bin_data, gltf)


def bytes_to_cpp_array(name: str, data: bytes) -> str:
    lines = []
    lines.append(f'static const uint8_t {name}[] = {{')
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_vals = ', '.join(f'0x{b:02X}' for b in chunk)
        lines.append(f'    {hex_vals},')
    lines.append('};')
    lines.append(f'static_assert(sizeof({name}) == {len(data)});')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description="Generate builtin glTF geometry.")
    parser.add_argument(
        "--export-dir",
        metavar="DIR",
        help="Write builtin_entity.glb and builtin_floor.glb to DIR (for Blender import / "
             "winding inspection) instead of printing the C++ arrays.")
    args = parser.parse_args()

    tet = build_tetrahedron()
    damaged = build_tetrahedron(damaged_wedge_vertices())
    floor = build_floor_plane()
    shapes = list(shape_glbs())

    if args.export_dir:
        os.makedirs(args.export_dir, exist_ok=True)
        exports = [("builtin_entity.glb", tet), ("builtin_entity_damaged.glb", damaged),
                   ("builtin_floor.glb", floor)]
        exports += [(stem + ".glb", data) for stem, _, data in shapes]
        for name, data in exports:
            path = os.path.join(args.export_dir, name)
            with open(path, "wb") as f:
                f.write(data)
            print(f"wrote {path} ({len(data)} bytes)")
        return

    # Compute the 4 faces (same geometry/winding as build_tetrahedron: origin at the flat bottom).
    faces = tetra_faces()

    print(f'// kTetrahedronGlb: {len(tet)} bytes')
    print(bytes_to_cpp_array('kTetrahedronGlb', tet))
    print()
    print(f'// kDamagedWedgeGlb: {len(damaged)} bytes')
    print(bytes_to_cpp_array('kDamagedWedgeGlb', damaged))
    print()
    print(f'// kFloorPlaneGlb: {len(floor)} bytes')
    print(bytes_to_cpp_array('kFloorPlaneGlb', floor))
    print()
    for i, face in enumerate(faces):
        glb = build_tetrahedron_face(face)
        print(f'// kTetrahedronFace{i}Glb: {len(glb)} bytes')
        print(bytes_to_cpp_array(f'kTetrahedronFace{i}Glb', glb))
        print()
    for _, array_name, data in shapes:
        print(f'// {array_name}: {len(data)} bytes')
        print(bytes_to_cpp_array(array_name, data))
        print()


if __name__ == '__main__':
    main()
