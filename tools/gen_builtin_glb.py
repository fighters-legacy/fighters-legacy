# SPDX-FileCopyrightText: 2026 John McKenzie
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Generate minimal glTF 2.0 binary (.glb) byte arrays for BuiltinGeometry.cpp.

Outputs a C++ snippet with two static const uint8_t[] definitions:
  kTetrahedronGlb — regular tetrahedron, ~10 m span, +X forward, per-face normals
  kFloorPlaneGlb  — flat 4 km × 4 km quad at Y=0, normal (0,1,0)

Run: python3 tools/gen_builtin_glb.py
"""

import json
import struct
import math


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


def build_tetrahedron() -> bytes:
    """
    Regular tetrahedron, ~10 m span, pointing in +X direction.
    4 faces × 3 vertices = 12 vertices, each with POSITION (vec3) + NORMAL (vec3).
    Vertex stride = 24 bytes. Total binary = 12 * 24 = 288 bytes.
    Per-face normals (flat shading): each triangle has its own 3 identical normals.
    """
    # Tetrahedron vertices in a coordinate system where the centroid is at origin,
    # one vertex points along +X (forward), and the base triangle faces -X.
    # Regular tetrahedron with circumradius R = 5.0 m (diameter ~10 m).
    R = 5.0  # circumradius in metres

    # Vertex positions of a regular tetrahedron inscribed in a sphere of radius R.
    # Oriented so v0 is the "nose" pointing along +X.
    # v0: front tip
    # v1, v2, v3: rear base triangle (equilateral, rotated 120° around X axis)
    v0 = (R, 0.0, 0.0)
    base_r = R * math.sqrt(8.0/9.0)  # radius of base circle
    base_x = -R / 3.0                # X position of base vertices
    angle_offset = 0.0  # rotate base so one vertex is up (+Y)
    v1 = (base_x,  base_r * math.cos(angle_offset + 0),              base_r * math.sin(angle_offset + 0))
    v2 = (base_x,  base_r * math.cos(angle_offset + 2*math.pi/3),    base_r * math.sin(angle_offset + 2*math.pi/3))
    v3 = (base_x,  base_r * math.cos(angle_offset + 4*math.pi/3),    base_r * math.sin(angle_offset + 4*math.pi/3))

    def norm(a, b, c):
        """Face normal from 3 vertices (a, b, c) — CCW winding."""
        ab = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
        ac = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
        nx = ab[1]*ac[2] - ab[2]*ac[1]
        ny = ab[2]*ac[0] - ab[0]*ac[2]
        nz = ab[0]*ac[1] - ab[1]*ac[0]
        length = math.sqrt(nx*nx + ny*ny + nz*nz)
        return (nx/length, ny/length, nz/length)

    # 4 faces with CCW winding when viewed from outside (glTF convention).
    # Swapping the last two vertices of the naive ordering corrects the cross-product
    # direction to point outward from the centroid at the origin.
    faces = [
        (v0, v2, v1),
        (v0, v3, v2),
        (v0, v1, v3),
        (v1, v2, v3),  # base, outward normal = -X direction
    ]

    pos_bin = b''
    norm_bin = b''
    pos_min = [float('inf')] * 3
    pos_max = [float('-inf')] * 3

    for face in faces:
        a, b, c = face
        n = norm(a, b, c)
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
        "nodes": [{"mesh": 0}],
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

    # 2 triangles (CCW from above):  v0,v2,v1 and v0,v3,v2
    # Indices as uint16
    indices = [0, 2, 1, 0, 3, 2]

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
        "nodes": [{"mesh": 0}],
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
    n = norm3(cross3(ab, ac))

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
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "mode": 4}]}],
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
    tet = build_tetrahedron()
    floor = build_floor_plane()

    # Compute the 4 CCW-wound faces (same geometry as build_tetrahedron).
    R = 5.0
    base_r = R * math.sqrt(8.0 / 9.0)
    base_x = -R / 3.0
    v0 = (R, 0.0, 0.0)
    v1 = (base_x, base_r * math.cos(0),              base_r * math.sin(0))
    v2 = (base_x, base_r * math.cos(2*math.pi/3),    base_r * math.sin(2*math.pi/3))
    v3 = (base_x, base_r * math.cos(4*math.pi/3),    base_r * math.sin(4*math.pi/3))
    faces = [(v0, v2, v1), (v0, v3, v2), (v0, v1, v3), (v1, v2, v3)]

    print(f'// kTetrahedronGlb: {len(tet)} bytes')
    print(bytes_to_cpp_array('kTetrahedronGlb', tet))
    print()
    print(f'// kFloorPlaneGlb: {len(floor)} bytes')
    print(bytes_to_cpp_array('kFloorPlaneGlb', floor))
    print()
    for i, face in enumerate(faces):
        glb = build_tetrahedron_face(face)
        print(f'// kTetrahedronFace{i}Glb: {len(glb)} bytes')
        print(bytes_to_cpp_array(f'kTetrahedronFace{i}Glb', glb))


if __name__ == '__main__':
    main()
