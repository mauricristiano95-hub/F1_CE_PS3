"""F1 Championship Edition scene graphs (.sg): cars, helmets, drivers, garage models.

  python -I tools/sg.py tree <file.sg>                 node tree, meshes (vertex/triangle counts, material), materials
  python -I tools/sg.py obj <file.sg> <out.obj> [--only lod0,suspensionskinsfl]
                                                       Wavefront OBJ + MTL for Blender (one object per mesh node,
                                                       named by its node path, world transforms applied); embedded
                                                       textures written as DDS next to the .obj, used by the MTL.
                                                       --only keeps the meshes under the named nodes
  python -I tools/sg.py gltf <file.sg> <out.glb> [--only lod0]
                                                       glTF binary for Blender, like obj but with both UV sets
                                                       (UVMap = paint, UVMap.001 = decal sheet) and the textures
                                                       as upright PNGs next to it; the decal sheet of each
                                                       material is named in its extras (f1ce_decals): run
                                                       tools/blender_decals.py in Blender to layer it on the paint
  python -I tools/sg.py gltfimport <file.sg> <edited.glb> <out.sg> [--ref <exported.sg>]
                                                       as objimport, from a glTF exported by Blender: the decal
                                                       UVs (second UV map) are imported too
  python -I tools/sg.py objimport <file.sg> <edited.obj> <out.sg> [--ref <exported.sg>]
                                                       replace the geometry of the meshes named in the OBJ (as obj
                                                       writes them; any vertex/triangle count, new UVs); materials
                                                       stay, decal UVs (uv1) and occlusion come from the nearest
                                                       original vertex, tangents are computed. Skinned parts: no.
                                                       --ref: the OBJ was exported from another model with the same
                                                       parts (CarParts.sg for the garage <Team>06_fe.sg): its node
                                                       transforms place the geometry
  python -I tools/sg.py textures <file.sg> <outdir>    embedded GTF textures, under their original paths
  python -I tools/sg.py settex <file.sg> <texture> <new.png|.dds|.gtf> [<texture> <file> ...] <out.sg>
                                                       replace embedded textures (by node name, path or file name,
                                                       e.g. ferrari06_cb); DDS (DXT1/3/5 or BGRA) or PNG of any size

Format (big endian, no alignment):
  string   u32 length, chars, u32 hash
  matrix   12 floats: 3x3 rotation/scale rows, then translation (row vectors: p' = p * M + T)
  file     u16 1, u16 0x14, root (a Transform without its type string), then the resources (materials, textures)
  node     string type, then by type:
    Transform  string name, matrix, u16 1, u16 flags, u32 child count, children
    Mesh       u16 1, u16 3, <Transform body>, 42 bytes (... u32 index count, u16 primitive 4 = triangles),
               string material, 6 floats (bounds), u32 bone count, bones {string name, 16 floats bind matrix},
               u32 0, u32 n, n bytes image, u32 0
    Camera     u16 1, u16 0, u16 1, u16 0, <Transform body>, 24 bytes        Light   the same, 56 bytes
  mesh image (pointers are image offsets, listed in a relocation table {u32 count, count x u32} at the image end)
    +0x00 u32 size (relocation table not included)    +0x2a u16 batch count    +0x2c ptr vertex format
    +0x30 ptr table of batch pointers (meshes over 64k vertices or skinned with many bones are split in batches)
    batch +0x0 u16 stream count, u16 ?, u16 vertex count, u16 triangle count
          +0xc ptr stream table {u32 ptr data, u32 -1} per stream     +0x10 ptr u16 indices (triangle list)
    vertex format +0x2c ptr attributes, +0x31 u8 attribute count; attribute = {u32 name hash, u8 0xF0 | stream,
    u8 stride, u8 count << 4 | type (2 float, 3 half), u8 offset}
  The scene tree holds Transform and Mesh nodes (Camera and Light in some exported scenes); materials (UberShader),
  their ShaderObjects and the Texture nodes (original path + a complete GTF file) follow the tree.
"""
import json
import math
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gtf  # noqa: E402

# vertex attribute name hashes (identified from the data: value ranges and component counts)
ATTR_NAMES = {0x6DAE4277: 'position', 0x5C8364F9: 'uv0', 0x2B84546F: 'uv1', 0xBB3C4621: 'normal',
              0x1D8C6B74: 'tangent', 0xB16BA86F: 'occlusion'}   # occlusion: baked grey (0.1-1) vertex colour


# node types made of u16 1, u16 0, u16 1, u16 0, <Transform body> and a fixed-size tail (camera: near/far planes ...; light: colours)
NODE_TAIL = {'Camera': 24, 'Light': 56}


class Reader:
    def __init__(self, data: bytes, pos=0):
        self.d = data
        self.p = pos

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('>H', self.d, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('>I', self.d, self.p)[0]; self.p += 4; return v

    def f32s(self, n):
        v = struct.unpack_from(f'>{n}f', self.d, self.p); self.p += 4 * n; return v

    def bytes(self, n):
        v = self.d[self.p:self.p + n]; self.p += n; return v

    def string(self):
        n = self.u32()
        if n > 4096:
            raise ValueError(f'bad string length {n} at {self.p - 4:#x}')
        s = self.bytes(n).decode('latin1')
        self.u32()      # hash
        return s


class Node:
    def __init__(self, kind, name, matrix, flags, offset):
        self.kind, self.name, self.matrix, self.flags, self.offset = kind, name, matrix, flags, offset
        self.children = []
        self.mesh = None


class Mesh:
    """Decoded geometry of one Mesh node: per-vertex attribute lists and the triangle list, all batches joined
    (self.batches = (first vertex, vertex count, first triangle, triangle count) per batch)."""

    def __init__(self, material, header, bounds, image):
        self.material = material
        self.header = header
        self.bounds = bounds
        self.image = image
        img = image
        rd = lambda o: struct.unpack_from('>I', img, o)[0]
        fmt = rd(0x2C)
        attrs, nattr = rd(fmt + 0x2C), img[fmt + 0x31]
        layout = []
        for i in range(nattr):
            h, sel, stride, ty, off = struct.unpack_from('>IBBBB', img, attrs + 8 * i)
            layout.append((ATTR_NAMES.get(h, f'attr_{h:08x}'), sel, stride, ty, off))
        self.layout = layout
        self.skinned = any(sel & 0xF0 != 0xF0 for _, sel, *_ in layout)
        self.attrs = {name: [] for name, *_ in layout}
        self.tris, self.batches = [], []
        self.nverts = self.ntris = 0
        nbatches = struct.unpack_from('>H', img, 0x2A)[0]
        for b in range(nbatches):
            bh = rd(rd(0x30) + 4 * b)
            nstreams, _, nv, nt = struct.unpack_from('>HHHH', img, bh)
            streams = [rd(rd(bh + 0xC) + 8 * i) for i in range(nstreams)]
            for name, sel, stride, ty, off in layout:
                # 0xF0 | n = stream n; skinned meshes mark the attributes the skinning transforms (1 position,
                # 2 normal, 3 ...) instead, all interleaved in stream 0
                stream = streams[sel & 15] if sel & 0xF0 == 0xF0 else streams[0]
                vals = self._read(stream, stride, ty >> 4, ty & 15, off, nv)
                if vals is None:
                    self.attrs[name] = None
                elif self.attrs[name] is not None:
                    self.attrs[name] += vals
            idx = struct.unpack_from(f'>{3 * nt}H', img, rd(bh + 0x10))
            base = self.nverts
            self.tris += [(idx[i] + base, idx[i + 1] + base, idx[i + 2] + base) for i in range(0, len(idx), 3)]
            self.batches.append((base, nv, self.ntris, nt))
            self.nverts += nv
            self.ntris += nt

    def _read(self, base, stride, count, ty, off, n):
        code = {2: 'f', 3: 'e', 1: 'B', 4: 'h', 5: 'H'}.get(ty)
        if code is None:
            return None
        return [struct.unpack_from(f'>{count}{code}', self.image, base + v * stride + off) for v in range(n)]


class Material:
    def __init__(self, name, offset):
        self.name, self.offset = name, offset
        self.samplers = {}      # slot (BaseColour, BaseColour2, Normal, Opacity, ...) -> texture node name


class Texture:
    def __init__(self, name, path, offset):
        self.name, self.path, self.offset = name, path, offset
        self.gtf = None         # gtf.Gtf when the node embeds the texture
        self.size_at = self.gtf_at = self.gtf_end = None


class SceneGraph:
    def __init__(self, data: bytes):
        self.data = data
        r = Reader(data)
        ver, _ = r.u16(), r.u16()
        if ver != 1:
            raise ValueError('not a scene graph')
        self.root = self._transform_body(r, 'Transform', 0)
        self.resources_at = r.p
        self.materials = self._find_materials()
        self.textures = self._find_textures()

    def _transform_body(self, r, kind, offset):
        name = r.string()
        m = r.f32s(12)
        _, flags = r.u16(), r.u16()
        n = r.u32()
        node = Node(kind, name, m, flags, offset)
        for _ in range(n):
            node.children.append(self._node(r))
        return node

    def _node(self, r):
        at = r.p
        kind = r.string()
        if kind == 'Transform':
            return self._transform_body(r, kind, at)
        if kind == 'Mesh':
            r.u16(); r.u16()
            node = self._transform_body(r, kind, at)
            hdr_at = r.p
            header = r.bytes(42)
            if struct.unpack_from('>I', header, 28)[0] != 2:
                # 2 = prebuilt GPU image (every car, helmet, driver); the front-end avatar body (and the tracks?) use
                # another layout with an inline attribute list, not decoded yet
                raise ValueError(f'mesh {node.name} at {at:#x}: storage kind '
                                 f'{struct.unpack_from(">I", header, 28)[0]} not handled')
            material = r.string()
            bounds_at = r.p
            bounds = r.f32s(6)
            bones = [(r.string(), r.f32s(16)) for _ in range(r.u32())]
            r.u32()
            size_at = r.p
            n = r.u32()
            image = r.bytes(n)
            node.mesh_at = (hdr_at, bounds_at, size_at, r.p)      # for rewriting the geometry
            r.u32()
            try:
                node.mesh = Mesh(material, header, bounds, image)
                node.mesh.bones = bones
            except (struct.error, IndexError, KeyError) as e:
                print(f'warning: mesh {node.name} at {at:#x} not decoded ({e})', file=sys.stderr)
            return node
        if kind in NODE_TAIL:
            r.u16(); r.u16(); r.u16(); r.u16()
            node = self._transform_body(r, kind, at)
            node.extra = r.bytes(NODE_TAIL[kind])
            return node
        raise ValueError(f'node type {kind!r} at {at:#x} not handled')

    def _find_materials(self):
        """UberShader nodes: string 'UberShader', u16 1, u16 ?, string name, parameters. A sampler parameter is
        u32 length + texture node name, u32 length + "Sampler<Slot>Texture", u32 hash."""
        d = self.data
        starts = [self.resources_at + m.start() for m in
                  re.finditer(rb'\x00\x00\x00\x0aUberShader', d[self.resources_at:])]
        out = []
        for i, at in enumerate(starts):
            end = starts[i + 1] if i + 1 < len(starts) else len(d)
            r = Reader(d, at + 4 + 10 + 4)
            r.u16(); r.u16()
            mat = Material(r.string(), at)
            for m in re.finditer(rb'\x00\x00\x00[\x10-\x40](Sampler\w+?Texture)', d[at:end]):
                s = at + m.start()
                slot = m.group(1).decode()[7:-7]
                # the texture name ends right before it (u32 length, chars; neither string has a hash here)
                for ln in range(1, 64):
                    p = s - 4 - ln
                    if p >= 0 and struct.unpack_from('>I', d, p)[0] == ln:
                        mat.samplers.setdefault(slot, d[p + 4:p + 4 + ln].decode('latin1'))
                        break
            out.append(mat)
        return out

    def _find_textures(self):
        """Texture nodes: string 'Texture', u16 1, u16 1, string name, u32 length + path (no hash), u32 kind:
        0x25 = u32 size + a GTF file of that size follows, 6 = no data (placeholder)."""
        d = self.data
        out = []
        for m in re.finditer(rb'\x00\x00\x00\x07Texture', d[self.resources_at:]):
            at = self.resources_at + m.start()
            r = Reader(d, at + 4 + 7 + 4)
            try:
                r.u16(); r.u16()
                name = r.string()
                path = r.bytes(r.u32()).decode('latin1')
                kind = r.u32()
                tex = Texture(name, path, at)
                if kind == 0x25:
                    tex.size_at = r.p
                    n = r.u32()
                    tex.gtf_at, tex.gtf_end = r.p, r.p + n
                    tex.gtf = gtf.Gtf(d[r.p:r.p + n])
                out.append(tex)
            except (ValueError, struct.error, UnicodeDecodeError):
                continue
        return out

    def walk(self, node=None, parent=None, depth=0, path=()):
        """(node, world matrix, depth, names of the node's ancestors below the root) in file order."""
        node = node or self.root
        world = mat_mul(node.matrix, parent) if parent else node.matrix
        yield node, world, depth, path
        for c in node.children:
            yield from self.walk(c, world, depth + 1, path + (node.name,) if depth else path)


def mat_mul(a, b):
    """Row-vector affine matrices (12 floats: 3x3 then translation): a then b."""
    ra = [a[0:3], a[3:6], a[6:9]]
    rb = [b[0:3], b[3:6], b[6:9]]
    out = []
    for i in range(3):
        out += [sum(ra[i][k] * rb[k][j] for k in range(3)) for j in range(3)]
    t = a[9:12]
    out += [sum(t[k] * rb[k][j] for k in range(3)) + b[9 + j] for j in range(3)]
    return out


def xform(p, m):
    return [p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9],
            p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
            p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11]]


def xform_dir(n, m):
    v = [n[0] * m[0] + n[1] * m[3] + n[2] * m[6],
         n[0] * m[1] + n[1] * m[4] + n[2] * m[7],
         n[0] * m[2] + n[1] * m[5] + n[2] * m[8]]
    ln = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / ln for c in v]


def cmd_tree(sg):
    for node, world, depth, path in sg.walk():
        line ='  ' * depth + f'{node.kind} {node.name}'
        if node.mesh:
            m = node.mesh
            line += f'  [{m.nverts} verts, {m.ntris} tris, material {m.material}, attrs {",".join(m.attrs)}]'
        elif node.kind == 'Mesh':
            line += '  [not decoded]'
        print(line)
    print(f'\nmaterials ({len(sg.materials)}):')
    for m in sg.materials:
        print(f'  {m.name}: ' + ', '.join(f'{k}={v}' for k, v in m.samplers.items()))
    print(f'textures ({len(sg.textures)}):')
    for t in sg.textures:
        g = t.gtf
        print(f'  {t.name}: {t.path}  ' + (f'{g.kind} {g.w}x{g.h} mips {g.mips}' if g else '(not embedded)'))


def safe(s):
    return re.sub(r'[^\w.\-]', '_', s)


def cmd_textures(sg, outdir):
    out = Path(outdir)
    for t in sg.textures:
        if t.gtf:
            p = out / safe_path(t.path)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(sg.data[t.gtf_at:t.gtf_end])
            print(p)


def safe_path(path):
    parts = [safe(x) for x in path.replace('\\', '/').split('/') if x not in ('', '.', '..')]
    return Path(*parts) if parts else Path('texture.gtf')


def cmd_obj(sg, out_obj, only=None):
    """only: node names; a mesh is exported when it or one of its ancestors has one of them (e.g. lod0)."""
    out_obj = Path(out_obj)
    tex_dir = out_obj.parent / (out_obj.stem + '_textures')
    mats = {m.name: m for m in sg.materials}
    lines = [f'# {sg.root.name} exported by tools/sg.py', f'mtllib {out_obj.stem}.mtl']
    vbase = tbase = nbase = 1
    used = {}
    for node, world, depth, path in sg.walk():
        m = node.mesh
        if not m or 'position' not in m.attrs:
            continue
        if only and not only.intersection(path + (node.name,)):
            continue
        # object name = node path below the root, so the parts of lod0, lod1, damage, ... stay apart in Blender
        lines.append(f'o {".".join(safe(n) for n in path + (node.name,))}')
        for p in m.attrs['position']:
            x, y, z = xform(p, world)
            lines.append(f'v {x:.6f} {y:.6f} {z:.6f}')
        # OBJ has one UV set: uv0 (base paint). Liveries also have uv1 for the decal sheet (BaseColour2)
        uv = m.attrs.get('uv0')
        if uv:
            lines += [f'vt {u:.6f} {1.0 - v:.6f}' for u, v in (t[:2] for t in uv)]
        nrm = m.attrs.get('normal')
        if nrm:
            for n in nrm:
                x, y, z = xform_dir(n, world)
                lines.append(f'vn {x:.5f} {y:.5f} {z:.5f}')
        mname = safe(m.material)
        used[mname] = m.material
        lines.append(f'usemtl {mname}')
        # v, vt and vn are numbered separately: meshes without UVs (windscreens) write no vt lines
        for tri in m.tris:
            lines.append('f ' + ' '.join(f'{k + vbase}/{k + tbase if uv else ""}/{k + nbase if nrm else ""}'
                                         .rstrip('/') for k in tri))
        vbase += m.nverts
        tbase += m.nverts if uv else 0
        nbase += m.nverts if nrm else 0
    out_obj.write_text('\n'.join(lines) + '\n', newline='\n')
    texs = {t.name: t for t in sg.textures}
    written = {}

    def tex_file(tname):
        t = texs.get(tname)
        if t is None or t.gtf is None:
            return None
        if t.path not in written:
            p = tex_dir / safe_path(t.path).with_suffix('.dds')
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(gtf.to_dds(t.gtf))
            written[t.path] = p.relative_to(out_obj.parent).as_posix()
        return written[t.path]

    mtl = []
    for mname, orig in used.items():
        mtl += [f'newmtl {mname}', 'Kd 0.8 0.8 0.8', 'Ks 0.2 0.2 0.2']
        smp = mats[orig].samplers if orig in mats else {}
        for slot, key in (('BaseColour', 'map_Kd'), ('Normal', 'map_Bump'), ('Opacity', 'map_d'),
                          ('SpecularIntensity', 'map_Ks'), ('EmissiveColour', 'map_Ke'), ('Reflectivity', 'map_refl')):
            f = tex_file(smp.get(slot))
            if f:
                mtl.append(f'{key} {f}')
        f = tex_file(smp.get('BaseColour2'))
        if f:
            mtl.append(f'# second colour layer (sponsor decals, second UV set, not in OBJ): {f}')
        mtl.append('')
    (out_obj.parent / (out_obj.stem + '.mtl')).write_text('\n'.join(mtl), newline='\n')
    print(f'{out_obj}: {vbase - 1} vertices, {len(used)} materials, {len(written)} textures')


def det3(m):
    return (m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
            m[2] * (m[3] * m[7] - m[4] * m[6]))


def cmd_gltf(sg, out_glb, only=None, texture_file=None):
    """glTF 2.0 binary: the scene tree below the root (node names and local matrices; with only, the branches that
    lead to the exported meshes), a mesh per mesh node, TEXCOORD_0 = uv0 (paint), TEXCOORD_1 = uv1 (decal sheet),
    the textures as upright PNG files (glTF v = 1 - v).
    Materials: BaseColour as the base colour texture; the decal sheet (BaseColour2) has no glTF slot, so it is
    named in the material's extras (f1ce_decals: PNG path relative to the .glb, f1ce_base: the paint's), which
    Blender keeps as custom properties for tools/blender_decals.py.
    texture_file: optional function(Texture) -> Path of a shared or existing PNG to use (written only when missing),
    or None for the default <out>_textures/<original path>.png (always written)."""
    out_glb = Path(out_glb)
    tex_dir = out_glb.parent / (out_glb.stem + '_textures')
    mats = {m.name: m for m in sg.materials}
    texs = {t.name: t for t in sg.textures}
    blob = bytearray()
    views, accessors, images, textures, materials, meshes, nodes = [], [], [], [], [], [], []
    pngs, gl_tex, mat_index = {}, {}, {}

    def add(fmt, values, count, typ, target, minmax=False):
        blob.extend(bytes(-len(blob) % 4))
        data = struct.pack(f'<{len(values)}{fmt}', *values)
        views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(data), 'target': target})
        blob.extend(data)
        acc = {'bufferView': len(views) - 1, 'componentType': {'f': 5126, 'H': 5123, 'I': 5125}[fmt],
               'count': count, 'type': typ}
        if minmax:
            n = len(values) // count
            acc['min'] = [min(values[i::n]) for i in range(n)]
            acc['max'] = [max(values[i::n]) for i in range(n)]
        accessors.append(acc)
        return len(accessors) - 1

    def png(tname):
        """The PNG of a texture node (relative URI), written on first use; None when not embedded/decodable and
        texture_file has no existing PNG for it (the wheels name their swapped textures without embedding them)."""
        t = texs.get(tname)
        if t is None or (t.gtf is not None and t.gtf.cube):
            return None
        if t.path not in pngs:
            f = texture_file(t) if texture_file else None
            if f is None or not Path(f).exists():
                if t.gtf is None:
                    pngs[t.path] = None
                    return None
                f = Path(f or tex_dir / safe_path(t.path).with_suffix('.png'))
                try:
                    w, h, px = gtf.decode_level(t.gtf)
                except (KeyError, ValueError, IndexError):
                    pngs[t.path] = None
                    return None
                f.parent.mkdir(parents=True, exist_ok=True)
                gtf.write_png(f, w, h, gtf.flip_rows(w, h, px))
            pngs[t.path] = os.path.relpath(f, out_glb.parent).replace('\\', '/')
        return pngs[t.path]

    def texture(uri):
        if uri not in gl_tex:
            images.append({'uri': uri, 'name': uri.split('/')[-1].rsplit('.', 1)[0]})
            textures.append({'source': len(images) - 1})
            gl_tex[uri] = len(textures) - 1
        return gl_tex[uri]

    def material(name):
        if name not in mat_index:
            smp = mats[name].samplers if name in mats else {}
            pbr = {'metallicFactor': 0.0, 'roughnessFactor': 0.5}
            mat = {'name': name, 'pbrMetallicRoughness': pbr, 'doubleSided': True}
            extras = {}
            base = png(smp.get('BaseColour'))
            if base:
                pbr['baseColorTexture'] = {'index': texture(base)}
                extras['f1ce_base'] = base
            if 'Opacity' in smp:
                mat['alphaMode'] = 'BLEND'
            decals = png(smp.get('BaseColour2'))
            if decals:
                extras['f1ce_decals'] = decals
            if extras:
                mat['extras'] = extras
            materials.append(mat)
            mat_index[name] = len(materials) - 1
        return mat_index[name]

    def exported(node, path):
        m = node.mesh
        return bool(m and 'position' in m.attrs and (not only or only.intersection(path + (node.name,))))

    def wanted(node, path):
        return exported(node, path) or any(wanted(c, path + (node.name,)) for c in node.children)

    nverts = 0

    def visit(node, path):
        """glTF node of a scene node below the root (its local matrix; glTF turns the triangles of mirrored nodes
        itself), with the wanted children; returns its index."""
        nonlocal nverts
        gl = {'name': safe(node.name)}
        mx = node.matrix
        if list(mx) != [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]:
            gl['matrix'] = [mx[0], mx[1], mx[2], 0, mx[3], mx[4], mx[5], 0, mx[6], mx[7], mx[8], 0,
                            mx[9], mx[10], mx[11], 1]
        if exported(node, path):
            m = node.mesh
            n = m.nverts
            attrs = {'POSITION': add('f', [c for p in m.attrs['position'] for c in p[:3]], n, 'VEC3', 34962,
                                     minmax=True)}
            if m.attrs.get('normal'):
                attrs['NORMAL'] = add('f', [c for v in m.attrs['normal'] for c in _norm(v[:3])], n, 'VEC3', 34962)
            for k, uv in enumerate((m.attrs.get('uv0'), m.attrs.get('uv1'))):
                if uv:
                    attrs[f'TEXCOORD_{k}'] = add('f', [c for t in uv for c in (t[0], 1.0 - t[1])], n, 'VEC2', 34962)
            flat = [k for t in m.tris for k in t]
            prim = {'attributes': attrs, 'material': material(m.material), 'mode': 4,
                    'indices': add('H' if n <= 0xFFFF else 'I', flat, len(flat), 'SCALAR', 34963)}
            meshes.append({'name': gl['name'], 'primitives': [prim]})
            gl['mesh'] = len(meshes) - 1
            nverts += n
        nodes.append(gl)
        at = len(nodes) - 1
        kids = [visit(c, path + (node.name,)) for c in node.children if wanted(c, path + (node.name,))]
        if kids:
            gl['children'] = kids
        return at

    roots = [visit(c, ()) for c in sg.root.children if wanted(c, ())]
    doc = {'asset': {'version': '2.0', 'generator': 'F1 CE tools/sg.py'}, 'scene': 0,
           'scenes': [{'name': sg.root.name, 'nodes': roots}], 'nodes': nodes, 'meshes': meshes,
           'materials': materials, 'textures': textures, 'images': images, 'samplers': [],
           'accessors': accessors, 'bufferViews': views, 'buffers': [{'byteLength': len(blob)}]}
    doc = {k: v for k, v in doc.items() if v != []}
    js = json.dumps(doc, separators=(',', ':')).encode()
    js += b' ' * (-len(js) % 4)
    blob.extend(bytes(-len(blob) % 4))
    out_glb.write_bytes(struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(js) + 8 + len(blob)) +
                        struct.pack('<I4s', len(js), b'JSON') + js + struct.pack('<I4s', len(blob), b'BIN\0') + blob)
    print(f'{out_glb}: {nverts} vertices, {len(materials)} materials, {len(images)} textures')


def find_texture(sg, key):
    """A Texture node by node name (file195), path (textures/ferrari06_cb.gtf) or file name (ferrari06_cb)."""
    k = key.replace('\\', '/').lower()
    for t in sg.textures:
        p = t.path.replace('\\', '/').lower()
        if k in (t.name.lower(), p, p.split('/')[-1], p.split('/')[-1].rsplit('.', 1)[0]):
            return t
    raise ValueError(f'no texture {key!r}; textures: ' + ', '.join(f'{t.name} ({t.path})' for t in sg.textures))


def replace_textures(data: bytes, changes) -> bytes:
    """New .sg bytes with embedded textures replaced: changes = [(key, new GTF bytes)]. Any size or format:
    the Texture node's size field is rewritten and the rest of the file moves (nothing in it holds file offsets)."""
    sg = SceneGraph(data)
    edits = []
    for key, new in changes:
        t = find_texture(sg, key)
        if t.gtf is None:
            raise ValueError(f'texture {key!r} is not embedded in this model ({t.path})')
        gtf.Gtf(new)        # validates
        edits.append((t.size_at, t.gtf_end, struct.pack('>I', len(new)) + new))
    out = bytearray()
    pos = 0
    for start, end, repl in sorted(edits):
        out += data[pos:start] + repl
        pos = end
    out += data[pos:]
    return bytes(out)


def texture_from_file(path, like: gtf.Gtf, flip=False):
    """GTF bytes from a .gtf, a .dds (any DXT1/3/5 or BGRA size/mips) or a .png (any size, encoded in the
    original's format with a new mip chain; flip: the PNG is upright, the game's textures are stored upside down);
    the channel remap is taken from the original."""
    data = Path(path).read_bytes()
    if data[:4] == b'\x89PNG':
        w, h, px = gtf.read_png(path)
        return gtf.gtf_from_rgba(w, h, gtf.flip_rows(w, h, px) if flip else px, like)
    if data[:4] == b'DDS ':
        return gtf.build_from_dds(data, like.remap)
    gtf.Gtf(data)
    return data


# ---------------------------------------------------------------------------------------------------- OBJ import
# A batch's vertices and indices must fit the budget of the game's biggest batches (77824 bytes, 1728 vertices),
# probably the SPU buffer the meshes are processed in.
BATCH_BYTES, BATCH_VERTS = 0x12000, 1700


def parse_obj(text):
    """{object name: [face = [(v, vt, vn) zero-based or None]]}, positions, uvs, normals."""
    v, vt, vn, objects = [], [], [], {}
    cur = None
    have_o = any(line.startswith('o ') for line in text.splitlines())
    for line in text.splitlines():
        if line.startswith('v '):
            v.append(tuple(float(x) for x in line.split()[1:4]))
        elif line.startswith('vt '):
            p = line.split()
            vt.append((float(p[1]), float(p[2]) if len(p) > 2 else 0.0))
        elif line.startswith('vn '):
            vn.append(tuple(float(x) for x in line.split()[1:4]))
        elif line.startswith('o ') or (line.startswith('g ') and not have_o):
            cur = line[2:].strip()
        elif line.startswith('f '):
            face = []
            for tok in line.split()[1:]:
                a = (tok.split('/') + ['', ''])[:3]
                fix = lambda s, n: None if not s else (int(s) - 1 if int(s) > 0 else n + int(s))  # noqa: E731
                face.append((fix(a[0], len(v)), fix(a[1], len(vt)), fix(a[2], len(vn))))
            objects.setdefault(cur, []).append(face)
    return objects, v, vt, vn


def mat_inverse(m):
    a = [m[0:3], m[3:6], m[6:9]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3] -
             a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)] for i in range(3)]
    flat = inv[0] + inv[1] + inv[2]
    t = m[9:12]
    return flat + [-sum(t[k] * inv[k][j] for k in range(3)) for j in range(3)]


def _norm(v):
    ln = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / ln for c in v]


class _Nearest:
    """Nearest original vertex (position; among coincident ones the closest uv0, normal and uv1 when known) for the
    attributes the file does not carry (uv1 in OBJ, occlusion)."""

    def __init__(self, pos, uv, nrm=None, uv1=None):
        self.pos, self.uv, self.nrm, self.uv1 = pos, uv, nrm, uv1
        mn = [min(p[i] for p in pos) for i in range(3)]
        mx = [max(p[i] for p in pos) for i in range(3)]
        self.cell = max(max(mx[i] - mn[i] for i in range(3)) / 24, 1e-4)
        self.grid = {}
        for i, p in enumerate(pos):
            self.grid.setdefault(self._key(p), []).append(i)

    def _key(self, p):
        return tuple(int(math.floor(c / self.cell)) for c in p)

    def find(self, p, uv, n=None, uv1=None):
        k = self._key(p)
        ring = lambda r: [i for dx in range(-r, r + 1) for dy in range(-r, r + 1) for dz in range(-r, r + 1)  # noqa
                          if max(abs(dx), abs(dy), abs(dz)) == r
                          for i in self.grid.get((k[0] + dx, k[1] + dy, k[2] + dz), ())]
        for r in range(0, 64):
            cands = ring(r)
            if cands:
                cands += ring(r + 1)      # a closer vertex can sit in the next ring of cells
                break
        else:
            cands = range(len(self.pos))
        d2 = {i: sum((self.pos[i][j] - p[j]) ** 2 for j in range(3)) for i in cands}
        best = min(d2.values())
        same = [i for i in cands if d2[i] <= best + 1e-10]      # coincident vertices (UV seams): closest UV
        if len(same) == 1:
            return same[0]
        # UV seams and hard edges: same position, different uv0, normal or decal uv1
        return min(same, key=lambda i: (sum((self.uv[i][j] - uv[j]) ** 2 for j in range(2)) if self.uv and uv else 0) +
                   (sum((self.nrm[i][j] - n[j]) ** 2 for j in range(3)) if self.nrm and n else 0) +
                   (sum((self.uv1[i][j] - uv1[j]) ** 2 for j in range(2)) if self.uv1 and uv1 else 0))


def obj_meshes(text):
    """{object name: (positions, uv0, uv1, normals, triangles)} of an OBJ: world space, game UVs (v down), no uv1;
    normals None when some corner has none."""
    objects, v, vt, vn = parse_obj(text)
    out = {}
    for name, faces in objects.items():
        verts, index, tris = [], {}, []
        for face in faces:
            ids = []
            for corner in face:
                if corner not in index:
                    index[corner] = len(verts)
                    verts.append(corner)
                ids.append(index[corner])
            tris += [(ids[0], ids[k], ids[k + 1]) for k in range(1, len(ids) - 1)]
        uv = [(vt[c[1]][0], 1.0 - vt[c[1]][1]) if c[1] is not None else (0.0, 0.0) for c in verts]
        nrm = [vn[c[2]] for c in verts] if all(c[2] is not None for c in verts) else None
        out[name] = ([v[c[0]] for c in verts], uv, None, nrm, tris)
    return out


def _quat_matrix(t, q, s):
    """glTF translation / rotation quaternion (x, y, z, w) / scale -> row-vector affine matrix (12 floats)."""
    x, y, z, w = q
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    # column vectors p' = R S p + t  ->  rows of (R S)^T
    return [r[j][i] * s[i] for i in range(3) for j in range(3)] + list(t)


def gltf_meshes(data: bytes, base_dir=None):
    """{node path: (positions, uv0, uv1, normals, triangles)} of a glTF 2.0 (.glb, or .gltf with its buffers as
    files in base_dir or data: URIs): node path = the names from the scene's top node down, joined by dots (as
    mesh_paths; Blender's .001 suffixes dropped), node transforms applied (world space), game UVs (v down, from
    glTF 1 - v), uv1 = TEXCOORD_1 when there is one, the primitives of a mesh joined."""
    import base64
    bins = []
    if data[:4] == b'glTF':
        p, doc = 12, None
        while p < len(data):
            n, kind = struct.unpack_from('<I4s', data, p)
            chunk = data[p + 8:p + 8 + n]
            if kind == b'JSON':
                doc = json.loads(chunk)
            elif kind == b'BIN\0':
                bins.append(chunk)
            p += 8 + n
    else:
        doc = json.loads(data)
    buffers = []
    for i, b in enumerate(doc.get('buffers', [])):
        uri = b.get('uri')
        if uri is None:
            buffers.append(bins[0] if bins else b'')
        elif uri.startswith('data:'):
            buffers.append(base64.b64decode(uri.split(',', 1)[1]))
        else:
            from urllib.parse import unquote
            buffers.append((Path(base_dir or '.') / unquote(uri)).read_bytes())
    sizes = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
    codes = {5120: ('b', 127), 5121: ('B', 255), 5122: ('h', 32767), 5123: ('H', 65535), 5125: ('I', 0),
             5126: ('f', 0)}

    def read(i):
        a = doc['accessors'][i]
        n, (code, scale) = sizes[a['type']], codes[a['componentType']]
        if 'bufferView' not in a or 'sparse' in a:
            raise ValueError('sparse or empty accessors are not handled (export without them)')
        bv = doc['bufferViews'][a['bufferView']]
        buf = buffers[bv['buffer']]
        csize = struct.calcsize(code)
        stride = bv.get('byteStride') or n * csize
        start = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        out = [struct.unpack_from(f'<{n}{code}', buf, start + k * stride) for k in range(a['count'])]
        if a.get('normalized') and scale:
            out = [tuple(max(c / scale, -1.0) for c in v) for v in out]
        return out

    out = {}

    def visit(i, parent, path):
        nd = doc['nodes'][i]
        path = path + (re.sub(r'\.\d{3}$', '', nd.get('name') or f'node{i}'),)
        if 'matrix' in nd:
            mm = nd['matrix']       # column major, column vectors
            local = [mm[0], mm[1], mm[2], mm[4], mm[5], mm[6], mm[8], mm[9], mm[10], mm[12], mm[13], mm[14]]
        else:
            local = _quat_matrix(nd.get('translation', (0, 0, 0)), nd.get('rotation', (0, 0, 0, 1)),
                                 nd.get('scale', (1, 1, 1)))
        world = mat_mul(local, parent) if parent else local
        if 'mesh' in nd:
            name = '.'.join(path)
            pos, uv0, uv1, nrm, tris = [], [], [], [], []
            flip = det3(world) < 0
            for prim in doc['meshes'][nd['mesh']]['primitives']:
                if prim.get('mode', 4) != 4:
                    continue
                at = prim['attributes']
                p = read(at['POSITION'])
                base = len(pos)
                pos += [xform(v, world) for v in p]
                nrm += [xform_dir(v, world) for v in read(at['NORMAL'])] if 'NORMAL' in at else [None] * len(p)
                for key, dst in (('TEXCOORD_0', uv0), ('TEXCOORD_1', uv1)):
                    dst += [(u, 1.0 - v) for u, v in read(at[key])] if key in at else [None] * len(p)
                idx = [k[0] for k in read(prim['indices'])] if 'indices' in prim else list(range(len(p)))
                tris += [(base + idx[k], base + idx[k + 2], base + idx[k + 1]) if flip else
                         (base + idx[k], base + idx[k + 1], base + idx[k + 2]) for k in range(0, len(idx) - 2, 3)]
            if pos:
                out[name] = (pos, [u or (0.0, 0.0) for u in uv0], uv1 if all(uv1) else None,
                             nrm if all(nrm) else None, tris)
        for c in nd.get('children', []):
            visit(c, world, path)

    scene = doc.get('scenes', [{}])[doc.get('scene', 0)] if doc.get('scenes') else {}
    roots = scene.get('nodes')
    if roots is None:
        kids = {c for nd in doc.get('nodes', []) for c in nd.get('children', [])}
        roots = [i for i in range(len(doc.get('nodes', []))) if i not in kids]
    for i in roots:
        visit(i, None, ())
    return out


def geometry_from_verts(mesh: Mesh, world, pos_w, uv, uv1, nrm_w, tris):
    """Vertices (attribute name -> value per vertex) and triangles of one object (world-space positions and normals,
    game UVs; uv1 and normals may be None), in the node's own space."""
    inv = mat_inverse(world)
    rot = [world[0:3], world[3:6], world[6:9]]
    pos = [xform(p, inv) for p in pos_w]
    verts = pos
    if nrm_w:
        # world -> node space: n * R^T (the rotation part of the node's world matrix)
        nrm = [_norm([sum(n[i] * rot[j][i] for i in range(3)) for j in range(3)]) for n in nrm_w]
    else:
        acc = [[0.0, 0.0, 0.0] for _ in verts]
        for a, b, c in tris:
            e1 = [pos[b][i] - pos[a][i] for i in range(3)]
            e2 = [pos[c][i] - pos[a][i] for i in range(3)]
            fn = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
            for k in (a, b, c):
                acc[k] = [acc[k][i] + fn[i] for i in range(3)]
        nrm = [_norm(n) for n in acc]
    # tangents from the UVs (per triangle, summed per vertex, made orthogonal to the normal)
    tan = [[0.0, 0.0, 0.0] for _ in verts]
    bit = [[0.0, 0.0, 0.0] for _ in verts]
    for a, b, c in tris:
        e1 = [pos[b][i] - pos[a][i] for i in range(3)]
        e2 = [pos[c][i] - pos[a][i] for i in range(3)]
        du1, dv1 = uv[b][0] - uv[a][0], uv[b][1] - uv[a][1]
        du2, dv2 = uv[c][0] - uv[a][0], uv[c][1] - uv[a][1]
        d = du1 * dv2 - du2 * dv1
        if abs(d) < 1e-12:
            continue
        r = 1.0 / d
        t = [(e1[i] * dv2 - e2[i] * dv1) * r for i in range(3)]
        bt = [(e2[i] * du1 - e1[i] * du2) * r for i in range(3)]
        for k in (a, b, c):
            tan[k] = [tan[k][i] + t[i] for i in range(3)]
            bit[k] = [bit[k][i] + bt[i] for i in range(3)]
    tangent = []
    for k in range(len(verts)):
        n, t = nrm[k], tan[k]
        dot = sum(n[i] * t[i] for i in range(3))
        t = [t[i] - n[i] * dot for i in range(3)]
        if sum(c * c for c in t) < 1e-12:
            t = [1.0, 0.0, 0.0] if abs(n[0]) < 0.9 else [0.0, 1.0, 0.0]
        t = _norm(t)
        cr = [n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]]
        w = 1.0 if sum(cr[i] * bit[k][i] for i in range(3)) >= 0 else -1.0
        tangent.append((t[0], t[1], t[2], w))
    out = {'position': pos, 'uv0': uv, 'normal': nrm, 'tangent': tangent}
    if uv1 and 'uv1' in mesh.attrs:
        out['uv1'] = uv1
    near = _Nearest(mesh.attrs['position'], mesh.attrs.get('uv0'), mesh.attrs.get('normal'), mesh.attrs.get('uv1'))
    nearest = [near.find(pos[k], uv[k], nrm[k], uv1[k] if uv1 else None) for k in range(len(verts))]
    for name, *_ in mesh.layout:
        if name not in out:
            src = mesh.attrs.get(name)
            if src is None:
                raise ValueError(f'attribute {name} of type not handled')
            out[name] = [src[i] for i in nearest]

    def same(name, k, i, n, eps):
        src = mesh.attrs.get(name)
        return not src or name not in out or sum((src[i][j] - out[name][k][j]) ** 2 for j in range(n)) < eps
    # unchanged: every vertex on an original one, with its UVs, and the same triangles on the original vertices
    # (Blender re-indexes and splits vertices, the transforms add noise, UVs are stored as halves); original vertices
    # the file cannot tell apart (repeated ones; decal seams in an OBJ, which has no uv1) count as one
    first = {}
    keys = ('position', 'uv0', 'normal') + (('uv1',) if uv1 else ())
    canon = [first.setdefault(tuple(tuple(mesh.attrs[a][i]) if mesh.attrs.get(a) else () for a in keys), i)
             for i in range(mesh.nverts)]

    def tri_key(t):
        t = tuple(canon[i] for i in t)
        k = t.index(min(t))
        return t[k:] + t[:k]
    out['_unchanged'] = len(tris) == mesh.ntris and all(
        same('position', k, i, 3, 1e-8) and same('uv0', k, i, 2, 1e-6) and same('uv1', k, i, 2, 1e-6)
        for k, i in enumerate(nearest)) and (
        sorted(tri_key(tuple(nearest[k] for k in t)) for t in tris) == sorted(tri_key(t) for t in mesh.tris))
    return out, tris


def split_batches(mesh: Mesh, tris):
    """Triangles grouped in batches within the size budget: [(vertex ids, local triangles)]."""
    stride = sum({sel: st for _, sel, st, *_ in mesh.layout}.values())
    batches, cur, ids, local = [], [], {}, []
    for t in tris:
        new = [k for k in t if k not in ids]
        size = 0x40 + (len(ids) + len(new)) * stride + (len(local) + 1) * 6 + 0x100
        if local and (len(ids) + len(new) > BATCH_VERTS or size > BATCH_BYTES):
            batches.append((cur, local))
            cur, ids, local = [], {}, []
            new = list(dict.fromkeys(t))
        for k in dict.fromkeys(new):
            ids[k] = len(cur)
            cur.append(k)
        local.append(tuple(ids[k] for k in t))
    if local:
        batches.append((cur, local))
    return batches


def encode_streams(mesh: Mesh, attrs, vids):
    """One bytes blob per vertex stream, laid out as the mesh's vertex format says."""
    streams = {}
    for name, sel, stride, ty, off in mesh.layout:
        s = sel & 15 if sel & 0xF0 == 0xF0 else 0
        streams.setdefault(s, [stride, []])[1].append((name, ty >> 4, ty & 15, off))
    out = []
    for s in sorted(streams):
        stride, fields = streams[s]
        blob = bytearray(stride * len(vids))
        for name, count, ty, off in fields:
            code = {2: 'f', 3: 'e', 1: 'B', 4: 'h', 5: 'H'}[ty]
            vals = attrs[name]
            for j, k in enumerate(vids):
                x = vals[k]
                if code in 'BhH':
                    x = [int(round(c)) for c in x]
                struct.pack_into(f'>{count}{code}', blob, j * stride + off, *x[:count])
        out.append(bytes(blob))
    return out


def skin_group_bytes(groups):
    """Per-group bytes of a rigid (one bone) skinning block: 4 on the first group, 8 on the last, padded to 16."""
    b = bytearray(groups + (-groups % 16))
    b[0], b[groups - 1] = 4, 8
    return bytes(b)


def old_skin(old, rd):
    """The skinning block of a skinned mesh's batches, when it is the rigid one-bone kind (helmets, steering wheels):
    (header, table B, table C), else raises. Block (batch +0x18 points to it, right after the indices): u16 groups
    (vertices / 4 rounded up, + 1), u16 2, u32 0, u16 bones, u16 0, ptr per-group bytes, ptr B, 3 x u32, ptr C;
    per-group bytes (skin_group_bytes), B and C 16 bytes each."""
    nb = struct.unpack_from('>H', old, 0x2A)[0]
    out = None
    for b in range(nb):
        S = rd(rd(rd(0x30) + 4 * b) + 0x18)
        if not S:
            return None
        groups = struct.unpack_from('>H', old, S)[0]
        A, B, C = rd(S + 0xC), rd(S + 0x10), rd(S + 0x20)
        if old[A:B] != skin_group_bytes(groups) or C != B + 16:
            raise ValueError('skinned with several bones (bone weights), not handled')
        blk = (old[S:S + 0x24], old[B:B + 16], old[C:C + 16])
        if out and blk[1:] != out[1:]:
            raise ValueError('skinning blocks differ between batches, not handled')
        out = blk
    return out


def build_image(old: bytes, batches):
    """A new mesh image: the old header and vertex-format block around new batches [(stream blobs, triangles)],
    pointers and the relocation table rebuilt (and the skinning block of rigid one-bone skinned meshes)."""
    rd = lambda o: struct.unpack_from('>I', old, o)[0]  # noqa: E731
    size = rd(0)
    relocs = [rd(size + 4 + 4 * i) for i in range(rd(size))]
    fmt, table = rd(0x2C), rd(0x30)
    first = rd(table)
    tmpl = old[first:first + 0x20]
    skin = old_skin(old, rd)
    out = bytearray(old[:table])
    struct.pack_into('>H', out, 0x2A, len(batches))
    out += bytes(4 * len(batches))
    pad = lambda a: out.extend(bytes(-len(out) % a))  # noqa: E731
    pad(16)
    head_relocs = [r for r in relocs if r < table and r not in (0, 0x2C)]
    batch_relocs = []
    for i, (blobs, tris) in enumerate(batches):
        pad(16)
        bh = len(out)
        struct.pack_into('>I', out, table + 4 * i, bh)
        out += tmpl + bytes(8 * len(blobs))
        ptrs = []
        for b in blobs:
            pad(16)
            ptrs.append(len(out))
            out += b
        pad(128)
        ib = len(out)
        flat = [k for t in tris for k in t]
        out += struct.pack(f'>{len(flat)}H', *flat)
        verts = max(flat) + 1 if flat else 0
        struct.pack_into('>HHHH', out, bh, len(blobs), struct.unpack_from('>H', tmpl, 2)[0], verts, len(tris))
        struct.pack_into('>II', out, bh + 0xC, bh + 0x20, ib)
        for s, p in enumerate(ptrs):
            struct.pack_into('>II', out, bh + 0x20 + 8 * s, p, 0xFFFFFFFF)
        batch_relocs += [table + 4 * i, bh + 0xC] + [bh + 0x20 + 8 * s for s in range(len(blobs))] + [bh + 0x10]
        if skin:
            pad(4)
            S = len(out)
            out += skin[0]
            groups = (verts + 3) // 4 + 1
            struct.pack_into('>H', out, S, groups)
            pad(16)
            A = len(out)
            out += skin_group_bytes(groups)
            B = len(out)
            out += skin[1] + skin[2]
            struct.pack_into('>I', out, S + 0xC, A)
            struct.pack_into('>I', out, S + 0x10, B)
            struct.pack_into('>I', out, S + 0x20, B + 16)
            struct.pack_into('>I', out, bh + 0x18, S)
            batch_relocs += [bh + 0x18, S + 0xC, S + 0x10, S + 0x20]
    pad(16)
    new_fmt = len(out)
    delta = new_fmt - fmt
    out += old[fmt:size]

    def moved(v):
        if v >= fmt:
            return v + delta
        if v < first:
            return v
        raise ValueError(f'pointer {v:#x} into the old vertex data')
    for r in head_relocs + [0x2C]:
        struct.pack_into('>I', out, r, moved(rd(r)))
    tail_relocs = []
    for r in relocs:
        if r >= fmt:
            struct.pack_into('>I', out, r + delta, moved(rd(r)))
            tail_relocs.append(r + delta)
    struct.pack_into('>I', out, 0, len(out))
    all_relocs = head_relocs + batch_relocs + [0x2C] + tail_relocs + ([0] if 0 in relocs else [])
    out += struct.pack(f'>I{len(all_relocs)}I', len(all_relocs), *all_relocs)
    return bytes(out)


def mesh_paths(sg):
    """{object name as cmd_obj writes it: (node, world matrix)}, and {node name: [(node, world)]}."""
    by_path, by_name = {}, {}
    for node, world, depth, path in sg.walk():
        if node.mesh:
            by_path['.'.join(safe(n) for n in path + (node.name,))] = (node, world)
            by_name.setdefault(safe(node.name), []).append((node, world))
    return by_path, by_name


def import_obj(data: bytes, obj_text: str, log=print, ref: bytes = None, only=None, changed=None):
    """New .sg bytes with the geometry of the meshes named in the OBJ replaced (object names as cmd_obj writes them:
    the node path; Blender's .001 suffixes are ignored). Each mesh keeps its material and vertex format; uv1
    (decal UVs) and occlusion come from the nearest original vertex; tangents are computed.
    ref: the model the OBJ was exported from, when it is another one with the same parts in the same node space
    (the garage model <Team>06_fe.sg has the race model's lod0 with its own occlusion and the suspension at rest):
    its world matrices take the geometry back to node space. only: the object names to import (others are left
    alone); changed: a list the names of the replaced meshes are appended to."""
    return import_meshes(data, obj_meshes(obj_text), log, ref, only, changed)


def import_gltf(data: bytes, gltf: bytes, log=print, ref: bytes = None, only=None, changed=None, base_dir=None):
    """import_obj for a glTF written by cmd_gltf and edited in Blender: the second UV map (TEXCOORD_1) gives the
    decal UVs (nearest original vertex when it was deleted); mirrored nodes get their triangles turned back as
    cmd_gltf turned them."""
    return import_meshes(data, gltf_meshes(gltf, base_dir), log, ref, only, changed, unmirror=True)


def import_meshes(data: bytes, objects, log=print, ref: bytes = None, only=None, changed=None, unmirror=False):
    """import_obj on {object name: (positions, uv0, uv1, normals, triangles)} (obj_meshes, gltf_meshes)."""
    sg = SceneGraph(data)
    by_path, by_name = mesh_paths(sg)
    ref_path = mesh_paths(SceneGraph(ref))[0] if ref is not None else {}
    edits = []
    for name, (pos_w, uv0, uv1, nrm_w, faces) in objects.items():
        key = re.sub(r'\.\d{3}$', '', name or '')
        if only is not None and key not in only:
            continue
        hit = by_path.get(key) or (by_name[key.split('.')[-1]][0]
                                   if len(by_name.get(key.split('.')[-1], [])) == 1 else None)
        if hit is None and len(key) >= 50:
            # Blender cuts object names to 63 bytes (less with a .001 suffix): the one node path starting so
            longer = [p for p in by_path if p.startswith(key)]
            if len(longer) == 1:
                key = longer[0]
                hit = by_path[key]
        if hit is None:
            log(f'{name}: no mesh of that name, skipped')
            continue
        node, world = hit
        if ref is not None:
            if key not in ref_path:
                log(f'{name}: not in the model the OBJ came from, skipped')
                continue
            world = ref_path[key][1]
        m = node.mesh
        if m.skinned:
            try:
                old_skin(m.image, lambda o: struct.unpack_from('>I', m.image, o)[0])
            except ValueError as e:
                log(f'{name}: {e}')
                continue
        if unmirror and det3(world) < 0:
            faces = [(a, c, b) for a, b, c in faces]
        attrs, tris = geometry_from_verts(m, world, pos_w, uv0, uv1, nrm_w, faces)
        if attrs.pop('_unchanged'):
            continue        # unchanged (an OBJ written back by Blender is re-indexed, nothing else)
        groups = split_batches(m, tris)
        batches = [(encode_streams(m, attrs, vids), local) for vids, local in groups]
        image = build_image(m.image, batches)
        Mesh(m.material, m.header, m.bounds, image)       # must decode
        pos = attrs['position']
        mn = [min(p[i] for p in pos) for i in range(3)]
        mx = [max(p[i] for p in pos) for i in range(3)]
        bounds = [(mn[i] + mx[i]) / 2 for i in range(3)] + [(mx[i] - mn[i]) / 2 for i in range(3)]
        hdr_at, bounds_at, size_at, end = node.mesh_at
        edits += [(hdr_at + 36, hdr_at + 40, struct.pack('>I', 3 * len(tris))),
                  (bounds_at, bounds_at + 24, struct.pack('>6f', *bounds)),
                  (size_at, end, struct.pack('>I', len(image)) + image)]
        log(f'{name}: {m.nverts} -> {len(pos)} vertices, {m.ntris} -> {len(tris)} triangles, {len(batches)} batch(es)')
        if changed is not None:
            changed.append(key)
    out = bytearray()
    p = 0
    for start, end, repl in sorted(edits):
        out += data[p:start] + repl
        p = end
    out += data[p:]
    return bytes(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__); return 1
    sg = SceneGraph(Path(argv[1]).read_bytes())
    if argv[0] == 'tree':
        cmd_tree(sg)
    elif argv[0] == 'obj' and len(argv) > 2:
        only = set(argv[argv.index('--only') + 1].split(',')) if '--only' in argv[3:-1] else None
        cmd_obj(sg, argv[2], only)
    elif argv[0] == 'gltf' and len(argv) > 2:
        only = set(argv[argv.index('--only') + 1].split(',')) if '--only' in argv[3:-1] else None
        cmd_gltf(sg, argv[2], only)
    elif argv[0] == 'gltfimport' and len(argv) > 3:
        ref = Path(argv[argv.index('--ref') + 1]).read_bytes() if '--ref' in argv[4:-1] else None
        new = import_gltf(sg.data, Path(argv[2]).read_bytes(), ref=ref, base_dir=Path(argv[2]).parent)
        SceneGraph(new)     # the result must parse
        Path(argv[3]).write_bytes(new)
        print(f'{argv[3]}: {len(new)} bytes')
    elif argv[0] == 'textures' and len(argv) > 2:
        cmd_textures(sg, argv[2])
    elif argv[0] == 'objimport' and len(argv) > 3:
        ref = Path(argv[argv.index('--ref') + 1]).read_bytes() if '--ref' in argv[4:-1] else None
        new = import_obj(sg.data, Path(argv[2]).read_text(encoding='utf-8', errors='replace'), ref=ref)
        SceneGraph(new)     # the result must parse
        Path(argv[3]).write_bytes(new)
        print(f'{argv[3]}: {len(new)} bytes')
    elif argv[0] == 'settex' and len(argv) > 4 and len(argv) % 2 == 1:
        pairs = list(zip(argv[2:-1:2], argv[3:-1:2]))
        new = replace_textures(sg.data, [(k, texture_from_file(f, find_texture(sg, k).gtf)) for k, f in pairs])
        SceneGraph(new)     # the result must parse
        Path(argv[-1]).write_bytes(new)
        print(f'{argv[-1]}: {len(pairs)} texture(s) replaced, {len(new)} bytes')
    else:
        print(__doc__); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
