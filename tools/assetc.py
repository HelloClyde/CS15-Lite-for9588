#!/usr/bin/env python3
"""Compile authorized GoldSrc CS 1.5 data into bounded BBK 9588 chunks."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import math
import re
import struct
import wave
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


PAK_MAGIC = b"C15PAK1\0"
PAK_VERSION = 2
PAK_ENDIAN = 0x12345678
PAK_HEADER = struct.Struct("<8sIIIIII32s")
PAK_ENTRY = struct.Struct("<4sIIIIII32sI")
PAK_ALIGNMENT = 512

BSP_MAGIC = b"C15BSP1\0"
BSP_VERSION = 3
BSP_HEADER = struct.Struct("<8sIIIII6h24s")
BSP_SECTION = struct.Struct("<4sIIIHHI8s")
BSP_SECTION_ALIGNMENT = 16

TEX_HEADER = struct.Struct("<4sHHHHIHHB3s")
MDL_HEADER = struct.Struct("<8s9I6h8s")
ANM_HEADER = struct.Struct("<8s6I")
ANM_SEQUENCE = struct.Struct("<4sHH4I")
ANM_POSITION = struct.Struct("<3h")
IMG_HEADER = struct.Struct("<8sHHI")
MUZZLE_HEADER = struct.Struct("<4sH6B16H")
MUZZLE_ANCHOR = struct.Struct("<3h")
SOUND_HEADER = struct.Struct("<8sIHH")
SOUND_CUE = struct.Struct("<II")
MDL_VERTEX = struct.Struct("<3h2B")
MDL_TRIANGLE = struct.Struct("<3HBB")
SURFACE_VERTEX = struct.Struct("<5h")
SURFACE = struct.Struct("<IHHHHBBH")
PLANE = struct.Struct("<3hiBBH2x")
NODE = struct.Struct("<I2h")
LEAF = struct.Struct("<ii2H")
CLIPNODE = struct.Struct("<i2h")
MODEL = struct.Struct("<9h2x4i3i")
SPAWN = struct.Struct("<3hhBB")
BOMB_SITE = struct.Struct("<3h")
HOSTAGE = struct.Struct("<3h")
ZONE = struct.Struct("<6hBB")
DYNAMIC_ENTITY = struct.Struct("<BBH6hII")

SOUND_CUE_SOURCES = (
    "weapons/knife_slash1.wav",
    "weapons/glock18-1.wav",
    "weapons/usp1.wav",
    "weapons/p228-1.wav",
    "weapons/deagle-1.wav",
    "weapons/elite_fire.wav",
    "weapons/fiveseven-1.wav",
    "weapons/m3-1.wav",
    "weapons/xm1014-1.wav",
    "weapons/mac10-1.wav",
    "weapons/tmp-1.wav",
    "weapons/mp5-1.wav",
    "weapons/ump45-1.wav",
    "weapons/p90-1.wav",
    "weapons/ak47-1.wav",
    "weapons/sg552-1.wav",
    "weapons/m4a1-1.wav",
    "weapons/aug-1.wav",
    "weapons/scout_fire-1.wav",
    "weapons/awp1.wav",
    "weapons/g3sg1-1.wav",
    "weapons/sg550-1.wav",
    "weapons/m249-1.wav",
    "weapons/generic_reload.wav",
    "weapons/c4_plant.wav",
    "weapons/c4_beep1.wav",
    "weapons/c4_explode1.wav",
    "weapons/c4_disarm.wav",
    "weapons/c4_disarmed.wav",
    "player/pl_step1.wav",
    "player/bhit_flesh-1.wav",
    "player/headshot1.wav",
    "player/die1.wav",
    "weapons/ric1.wav",
    "weapons/grenade_hit1.wav",
    "weapons/hegrenade-1.wav",
    "weapons/flashbang-1.wav",
    "weapons/sg_explode.wav",
    "hostage/hos1.wav",
    "radio/ctwin.wav",
    "radio/terwin.wav",
    "items/kevlar.wav",
    "player/pl_pain2.wav",
)

STUDIO_HEADER_SIZE = 244
STUDIO_BONE_SIZE = 112
STUDIO_SEQUENCE_SIZE = 176
STUDIO_ANIM_SIZE = 12
STUDIO_TEXTURE_SIZE = 80
STUDIO_BODY_PART_SIZE = 76
STUDIO_SUBMODEL_SIZE = 112
STUDIO_MESH_SIZE = 20
STUDIO_ATTACHMENT_SIZE = 88

GOLDSRC_LUMP_COUNT = 15
LUMP_ENTITIES = 0
LUMP_PLANES = 1
LUMP_TEXTURES = 2
LUMP_VERTICES = 3
LUMP_VISIBILITY = 4
LUMP_NODES = 5
LUMP_TEXINFO = 6
LUMP_FACES = 7
LUMP_LIGHTING = 8
LUMP_CLIPNODES = 9
LUMP_LEAVES = 10
LUMP_MARKSURFACES = 11
LUMP_EDGES = 12
LUMP_SURFEDGES = 13
LUMP_MODELS = 14


class AssetError(ValueError):
    pass


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fnv1a(name: str) -> int:
    value = 2166136261
    for byte in name.encode("ascii"):
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def fixed_name(name: str, size: int) -> bytes:
    encoded = name.encode("ascii")
    if len(encoded) >= size:
        raise AssetError(f"name too long for {size}-byte field: {name}")
    return encoded + bytes(size - len(encoded))


def checked_i16(value: float | int, label: str) -> int:
    rounded = int(round(value))
    if not -32768 <= rounded <= 32767:
        raise AssetError(f"{label} out of int16 range: {value}")
    return rounded


def unpack_array(blob: bytes, fmt: struct.Struct, label: str) -> list[tuple]:
    if len(blob) % fmt.size:
        raise AssetError(f"{label} size {len(blob)} is not a multiple of {fmt.size}")
    return [fmt.unpack_from(blob, offset) for offset in range(0, len(blob), fmt.size)]


@dataclass(frozen=True)
class Lump:
    offset: int
    size: int


class GoldSrcBsp:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < 4 + GOLDSRC_LUMP_COUNT * 8:
            raise AssetError("BSP header is truncated")
        self.version = struct.unpack_from("<i", self.data, 0)[0]
        if self.version != 30:
            raise AssetError(f"expected GoldSrc BSP version 30, got {self.version}")
        self.lumps: list[Lump] = []
        for index in range(GOLDSRC_LUMP_COUNT):
            offset, size = struct.unpack_from("<ii", self.data, 4 + index * 8)
            if offset < 0 or size < 0 or offset > len(self.data) - size:
                raise AssetError(f"BSP lump {index} is outside file")
            self.lumps.append(Lump(offset, size))

    def lump(self, index: int) -> bytes:
        lump = self.lumps[index]
        return self.data[lump.offset : lump.offset + lump.size]


@dataclass(frozen=True)
class MipTexture:
    name: str
    width: int
    height: int
    offsets: tuple[int, int, int, int]
    blob: bytes | None
    source: str


def read_c_string(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin1")


def parse_miptex(blob: bytes, offset: int, source: str) -> MipTexture:
    if offset < 0 or offset + 40 > len(blob):
        raise AssetError(f"miptex header outside {source}")
    name_raw, width, height, o0, o1, o2, o3 = struct.unpack_from(
        "<16sII4I", blob, offset
    )
    name = read_c_string(name_raw).lower()
    if not name or width == 0 or height == 0:
        raise AssetError(f"invalid miptex in {source}")
    offsets = (o0, o1, o2, o3)
    if o0 == 0:
        payload = None
    else:
        payload = blob[offset:]
    return MipTexture(name, width, height, offsets, payload, source)


def parse_bsp_textures(bsp: GoldSrcBsp) -> list[MipTexture]:
    blob = bsp.lump(LUMP_TEXTURES)
    if len(blob) < 4:
        raise AssetError("BSP texture lump is truncated")
    count = struct.unpack_from("<i", blob, 0)[0]
    if count < 0 or count > 4096 or len(blob) < 4 + count * 4:
        raise AssetError(f"invalid BSP texture count {count}")
    textures: list[MipTexture] = []
    for index in range(count):
        offset = struct.unpack_from("<i", blob, 4 + index * 4)[0]
        if offset == -1:
            textures.append(MipTexture(f"missing{index}", 16, 16, (0, 0, 0, 0), None, "BSP"))
        else:
            textures.append(parse_miptex(blob, offset, "BSP"))
    return textures


class WadIndex:
    def __init__(self, roots: Sequence[Path]):
        self.textures: dict[str, tuple[Path, int, int]] = {}
        self.wads: list[dict[str, object]] = []
        seen: set[Path] = set()
        for root in roots:
            if not root.is_dir():
                continue
            for path in sorted(root.glob("*.wad"), key=lambda item: item.name.lower()):
                resolved = path.resolve()
                if resolved in seen:
                    continue
                seen.add(resolved)
                self._index_wad(path)
            for path in sorted(root.glob("*.WAD"), key=lambda item: item.name.lower()):
                resolved = path.resolve()
                if resolved in seen:
                    continue
                seen.add(resolved)
                self._index_wad(path)

    def _index_wad(self, path: Path) -> None:
        data = path.read_bytes()
        if len(data) < 12 or data[:4] not in (b"WAD2", b"WAD3"):
            return
        count, directory_offset = struct.unpack_from("<ii", data, 4)
        if count < 0 or count > 65536 or directory_offset < 0:
            raise AssetError(f"invalid WAD directory: {path}")
        if directory_offset + count * 32 > len(data):
            raise AssetError(f"truncated WAD directory: {path}")
        indexed = 0
        for index in range(count):
            offset, disk_size, size, lump_type, compression, _, _, name_raw = (
                struct.unpack_from("<iiiBBBB16s", data, directory_offset + index * 32)
            )
            if compression != 0 or offset < 0 or disk_size < 40:
                continue
            if offset > len(data) - disk_size:
                raise AssetError(f"WAD lump outside file: {path}")
            name = read_c_string(name_raw).lower()
            if lump_type == 0x43 and name and name not in self.textures:
                self.textures[name] = (path, offset, disk_size)
                indexed += 1
        self.wads.append(
            {
                "path": str(path),
                "sha256": hashlib.sha256(data).hexdigest(),
                "bytes": len(data),
                "textures_indexed": indexed,
            }
        )

    def load(self, name: str) -> MipTexture | None:
        item = self.textures.get(name.lower())
        if not item:
            return None
        path, offset, size = item
        with path.open("rb") as stream:
            stream.seek(offset)
            blob = stream.read(size)
        if len(blob) != size:
            raise AssetError(f"short WAD read: {path}")
        return parse_miptex(blob, 0, path.name)


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def compile_texture(texture: MipTexture) -> tuple[bytes, dict[str, object]]:
    if texture.blob is None:
        raise AssetError(f"texture {texture.name} has no pixel data")
    level = 0
    while level < 3 and max(texture.width >> level, texture.height >> level) > 32:
        level += 1
    width = max(1, texture.width >> level)
    height = max(1, texture.height >> level)
    pixel_offset = texture.offsets[level]
    pixel_bytes = width * height
    if pixel_offset <= 0 or pixel_offset > len(texture.blob) - pixel_bytes:
        raise AssetError(f"texture {texture.name} mip {level} is truncated")
    source_pixels = texture.blob[pixel_offset : pixel_offset + pixel_bytes]

    mip3_width = max(1, texture.width >> 3)
    mip3_height = max(1, texture.height >> 3)
    palette_offset = texture.offsets[3] + mip3_width * mip3_height
    if palette_offset < 0 or palette_offset + 2 > len(texture.blob):
        raise AssetError(f"texture {texture.name} palette count is truncated")
    palette_count = struct.unpack_from("<H", texture.blob, palette_offset)[0]
    if palette_count != 256:
        raise AssetError(
            f"texture {texture.name} has unsupported palette count {palette_count}"
        )
    palette_offset += 2
    if palette_offset + 256 * 3 > len(texture.blob):
        raise AssetError(f"texture {texture.name} palette is truncated")
    palette_source = texture.blob[palette_offset : palette_offset + 256 * 3]
    flags = 1 if texture.name.startswith("{") else 0
    histogram = Counter(source_pixels)
    sums = [[0, 0, 0, 0] for _ in range(64)]
    remap = bytearray(256)
    for index in range(256):
        red, green, blue = palette_source[index * 3 : index * 3 + 3]
        target = ((red >> 6) << 4) | ((green >> 6) << 2) | (blue >> 6)
        if flags and index == 255:
            target = 63
        elif flags and target == 63:
            target = 62
        remap[index] = target
        weight = histogram.get(index, 0)
        if weight:
            sums[target][0] += red * weight
            sums[target][1] += green * weight
            sums[target][2] += blue * weight
            sums[target][3] += weight
    palette = bytearray()
    for index, (red, green, blue, weight) in enumerate(sums):
        if flags and index == 63:
            red, green, blue = 0, 0, 255
        elif weight:
            red //= weight
            green //= weight
            blue //= weight
        else:
            red = ((index >> 4) & 3) * 85
            green = ((index >> 2) & 3) * 85
            blue = (index & 3) * 85
        palette += struct.pack("<H", rgb565(red, green, blue))
    pixels = bytes(remap[index] for index in source_pixels)
    header = TEX_HEADER.pack(
        b"CTX1",
        width,
        height,
        flags,
        64,
        len(pixels),
        texture.width,
        texture.height,
        level,
        bytes(3),
    )
    output = header + palette + pixels
    return output, {
        "name": texture.name,
        "source": texture.source,
        "source_width": texture.width,
        "source_height": texture.height,
        "width": width,
        "height": height,
        "selected_mip": level,
        "resident_bytes": len(palette) + len(pixels),
        "chunk_bytes": len(output),
        "transparent": bool(flags),
    }


def checked_blob_range(data: bytes, offset: int, size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset > len(data) - size:
        raise AssetError(
            f"{label} range {offset:#x}+{size:#x} exceeds "
            f"{len(data):#x}-byte file"
        )


def studio_matrix(values: tuple[float, ...]) -> tuple[float, ...]:
    tx, ty, tz, rx, ry, rz = values
    sr, cr = math.sin(rx * 0.5), math.cos(rx * 0.5)
    sp, cp = math.sin(ry * 0.5), math.cos(ry * 0.5)
    sy, cy = math.sin(rz * 0.5), math.cos(rz * 0.5)
    qx = sr * cp * cy - cr * sp * sy
    qy = cr * sp * cy + sr * cp * sy
    qz = cr * cp * sy - sr * sp * cy
    qw = cr * cp * cy + sr * sp * sy
    xx, yy, zz = qx * qx, qy * qy, qz * qz
    xy, xz, yz = qx * qy, qx * qz, qy * qz
    wx, wy, wz = qw * qx, qw * qy, qw * qz
    return (
        1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz),
        2.0 * (xz + wy), tx,
        2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz),
        2.0 * (yz - wx), ty,
        2.0 * (xz - wy), 2.0 * (yz + wx),
        1.0 - 2.0 * (xx + yy), tz,
    )


def concat_studio_matrices(
    parent: tuple[float, ...], child: tuple[float, ...]
) -> tuple[float, ...]:
    result: list[float] = []
    for row in range(3):
        for column in range(3):
            result.append(sum(
                parent[row * 4 + axis] * child[axis * 4 + column]
                for axis in range(3)
            ))
        result.append(
            sum(
                parent[row * 4 + axis] * child[axis * 4 + 3]
                for axis in range(3)
            ) + parent[row * 4 + 3]
        )
    return tuple(result)


def transform_studio_vertex(
    matrix: tuple[float, ...], vertex: tuple[float, float, float]
) -> tuple[float, float, float]:
    return tuple(
        matrix[row * 4] * vertex[0]
        + matrix[row * 4 + 1] * vertex[1]
        + matrix[row * 4 + 2] * vertex[2]
        + matrix[row * 4 + 3]
        for row in range(3)
    )  # type: ignore[return-value]


def studio_animation_frame_delta(
    data: bytes,
    animation_offset: int,
    channel: int,
    frame: int,
    label: str,
) -> int:
    if channel < 0 or channel >= 6:
        raise AssetError(f"{label} animation channel is invalid")
    if frame < 0:
        raise AssetError(f"{label} animation frame is invalid")
    checked_blob_range(
        data, animation_offset, STUDIO_ANIM_SIZE, f"{label} animation"
    )
    value_offset = struct.unpack_from(
        "<H", data, animation_offset + channel * 2
    )[0]
    if value_offset == 0:
        return 0
    stream = animation_offset + value_offset
    remaining = frame
    guard = 0
    while True:
        checked_blob_range(data, stream, 2, f"{label} animation span")
        valid = data[stream]
        total = data[stream + 1]
        if valid == 0 or total == 0 or valid > total:
            raise AssetError(f"{label} animation span is invalid")
        checked_blob_range(
            data, stream + 2, valid * 2, f"{label} animation values"
        )
        if remaining < total:
            value_index = min(remaining, valid - 1)
            return struct.unpack_from(
                "<h", data, stream + 2 + value_index * 2
            )[0]
        remaining -= total
        stream += 2 + valid * 2
        guard += 1
        if guard > frame + 1:
            raise AssetError(f"{label} animation stream does not advance")


def studio_animation_frame_zero_delta(
    data: bytes, animation_offset: int, channel: int, label: str
) -> int:
    return studio_animation_frame_delta(
        data, animation_offset, channel, 0, label
    )


def studio_sequence_records(
    data: bytes, label: str
) -> list[dict[str, int | float | str]]:
    sequence_count, sequence_offset = struct.unpack_from("<2i", data, 164)
    if sequence_count <= 0:
        raise AssetError(f"{label} has no animation sequences")
    checked_blob_range(
        data,
        sequence_offset,
        sequence_count * STUDIO_SEQUENCE_SIZE,
        f"{label} animation sequences",
    )
    records: list[dict[str, int | float | str]] = []
    for index in range(sequence_count):
        sequence = sequence_offset + index * STUDIO_SEQUENCE_SIZE
        records.append({
            "index": index,
            "label": read_c_string(data[sequence:sequence + 32]),
            "fps": struct.unpack_from("<f", data, sequence + 32)[0],
            "flags": struct.unpack_from("<i", data, sequence + 36)[0],
            "frames": struct.unpack_from("<i", data, sequence + 56)[0],
            "blends": struct.unpack_from("<i", data, sequence + 120)[0],
            "animation": struct.unpack_from("<i", data, sequence + 124)[0],
            "group": struct.unpack_from("<i", data, sequence + 156)[0],
        })
    return records


def studio_bone_matrices(
    data: bytes,
    bone_count: int,
    bone_offset: int,
    sequence: dict[str, int | float | str],
    frame: int,
    label: str,
    bone_overrides: dict[str, tuple[float, ...]] | None = None,
) -> list[tuple[float, ...]]:
    local_values = studio_bone_local_values(
        data, bone_count, bone_offset, sequence, frame, label
    )
    matrices: list[tuple[float, ...]] = []
    for index in range(bone_count):
        bone = bone_offset + index * STUDIO_BONE_SIZE
        bone_name = read_c_string(data[bone:bone + 32])
        parent = struct.unpack_from("<i", data, bone + 32)[0]
        matrix = studio_matrix(local_values[index])
        if bone_overrides is not None and bone_name in bone_overrides:
            matrix = bone_overrides[bone_name]
        elif parent >= 0:
            if parent >= len(matrices):
                raise AssetError(f"{label} bone parent order is invalid")
            matrix = concat_studio_matrices(matrices[parent], matrix)
        matrices.append(matrix)
    return matrices


def studio_bone_local_values(
    data: bytes,
    bone_count: int,
    bone_offset: int,
    sequence: dict[str, int | float | str],
    frame: int,
    label: str,
) -> list[tuple[float, ...]]:
    frame_count = int(sequence["frames"])
    blend_count = int(sequence["blends"])
    sequence_group = int(sequence["group"])
    animation_base = int(sequence["animation"])
    if (
        frame < 0 or frame >= frame_count or frame_count <= 0
        or blend_count <= 0 or sequence_group != 0
    ):
        raise AssetError(f"{label} animation sequence/frame is unsupported")
    checked_blob_range(
        data,
        animation_base,
        bone_count * STUDIO_ANIM_SIZE,
        f"{label} animation",
    )
    result: list[tuple[float, ...]] = []
    for index in range(bone_count):
        bone = bone_offset + index * STUDIO_BONE_SIZE
        values = list(struct.unpack_from("<6f", data, bone + 64))
        scales = struct.unpack_from("<6f", data, bone + 88)
        animation = animation_base + index * STUDIO_ANIM_SIZE
        for channel in range(6):
            values[channel] += (
                studio_animation_frame_delta(
                    data, animation, channel, frame, label
                )
                * scales[channel]
            )
        result.append(tuple(values))
    return result


def studio_hybrid_bone_matrices(
    data: bytes,
    bone_count: int,
    bone_offset: int,
    upper_sequence: dict[str, int | float | str],
    upper_frame: int,
    gait_sequence: dict[str, int | float | str],
    gait_frame: int,
    label: str,
) -> list[tuple[float, ...]]:
    """Apply gait to the lower body while retaining the rifle upper body."""
    upper_values = studio_bone_local_values(
        data, bone_count, bone_offset,
        upper_sequence, upper_frame, label,
    )
    gait_values = studio_bone_local_values(
        data, bone_count, bone_offset,
        gait_sequence, gait_frame, label,
    )
    matrices: list[tuple[float, ...]] = []
    upper_body: list[bool] = []
    for index in range(bone_count):
        bone = bone_offset + index * STUDIO_BONE_SIZE
        bone_name = read_c_string(data[bone:bone + 32])
        parent = struct.unpack_from("<i", data, bone + 32)[0]
        if parent >= index:
            raise AssetError(f"{label} bone parent order is invalid")
        is_upper = (
            bone_name.lower() == "bip01 spine"
            or (parent >= 0 and upper_body[parent])
        )
        upper_body.append(is_upper)
        local = upper_values[index] if is_upper else gait_values[index]
        matrix = studio_matrix(local)
        if parent >= 0:
            matrix = concat_studio_matrices(matrices[parent], matrix)
        matrices.append(matrix)
    return matrices


def studio_idle_bone_matrices(
    data: bytes,
    bone_count: int,
    bone_offset: int,
    label: str,
    bone_overrides: dict[str, tuple[float, ...]] | None = None,
) -> tuple[list[tuple[float, ...]], str]:
    selected: dict[str, int | float | str] | None = None
    for sequence in studio_sequence_records(data, label):
        sequence_label = str(sequence["label"])
        if (
            sequence_label.lower().startswith("idle")
            and int(sequence["frames"]) > 0
            and int(sequence["blends"]) > 0
            and int(sequence["group"]) == 0
        ):
            selected = sequence
            break
    if selected is None:
        raise AssetError(f"{label} has no inline idle animation")
    return (
        studio_bone_matrices(
            data, bone_count, bone_offset, selected, 0, label,
            bone_overrides,
        ),
        str(selected["label"]),
    )


def studio_named_bone_matrices(
    data: bytes,
    bone_count: int,
    bone_offset: int,
    sequence_name: str,
    label: str,
) -> tuple[list[tuple[float, ...]], str]:
    selected = next(
        (
            sequence for sequence in studio_sequence_records(data, label)
            if str(sequence["label"]).lower() == sequence_name.lower()
            and int(sequence["frames"]) > 0
            and int(sequence["blends"]) > 0
            and int(sequence["group"]) == 0
        ),
        None,
    )
    if selected is None:
        raise AssetError(
            f"{label} has no inline {sequence_name} animation"
        )
    return (
        studio_bone_matrices(
            data, bone_count, bone_offset, selected, 0, label
        ),
        str(selected["label"]),
    )


def resample_studio_texture(
    pixels: bytes,
    old_width: int,
    old_height: int,
    new_width: int,
    new_height: int,
) -> bytes:
    output = bytearray(new_width * new_height)
    for y in range(new_height):
        source_y = min(old_height - 1, y * old_height // new_height)
        for x in range(new_width):
            source_x = min(old_width - 1, x * old_width // new_width)
            output[y * new_width + x] = (
                pixels[source_y * old_width + source_x]
            )
    return bytes(output)


def compile_studio_texture(
    name: str,
    flags: int,
    width: int,
    height: int,
    pixels: bytes,
    palette_source: bytes,
    maximum_dimension: int = 64,
) -> tuple[bytes, dict[str, object], int]:
    if maximum_dimension <= 0 or maximum_dimension > 64:
        raise AssetError("studio texture maximum dimension is invalid")
    level = 0
    while (
        level < 7 and
        max(width >> level, height >> level) > maximum_dimension
    ):
        level += 1
    target_width = max(1, width >> level)
    target_height = max(1, height >> level)
    target_pixels = resample_studio_texture(
        pixels, width, height, target_width, target_height
    )
    palette = bytearray()
    for index in range(256):
        red, green, blue = palette_source[index * 3:index * 3 + 3]
        palette += struct.pack("<H", rgb565(red, green, blue))
    transparent = (flags & 0x40) != 0
    header = TEX_HEADER.pack(
        b"CTX1", target_width, target_height,
        1 if transparent else 0, 256, len(target_pixels),
        width, height, level, bytes(3)
    )
    output = header + palette + target_pixels
    return output, {
        "name": name,
        "source_width": width,
        "source_height": height,
        "width": target_width,
        "height": target_height,
        "selected_mip": level,
        "resident_bytes": len(palette) + len(target_pixels),
        "chunk_bytes": len(output),
        "transparent": transparent,
    }, level


def select_view_animation_sequences(
    data: bytes, asset_base: str
) -> list[tuple[bytes, dict[str, int | float | str], int]]:
    sequences = studio_sequence_records(data, asset_base)
    by_name = {
        str(sequence["label"]).lower(): sequence
        for sequence in sequences
        if int(sequence["frames"]) > 0
        and float(sequence["fps"]) > 0.0
        and int(sequence["blends"]) > 0
        and int(sequence["group"]) == 0
    }
    unsilenced = asset_base.endswith(("/v_m4a1", "/v_usp"))

    def first_named(*names: str) -> dict[str, int | float | str] | None:
        for name in names:
            found = by_name.get(name)
            if found is not None:
                return found
        return None

    idle = first_named("idle_unsil" if unsilenced else "", "idle", "idle1")
    if idle is None:
        idle = next(
            (item for name, item in by_name.items() if name.startswith("idle")),
            None,
        )
    if asset_base.endswith("/v_knife"):
        fire = first_named("slash1", "midslash1", "stab")
        reload_sequence = idle
    elif asset_base.endswith("/v_c4"):
        fire = first_named("pressbutton", "drop")
        reload_sequence = idle
    elif asset_base.endswith("/v_elite"):
        fire = first_named("shoot_right1", "shoot_left1")
        reload_sequence = first_named("reload")
    else:
        fire = first_named(
            "shoot1_unsil" if unsilenced else "",
            "shoot1", "shoot",
        )
        reload_sequence = first_named(
            "reload_unsil" if unsilenced else "",
            "reload", "start_reload", "insert"
        )
    draw = first_named(
        "draw_unsil" if unsilenced else "", "draw", "draw2"
    )
    selected = [
        (b"IDLE", idle, 4),
        (b"FIRE", fire, 5),
        (b"RLOD", reload_sequence, 10),
        (b"DRAW", draw, 6),
    ]
    if any(sequence is None for _, sequence, _ in selected):
        missing = [
            tag.decode("ascii")
            for tag, sequence, _ in selected
            if sequence is None
        ]
        raise AssetError(
            f"{asset_base} lacks required view animations: "
            + ", ".join(missing)
        )
    return [
        (tag, sequence, maximum)
        for tag, sequence, maximum in selected
        if sequence is not None
    ]


def compile_studio_animation(
    data: bytes,
    asset_base: str,
    source_crc: int,
    bone_count: int,
    bone_offset: int,
    animation_sources: list[
        tuple[int, tuple[float, float, float]]
    ],
) -> tuple[bytes, dict[str, object]]:
    selected = select_view_animation_sequences(data, asset_base)
    vertex_count = len(animation_sources)
    frame_stride = vertex_count * ANM_POSITION.size
    sequence_table_bytes = len(selected) * ANM_SEQUENCE.size
    cursor = align(ANM_HEADER.size + sequence_table_bytes, 16)
    output = bytearray(cursor)
    sequence_details: list[dict[str, object]] = []
    records: list[bytes] = []
    for tag, sequence, maximum_samples in selected:
        source_frames = int(sequence["frames"])
        source_fps = float(sequence["fps"])
        duration_ms = max(
            1, int(round((source_frames - 1) * 1000.0 / source_fps))
        )
        desired_samples = max(2, int(math.ceil(duration_ms / 100.0)) + 1)
        sample_count = min(maximum_samples, desired_samples)
        if source_frames == 1:
            sample_count = 1
        sample_period_ms = (
            max(1, int(round(duration_ms / (sample_count - 1))))
            if sample_count > 1 else 100
        )
        data_offset = cursor
        sampled_source_frames: list[int] = []
        for sample in range(sample_count):
            source_frame = (
                int(round(
                    sample * (source_frames - 1) / (sample_count - 1)
                ))
                if sample_count > 1 else 0
            )
            sampled_source_frames.append(source_frame)
            matrices = studio_bone_matrices(
                data, bone_count, bone_offset,
                sequence, source_frame, asset_base
            )
            frame_data = bytearray()
            for bone, source in animation_sources:
                position = transform_studio_vertex(matrices[bone], source)
                frame_data += ANM_POSITION.pack(
                    checked_i16(position[0] * 16.0, "animation vertex x"),
                    checked_i16(position[1] * 16.0, "animation vertex y"),
                    checked_i16(position[2] * 16.0, "animation vertex z"),
                )
            output.extend(frame_data)
            cursor += len(frame_data)
        records.append(ANM_SEQUENCE.pack(
            tag,
            sample_count,
            sample_period_ms,
            int(sequence["index"]),
            source_frames,
            int(round(source_fps * 256.0)),
            data_offset,
        ))
        sequence_details.append({
            "action": tag.decode("ascii"),
            "source_sequence": str(sequence["label"]),
            "source_sequence_index": int(sequence["index"]),
            "source_frames": source_frames,
            "source_fps": source_fps,
            "sampled_frames": sampled_source_frames,
            "sample_count": sample_count,
            "sample_period_ms": sample_period_ms,
            "duration_ms": sample_period_ms * max(0, sample_count - 1),
        })
    output[:ANM_HEADER.size] = ANM_HEADER.pack(
        b"C15ANM1\0", 1, len(output), source_crc,
        vertex_count, len(selected), frame_stride
    )
    table = ANM_HEADER.size
    for record in records:
        output[table:table + len(record)] = record
        table += len(record)
    return bytes(output), {
        "name": asset_base.replace("mdl/", "anim/", 1),
        "chunk_bytes": len(output),
        "vertex_count": vertex_count,
        "frame_stride": frame_stride,
        "sequences": sequence_details,
    }


def compile_world_locomotion_animation(
    data: bytes,
    asset_base: str,
    source_crc: int,
    bone_count: int,
    bone_offset: int,
    animation_sources: list[
        tuple[int, tuple[float, float, float]]
    ],
    merge_data: bytes | None = None,
) -> tuple[bytes, dict[str, object]]:
    parent_data = merge_data if merge_data is not None else data
    parent_label = (
        f"{asset_base} bone-merge parent"
        if merge_data is not None else asset_base
    )
    parent_bone_count, parent_bone_offset = struct.unpack_from(
        "<2i", parent_data, 140
    )
    sequences = {
        str(sequence["label"]).lower(): sequence
        for sequence in studio_sequence_records(parent_data, parent_label)
        if int(sequence["frames"]) > 0
        and float(sequence["fps"]) > 0.0
        and int(sequence["blends"]) > 0
        and int(sequence["group"]) == 0
    }
    upper = sequences.get("ref_aim_carbine")
    walk = sequences.get("walk")
    if upper is None or walk is None:
        raise AssetError(
            f"{parent_label} lacks ref_aim_carbine/walk locomotion"
        )
    child_idle = None
    if merge_data is not None:
        child_idle = next(
            (
                sequence for sequence in studio_sequence_records(
                    data, asset_base
                )
                if str(sequence["label"]).lower().startswith("idle")
                and int(sequence["frames"]) > 0
                and int(sequence["blends"]) > 0
                and int(sequence["group"]) == 0
            ),
            None,
        )
        if child_idle is None:
            raise AssetError(f"{asset_base} lacks held-weapon idle pose")

    def frame_matrices(
        gait_frame: int | None,
    ) -> list[tuple[float, ...]]:
        if gait_frame is None:
            parent_matrices = studio_bone_matrices(
                parent_data, parent_bone_count, parent_bone_offset,
                upper, 0, parent_label,
            )
        else:
            parent_matrices = studio_hybrid_bone_matrices(
                parent_data, parent_bone_count, parent_bone_offset,
                upper, 0, walk, gait_frame, parent_label,
            )
        if merge_data is None:
            return parent_matrices
        overrides = {
            read_c_string(
                parent_data[
                    parent_bone_offset + index * STUDIO_BONE_SIZE:
                    parent_bone_offset + index * STUDIO_BONE_SIZE + 32
                ]
            ): parent_matrices[index]
            for index in range(parent_bone_count)
        }
        assert child_idle is not None
        return studio_bone_matrices(
            data, bone_count, bone_offset,
            child_idle, 0, asset_base, overrides,
        )

    vertex_count = len(animation_sources)
    frame_stride = vertex_count * ANM_POSITION.size
    selected_frames = [0]
    walk_frames = int(walk["frames"])
    walk_fps = float(walk["fps"])
    sample_count = min(6, walk_frames)
    sampled_walk_frames = [
        sample * walk_frames // sample_count
        for sample in range(sample_count)
    ]
    sample_period_ms = max(
        1, int(round(walk_frames * 1000.0 / walk_fps / sample_count))
    )
    sequences_to_write = [
        (
            b"IDLE", selected_frames, 100,
            int(upper["index"]), int(upper["frames"]),
            float(upper["fps"]), "ref_aim_carbine",
        ),
        (
            b"WALK", sampled_walk_frames, sample_period_ms,
            int(walk["index"]), walk_frames, walk_fps,
            "walk + ref_aim_carbine",
        ),
    ]
    cursor = align(
        ANM_HEADER.size + len(sequences_to_write) * ANM_SEQUENCE.size, 16
    )
    output = bytearray(cursor)
    records: list[bytes] = []
    sequence_details: list[dict[str, object]] = []
    for (
        tag, sampled_frames, frame_ms, source_index,
        source_frames, source_fps, source_name,
    ) in sequences_to_write:
        data_offset = cursor
        for source_frame in sampled_frames:
            matrices = frame_matrices(
                None if tag == b"IDLE" else source_frame
            )
            frame_data = bytearray()
            for bone, source in animation_sources:
                position = transform_studio_vertex(matrices[bone], source)
                frame_data += ANM_POSITION.pack(
                    checked_i16(position[0] * 16.0, "locomotion vertex x"),
                    checked_i16(position[1] * 16.0, "locomotion vertex y"),
                    checked_i16(position[2] * 16.0, "locomotion vertex z"),
                )
            output.extend(frame_data)
            cursor += len(frame_data)
        records.append(ANM_SEQUENCE.pack(
            tag, len(sampled_frames), frame_ms,
            source_index, source_frames,
            int(round(source_fps * 256.0)), data_offset,
        ))
        sequence_details.append({
            "action": tag.decode("ascii"),
            "source_sequence": source_name,
            "source_sequence_index": source_index,
            "source_frames": source_frames,
            "source_fps": source_fps,
            "sampled_frames": sampled_frames,
            "sample_count": len(sampled_frames),
            "sample_period_ms": frame_ms,
        })
    output[:ANM_HEADER.size] = ANM_HEADER.pack(
        b"C15ANM1\0", 1, len(output), source_crc,
        vertex_count, len(sequences_to_write), frame_stride,
    )
    table = ANM_HEADER.size
    for record in records:
        output[table:table + len(record)] = record
        table += len(record)
    return bytes(output), {
        "name": asset_base.replace("mdl/", "anim/", 1),
        "chunk_bytes": len(output),
        "vertex_count": vertex_count,
        "frame_stride": frame_stride,
        "sequences": sequence_details,
        "hybrid_upper_sequence": "ref_aim_carbine",
        "hybrid_gait_sequence": "walk",
        "bone_merged": merge_data is not None,
    }


def read_goldsrc_sprite(
    path: Path,
) -> tuple[list[list[tuple[int, int, int]]], int, int]:
    data = path.read_bytes()
    header = struct.Struct("<4siiifiiifi")
    if len(data) < header.size + 2:
        raise AssetError(f"{path.name} is truncated")
    (
        magic, version, _sprite_type, texture_format, _radius,
        width, height, frame_count, _beam_length, _sync_type,
    ) = header.unpack_from(data)
    if magic != b"IDSP" or version != 2:
        raise AssetError(f"{path.name} is not a GoldSrc v2 sprite")
    if texture_format != 1:
        raise AssetError(f"{path.name} is not an additive sprite")
    if width <= 0 or height <= 0 or frame_count <= 0:
        raise AssetError(f"{path.name} has invalid dimensions")
    palette_count = struct.unpack_from("<H", data, header.size)[0]
    if palette_count != 256:
        raise AssetError(f"{path.name} does not have a 256-color palette")
    palette_offset = header.size + 2
    checked_blob_range(
        data, palette_offset, palette_count * 3, "sprite palette"
    )
    palette = [
        tuple(data[palette_offset + index * 3:
                   palette_offset + index * 3 + 3])
        for index in range(palette_count)
    ]
    frames: list[list[tuple[int, int, int]]] = []
    cursor = palette_offset + palette_count * 3
    for frame_index in range(frame_count):
        checked_blob_range(data, cursor, 20, "sprite frame")
        frame_type = struct.unpack_from("<i", data, cursor)[0]
        if frame_type != 0:
            raise AssetError(
                f"{path.name} grouped sprite frames are unsupported"
            )
        _, _, frame_width, frame_height = struct.unpack_from(
            "<4i", data, cursor + 4
        )
        cursor += 20
        if frame_width != width or frame_height != height:
            raise AssetError(f"{path.name} frame dimensions vary")
        checked_blob_range(
            data, cursor, width * height,
            f"sprite frame {frame_index} pixels"
        )
        frames.append([
            palette[index]
            for index in data[cursor:cursor + width * height]
        ])
        cursor += width * height
    return frames, width, height


def resample_rgb_sprite(
    source: list[tuple[int, int, int]],
    source_width: int,
    source_height: int,
    target_width: int,
    target_height: int,
) -> list[tuple[int, int, int]]:
    output: list[tuple[int, int, int]] = []
    for y in range(target_height):
        top = y * source_height // target_height
        bottom = max(top + 1, (y + 1) * source_height // target_height)
        for x in range(target_width):
            left = x * source_width // target_width
            right = max(left + 1, (x + 1) * source_width // target_width)
            count = (right - left) * (bottom - top)
            red = green = blue = 0
            for source_y in range(top, bottom):
                row = source_y * source_width
                for source_x in range(left, right):
                    color = source[row + source_x]
                    red += color[0]
                    green += color[1]
                    blue += color[2]
            output.append((
                red // count, green // count, blue // count
            ))
    return output


def quantize_additive_frames(
    frames: list[list[tuple[int, int, int]]],
) -> tuple[list[tuple[int, int, int]], list[list[int]]]:
    counts = Counter(
        color
        for frame in frames
        for color in frame
        if sum(color) >= 12
    )
    boxes = [list(counts)] if counts else []
    while len(boxes) < 15:
        choices = [
            (
                max(max(color[channel] for color in box) -
                    min(color[channel] for color in box)
                    for channel in range(3)) *
                sum(counts[color] for color in box),
                index,
            )
            for index, box in enumerate(boxes)
            if len(box) > 1
        ]
        if not choices:
            break
        _, selected = max(choices)
        box = boxes.pop(selected)
        channel = max(
            range(3),
            key=lambda item: (
                max(color[item] for color in box) -
                min(color[item] for color in box)
            ),
        )
        box.sort(key=lambda color: color[channel])
        total = sum(counts[color] for color in box)
        accumulated = 0
        split = 1
        for split, color in enumerate(box, 1):
            accumulated += counts[color]
            if accumulated * 2 >= total:
                break
        split = min(max(1, split), len(box) - 1)
        boxes.extend((box[:split], box[split:]))

    palette: list[tuple[int, int, int]] = [(0, 0, 0)]
    for box in boxes:
        weight = sum(counts[color] for color in box)
        palette.append(tuple(
            sum(color[channel] * counts[color] for color in box) // weight
            for channel in range(3)
        ))
    while len(palette) < 16:
        palette.append((0, 0, 0))

    indexed: list[list[int]] = []
    for frame in frames:
        converted: list[int] = []
        for color in frame:
            if sum(color) < 12:
                converted.append(0)
                continue
            converted.append(min(
                range(1, 1 + len(boxes)),
                key=lambda index: sum(
                    (color[channel] - palette[index][channel]) ** 2
                    for channel in range(3)
                ),
            ))
        indexed.append(converted)
    return palette, indexed


def compile_view_muzzle_sprite(
    path: Path,
    studio_data: bytes,
    asset_base: str,
    animation_detail: dict[str, object],
) -> tuple[bytes, dict[str, object]]:
    source_frames, source_width, source_height = read_goldsrc_sprite(path)
    target_dimension = 23
    frames = [
        resample_rgb_sprite(
            frame, source_width, source_height,
            target_dimension, target_dimension,
        )
        for frame in source_frames
    ]
    palette, indexed_frames = quantize_additive_frames(frames)

    sequences = animation_detail["sequences"]
    assert isinstance(sequences, list)
    fire = next(
        item for item in sequences
        if isinstance(item, dict) and item.get("action") == "FIRE"
    )
    sampled_frames = fire["sampled_frames"]
    assert isinstance(sampled_frames, list)
    bone_count, bone_offset = struct.unpack_from("<2i", studio_data, 140)
    attachment_count, attachment_offset = struct.unpack_from(
        "<2i", studio_data, 212
    )
    if attachment_count <= 0:
        raise AssetError(f"{asset_base} has no muzzle attachment")
    checked_blob_range(
        studio_data, attachment_offset,
        attachment_count * STUDIO_ATTACHMENT_SIZE,
        "studio attachments",
    )
    attachment_bone = struct.unpack_from(
        "<i", studio_data, attachment_offset + 36
    )[0]
    attachment_origin = struct.unpack_from(
        "<3f", studio_data, attachment_offset + 40
    )
    fire_sequence = next(
        sequence for tag, sequence, _ in
        select_view_animation_sequences(studio_data, asset_base)
        if tag == b"FIRE"
    )
    anchors = bytearray()
    anchor_manifest: list[list[int]] = []
    for source_frame in sampled_frames:
        matrices = studio_bone_matrices(
            studio_data, bone_count, bone_offset,
            fire_sequence, int(source_frame), asset_base,
        )
        position = transform_studio_vertex(
            matrices[attachment_bone], attachment_origin
        )
        quantized = [
            checked_i16(value * 16.0, "muzzle attachment")
            for value in position
        ]
        anchors.extend(MUZZLE_ANCHOR.pack(*quantized))
        anchor_manifest.append(quantized)

    packed_pixels = bytearray()
    for frame in indexed_frames:
        packed = bytearray((len(frame) + 1) // 2)
        for index, color in enumerate(frame):
            packed[index // 2] |= color << (4 * (index & 1))
        packed_pixels.extend(packed)
    file_size = MUZZLE_HEADER.size + len(anchors) + len(packed_pixels)
    if file_size > 65535:
        raise AssetError(f"{path.name} compiled muzzle sprite is too large")
    display_size = 44 if source_width <= 48 else 50
    output = bytearray()
    output.extend(MUZZLE_HEADER.pack(
        b"MSP1", file_size, target_dimension, target_dimension,
        len(indexed_frames), len(sampled_frames), display_size, 0,
        *(rgb565(*color) for color in palette),
    ))
    output.extend(anchors)
    output.extend(packed_pixels)
    return bytes(output), {
        "name": asset_base.replace("mdl/", "muzzle/", 1),
        "source": str(path),
        "source_width": source_width,
        "source_height": source_height,
        "width": target_dimension,
        "height": target_dimension,
        "frames": len(indexed_frames),
        "anchor_frames": len(sampled_frames),
        "anchors_q4": anchor_manifest,
        "display_size": display_size,
        "resident_bytes": len(output),
    }


def compile_studio_model(
    path: Path,
    asset_base: str,
    with_animation: bool = False,
    with_locomotion_animation: bool = False,
    maximum_texture_dimension: int = 64,
    pose_sequence_name: str | None = None,
    bone_merge_path: Path | None = None,
    bone_merge_sequence: str = "ref_aim_carbine",
) -> tuple[
    bytes,
    list["PakAsset"],
    dict[str, object],
    tuple[bytes, dict[str, object]] | None,
]:
    data = path.read_bytes()
    if len(data) < STUDIO_HEADER_SIZE or data[:4] != b"IDST":
        raise AssetError(f"{path.name} is not a GoldSrc StudioMDL")
    if struct.unpack_from("<i", data, 4)[0] != 10:
        raise AssetError(f"{path.name} is not StudioMDL version 10")
    if struct.unpack_from("<i", data, 72)[0] != len(data):
        raise AssetError(f"{path.name} length field does not match the file")

    bone_count, bone_offset = struct.unpack_from("<2i", data, 140)
    checked_blob_range(
        data, bone_offset, bone_count * STUDIO_BONE_SIZE, "studio bones"
    )
    if with_animation and with_locomotion_animation:
        raise AssetError("model cannot use both animation profiles")
    bone_overrides: dict[str, tuple[float, ...]] | None = None
    bone_merge_source: str | None = None
    merge_data: bytes | None = None
    if bone_merge_path is not None:
        merge_data = bone_merge_path.read_bytes()
        if (
            len(merge_data) < STUDIO_HEADER_SIZE
            or merge_data[:4] != b"IDST"
            or struct.unpack_from("<i", merge_data, 4)[0] != 10
        ):
            raise AssetError(
                f"{bone_merge_path.name} is not a GoldSrc StudioMDL"
            )
        merge_bone_count, merge_bone_offset = struct.unpack_from(
            "<2i", merge_data, 140
        )
        checked_blob_range(
            merge_data, merge_bone_offset,
            merge_bone_count * STUDIO_BONE_SIZE,
            "bone-merge source bones",
        )
        merge_matrices, _ = studio_named_bone_matrices(
            merge_data, merge_bone_count, merge_bone_offset,
            bone_merge_sequence, bone_merge_path.name,
        )
        bone_overrides = {
            read_c_string(
                merge_data[
                    merge_bone_offset + index * STUDIO_BONE_SIZE:
                    merge_bone_offset + index * STUDIO_BONE_SIZE + 32
                ]
            ): merge_matrices[index]
            for index in range(merge_bone_count)
        }
        bone_merge_source = str(bone_merge_path)
    if pose_sequence_name is None:
        bone_matrices, pose_sequence = studio_idle_bone_matrices(
            data, bone_count, bone_offset, path.name, bone_overrides
        )
    else:
        if bone_overrides is not None:
            raise AssetError(
                "pose sequence and bone merge cannot be combined"
            )
        bone_matrices, pose_sequence = studio_named_bone_matrices(
            data, bone_count, bone_offset,
            pose_sequence_name, path.name,
        )

    texture_count, texture_offset = struct.unpack_from("<2i", data, 180)
    checked_blob_range(
        data, texture_offset,
        texture_count * STUDIO_TEXTURE_SIZE, "studio textures"
    )
    texture_assets: list[PakAsset] = []
    texture_details: list[dict[str, object]] = []
    texture_dimensions: list[tuple[int, int, int]] = []
    texture_ids: list[int] = []
    for index in range(texture_count):
        descriptor = texture_offset + index * STUDIO_TEXTURE_SIZE
        raw_name, flags, width, height, pixels_offset = struct.unpack_from(
            "<64s4i", data, descriptor
        )
        name = read_c_string(raw_name)
        checked_blob_range(
            data, pixels_offset, width * height + 256 * 3,
            f"studio texture {index}"
        )
        pixels = data[pixels_offset:pixels_offset + width * height]
        palette = data[
            pixels_offset + width * height:
            pixels_offset + width * height + 256 * 3
        ]
        chunk, detail, level = compile_studio_texture(
            name, flags, width, height, pixels, palette,
            maximum_texture_dimension,
        )
        texture_name = f"{asset_base}/t{index}"
        texture_assets.append(PakAsset(b"TEX0", texture_name, chunk))
        texture_details.append(detail)
        texture_dimensions.append((
            int(detail["width"]), int(detail["height"]), level
        ))
        texture_ids.append(fnv1a(texture_name))

    skin_refs, skin_groups, skin_offset = struct.unpack_from("<3i", data, 192)
    if skin_refs <= 0 or skin_groups <= 0:
        raise AssetError(f"{path.name} has no skin table")
    checked_blob_range(
        data, skin_offset, skin_refs * skin_groups * 2, "studio skins"
    )
    base_skin = [
        struct.unpack_from("<H", data, skin_offset + index * 2)[0]
        for index in range(skin_refs)
    ]

    body_count, body_offset = struct.unpack_from("<2i", data, 204)
    checked_blob_range(
        data, body_offset,
        body_count * STUDIO_BODY_PART_SIZE, "studio body parts"
    )
    vertices = bytearray()
    triangles = bytearray()
    unique_vertices: dict[tuple[int, int, int, int, int], int] = {}
    quantized_positions: list[tuple[int, int, int]] = []
    animation_sources: list[
        tuple[int, tuple[float, float, float]]
    ] = []
    triangle_count = 0

    def vertex_index(
        position: tuple[float, float, float],
        u: int,
        v: int,
        bone: int,
        source: tuple[float, float, float],
    ) -> int:
        qx = checked_i16(position[0] * 16.0, "model vertex x")
        qy = checked_i16(position[1] * 16.0, "model vertex y")
        qz = checked_i16(position[2] * 16.0, "model vertex z")
        key = (qx, qy, qz, u, v)
        existing = unique_vertices.get(key)
        if existing is not None:
            return existing
        result = len(unique_vertices)
        if result >= 65535:
            raise AssetError(f"{path.name} has too many compiled vertices")
        unique_vertices[key] = result
        vertices.extend(MDL_VERTEX.pack(qx, qy, qz, u, v))
        quantized_positions.append((qx, qy, qz))
        animation_sources.append((bone, source))
        return result

    for body_index in range(body_count):
        body = body_offset + body_index * STUDIO_BODY_PART_SIZE
        model_count = struct.unpack_from("<i", data, body + 64)[0]
        model_offset = struct.unpack_from("<i", data, body + 72)[0]
        if model_count <= 0:
            continue
        checked_blob_range(
            data, model_offset,
            model_count * STUDIO_SUBMODEL_SIZE, "studio submodels"
        )
        model = model_offset
        mesh_count, mesh_offset = struct.unpack_from("<2i", data, model + 72)
        vertex_count, bone_info_offset, vertex_offset = struct.unpack_from(
            "<3i", data, model + 80
        )
        checked_blob_range(
            data, mesh_offset, mesh_count * STUDIO_MESH_SIZE, "studio meshes"
        )
        checked_blob_range(
            data, bone_info_offset, vertex_count, "studio vertex bones"
        )
        checked_blob_range(
            data, vertex_offset, vertex_count * 12, "studio vertices"
        )
        source_vertices: list[tuple[float, float, float]] = []
        source_bones: list[int] = []
        transformed: list[tuple[float, float, float]] = []
        for index in range(vertex_count):
            bone = data[bone_info_offset + index]
            if bone >= len(bone_matrices):
                raise AssetError(f"{path.name} vertex bone is invalid")
            source = struct.unpack_from("<3f", data, vertex_offset + index * 12)
            source_vertices.append(source)
            source_bones.append(bone)
            transformed.append(
                transform_studio_vertex(bone_matrices[bone], source)
            )
        for mesh_index in range(mesh_count):
            mesh = mesh_offset + mesh_index * STUDIO_MESH_SIZE
            commands = struct.unpack_from("<i", data, mesh + 4)[0]
            skin_ref = struct.unpack_from("<i", data, mesh + 8)[0]
            if skin_ref < 0 or skin_ref >= len(base_skin):
                raise AssetError(f"{path.name} mesh skin reference is invalid")
            texture_id = base_skin[skin_ref]
            if texture_id >= texture_count:
                raise AssetError(f"{path.name} mesh texture is invalid")
            width, height, level = texture_dimensions[texture_id]
            cursor = commands
            while True:
                checked_blob_range(data, cursor, 2, "studio commands")
                command_count = struct.unpack_from("<h", data, cursor)[0]
                cursor += 2
                if command_count == 0:
                    break
                fan = command_count < 0
                count = abs(command_count)
                checked_blob_range(
                    data, cursor, count * 8, "studio command vertices"
                )
                command_vertices: list[int] = []
                for _ in range(count):
                    source_index, _, source_u, source_v = struct.unpack_from(
                        "<4h", data, cursor
                    )
                    cursor += 8
                    if source_index < 0 or source_index >= vertex_count:
                        raise AssetError(
                            f"{path.name} command vertex is invalid"
                        )
                    u = ((source_u >> level) % width) & 0xFF
                    v = ((source_v >> level) % height) & 0xFF
                    command_vertices.append(
                        vertex_index(
                            transformed[source_index], u, v,
                            source_bones[source_index],
                            source_vertices[source_index],
                        )
                    )
                for index in range(1, count - 1):
                    if fan:
                        order = (0, index, index + 1)
                    elif index & 1:
                        order = (index - 1, index, index + 1)
                    else:
                        order = (index, index - 1, index + 1)
                    triangles.extend(MDL_TRIANGLE.pack(
                        command_vertices[order[0]],
                        command_vertices[order[1]],
                        command_vertices[order[2]],
                        texture_id, 0
                    ))
                    triangle_count += 1

    if not quantized_positions or triangle_count == 0:
        raise AssetError(f"{path.name} compiled to an empty model")
    bounds = []
    for axis in range(3):
        bounds.append(min(position[axis] for position in quantized_positions))
    for axis in range(3):
        bounds.append(max(position[axis] for position in quantized_positions))
    vertex_offset = MDL_HEADER.size
    triangle_offset = align(vertex_offset + len(vertices), 16)
    reference_offset = align(triangle_offset + len(triangles), 16)
    file_size = reference_offset + len(texture_ids) * 4
    output = bytearray(file_size)
    output[vertex_offset:vertex_offset + len(vertices)] = vertices
    output[triangle_offset:triangle_offset + len(triangles)] = triangles
    for index, identifier in enumerate(texture_ids):
        struct.pack_into("<I", output, reference_offset + index * 4, identifier)
    output[:MDL_HEADER.size] = MDL_HEADER.pack(
        b"C15MDL1\0", 1, file_size, crc32(data),
        len(unique_vertices), triangle_count, texture_count,
        vertex_offset, triangle_offset, reference_offset,
        *bounds, bytes(8)
    )
    detail = {
        "name": asset_base,
        "source": str(path),
        "source_bytes": len(data),
        "source_sha256": sha256(data),
        "chunk_bytes": len(output),
        "vertices": len(unique_vertices),
        "triangles": triangle_count,
        "pose_sequence": pose_sequence,
        "pose_frame": 0,
        "textures": texture_details,
        "resident_bytes": (
            len(output) + sum(
                int(item["resident_bytes"]) for item in texture_details
            )
        ),
        "bounds_q4": bounds,
    }
    if bone_merge_source is not None:
        detail["bone_merge_source"] = bone_merge_source
        detail["bone_merge_sequence"] = bone_merge_sequence
    animation = None
    if with_animation:
        animation = compile_studio_animation(
            data, asset_base, crc32(data),
            bone_count, bone_offset, animation_sources
        )
        detail["animation"] = animation[1]
    elif with_locomotion_animation:
        animation = compile_world_locomotion_animation(
            data, asset_base, crc32(data),
            bone_count, bone_offset, animation_sources,
            merge_data,
        )
        detail["animation"] = animation[1]
    return bytes(output), texture_assets, detail, animation


def compile_rgb565_bmp(
    path: Path, target_width: int, target_height: int
) -> tuple[bytes, dict[str, object]]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise AssetError(f"{path.name} is not a Windows BMP")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    planes, bits = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if dib_size < 40 or width <= 0 or height == 0 or planes != 1 or \
            bits != 24 or compression != 0:
        raise AssetError(
            f"{path.name} must be an uncompressed 24-bit BMP"
        )
    source_height = abs(height)
    row_bytes = align(width * 3, 4)
    checked_blob_range(
        data, pixel_offset, row_bytes * source_height, "BMP pixels"
    )
    pixels = bytearray(target_width * target_height * 2)
    for y in range(target_height):
        source_y = y * source_height // target_height
        stored_y = source_height - 1 - source_y if height > 0 else source_y
        row = pixel_offset + stored_y * row_bytes
        for x in range(target_width):
            source_x = x * width // target_width
            blue, green, red = data[
                row + source_x * 3:row + source_x * 3 + 3
            ]
            struct.pack_into(
                "<H", pixels, (y * target_width + x) * 2,
                rgb565(red, green, blue)
            )
    output = IMG_HEADER.pack(
        b"C15IMG1\0", target_width, target_height, len(pixels)
    ) + pixels
    return bytes(output), {
        "name": path.name,
        "source": str(path),
        "source_bytes": len(data),
        "source_sha256": sha256(data),
        "source_width": width,
        "source_height": source_height,
        "width": target_width,
        "height": target_height,
        "pixel_format": "rgb565",
        "resident_bytes": len(pixels),
        "chunk_bytes": len(output),
    }


def pack_section(
    section_type: bytes,
    payload: bytes,
    count: int,
    stride: int,
    flags: int = 0,
) -> tuple[bytes, bytes, int, int, int]:
    if len(section_type) != 4:
        raise AssetError("section type must have four bytes")
    if count < 0 or stride < 0 or flags < 0:
        raise AssetError("negative section metadata")
    return section_type, payload, count, stride, flags


def light_for_face(
    face: tuple,
    face_positions: list[tuple[float, float, float]],
    texinfo: tuple,
    lighting: bytes,
) -> int:
    light_offset = face[9]
    if light_offset < 0 or not lighting:
        return 192
    vec_s = texinfo[0:4]
    vec_t = texinfo[4:8]
    values_s = [
        x * vec_s[0] + y * vec_s[1] + z * vec_s[2] + vec_s[3]
        for x, y, z in face_positions
    ]
    values_t = [
        x * vec_t[0] + y * vec_t[1] + z * vec_t[2] + vec_t[3]
        for x, y, z in face_positions
    ]
    mins_s = math.floor(min(values_s) / 16.0)
    maxs_s = math.ceil(max(values_s) / 16.0)
    mins_t = math.floor(min(values_t) / 16.0)
    maxs_t = math.ceil(max(values_t) / 16.0)
    sample_count = (maxs_s - mins_s + 1) * (maxs_t - mins_t + 1)
    byte_count = sample_count * 3
    if sample_count <= 0 or light_offset > len(lighting) - byte_count:
        return 192
    samples = lighting[light_offset : light_offset + byte_count]
    total = 0
    for index in range(0, len(samples), 3):
        total += (samples[index] * 77 + samples[index + 1] * 150 +
                  samples[index + 2] * 29) >> 8
    # The 9588 LCD loses the lowest GoldSrc lightmap levels; keeping a modest
    # offline floor makes Nuke/Office corridors readable without runtime
    # gamma tables or full-precision lightmaps.
    return max(48, min(255, total // sample_count))


def parse_entities(entity_blob: bytes) -> list[dict[str, str]]:
    text = entity_blob.decode("latin1", errors="replace")
    return [
        {
            key: value
            for key, value in re.findall(r'"([^"]*)"\s*"([^"]*)"', block)
        }
        for block in re.findall(r"\{([^}]*)\}", text, flags=re.DOTALL)
    ]


def compile_spawns(entity_blob: bytes) -> tuple[bytes, list[dict[str, int]]]:
    output = bytearray()
    manifest: list[dict[str, int]] = []
    for values in parse_entities(entity_blob):
        classname = values.get("classname", "").lower()
        if classname == "info_player_deathmatch":
            team = 1
        elif classname == "info_player_start":
            team = 2
        else:
            continue
        origin_values = values.get("origin", "").split()
        if len(origin_values) != 3:
            raise AssetError(f"spawn has invalid origin: {values.get('origin')!r}")
        origin = [
            checked_i16(float(value), "spawn origin") for value in origin_values
        ]
        yaw = checked_i16(float(values.get("angle", "0")), "spawn yaw")
        output += SPAWN.pack(origin[0], origin[1], origin[2], yaw, team, 0)
        manifest.append(
            {"x": origin[0], "y": origin[1], "z": origin[2],
             "yaw": yaw, "team": team}
        )
    if not manifest:
        raise AssetError("map contains no T/CT player spawns")
    return bytes(output), manifest


def compile_bomb_sites(
    entity_blob: bytes, models: list[tuple]
) -> tuple[bytes, list[dict[str, int]]]:
    output = bytearray()
    manifest: list[dict[str, int]] = []
    seen: set[tuple[int, int, int]] = set()
    for values in parse_entities(entity_blob):
        if values.get("classname", "").lower() not in (
            "func_bomb_target", "info_bomb_target"
        ):
            continue
        origin_values = values.get("origin", "").split()
        if len(origin_values) == 3:
            origin = tuple(
                checked_i16(float(value), "bomb-site origin")
                for value in origin_values
            )
        else:
            model_name = values.get("model", "")
            if not re.fullmatch(r"\*[0-9]+", model_name):
                continue
            model_index = int(model_name[1:])
            if model_index <= 0 or model_index >= len(models):
                continue
            model = models[model_index]
            origin = tuple(
                checked_i16(
                    (float(model[axis]) + float(model[axis + 3])) * 0.5,
                    "bomb-site brush center",
                )
                for axis in range(3)
            )
        if origin in seen:
            continue
        seen.add(origin)
        output += BOMB_SITE.pack(*origin)
        manifest.append({"x": origin[0], "y": origin[1], "z": origin[2]})
    return bytes(output), manifest


def entity_bounds(
    values: dict[str, str],
    models: list[tuple],
    label: str,
    point_radius: int = 96,
) -> tuple[int, int, int, int, int, int] | None:
    model_name = values.get("model", "")
    if re.fullmatch(r"\*[0-9]+", model_name):
        model_index = int(model_name[1:])
        if 0 < model_index < len(models):
            model = models[model_index]
            return tuple(
                checked_i16(model[index], f"{label} bound")
                for index in range(6)
            )
    origin_values = values.get("origin", "").split()
    if len(origin_values) != 3:
        return None
    origin = [
        checked_i16(float(value), f"{label} origin")
        for value in origin_values
    ]
    return (
        checked_i16(origin[0] - point_radius, f"{label} minimum x"),
        checked_i16(origin[1] - point_radius, f"{label} minimum y"),
        checked_i16(origin[2] - point_radius, f"{label} minimum z"),
        checked_i16(origin[0] + point_radius, f"{label} maximum x"),
        checked_i16(origin[1] + point_radius, f"{label} maximum y"),
        checked_i16(origin[2] + point_radius, f"{label} maximum z"),
    )


def compile_hostages(
    entity_blob: bytes,
) -> tuple[bytes, list[dict[str, int]]]:
    output = bytearray()
    manifest: list[dict[str, int]] = []
    for values in parse_entities(entity_blob):
        if values.get("classname", "").lower() != "hostage_entity":
            continue
        raw_origin = values.get("origin", "").split()
        if len(raw_origin) != 3:
            continue
        origin = [
            checked_i16(float(value), "hostage origin")
            for value in raw_origin
        ]
        output += HOSTAGE.pack(*origin)
        manifest.append(
            {"x": origin[0], "y": origin[1], "z": origin[2]}
        )
    return bytes(output), manifest


def compile_zones(
    entity_blob: bytes,
    models: list[tuple],
    classnames: tuple[str, ...],
    label: str,
    point_radius: int = 96,
    include_team: bool = False,
) -> tuple[bytes, list[dict[str, int]]]:
    output = bytearray()
    manifest: list[dict[str, int]] = []
    for values in parse_entities(entity_blob):
        if values.get("classname", "").lower() not in classnames:
            continue
        bounds = entity_bounds(values, models, label, point_radius)
        if bounds is None:
            continue
        team = 0
        if include_team:
            try:
                team = int(values.get("team", "0"))
            except ValueError:
                team = 0
            if team not in (1, 2):
                team = 0
        output += ZONE.pack(*bounds, team, 0)
        manifest.append({
            "minimum_x": bounds[0],
            "minimum_y": bounds[1],
            "minimum_z": bounds[2],
            "maximum_x": bounds[3],
            "maximum_y": bounds[4],
            "maximum_z": bounds[5],
            "team": team,
        })
    return bytes(output), manifest


def compile_dynamic_entities(
    entity_blob: bytes,
    models: list[tuple],
) -> tuple[bytes, list[dict[str, int | str]]]:
    kinds = {
        "func_door": 1,
        "func_door_rotating": 1,
        "func_button": 2,
        "func_breakable": 3,
        "func_plat": 4,
        "func_train": 4,
        "func_tracktrain": 4,
    }
    output = bytearray()
    manifest: list[dict[str, int | str]] = []
    for values in parse_entities(entity_blob):
        classname = values.get("classname", "").lower()
        kind = kinds.get(classname)
        if kind is None:
            continue
        bounds = entity_bounds(values, models, classname, 64)
        if bounds is None:
            continue
        model_name = values.get("model", "")
        model_index = (
            int(model_name[1:])
            if re.fullmatch(r"\*[0-9]+", model_name) else 0
        )
        target = values.get("target", "").lower()
        targetname = values.get("targetname", "").lower()
        flags = 1 if values.get("spawnflags", "0") == "1" else 0
        output += DYNAMIC_ENTITY.pack(
            kind, flags, model_index, *bounds,
            fnv1a(target) if target else 0,
            fnv1a(targetname) if targetname else 0,
        )
        manifest.append({
            "classname": classname,
            "kind": kind,
            "model": model_index,
            "target": target,
            "targetname": targetname,
        })
    return bytes(output), manifest


def compile_bsp(
    bsp: GoldSrcBsp, textures: list[MipTexture], selected_mips: list[int]
) -> tuple[bytes, dict[str, object]]:
    plane_source = unpack_array(bsp.lump(LUMP_PLANES), struct.Struct("<4fi"), "planes")
    vertex_source = unpack_array(bsp.lump(LUMP_VERTICES), struct.Struct("<3f"), "vertices")
    node_source = unpack_array(
        bsp.lump(LUMP_NODES), struct.Struct("<i2h6h2H"), "nodes"
    )
    texinfo_source = unpack_array(
        bsp.lump(LUMP_TEXINFO), struct.Struct("<8fii"), "texinfo"
    )
    face_source = unpack_array(
        bsp.lump(LUMP_FACES), struct.Struct("<HHiHH4Bi"), "faces"
    )
    clip_source = unpack_array(
        bsp.lump(LUMP_CLIPNODES), struct.Struct("<i2h"), "clipnodes"
    )
    leaf_source = unpack_array(
        bsp.lump(LUMP_LEAVES), struct.Struct("<ii6h2H4B"), "leaves"
    )
    edge_source = unpack_array(bsp.lump(LUMP_EDGES), struct.Struct("<2H"), "edges")
    surfedges = [
        item[0]
        for item in unpack_array(
            bsp.lump(LUMP_SURFEDGES), struct.Struct("<i"), "surfedges"
        )
    ]
    marksurfaces = [
        item[0]
        for item in unpack_array(
            bsp.lump(LUMP_MARKSURFACES), struct.Struct("<H"), "marksurfaces"
        )
    ]
    model_source = unpack_array(
        bsp.lump(LUMP_MODELS), struct.Struct("<9f4i3i"), "models"
    )
    lighting = bsp.lump(LUMP_LIGHTING)

    vertices_out = bytearray()
    surfaces_out = bytearray()
    surface_vertex_count = 0
    max_surface_vertices = 0
    for face_index, face in enumerate(face_source):
        _, _, first_edge, edge_count, texinfo_index = face[:5]
        if edge_count < 3:
            raise AssetError(f"face {face_index} has fewer than three edges")
        if first_edge < 0 or first_edge > len(surfedges) - edge_count:
            raise AssetError(f"face {face_index} surfedge range is invalid")
        if texinfo_index >= len(texinfo_source):
            raise AssetError(f"face {face_index} texinfo is invalid")
        texinfo = texinfo_source[texinfo_index]
        texture_id = texinfo[8]
        if texture_id < 0 or texture_id >= len(textures):
            raise AssetError(f"face {face_index} texture id is invalid")
        selected_mip = selected_mips[texture_id]
        scale = 1.0 / float(1 << selected_mip)
        face_positions: list[tuple[float, float, float]] = []
        face_uv: list[tuple[float, float]] = []
        for corner in range(edge_count):
            signed_edge = surfedges[first_edge + corner]
            edge_index = abs(signed_edge)
            if edge_index >= len(edge_source):
                raise AssetError(f"face {face_index} edge is invalid")
            edge = edge_source[edge_index]
            vertex_index = edge[0] if signed_edge >= 0 else edge[1]
            if vertex_index >= len(vertex_source):
                raise AssetError(f"face {face_index} vertex is invalid")
            position = vertex_source[vertex_index]
            face_positions.append(position)
            u = (
                position[0] * texinfo[0]
                + position[1] * texinfo[1]
                + position[2] * texinfo[2]
                + texinfo[3]
            )
            v = (
                position[0] * texinfo[4]
                + position[1] * texinfo[5]
                + position[2] * texinfo[6]
                + texinfo[7]
            )
            face_uv.append((u * scale, v * scale))
        # Shift whole-surface UVs by texture periods to keep Q12.4 in int16
        # without changing repeated sampling.
        tw = max(1, textures[texture_id].width >> selected_mip)
        th = max(1, textures[texture_id].height >> selected_mip)
        minimum_u = min(item[0] for item in face_uv)
        maximum_u = max(item[0] for item in face_uv)
        minimum_v = min(item[1] for item in face_uv)
        maximum_v = max(item[1] for item in face_uv)
        # Center the face's range before choosing a texture-period offset.
        # Basing the shift only on the minimum can turn an exactly representable
        # 2048.0 edge into +32768 in Q12.4 (de_inferno face 3910).
        base_u = round(((minimum_u + maximum_u) * 0.5) / tw) * tw
        base_v = round(((minimum_v + maximum_v) * 0.5) / th) * th
        for position, uv in zip(face_positions, face_uv):
            vertices_out += SURFACE_VERTEX.pack(
                checked_i16(position[0], f"face {face_index} x"),
                checked_i16(position[1], f"face {face_index} y"),
                checked_i16(position[2], f"face {face_index} z"),
                checked_i16((uv[0] - base_u) * 16.0, f"face {face_index} u"),
                checked_i16((uv[1] - base_v) * 16.0, f"face {face_index} v"),
            )
        brightness = light_for_face(face, face_positions, texinfo, lighting)
        style = face[5]
        surface_flags = (texinfo[9] & 0x7FFF) | (0x8000 if face[1] else 0)
        surfaces_out += SURFACE.pack(
            surface_vertex_count,
            edge_count,
            texture_id,
            face[0],
            surface_flags,
            brightness,
            style,
            0,
        )
        surface_vertex_count += edge_count
        max_surface_vertices = max(max_surface_vertices, edge_count)

    planes_out = bytearray()
    for nx, ny, nz, distance, plane_type in plane_source:
        normal = [checked_i16(value * 16384.0, "plane normal") for value in (nx, ny, nz)]
        signbits = ((1 if nx < 0 else 0) | (2 if ny < 0 else 0) |
                    (4 if nz < 0 else 0))
        planes_out += PLANE.pack(
            normal[0], normal[1], normal[2],
            int(round(distance * 16.0)), plane_type & 0xFF, signbits, 0
        )

    nodes_out = bytearray()
    for item in node_source:
        nodes_out += NODE.pack(item[0], item[1], item[2])
    leaves_out = bytearray()
    for item in leaf_source:
        leaves_out += LEAF.pack(item[0], item[1], item[8], item[9])
    clips_out = bytearray()
    for item in clip_source:
        clips_out += CLIPNODE.pack(*item)
    marks_out = b"".join(struct.pack("<H", value) for value in marksurfaces)
    models_out = bytearray()
    for item in model_source:
        bounds = [checked_i16(value, "model bound/origin") for value in item[:9]]
        models_out += MODEL.pack(*bounds, *item[9:13], *item[13:16])
    texture_names = b"".join(fixed_name(texture.name, 16) for texture in textures)
    spawns_out, spawns_manifest = compile_spawns(bsp.lump(LUMP_ENTITIES))
    bomb_sites_out, bomb_sites_manifest = compile_bomb_sites(
        bsp.lump(LUMP_ENTITIES), model_source
    )
    hostages_out, hostages_manifest = compile_hostages(
        bsp.lump(LUMP_ENTITIES)
    )
    rescue_out, rescue_manifest = compile_zones(
        bsp.lump(LUMP_ENTITIES), model_source,
        ("func_hostage_rescue", "info_hostage_rescue"),
        "hostage rescue zone", 128,
    )
    buy_out, buy_manifest = compile_zones(
        bsp.lump(LUMP_ENTITIES), model_source,
        ("func_buyzone",), "buy zone", 160, True,
    )
    ladders_out, ladders_manifest = compile_zones(
        bsp.lump(LUMP_ENTITIES), model_source,
        ("func_ladder",), "ladder", 32,
    )
    dynamic_out, dynamic_manifest = compile_dynamic_entities(
        bsp.lump(LUMP_ENTITIES), model_source
    )

    bounds_values: list[float] = []
    if model_source:
        bounds_values = list(model_source[0][:6])
    elif vertex_source:
        for axis in range(3):
            bounds_values.append(min(vertex[axis] for vertex in vertex_source))
        for axis in range(3):
            bounds_values.append(max(vertex[axis] for vertex in vertex_source))
    else:
        bounds_values = [0.0] * 6
    bounds = [checked_i16(value, "world bounds") for value in bounds_values]

    sections = [
        pack_section(b"VERT", bytes(vertices_out), surface_vertex_count, SURFACE_VERTEX.size),
        pack_section(b"SURF", bytes(surfaces_out), len(face_source), SURFACE.size),
        pack_section(b"PLAN", bytes(planes_out), len(plane_source), PLANE.size),
        pack_section(b"NODE", bytes(nodes_out), len(node_source), NODE.size),
        pack_section(b"LEAF", bytes(leaves_out), len(leaf_source), LEAF.size),
        pack_section(b"MARK", marks_out, len(marksurfaces), 2),
        pack_section(
            b"VISI", bsp.lump(LUMP_VISIBILITY),
            len(bsp.lump(LUMP_VISIBILITY)), 1, 1
        ),
        pack_section(b"CLIP", bytes(clips_out), len(clip_source), CLIPNODE.size),
        pack_section(b"MODL", bytes(models_out), len(model_source), MODEL.size),
        pack_section(b"TNAM", texture_names, len(textures), 16),
        pack_section(b"SPWN", spawns_out, len(spawns_manifest), SPAWN.size),
        pack_section(
            b"BSIT", bomb_sites_out,
            len(bomb_sites_manifest), BOMB_SITE.size
        ),
        pack_section(
            b"HSTG", hostages_out,
            len(hostages_manifest), HOSTAGE.size
        ),
        pack_section(
            b"RSQZ", rescue_out,
            len(rescue_manifest), ZONE.size
        ),
        pack_section(
            b"BYZN", buy_out,
            len(buy_manifest), ZONE.size
        ),
        pack_section(
            b"LADR", ladders_out,
            len(ladders_manifest), ZONE.size
        ),
        pack_section(
            b"DENT", dynamic_out,
            len(dynamic_manifest), DYNAMIC_ENTITY.size
        ),
    ]
    directory_size = len(sections) * BSP_SECTION.size
    cursor = align(BSP_HEADER.size + directory_size, BSP_SECTION_ALIGNMENT)
    records: list[bytes] = []
    placements: list[tuple[int, bytes]] = []
    for section_type, payload, count, stride, flags in sections:
        cursor = align(cursor, BSP_SECTION_ALIGNMENT)
        records.append(
            BSP_SECTION.pack(
                section_type, cursor, len(payload), count, stride, flags,
                crc32(payload), bytes(8)
            )
        )
        placements.append((cursor, payload))
        cursor += len(payload)
    output = bytearray(cursor)
    output[: BSP_HEADER.size] = BSP_HEADER.pack(
        BSP_MAGIC,
        BSP_VERSION,
        len(sections),
        len(output),
        crc32(bsp.data),
        0,
        *bounds,
        bytes(24),
    )
    directory_offset = BSP_HEADER.size
    for record in records:
        output[directory_offset : directory_offset + len(record)] = record
        directory_offset += len(record)
    for offset, payload in placements:
        output[offset : offset + len(payload)] = payload

    return bytes(output), {
        "source": str(bsp.path),
        "source_bytes": len(bsp.data),
        "source_sha256": sha256(bsp.data),
        "source_crc32": f"{crc32(bsp.data):08x}",
        "bounds": bounds,
        "planes": len(plane_source),
        "nodes": len(node_source),
        "leaves": len(leaf_source),
        "clipnodes": len(clip_source),
        "models": len(model_source),
        "surfaces": len(face_source),
        "surface_vertices": surface_vertex_count,
        "max_surface_vertices": max_surface_vertices,
        "textures": len(textures),
        "spawns": spawns_manifest,
        "bomb_sites": bomb_sites_manifest,
        "hostages": hostages_manifest,
        "rescue_zones": rescue_manifest,
        "buy_zones": buy_manifest,
        "ladders": ladders_manifest,
        "dynamic_entities": dynamic_manifest,
        "visibility_bytes": len(bsp.lump(LUMP_VISIBILITY)),
        "source_lighting_bytes": len(lighting),
        "chunk_bytes": len(output),
    }


def validate_texture_chunk(data: bytes) -> dict[str, int]:
    if len(data) < TEX_HEADER.size + 128:
        raise AssetError("C15TEX chunk is truncated")
    magic, width, height, flags, colors, pixel_bytes, sw, sh, mip, _ = (
        TEX_HEADER.unpack_from(data)
    )
    if magic != b"CTX1" or colors not in (64, 256):
        raise AssetError("invalid C15TEX header")
    if width == 0 or height == 0 or width * height != pixel_bytes:
        raise AssetError("invalid C15TEX dimensions")
    palette_bytes = colors * 2
    if len(data) != TEX_HEADER.size + palette_bytes + pixel_bytes:
        raise AssetError("C15TEX size mismatch")
    return {
        "width": width,
        "height": height,
        "flags": flags,
        "palette_colors": colors,
        "source_width": sw,
        "source_height": sh,
        "mip": mip,
        "resident_bytes": palette_bytes + pixel_bytes,
    }


def validate_model_chunk(data: bytes) -> dict[str, object]:
    if len(data) < MDL_HEADER.size:
        raise AssetError("C15MDL chunk is truncated")
    unpacked = MDL_HEADER.unpack_from(data)
    (
        magic, version, file_size, source_crc,
        vertex_count, triangle_count, texture_count,
        vertex_offset, triangle_offset, reference_offset,
        *tail,
    ) = unpacked
    if magic != b"C15MDL1\0" or version != 1 or file_size != len(data):
        raise AssetError("invalid C15MDL header")
    if vertex_count == 0 or vertex_count > 65535:
        raise AssetError("invalid C15MDL vertex count")
    if triangle_count == 0 or texture_count == 0 or texture_count > 64:
        raise AssetError("invalid C15MDL triangle/texture count")
    vertex_bytes = vertex_count * MDL_VERTEX.size
    triangle_bytes = triangle_count * MDL_TRIANGLE.size
    reference_bytes = texture_count * 4
    if (
        vertex_offset < MDL_HEADER.size
        or vertex_offset > len(data) - vertex_bytes
        or triangle_offset > len(data) - triangle_bytes
        or reference_offset > len(data) - reference_bytes
    ):
        raise AssetError("C15MDL array is outside chunk")
    ranges = sorted([
        (vertex_offset, vertex_offset + vertex_bytes, "vertices"),
        (triangle_offset, triangle_offset + triangle_bytes, "triangles"),
        (reference_offset, reference_offset + reference_bytes, "textures"),
    ])
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            raise AssetError(
                f"C15MDL arrays overlap: {previous[2]} and {current[2]}"
            )
    for index in range(triangle_count):
        a, b, c, texture, _ = MDL_TRIANGLE.unpack_from(
            data, triangle_offset + index * MDL_TRIANGLE.size
        )
        if max(a, b, c) >= vertex_count or texture >= texture_count:
            raise AssetError(f"C15MDL triangle {index} is invalid")
    return {
        "source_crc32": f"{source_crc:08x}",
        "vertices": vertex_count,
        "triangles": triangle_count,
        "textures": texture_count,
        "bounds_q4": list(tail[:6]),
        "resident_bytes": len(data),
    }


def validate_animation_chunk(data: bytes) -> dict[str, object]:
    if len(data) < ANM_HEADER.size:
        raise AssetError("C15ANM chunk is truncated")
    (
        magic, version, file_size, source_crc,
        vertex_count, sequence_count, frame_stride,
    ) = ANM_HEADER.unpack_from(data)
    if magic != b"C15ANM1\0" or version != 1 or file_size != len(data):
        raise AssetError("invalid C15ANM header")
    if (
        vertex_count == 0 or vertex_count > 65535
        or sequence_count == 0 or sequence_count > 16
        or frame_stride != vertex_count * ANM_POSITION.size
    ):
        raise AssetError("invalid C15ANM dimensions")
    table_end = ANM_HEADER.size + sequence_count * ANM_SEQUENCE.size
    if table_end > len(data):
        raise AssetError("C15ANM sequence table is truncated")
    tags: set[bytes] = set()
    ranges: list[tuple[int, int, str]] = []
    sequences: list[dict[str, object]] = []
    for index in range(sequence_count):
        (
            tag, frame_count, frame_ms, source_sequence,
            source_frames, source_fps_q8, frame_offset,
        ) = ANM_SEQUENCE.unpack_from(
            data, ANM_HEADER.size + index * ANM_SEQUENCE.size
        )
        frame_bytes = frame_count * frame_stride
        if (
            tag in tags or frame_count == 0 or frame_ms == 0
            or source_frames == 0 or source_fps_q8 == 0
            or frame_offset < table_end
            or frame_offset > len(data) - frame_bytes
        ):
            raise AssetError(f"C15ANM sequence {index} is invalid")
        tags.add(tag)
        label = tag.decode("ascii")
        ranges.append((frame_offset, frame_offset + frame_bytes, label))
        sequences.append({
            "action": label,
            "frame_count": frame_count,
            "frame_ms": frame_ms,
            "source_sequence": source_sequence,
            "source_frames": source_frames,
            "source_fps_q8": source_fps_q8,
            "frame_offset": frame_offset,
        })
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            raise AssetError(
                f"C15ANM sequences overlap: {previous[2]} and {current[2]}"
            )
    return {
        "source_crc32": f"{source_crc:08x}",
        "vertices": vertex_count,
        "frame_stride": frame_stride,
        "sequences": sequences,
        "streamed_bytes": len(data),
    }


def validate_muzzle_chunk(data: bytes) -> dict[str, int]:
    if len(data) < MUZZLE_HEADER.size:
        raise AssetError("C15 muzzle sprite is truncated")
    unpacked = MUZZLE_HEADER.unpack_from(data)
    (
        magic, file_size, width, height, frame_count,
        anchor_count, display_size, reserved, *_palette,
    ) = unpacked
    frame_bytes = (width * height + 1) // 2
    expected = (
        MUZZLE_HEADER.size +
        anchor_count * MUZZLE_ANCHOR.size +
        frame_count * frame_bytes
    )
    if (
        magic != b"MSP1" or file_size != len(data)
        or width == 0 or width > 32 or height == 0 or height > 32
        or frame_count == 0 or frame_count > 4
        or anchor_count == 0 or anchor_count > 16
        or display_size == 0 or display_size > 64 or reserved != 0
        or expected != len(data)
    ):
        raise AssetError("invalid C15 muzzle sprite")
    return {
        "width": width,
        "height": height,
        "frames": frame_count,
        "anchor_frames": anchor_count,
        "display_size": display_size,
        "resident_bytes": len(data),
    }


def validate_bsp_chunk(data: bytes) -> dict[str, object]:
    if len(data) < BSP_HEADER.size:
        raise AssetError("C15BSP chunk is truncated")
    magic, version, section_count, file_size, source_crc, flags, *tail = (
        BSP_HEADER.unpack_from(data)
    )
    if magic != BSP_MAGIC or version != BSP_VERSION or file_size != len(data):
        raise AssetError("invalid C15BSP header")
    if section_count == 0 or section_count > 64:
        raise AssetError("invalid C15BSP section count")
    directory_end = BSP_HEADER.size + section_count * BSP_SECTION.size
    if directory_end > len(data):
        raise AssetError("C15BSP section table is truncated")
    sections: dict[str, dict[str, int]] = {}
    ranges: list[tuple[int, int, str]] = []
    for index in range(section_count):
        item = BSP_SECTION.unpack_from(data, BSP_HEADER.size + index * BSP_SECTION.size)
        kind_raw, offset, size, count, stride, section_flags, checksum, _ = item
        kind = kind_raw.decode("ascii")
        if kind in sections:
            raise AssetError(f"duplicate C15BSP section {kind}")
        if offset % BSP_SECTION_ALIGNMENT or offset > len(data) - size:
            raise AssetError(f"C15BSP section {kind} is outside chunk")
        payload = data[offset : offset + size]
        if crc32(payload) != checksum:
            raise AssetError(f"C15BSP section {kind} CRC mismatch")
        if stride and count * stride != size:
            raise AssetError(f"C15BSP section {kind} count/stride mismatch")
        sections[kind] = {
            "offset": offset,
            "size": size,
            "count": count,
            "stride": stride,
            "flags": section_flags,
        }
        ranges.append((offset, offset + size, kind))
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            raise AssetError(
                f"C15BSP sections overlap: {previous[2]} and {current[2]}"
            )
    required = {
        "VERT", "SURF", "PLAN", "NODE", "LEAF", "MARK",
        "VISI", "CLIP", "MODL", "TNAM", "SPWN", "BSIT",
        "HSTG", "RSQZ", "BYZN", "LADR", "DENT",
    }
    if required - sections.keys():
        raise AssetError(f"missing C15BSP sections: {sorted(required - sections.keys())}")
    return {
        "source_crc32": f"{source_crc:08x}",
        "flags": flags,
        "bounds": list(tail[:6]),
        "sections": sections,
    }


@dataclass(frozen=True)
class PakAsset:
    kind: bytes
    name: str
    data: bytes
    flags: int = 0


def build_pack(assets: Sequence[PakAsset]) -> bytes:
    normalized: set[str] = set()
    identifiers: set[int] = set()
    for asset in assets:
        if len(asset.kind) != 4:
            raise AssetError("pack entry kind must be four bytes")
        if asset.name != asset.name.lower() or "\\" in asset.name:
            raise AssetError(f"pack name is not normalized: {asset.name}")
        if asset.name in normalized:
            raise AssetError(f"duplicate pack name: {asset.name}")
        identifier = fnv1a(asset.name)
        if identifier in identifiers:
            raise AssetError(f"asset ID collision: {asset.name}")
        normalized.add(asset.name)
        identifiers.add(identifier)

    ordered_assets = sorted(assets, key=lambda asset: fnv1a(asset.name))
    directory_offset = PAK_HEADER.size
    data_offset = align(
        directory_offset + len(ordered_assets) * PAK_ENTRY.size,
        PAK_ALIGNMENT,
    )
    cursor = data_offset
    entries: list[bytes] = []
    placements: list[tuple[int, bytes]] = []
    for asset in ordered_assets:
        cursor = align(cursor, PAK_ALIGNMENT)
        entries.append(
            PAK_ENTRY.pack(
                asset.kind,
                asset.flags,
                fnv1a(asset.name),
                cursor,
                len(asset.data),
                len(asset.data),
                crc32(asset.data),
                fixed_name(asset.name, 32),
                0,
            )
        )
        placements.append((cursor, asset.data))
        cursor += len(asset.data)
    output = bytearray(cursor)
    output[: PAK_HEADER.size] = PAK_HEADER.pack(
        PAK_MAGIC,
        PAK_VERSION,
        PAK_ENDIAN,
        len(output),
        len(ordered_assets),
        directory_offset,
        data_offset,
        bytes(32),
    )
    offset = directory_offset
    for entry in entries:
        output[offset : offset + len(entry)] = entry
        offset += len(entry)
    for placement, payload in placements:
        output[placement : placement + len(payload)] = payload
    return bytes(output)


def compile_sound_bank(cstrike: Path) -> tuple[bytes, dict[str, object]]:
    converted: list[bytes] = []
    cue_manifest: list[dict[str, object]] = []
    for relative in SOUND_CUE_SOURCES:
        path = cstrike / "sound" / relative
        if not path.is_file():
            raise AssetError(f"historical sound not found: {path}")
        with wave.open(str(path), "rb") as source:
            if (
                source.getcomptype() != "NONE"
                or source.getnchannels() != 1
                or source.getframerate() not in (11025, 22050)
                or source.getsampwidth() not in (1, 2)
            ):
                raise AssetError(
                    f"{path.name} must be 11025/22050 Hz mono PCM, "
                    "8 or 16 bit"
                )
            source_frames = source.getnframes()
            source_rate = source.getframerate()
            raw = source.readframes(source_frames)
            if source.getsampwidth() == 1:
                pcm = bytearray(len(raw) * 2)
                for index, value in enumerate(raw):
                    struct.pack_into("<h", pcm, index * 2, (value - 128) << 8)
                output = bytes(pcm)
            else:
                output = raw
            if source_rate == 22050:
                output = b"".join(
                    output[index:index + 2]
                    for index in range(0, len(output), 4)
                )
        converted.append(output)
        cue_manifest.append({
            "source": str(path),
            "source_rate": source_rate,
            "source_bits": len(raw) * 8 // max(source_frames, 1),
            "samples": len(output) // 2,
            "pcm_bytes": len(output),
        })

    cursor = SOUND_HEADER.size + len(converted) * SOUND_CUE.size
    directory = bytearray()
    payload = bytearray()
    for pcm in converted:
        directory.extend(SOUND_CUE.pack(cursor, len(pcm)))
        payload.extend(pcm)
        cursor += len(pcm)
    chunk = (
        SOUND_HEADER.pack(b"C15SND1\0", 11025, len(converted), 0)
        + bytes(directory)
        + bytes(payload)
    )
    return chunk, {
        "name": "sound/game",
        "sample_rate": 11025,
        "bits": 16,
        "channels": 1,
        "cues": cue_manifest,
        "resident_bytes": 0,
        "streamed_bytes": len(chunk),
    }


def validate_sound_chunk(data: bytes) -> dict[str, object]:
    if len(data) < SOUND_HEADER.size:
        raise AssetError("sound bank is truncated")
    magic, sample_rate, count, reserved = SOUND_HEADER.unpack_from(data)
    if (
        magic != b"C15SND1\0"
        or sample_rate != 11025
        or count != len(SOUND_CUE_SOURCES)
        or reserved != 0
        or len(data) < SOUND_HEADER.size + count * SOUND_CUE.size
    ):
        raise AssetError("invalid sound bank header")
    cues: list[dict[str, int]] = []
    minimum_offset = SOUND_HEADER.size + count * SOUND_CUE.size
    for index in range(count):
        offset, size = SOUND_CUE.unpack_from(
            data, SOUND_HEADER.size + index * SOUND_CUE.size
        )
        if (
            offset < minimum_offset
            or offset & 1
            or size & 1
            or offset > len(data) - size
        ):
            raise AssetError(f"sound cue {index} is outside bank")
        cues.append({"offset": offset, "bytes": size})
    return {
        "sample_rate": sample_rate,
        "bits": 16,
        "channels": 1,
        "cues": cues,
    }


def validate_pack(data: bytes, deep: bool = True) -> dict[str, object]:
    if len(data) < PAK_HEADER.size:
        raise AssetError("C15PAK is truncated")
    magic, version, endian, file_size, count, directory, data_offset, _ = (
        PAK_HEADER.unpack_from(data)
    )
    if magic != PAK_MAGIC or version != PAK_VERSION or endian != PAK_ENDIAN:
        raise AssetError("invalid C15PAK header")
    if file_size != len(data):
        raise AssetError("C15PAK file-size mismatch")
    if count > 4096 or directory > len(data) - count * PAK_ENTRY.size:
        raise AssetError("C15PAK directory is outside file")
    entries: list[dict[str, object]] = []
    ranges: list[tuple[int, int, str]] = []
    ids: set[int] = set()
    names: set[str] = set()
    previous_identifier = -1
    for index in range(count):
        item = PAK_ENTRY.unpack_from(data, directory + index * PAK_ENTRY.size)
        kind_raw, flags, identifier, offset, packed, unpacked, checksum, name_raw, _ = item
        kind = kind_raw.decode("ascii")
        name = read_c_string(name_raw)
        if identifier != fnv1a(name) or identifier in ids or name in names:
            raise AssetError(f"invalid or duplicate C15PAK identity: {name}")
        if identifier <= previous_identifier:
            raise AssetError("C15PAK directory is not sorted by asset ID")
        if offset % PAK_ALIGNMENT or offset < data_offset or offset > len(data) - packed:
            raise AssetError(f"C15PAK entry outside file: {name}")
        if packed != unpacked:
            raise AssetError(f"unsupported compressed entry: {name}")
        payload = data[offset : offset + packed]
        if crc32(payload) != checksum:
            raise AssetError(f"C15PAK CRC mismatch: {name}")
        detail: dict[str, object] = {}
        if deep and kind == "BSP0":
            detail = validate_bsp_chunk(payload)
        elif deep and kind == "TEX0":
            detail = validate_texture_chunk(payload)
        elif deep and kind == "MDL0":
            detail = validate_model_chunk(payload)
        elif deep and kind == "ANM0":
            detail = validate_animation_chunk(payload)
        elif deep and kind == "MSP0":
            detail = validate_muzzle_chunk(payload)
        elif deep and kind == "SND0":
            detail = validate_sound_chunk(payload)
        entries.append(
            {
                "type": kind,
                "name": name,
                "asset_id": f"{identifier:08x}",
                "offset": offset,
                "bytes": packed,
                "crc32": f"{checksum:08x}",
                "flags": flags,
                "detail": detail,
            }
        )
        ids.add(identifier)
        names.add(name)
        previous_identifier = identifier
        ranges.append((offset, offset + packed, name))
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            raise AssetError(
                f"C15PAK entries overlap: {previous[2]} and {current[2]}"
            )
    return {
        "version": version,
        "file_bytes": len(data),
        "entry_count": count,
        "entries": entries,
    }


def resolve_texture(
    texture: MipTexture, wad_index: WadIndex
) -> MipTexture | None:
    if texture.blob is not None:
        return texture
    return wad_index.load(texture.name)


def parse_map_specification(
    cstrike: Path, specification: str
) -> tuple[str, Path]:
    parts = specification.split("=", 1)
    name = parts[0].strip().lower()
    if not re.fullmatch(r"[a-z0-9_]+", name):
        raise AssetError(f"invalid map asset name: {parts[0]}")
    if len(parts) == 1:
        source = cstrike / "maps" / f"{name}.bsp"
    else:
        raw_source = parts[1].strip()
        if not raw_source:
            raise AssetError(f"map source path is empty: {specification}")
        source = Path(raw_source).expanduser()
    source = source.resolve()
    if not source.is_file():
        raise AssetError(f"map not found: {source}")
    return name, source


def placeholder_texture(
    reference: MipTexture,
) -> tuple[bytes, dict[str, object]]:
    # Special tool/sky surfaces do not sample a WAD texture at runtime.
    placeholder_pixels = bytes([0] * 16 * 16)
    placeholder_palette = bytearray(128)
    struct.pack_into("<H", placeholder_palette, 0, rgb565(48, 80, 96))
    chunk = TEX_HEADER.pack(
        b"CTX1", 16, 16, 0, 64, len(placeholder_pixels),
        16, 16, 0, bytes(3)
    ) + bytes(placeholder_palette) + placeholder_pixels
    return chunk, {
        "name": reference.name,
        "source": "procedural-special",
        "source_width": reference.width,
        "source_height": reference.height,
        "width": 16,
        "height": 16,
        "selected_mip": 0,
        "resident_bytes": len(placeholder_palette) + len(placeholder_pixels),
        "chunk_bytes": len(chunk),
        "transparent": False,
        "special": True,
    }


def build_command(args: argparse.Namespace) -> None:
    cstrike = args.cstrike.resolve()
    wad_roots = [cstrike]
    if args.valve:
        wad_roots.append(args.valve.resolve())
    wad_index = WadIndex(wad_roots)

    specifications = args.map or ["de_dust2"]
    map_sources = [
        parse_map_specification(cstrike, specification)
        for specification in specifications
    ]
    map_names = [name for name, _ in map_sources]
    if len(set(map_names)) != len(map_names):
        raise AssetError("duplicate map asset name")

    map_assets: list[PakAsset] = []
    map_manifest: list[dict[str, object]] = []
    texture_assets: dict[str, PakAsset] = {}
    texture_details: dict[str, dict[str, object]] = {}
    texture_manifest: list[dict[str, object]] = []
    missing_by_map: dict[str, list[str]] = {}
    drawable_missing_by_map: dict[str, list[str]] = {}
    for map_name, map_path in map_sources:
        bsp = GoldSrcBsp(map_path)
        references = parse_bsp_textures(bsp)
        runtime_references: list[MipTexture] = []
        selected_mips: list[int] = []
        missing_for_map: list[str] = []
        for reference in references:
            texture = resolve_texture(reference, wad_index)
            if texture is None:
                missing_for_map.append(reference.name)
                chunk, detail = placeholder_texture(reference)
            else:
                chunk, detail = compile_texture(texture)
            selected_mips.append(int(detail["selected_mip"]))
            runtime_reference = reference
            asset_name = f"tex/{runtime_reference.name}"
            previous = texture_assets.get(asset_name)
            if previous is not None and previous.data != chunk:
                # Some historical maps embed a different bitmap under a WAD
                # name also used by another map (for example Office's
                # painting1). Give only the conflicting copy a compact,
                # deterministic runtime name and rewrite this map's TNAM
                # reference. Shared identical textures remain deduplicated.
                suffix = f"{fnv1a(map_name + '/' + reference.name):08x}"
                alias = f"{reference.name[:6]}_{suffix}"[:15]
                asset_name = f"tex/{alias}"
                collision = texture_assets.get(asset_name)
                if collision is not None and collision.data != chunk:
                    raise AssetError(
                        f"texture alias collision: {asset_name}"
                    )
                runtime_reference = MipTexture(
                    alias, reference.width, reference.height,
                    reference.offsets, reference.blob, reference.source
                )
                previous = collision
            runtime_references.append(runtime_reference)
            if previous is None:
                texture_assets[asset_name] = PakAsset(
                    b"TEX0", asset_name, chunk
                )
                stored_detail = dict(detail)
                stored_detail["used_by_maps"] = [map_name]
                texture_details[asset_name] = stored_detail
            else:
                used_by = texture_details[asset_name]["used_by_maps"]
                assert isinstance(used_by, list)
                used_by.append(map_name)
        drawable_for_map = [
            name for name in missing_for_map
            if not name.lower().startswith(
                ("sky", "aaatrigger", "clip", "origin")
            )
        ]
        if drawable_for_map and not args.allow_missing:
            raise AssetError(
                f"{map_name} drawable textures not found in BSP/WAD roots: "
                + ", ".join(drawable_for_map)
                + " (provide the Half-Life valve WAD directory or use "
                  "--allow-missing only for an incomplete development build)"
            )
        bsp_chunk, bsp_detail = compile_bsp(
            bsp, runtime_references, selected_mips
        )
        bsp_detail["name"] = map_name
        map_assets.append(PakAsset(
            b"BSP0", f"maps/{map_name}", bsp_chunk
        ))
        map_manifest.append(bsp_detail)
        missing_by_map[map_name] = missing_for_map
        drawable_missing_by_map[map_name] = drawable_for_map

    texture_manifest.extend(texture_details.values())
    missing = list(dict.fromkeys(
        name
        for names in missing_by_map.values()
        for name in names
    ))
    drawable_missing = list(dict.fromkeys(
        name
        for names in drawable_missing_by_map.values()
        for name in names
    ))
    assets: list[PakAsset] = map_assets + list(texture_assets.values())
    model_manifest: list[dict[str, object]] = []
    muzzle_manifest: list[dict[str, object]] = []
    weapons = args.weapon or ["v_knife", "v_glock18", "v_ak47"]
    for weapon in weapons:
        normalized = weapon.lower()
        if normalized.endswith(".mdl"):
            normalized = normalized[:-4]
        if not re.fullmatch(r"v_[a-z0-9_]+", normalized):
            raise AssetError(f"invalid view-model name: {weapon}")
        model_path = cstrike / "models" / f"{normalized}.mdl"
        if not model_path.is_file():
            raise AssetError(f"view model not found: {model_path}")
        asset_base = f"mdl/{normalized}"
        model_chunk, model_textures, detail, animation = compile_studio_model(
            model_path, asset_base, with_animation=True,
            maximum_texture_dimension=32,
        )
        assets.append(PakAsset(b"MDL0", asset_base, model_chunk))
        if animation is None:
            raise AssetError(f"view animation was not built: {asset_base}")
        assets.append(PakAsset(
            b"ANM0", asset_base.replace("mdl/", "anim/", 1),
            animation[0]
        ))
        if normalized not in (
            "v_knife", "v_c4", "v_hegrenade",
            "v_flashbang", "v_smokegrenade"
        ):
            muzzle_number = (
                2 if normalized in ("v_ak47", "v_m4a1") else 1
            )
            muzzle_path = (
                cstrike / "sprites" / f"muzzleflash{muzzle_number}.spr"
            )
            if not muzzle_path.is_file():
                raise AssetError(
                    f"historical muzzle sprite not found: {muzzle_path}"
                )
            muzzle_chunk, muzzle_detail = compile_view_muzzle_sprite(
                muzzle_path, model_path.read_bytes(), asset_base,
                animation[1],
            )
            assets.append(PakAsset(
                b"MSP0", asset_base.replace("mdl/", "muzzle/", 1),
                muzzle_chunk,
            ))
            detail["muzzle"] = muzzle_detail
            muzzle_manifest.append(muzzle_detail)
        assets.extend(model_textures)
        model_manifest.append(detail)
    world_model_manifest: list[dict[str, object]] = []
    for specification in args.model or []:
        if "=" not in specification:
            raise AssetError(
                "world model must be SOURCE=ASSET, for example "
                "player/terror/terror=player_terror"
            )
        source_name, asset_name = (
            part.strip().lower() for part in specification.split("=", 1)
        )
        if source_name.endswith(".mdl"):
            source_name = source_name[:-4]
        if not re.fullmatch(r"[a-z0-9_/-]+", source_name) or ".." in source_name:
            raise AssetError(f"invalid world-model source: {source_name}")
        if not re.fullmatch(r"[a-z0-9_]+", asset_name):
            raise AssetError(f"invalid world-model asset name: {asset_name}")
        model_path = cstrike / "models" / f"{source_name}.mdl"
        if not model_path.is_file():
            raise AssetError(f"world model not found: {model_path}")
        asset_base = f"mdl/{asset_name}"
        player_pose = (
            "ref_aim_carbine"
            if asset_name.startswith("player_") else None
        )
        merge_source = {
            "p_ak47": "player/terror/terror",
            "p_m4a1": "player/urban/urban",
        }.get(asset_name)
        merge_path = (
            cstrike / "models" / f"{merge_source}.mdl"
            if merge_source is not None else None
        )
        model_chunk, model_textures, detail, locomotion = compile_studio_model(
            model_path, asset_base,
            with_locomotion_animation=True,
            maximum_texture_dimension=(
                16 if asset_name.startswith("player_") else 64
            ),
            pose_sequence_name=player_pose,
            bone_merge_path=merge_path,
        )
        assets.append(PakAsset(b"MDL0", asset_base, model_chunk))
        if locomotion is None:
            raise AssetError(
                f"world locomotion was not built: {asset_base}"
            )
        assets.append(PakAsset(
            b"ANM0", asset_base.replace("mdl/", "anim/", 1),
            locomotion[0],
        ))
        assets.extend(model_textures)
        world_model_manifest.append(detail)
    ui_manifest: list[dict[str, object]] = []
    if args.splash:
        splash_path = cstrike / "gfx" / "shell" / "splash.bmp"
        if not splash_path.is_file():
            raise AssetError(f"menu splash not found: {splash_path}")
        splash_chunk, splash_detail = compile_rgb565_bmp(
            splash_path, 320, 240
        )
        assets.append(PakAsset(b"IMG0", "ui/menu_splash", splash_chunk))
        ui_manifest.append(splash_detail)
    sound_manifest: list[dict[str, object]] = []
    if args.audio:
        sound_chunk, sound_detail = compile_sound_bank(cstrike)
        assets.append(PakAsset(b"SND0", "sound/game", sound_chunk))
        sound_manifest.append(sound_detail)
    assets.append(PakAsset(
        b"VER0", "meta/m15", b"CS15LITE-M15-MAPS-DROPS-SPECTATOR-TACTICS\0"
    ))
    pack = build_pack(assets)
    validation = validate_pack(pack)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pack)
    manifest = {
        "format": "CS15 Lite asset manifest",
        "format_version": 1,
        "runtime_resource_revision": "M15",
        "map": map_manifest[0],
        "maps": map_manifest,
        "textures": texture_manifest,
        "view_models": model_manifest,
        "muzzle_sprites": muzzle_manifest,
        "world_models": world_model_manifest,
        "ui_images": ui_manifest,
        "sounds": sound_manifest,
        "missing_special_textures": missing,
        "missing_drawable_textures": drawable_missing,
        "missing_textures_by_map": missing_by_map,
        "missing_drawable_textures_by_map": drawable_missing_by_map,
        "complete_historical_texture_set": not drawable_missing,
        "wads_scanned": wad_index.wads,
        "pack": {
            "path": str(args.output),
            "bytes": len(pack),
            "sha256": sha256(pack),
            "entries": validation["entry_count"],
            "resident_texture_bytes": sum(
                int(item["resident_bytes"]) for item in texture_manifest
            ),
        },
    }
    manifest_path = args.manifest or args.output.with_suffix(".manifest.json")
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(
        f"output={args.output} bytes={len(pack)} sha256={sha256(pack)} "
        f"entries={validation['entry_count']}"
    )
    for item in map_manifest:
        print(
            f"map={item['name']} surfaces={item['surfaces']} "
            f"surface_vertices={item['surface_vertices']} "
            f"chunk_bytes={item['chunk_bytes']}"
        )
    print(
        f"textures={len(texture_manifest)} "
        f"resident_bytes={manifest['pack']['resident_texture_bytes']} "
        f"missing_special={len(missing)}"
    )
    print(
        f"view_models={len(model_manifest)} "
        f"resident_bytes={sum(int(item['resident_bytes']) for item in model_manifest)}"
    )
    print(
        f"muzzle_sprites={len(muzzle_manifest)} "
        f"maximum_resident_bytes="
        f"{max((int(item['resident_bytes']) for item in muzzle_manifest), default=0)}"
    )
    print(
        f"world_models={len(world_model_manifest)} "
        f"resident_bytes="
        f"{sum(int(item['resident_bytes']) for item in world_model_manifest)}"
    )
    print(
        f"ui_images={len(ui_manifest)} "
        f"resident_bytes="
        f"{sum(int(item['resident_bytes']) for item in ui_manifest)}"
    )
    print(f"manifest={manifest_path}")


def inspect_command(args: argparse.Namespace) -> None:
    data = args.pack.read_bytes()
    result = validate_pack(data, deep=not args.shallow)
    result["sha256"] = sha256(data)
    print(json.dumps(result, indent=2, ensure_ascii=False))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    build = sub.add_parser(
        "build", help="compile one or more historical GoldSrc maps"
    )
    build.add_argument("--cstrike", type=Path, required=True)
    build.add_argument("--valve", type=Path)
    build.add_argument(
        "--map",
        action="append",
        help="map name, repeatable; use NAME=PATH for an external BSP; "
             "defaults to de_dust2",
    )
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--manifest", type=Path)
    build.add_argument(
        "--weapon",
        action="append",
        help="view model basename to compile; defaults to v_knife, "
             "v_glock18, and v_ak47",
    )
    build.add_argument(
        "--model",
        action="append",
        help="additional StudioMDL as SOURCE=ASSET, relative to cstrike/models; "
             "for example player/terror/terror=player_terror",
    )
    build.add_argument(
        "--splash",
        action="store_true",
        help="compile the historical gfx/shell/splash.bmp menu background",
    )
    build.add_argument(
        "--audio",
        action="store_true",
        help="compile the M15 historical combat/objective cues into sound/game",
    )
    build.add_argument(
        "--allow-missing",
        action="store_true",
        help="emit marked placeholders for missing drawable WAD textures; "
             "development only and never a final acceptance build",
    )
    build.set_defaults(function=build_command)
    inspect = sub.add_parser("inspect", help="validate and inspect C15PAK")
    inspect.add_argument("pack", type=Path)
    inspect.add_argument("--shallow", action="store_true")
    inspect.set_defaults(function=inspect_command)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        args.function(args)
    except (AssetError, OSError, struct.error, wave.Error) as error:
        raise SystemExit(f"assetc: error: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
