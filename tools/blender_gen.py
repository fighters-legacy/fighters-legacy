# SPDX-FileCopyrightText: 2026 John McKenzie
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Blender 4.x headless fighter aircraft mesh generator.

Produces engine-compatible .glb assets (glTF 2.0, PBR metallic-roughness,
external .ktx2 URI references) that pass validate-mesh and load in the renderer.

Usage:
    blender --background --python tools/blender_gen.py -- [options]

    Linux:
        blender --background --python tools/blender_gen.py -- --id fa18c ...
    macOS:
        /Applications/Blender.app/Contents/MacOS/blender --background \\
            --python tools/blender_gen.py -- --id fa18c ...
    Windows (PowerShell):
        & "C:\\Program Files\\Blender Foundation\\Blender 4.x\\blender.exe" \\
            --background --python tools\\blender_gen.py -- --id fa18c ...

Options:
    --id <name>          Asset ID, e.g. fa18c  [required, ^[a-z][a-z0-9_]*$]
    --output-dir <path>  Output directory       [required]
    --wing-style <s>     delta | swept | straight  (default: swept)
    --length <m>         Fuselage length in metres (default: 15.0)
    --lod                Also export lod0/lod1/lod2 variants
    --bake-textures      Bake diffuse PNG + generate ORM PNG via Cycles CPU
    --tex-size <px>      Bake resolution in pixels, power-of-two (default: 1024)
    --seed <n>           RNG seed for damage variation (default: 42)

Outputs (example for --id fa18c --output-dir assets/aircraft/fa18c/):
    fa18c.glb           clean mesh + _b damage node; .ktx2 URIs pre-wired
    fa18c_dmg.glb       SceneRenderer damageMeshName target
    fa18c_lod0.glb      (--lod) ~50 % vertex reduction
    fa18c_lod1.glb      (--lod) ~25 %
    fa18c_lod2.glb      (--lod) ~10 %
    fa18c_diffuse.png   (--bake-textures) feed to: tex-compress --type diffuse
    fa18c_orm.png       (--bake-textures) feed to: tex-compress --type orm
"""

import sys

# Version guard — must precede any bpy import so the error is readable.
if sys.version_info < (3, 10):
    sys.exit("blender_gen.py requires Blender 4.0+ (Python 3.10+)")

import json
import math
import random
import re
import struct
import argparse
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector

if bpy.app.version < (4, 0, 0):
    sys.exit(f"blender_gen.py requires Blender 4.0+, got {bpy.app.version_string}")

try:
    import numpy as np
    _HAVE_NUMPY = True
except ImportError:
    _HAVE_NUMPY = False

# ─── Wing style table ─────────────────────────────────────────────────────────
#   key → (root_chord_frac, tip_chord_frac, sweep_degrees)
_WING_STYLES: dict[str, tuple[float, float, float]] = {
    "swept":    (0.35, 0.15, 30.0),
    "delta":    (0.55, 0.05, 55.0),
    "straight": (0.25, 0.22,  5.0),
}

# ─── Argument parsing ─────────────────────────────────────────────────────────

def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser(prog="blender_gen.py")
    p.add_argument("--id", required=True, metavar="NAME")
    p.add_argument("--output-dir", required=True, metavar="PATH")
    p.add_argument("--wing-style", choices=_WING_STYLES, default="swept")
    p.add_argument("--length", type=float, default=15.0, metavar="M")
    p.add_argument("--lod", action="store_true")
    p.add_argument("--bake-textures", action="store_true")
    p.add_argument("--tex-size", type=int, default=1024, metavar="PX")
    p.add_argument("--seed", type=int, default=42)
    return p.parse_args(argv)


def _validate_id(asset_id: str) -> None:
    if not re.match(r"^[a-z][a-z0-9_]*$", asset_id):
        sys.exit(
            f"Error: --id '{asset_id}' is invalid. "
            "Must match ^[a-z][a-z0-9_]*$ (lowercase, digits, underscores; "
            "must start with a letter). validate-mesh will reject anything else."
        )

# ─── Scene management ─────────────────────────────────────────────────────────

def _clear_scene() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)

# ─── Geometry helpers ─────────────────────────────────────────────────────────

def _fuselage_radius(t: float, max_r: float) -> float:
    """Cross-section radius at normalised length position t ∈ [0, 1]."""
    r = max_r * math.sin(t * math.pi) * 1.05
    return max(r, max_r * 0.12)


def _add_fuselage(
    bm: bmesh.types.BMesh,
    length: float,
    max_r: float,
    rings: int = 10,
    steps: int = 16,
) -> None:
    """Add fuselage tube (vertex rings + quad strips + nose/tail caps)."""
    ring_list: list[list[bmesh.types.BMVert]] = []
    for j in range(rings):
        t = j / (rings - 1)
        x = -length / 2 + t * length
        r = _fuselage_radius(t, max_r)
        ring: list[bmesh.types.BMVert] = []
        for i in range(steps):
            a = i * math.tau / steps
            ring.append(bm.verts.new(Vector((x, r * math.cos(a), r * math.sin(a)))))
        ring_list.append(ring)

    for j in range(rings - 1):
        for i in range(steps):
            ni = (i + 1) % steps
            bm.faces.new([ring_list[j][i], ring_list[j][ni],
                          ring_list[j + 1][ni], ring_list[j + 1][i]])

    # Tail cap — fan triangles, CCW when viewed from -X
    tc = bm.verts.new(Vector((-length / 2, 0.0, 0.0)))
    for i in range(steps):
        bm.faces.new([ring_list[0][(i + 1) % steps], ring_list[0][i], tc])

    # Nose cap — fan triangles, CCW when viewed from +X
    nc = bm.verts.new(Vector((length / 2, 0.0, 0.0)))
    for i in range(steps):
        bm.faces.new([ring_list[-1][i], ring_list[-1][(i + 1) % steps], nc])


def _add_wing_pair(
    bm: bmesh.types.BMesh,
    length: float,
    fuse_r: float,
    wing_style: str,
) -> None:
    """Add left + right wing slabs."""
    rc_frac, tc_frac, sweep_deg = _WING_STYLES[wing_style]
    root_chord = length * rc_frac
    tip_chord  = length * tc_frac
    span       = length * 0.27          # half-span (fuselage edge → tip)
    wing_t     = root_chord * 0.05      # half-thickness
    z_off      = -fuse_r * 0.25
    sweep_tan  = math.tan(math.radians(sweep_deg))

    # Leading-edge root is set back from the mid-point
    x_le_root = -root_chord * 0.55
    x_te_root = x_le_root + root_chord
    x_le_tip  = x_le_root + span * sweep_tan
    x_te_tip  = x_le_tip + tip_chord

    for side in (+1, -1):
        y_root = fuse_r * side
        y_tip  = (fuse_r + span) * side
        z_lo   = z_off - wing_t
        z_hi   = z_off + wing_t

        def _v(x: float, y: float, z: float) -> bmesh.types.BMVert:
            return bm.verts.new(Vector((x, y, z)))

        b = [
            _v(x_le_root, y_root, z_lo),
            _v(x_te_root, y_root, z_lo),
            _v(x_te_tip,  y_tip,  z_lo),
            _v(x_le_tip,  y_tip,  z_lo),
        ]
        t = [
            _v(x_le_root, y_root, z_hi),
            _v(x_te_root, y_root, z_hi),
            _v(x_te_tip,  y_tip,  z_hi),
            _v(x_le_tip,  y_tip,  z_hi),
        ]

        # 6 faces — winding consistent; recalc_face_normals fixes orientation.
        bm.faces.new([b[0], b[1], b[2], b[3]])         # bottom
        bm.faces.new([t[3], t[2], t[1], t[0]])         # top
        bm.faces.new([b[0], t[0], t[3], b[3]])         # leading edge
        bm.faces.new([b[1], b[2], t[2], t[1]])         # trailing edge
        bm.faces.new([b[0], b[1], t[1], t[0]])         # root
        bm.faces.new([b[3], t[3], t[2], b[2]])         # tip


def _add_canopy(
    bm: bmesh.types.BMesh,
    length: float,
    fuse_r: float,
) -> None:
    """Add cockpit canopy as a squashed upper-hemisphere dome."""
    cx     = length * 0.25
    cz     = fuse_r * 0.85
    r_lat  = fuse_r * 0.70     # lateral radius
    r_lon  = fuse_r * 0.45     # longitudinal radius (narrower fore–aft)
    h      = fuse_r * 1.20     # height from base to apex
    seg    = 10
    rngs   = 5

    dome: list[list[bmesh.types.BMVert]] = []
    for j in range(rngs + 1):
        phi  = (j / rngs) * math.pi / 2     # 0 → π/2
        ring: list[bmesh.types.BMVert] = []
        for i in range(seg):
            th = i * math.tau / seg
            x  = cx + math.cos(th) * r_lon * math.cos(phi)
            y  = math.sin(th) * r_lat * math.cos(phi)
            z  = cz + math.sin(phi) * h
            ring.append(bm.verts.new(Vector((x, y, z))))
        dome.append(ring)

    for j in range(rngs):
        for i in range(seg):
            ni = (i + 1) % seg
            bm.faces.new([dome[j][i], dome[j][ni], dome[j + 1][ni], dome[j + 1][i]])

    base_c = bm.verts.new(Vector((cx, 0.0, cz)))
    for i in range(seg):
        bm.faces.new([dome[0][(i + 1) % seg], dome[0][i], base_c])

    apex = bm.verts.new(Vector((cx, 0.0, cz + h)))
    for i in range(seg):
        bm.faces.new([dome[-1][i], dome[-1][(i + 1) % seg], apex])


def _add_vertical_fin(
    bm: bmesh.types.BMesh,
    length: float,
    fuse_r: float,
) -> None:
    """Add a single swept vertical stabiliser fin."""
    fin_h      = length * 0.18
    fin_root_c = length * 0.28
    fin_tip_c  = length * 0.12
    fin_t      = fin_root_c * 0.04
    z_base     = fuse_r * 0.55
    sweep_tan  = math.tan(math.radians(35.0))

    x_le_root = length * 0.03
    x_te_root = x_le_root + fin_root_c
    x_le_tip  = x_le_root + fin_h * sweep_tan
    x_te_tip  = x_le_tip + fin_tip_c

    def _v(x: float, y: float, z: float) -> bmesh.types.BMVert:
        return bm.verts.new(Vector((x, y, z)))

    b = [
        _v(x_le_root, -fin_t, z_base),
        _v(x_te_root, -fin_t, z_base),
        _v(x_te_tip,  -fin_t, z_base + fin_h),
        _v(x_le_tip,  -fin_t, z_base + fin_h),
    ]
    top = [
        _v(x_le_root, fin_t, z_base),
        _v(x_te_root, fin_t, z_base),
        _v(x_te_tip,  fin_t, z_base + fin_h),
        _v(x_le_tip,  fin_t, z_base + fin_h),
    ]

    bm.faces.new([b[0], b[1], b[2], b[3]])
    bm.faces.new([top[3], top[2], top[1], top[0]])
    bm.faces.new([b[0], top[0], top[3], b[3]])     # leading edge
    bm.faces.new([b[1], b[2], top[2], top[1]])     # trailing edge
    bm.faces.new([b[0], b[1], top[1], top[0]])     # base
    bm.faces.new([b[3], top[3], top[2], b[2]])     # tip


def _add_intakes(
    bm: bmesh.types.BMesh,
    length: float,
    fuse_r: float,
) -> None:
    """Add simple rectangular engine intake scoops (left + right)."""
    h       = fuse_r * 0.60
    w       = fuse_r * 0.55
    depth   = length * 0.18
    z0      = -fuse_r * 0.40
    x_start = -length * 0.15
    x_end   = x_start + depth

    for side in (+1, -1):
        y_in  = fuse_r * 0.45 * side
        y_out = (fuse_r * 0.45 + w) * side

        def _v(x: float, y: float, z: float) -> bmesh.types.BMVert:
            return bm.verts.new(Vector((x, y, z)))

        vs = [
            _v(x_start, y_in,  z0),         # 0 front-inner-bottom
            _v(x_start, y_out, z0),         # 1 front-outer-bottom
            _v(x_start, y_out, z0 + h),     # 2 front-outer-top
            _v(x_start, y_in,  z0 + h),     # 3 front-inner-top
            _v(x_end,   y_in,  z0),         # 4 back-inner-bottom
            _v(x_end,   y_out, z0),         # 5 back-outer-bottom
            _v(x_end,   y_out, z0 + h),     # 6 back-outer-top
            _v(x_end,   y_in,  z0 + h),     # 7 back-inner-top
        ]
        for face_idx in [
            [vs[0], vs[3], vs[2], vs[1]],   # front (intake opening)
            [vs[4], vs[5], vs[6], vs[7]],   # back
            [vs[0], vs[1], vs[5], vs[4]],   # bottom
            [vs[2], vs[3], vs[7], vs[6]],   # top
            [vs[1], vs[2], vs[6], vs[5]],   # outer
            [vs[0], vs[4], vs[7], vs[3]],   # inner
        ]:
            bm.faces.new(face_idx)


def _assign_uvs(
    bm: bmesh.types.BMesh,
    length: float,
    fuse_r: float,
) -> None:
    """Per-face UV projection: cylindrical for fuselage, planar for flat surfaces."""
    uv = bm.loops.layers.uv.verify()
    for face in bm.faces:
        n  = face.normal.normalized()
        nx = abs(n.x)
        nz = abs(n.z)
        for loop in face.loops:
            co = loop.vert.co
            if nz > 0.60:
                # Wings / fin top&bottom — top-down planar
                u = (co.y / (length * 0.35) + 0.5) % 1.0
                v = (co.x / length + 0.5) % 1.0
            elif nx > 0.70:
                # Nose / tail caps — YZ planar
                fr = max(fuse_r, 1e-6)
                u = (co.y / (2.0 * fr) + 0.5) % 1.0
                v = (co.z / (2.0 * fr) + 0.5) % 1.0
            else:
                # Fuselage, fin sides, intakes — cylindrical around X axis
                u = (math.atan2(co.z, co.y) / math.tau + 0.5) % 1.0
                v = (co.x / length + 0.5) % 1.0
            loop[uv].uv = (u, v)


def _build_mesh(
    length: float,
    wing_style: str,
    fuselage_steps: int = 16,
    fuselage_rings: int = 10,
) -> bmesh.types.BMesh:
    """Return a fresh bmesh containing the complete aircraft geometry."""
    fuse_r = length * 0.06
    bm = bmesh.new()
    _add_fuselage(bm, length, fuse_r, rings=fuselage_rings, steps=fuselage_steps)
    _add_wing_pair(bm, length, fuse_r, wing_style)
    _add_canopy(bm, length, fuse_r)
    _add_vertical_fin(bm, length, fuse_r)
    if wing_style != "straight":
        _add_intakes(bm, length, fuse_r)
    _assign_uvs(bm, length, fuse_r)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return bm


def _apply_damage(bm: bmesh.types.BMesh, seed: int) -> None:
    """Randomly delete ~15 % of faces to simulate hull breaches."""
    rng = random.Random(seed)
    faces = list(bm.faces)
    k = max(1, int(len(faces) * 0.15))
    to_del = rng.sample(faces, min(k, len(faces)))
    bmesh.ops.delete(bm, geom=to_del, context="FACES")

# ─── Blender object helpers ───────────────────────────────────────────────────

def _commit(bm: bmesh.types.BMesh, name: str) -> bpy.types.Object:
    """Commit a bmesh to the scene; return the linked object."""
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def _make_material(
    name: str,
    base_color: tuple[float, float, float],
    roughness: float,
    metallic: float,
) -> bpy.types.Material:
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = next(
        (n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None
    )
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (*base_color, 1.0)
        bsdf.inputs["Roughness"].default_value  = roughness
        bsdf.inputs["Metallic"].default_value   = metallic
    return mat


def _set_material(obj: bpy.types.Object, mat: bpy.types.Material) -> None:
    obj.data.materials.clear()
    obj.data.materials.append(mat)

# ─── GLB patching ─────────────────────────────────────────────────────────────

def _patch_glb(path: Path, diffuse_uri: str, orm_uri: str) -> None:
    """Inject .ktx2 image / texture / material references into a GLB JSON chunk.

    validate-mesh checks only that no images are *embedded* (bufferView >= 0 or
    non-empty pixel data).  It does NOT require the .ktx2 files to exist on disk,
    so we can pre-wire the references before tex-compress has been run.
    """
    raw = path.read_bytes()
    magic, _ver, _total = struct.unpack_from("<III", raw, 0)
    if magic != 0x46546C67:
        raise ValueError(f"Not a GLB file: {path}")

    json_chunk_len = struct.unpack_from("<I", raw, 12)[0]
    gltf_json = json.loads(raw[20 : 20 + json_chunk_len])
    bin_tail  = raw[20 + json_chunk_len :]   # BIN chunk verbatim

    gltf_json["images"]   = [{"uri": diffuse_uri}, {"uri": orm_uri}]
    gltf_json["textures"] = [{"source": 0}, {"source": 1}]

    for mat in gltf_json.get("materials", []):
        pbr = mat.setdefault("pbrMetallicRoughness", {})
        pbr["baseColorTexture"]         = {"index": 0}
        pbr["metallicRoughnessTexture"] = {"index": 1}
        mat["occlusionTexture"]         = {"index": 1}

    new_json = json.dumps(gltf_json, separators=(",", ":")).encode()
    pad      = (4 - len(new_json) % 4) % 4
    new_json += b" " * pad

    new_total = 12 + 8 + len(new_json) + len(bin_tail)
    path.write_bytes(
        struct.pack("<III", 0x46546C67, 2, new_total)
        + struct.pack("<II", len(new_json), 0x4E4F534A)
        + new_json
        + bin_tail
    )

# ─── Export helpers ───────────────────────────────────────────────────────────

def _raw_export(path: Path, use_selection: bool = False) -> None:
    bpy.ops.export_scene.gltf(
        filepath=str(path),
        export_format="GLB",
        export_image_format="NONE",
        export_normals=True,
        export_tangents=True,
        export_texcoords=True,
        use_selection=use_selection,
    )


def _select_only(obj: bpy.types.Object) -> None:
    for o in bpy.context.scene.objects:
        o.select_set(o is obj)
    bpy.context.view_layer.objects.active = obj


def export_main_glb(
    asset_id: str,
    output_dir: Path,
) -> None:
    """Export all scene objects to <id>.glb and inject .ktx2 URI refs."""
    out = output_dir / f"{asset_id}.glb"
    _raw_export(out, use_selection=False)
    _patch_glb(out,
               diffuse_uri=f"{asset_id}_diffuse.ktx2",
               orm_uri=f"{asset_id}_orm.ktx2")
    print(f"  Wrote {out}")


def export_dmg_glb(
    asset_id: str,
    output_dir: Path,
    obj_b: bpy.types.Object,
) -> None:
    """Export <id>_dmg.glb (single node) for SceneRenderer damageMeshName."""
    dmg_name = f"{asset_id}_dmg"
    orig_obj  = obj_b.name
    orig_mesh = obj_b.data.name
    try:
        obj_b.name       = dmg_name
        obj_b.data.name  = dmg_name
        _select_only(obj_b)
        out = output_dir / f"{dmg_name}.glb"
        _raw_export(out, use_selection=True)
        _patch_glb(out,
                   diffuse_uri=f"{dmg_name}_diffuse.ktx2",
                   orm_uri=f"{dmg_name}_orm.ktx2")
        print(f"  Wrote {out}")
    finally:
        obj_b.name      = orig_obj
        obj_b.data.name = orig_mesh


def export_lod_glb(
    asset_id: str,
    lod_idx: int,
    output_dir: Path,
    length: float,
    wing_style: str,
    fuselage_steps: int,
    fuselage_rings: int,
) -> None:
    """Build + export a single LOD variant then clean it up from the scene."""
    lod_name = f"{asset_id}_lod{lod_idx}"
    bm = _build_mesh(length, wing_style,
                     fuselage_steps=fuselage_steps,
                     fuselage_rings=fuselage_rings)
    mesh = bpy.data.meshes.new(lod_name)
    bm.to_mesh(mesh)
    mesh.update()
    bm.free()

    obj = bpy.data.objects.new(lod_name, mesh)
    bpy.context.collection.objects.link(obj)
    mat = _make_material(lod_name, (0.55, 0.55, 0.60), 0.4, 0.8)
    _set_material(obj, mat)

    _select_only(obj)
    out = output_dir / f"{lod_name}.glb"
    _raw_export(out, use_selection=True)
    _patch_glb(out,
               diffuse_uri=f"{asset_id}_diffuse.ktx2",
               orm_uri=f"{asset_id}_orm.ktx2")
    print(f"  Wrote {out}")

    bpy.data.objects.remove(obj)
    bpy.data.meshes.remove(mesh)
    bpy.data.materials.remove(mat)

# ─── Texture helpers ──────────────────────────────────────────────────────────

def _setup_cycles_cpu() -> None:
    bpy.context.scene.render.engine  = "CYCLES"
    bpy.context.scene.cycles.device  = "CPU"
    bpy.context.scene.cycles.samples = 16
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "NONE"
    except (KeyError, AttributeError):
        pass


def bake_diffuse(
    name: str,
    output_dir: Path,
    obj: bpy.types.Object,
    mat: bpy.types.Material,
    tex_size: int,
) -> bool:
    """Bake diffuse colour to PNG via Cycles CPU.  Returns True on success."""
    _setup_cycles_cpu()

    img      = bpy.data.images.new(f"_bake_{name}", tex_size, tex_size)
    nodes    = mat.node_tree.nodes
    img_node = nodes.new("ShaderNodeTexImage")
    img_node.image  = img
    nodes.active    = img_node

    _select_only(obj)
    try:
        bpy.ops.object.bake(type="DIFFUSE", pass_filter={"COLOR"})
    except RuntimeError as exc:
        print(f"  Warning: diffuse bake failed ({exc}); skipping")
        nodes.remove(img_node)
        bpy.data.images.remove(img)
        return False

    out = output_dir / f"{name}_diffuse.png"
    img.filepath_raw = str(out)
    img.file_format  = "PNG"
    img.save()
    nodes.remove(img_node)
    bpy.data.images.remove(img)
    print(f"  Wrote {out}")
    return True


def write_orm_png(
    name: str,
    output_dir: Path,
    roughness: float,
    metallic: float,
    tex_size: int,
) -> None:
    """Write a solid-colour ORM PNG (R=AO=1, G=roughness, B=metallic)."""
    img = bpy.data.images.new(f"_orm_{name}", tex_size, tex_size, alpha=True)

    if _HAVE_NUMPY:
        px       = np.empty((tex_size, tex_size, 4), dtype=np.float32)
        px[..., 0] = 1.0
        px[..., 1] = roughness
        px[..., 2] = metallic
        px[..., 3] = 1.0
        img.pixels = px.flatten().tolist()
    else:
        # Fallback without numpy — slower but always works.
        pixel = [1.0, roughness, metallic, 1.0]
        img.pixels = pixel * (tex_size * tex_size)

    out = output_dir / f"{name}_orm.png"
    img.filepath_raw = str(out)
    img.file_format  = "PNG"
    img.save()
    bpy.data.images.remove(img)
    print(f"  Wrote {out}")

# ─── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    args = _parse_args()
    _validate_id(args.id)

    asset_id   = args.id
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    length     = args.length
    roughness  = 0.40
    metallic   = 0.80

    print(f"\n=== blender_gen.py  id={asset_id}  wing={args.wing_style}"
          f"  length={length}m  lod={args.lod}  bake={args.bake_textures} ===\n")

    _clear_scene()

    # ── 1. Clean mesh ──────────────────────────────────────────────────────────
    print("[1/4] Building clean mesh …")
    bm_clean = _build_mesh(length, args.wing_style)
    obj_clean = _commit(bm_clean, asset_id)
    bm_clean.free()
    mat_clean = _make_material(asset_id, (0.55, 0.55, 0.60), roughness, metallic)
    _set_material(obj_clean, mat_clean)

    # ── 2. Damage mesh (_b node, same file; also exported as standalone dmg) ──
    print("[2/4] Building damage mesh …")
    bm_dmg = _build_mesh(length, args.wing_style)
    _apply_damage(bm_dmg, args.seed)
    obj_dmg = _commit(bm_dmg, f"{asset_id}_b")
    bm_dmg.free()
    mat_dmg = _make_material(f"{asset_id}_b", (0.15, 0.10, 0.08), 0.90, 0.15)
    _set_material(obj_dmg, mat_dmg)

    # ── 3. Texture baking (optional) ──────────────────────────────────────────
    if args.bake_textures:
        print("[3/4] Baking textures …")
        bake_diffuse(asset_id, output_dir, obj_clean, mat_clean, args.tex_size)
        write_orm_png(asset_id, output_dir, roughness, metallic, args.tex_size)
        bake_diffuse(f"{asset_id}_dmg", output_dir, obj_dmg, mat_dmg, args.tex_size)
        write_orm_png(f"{asset_id}_dmg", output_dir, 0.90, 0.15, args.tex_size)
    else:
        print("[3/4] Skipping texture bake (pass --bake-textures to enable)")

    # ── 4. Export GLB files ────────────────────────────────────────────────────
    print("[4/4] Exporting GLB files …")

    # Main file: both objects present → validate-mesh sees <id> + <id>_b pair.
    # obj_clean was created first → meshes[0] in the exported glTF = clean mesh
    # which is what VkResources::createMesh reads.
    export_main_glb(asset_id, output_dir)

    # Separate damage file for SceneRenderer damageMeshName.
    export_dmg_glb(asset_id, output_dir, obj_dmg)

    # LOD variants (regenerated at lower resolution; no damage variant needed).
    if args.lod:
        lod_table = [
            (0, 12, 8),    # (index, fuselage_steps, fuselage_rings)
            (1,  8, 6),
            (2,  4, 4),
        ]
        for idx, fsteps, frings in lod_table:
            export_lod_glb(asset_id, idx, output_dir, length, args.wing_style,
                           fsteps, frings)

    # ── Summary ────────────────────────────────────────────────────────────────
    print(f"\nOutput directory: {output_dir}")
    if args.bake_textures:
        print("\nNext step — run tex-compress on the baked PNGs:")
        for stem in [asset_id, f"{asset_id}_dmg"]:
            print(f"  tex-compress --type diffuse {output_dir / (stem + '_diffuse.png')}")
            print(f"  tex-compress --type orm     {output_dir / (stem + '_orm.png')}")
        print("(Normal maps use the engine default — no bake needed for a placeholder.)")
    else:
        print("\nTip: pass --bake-textures to also generate diffuse/ORM PNGs for tex-compress.")
    print()


if __name__ == "__main__":
    main()
