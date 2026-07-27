import importlib.util
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("assetc", ROOT / "tools" / "assetc.py")
assert SPEC and SPEC.loader
assetc = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = assetc
SPEC.loader.exec_module(assetc)


class AssetFormatTests(unittest.TestCase):
    def test_texture_and_pack_roundtrip(self):
        width = 4
        height = 4
        pixels = bytes(range(width * height))
        palette = b"".join(struct.pack("<H", index) for index in range(256))
        texture = assetc.TEX_HEADER.pack(
            b"CTX1", width, height, 0, 256, len(pixels),
            width, height, 0, bytes(3)
        ) + palette + pixels
        self.assertEqual(
            assetc.validate_texture_chunk(texture)["resident_bytes"],
            512 + len(pixels),
        )
        pack = assetc.build_pack(
            [assetc.PakAsset(b"TEX0", "tex/test", texture)]
        )
        result = assetc.validate_pack(pack)
        self.assertEqual(result["entry_count"], 1)
        self.assertEqual(result["entries"][0]["name"], "tex/test")
        self.assertEqual(len(pack) % 1, 0)

    def test_world_texture_is_mip_selected_and_quantized_to_64_colors(self):
        width = 64
        height = 32
        mip_sizes = [
            width * height,
            (width // 2) * (height // 2),
            (width // 4) * (height // 4),
            (width // 8) * (height // 8),
        ]
        offsets = []
        cursor = 40
        blob = bytearray(40 + sum(mip_sizes) + 2 + 256 * 3)
        for level, size in enumerate(mip_sizes):
            offsets.append(cursor)
            blob[cursor:cursor + size] = bytes(
                index & 0xff for index in range(size)
            )
            cursor += size
        struct.pack_into("<H", blob, cursor, 256)
        cursor += 2
        for index in range(256):
            blob[cursor + index * 3:cursor + index * 3 + 3] = bytes(
                (index, 255 - index, index // 2)
            )
        texture = assetc.MipTexture(
            "stone", width, height, tuple(offsets), bytes(blob), "test.wad"
        )
        chunk, detail = assetc.compile_texture(texture)
        validated = assetc.validate_texture_chunk(chunk)
        self.assertEqual(detail["selected_mip"], 1)
        self.assertEqual((detail["width"], detail["height"]), (32, 16))
        self.assertEqual(validated["palette_colors"], 64)
        self.assertEqual(validated["resident_bytes"], 64 * 2 + 32 * 16)

    def test_corrupt_pack_crc_is_rejected(self):
        texture = assetc.TEX_HEADER.pack(
            b"CTX1", 1, 1, 0, 256, 1, 1, 1, 0, bytes(3)
        ) + bytes(512) + b"\0"
        pack = bytearray(assetc.build_pack(
            [assetc.PakAsset(b"TEX0", "tex/test", texture)]
        ))
        entry = assetc.PAK_ENTRY.unpack_from(pack, assetc.PAK_HEADER.size)
        offset = entry[3]
        pack[offset] ^= 1
        with self.assertRaises(assetc.AssetError):
            assetc.validate_pack(bytes(pack))

    def test_studio_texture_target_dimension(self):
        width = 128
        height = 64
        chunk, detail, level = assetc.compile_studio_texture(
            "player.bmp",
            0,
            width,
            height,
            bytes(width * height),
            bytes(256 * 3),
            maximum_dimension=16,
        )
        self.assertEqual((detail["width"], detail["height"]), (16, 8))
        self.assertEqual(detail["resident_bytes"], 512 + 16 * 8)
        self.assertEqual(level, 3)
        self.assertEqual(
            assetc.validate_texture_chunk(chunk)["resident_bytes"],
            detail["resident_bytes"],
        )
        with self.assertRaises(assetc.AssetError):
            assetc.compile_studio_texture(
                "bad.bmp", 0, 1, 1, b"\0", bytes(256 * 3),
                maximum_dimension=0,
            )

    def test_name_normalization_is_enforced(self):
        with self.assertRaises(assetc.AssetError):
            assetc.build_pack([assetc.PakAsset(b"TEX0", "TEX/Bad", b"x")])

    def test_pack_directory_is_sorted_for_bounded_runtime_lookup(self):
        pack = assetc.build_pack([
            assetc.PakAsset(b"TEST", "test/zulu", b"z"),
            assetc.PakAsset(b"TEST", "test/alpha", b"a"),
            assetc.PakAsset(b"TEST", "test/mike", b"m"),
        ])
        identifiers = [
            assetc.PAK_ENTRY.unpack_from(
                pack, assetc.PAK_HEADER.size + index * assetc.PAK_ENTRY.size
            )[2]
            for index in range(3)
        ]
        self.assertEqual(identifiers, sorted(identifiers))
        swapped = bytearray(pack)
        first = assetc.PAK_HEADER.size
        second = first + assetc.PAK_ENTRY.size
        left = bytes(swapped[first:second])
        right = bytes(swapped[second:second + assetc.PAK_ENTRY.size])
        swapped[first:second] = right
        swapped[second:second + assetc.PAK_ENTRY.size] = left
        with self.assertRaises(assetc.AssetError):
            assetc.validate_pack(bytes(swapped))

    def test_model_chunk_roundtrip_and_bad_index(self):
        vertex_offset = assetc.MDL_HEADER.size
        vertices = b"".join([
            assetc.MDL_VERTEX.pack(0, 0, 0, 0, 0),
            assetc.MDL_VERTEX.pack(16, 0, 0, 255, 0),
            assetc.MDL_VERTEX.pack(0, 16, 0, 0, 255),
        ])
        triangle_offset = assetc.align(vertex_offset + len(vertices), 16)
        triangle = assetc.MDL_TRIANGLE.pack(0, 1, 2, 0, 0)
        reference_offset = assetc.align(triangle_offset + len(triangle), 16)
        model = bytearray(reference_offset + 4)
        model[:assetc.MDL_HEADER.size] = assetc.MDL_HEADER.pack(
            b"C15MDL1\0",
            1,
            len(model),
            0x12345678,
            3,
            1,
            1,
            vertex_offset,
            triangle_offset,
            reference_offset,
            0,
            0,
            0,
            16,
            16,
            0,
            bytes(8),
        )
        model[vertex_offset:vertex_offset + len(vertices)] = vertices
        model[triangle_offset:triangle_offset + len(triangle)] = triangle
        struct.pack_into("<I", model, reference_offset, 0x87654321)

        result = assetc.validate_model_chunk(bytes(model))
        self.assertEqual(result["vertices"], 3)
        self.assertEqual(result["triangles"], 1)
        self.assertEqual(result["textures"], 1)

        pack = assetc.build_pack(
            [assetc.PakAsset(b"MDL0", "mdl/test", bytes(model))]
        )
        deep = assetc.validate_pack(pack, deep=True)
        self.assertEqual(deep["entries"][0]["detail"]["triangles"], 1)

        struct.pack_into("<H", model, triangle_offset, 3)
        with self.assertRaises(assetc.AssetError):
            assetc.validate_model_chunk(bytes(model))

    def test_inspect_file_roundtrip(self):
        pack = assetc.build_pack([assetc.PakAsset(b"TEST", "test", b"abc")])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.c15pak"
            path.write_bytes(pack)
            self.assertEqual(assetc.validate_pack(path.read_bytes())["file_bytes"], len(pack))

    def test_studio_frame_zero_animation_delta(self):
        animation = bytearray(28)
        struct.pack_into("<H", animation, 0, assetc.STUDIO_ANIM_SIZE)
        animation[assetc.STUDIO_ANIM_SIZE] = 2
        animation[assetc.STUDIO_ANIM_SIZE + 1] = 4
        struct.pack_into("<2h", animation, assetc.STUDIO_ANIM_SIZE + 2, -27, 14)
        animation[assetc.STUDIO_ANIM_SIZE + 6] = 1
        animation[assetc.STUDIO_ANIM_SIZE + 7] = 2
        struct.pack_into("<h", animation, assetc.STUDIO_ANIM_SIZE + 8, 33)
        self.assertEqual(
            assetc.studio_animation_frame_zero_delta(
                bytes(animation), 0, 0, "test"
            ),
            -27,
        )
        self.assertEqual(
            assetc.studio_animation_frame_zero_delta(
                bytes(animation), 0, 1, "test"
            ),
            0,
        )
        self.assertEqual(
            [
                assetc.studio_animation_frame_delta(
                    bytes(animation), 0, 0, frame, "test"
                )
                for frame in range(6)
            ],
            [-27, 14, 14, 14, 33, 33],
        )

    def test_studio_bone_merge_override_keeps_child_local_transform(self):
        bone_count = 2
        bone_bytes = bone_count * assetc.STUDIO_BONE_SIZE
        data = bytearray(bone_bytes + bone_count * assetc.STUDIO_ANIM_SIZE)
        data[0:32] = assetc.fixed_name("Root", 32)
        struct.pack_into("<i", data, 32, -1)
        struct.pack_into("<6f", data, 64, 1, 2, 3, 0, 0, 0)
        data[112:144] = assetc.fixed_name("Child", 32)
        struct.pack_into("<i", data, 144, 0)
        struct.pack_into("<6f", data, 176, 4, 0, 0, 0, 0, 0)
        sequence = {
            "frames": 1,
            "blends": 1,
            "group": 0,
            "animation": bone_bytes,
        }
        override = assetc.studio_matrix((10, 20, 30, 0, 0, 0))
        matrices = assetc.studio_bone_matrices(
            bytes(data), bone_count, 0, sequence, 0, "merge-test",
            {"Root": override},
        )
        self.assertEqual(matrices[0][3::4], (10, 20, 30))
        self.assertEqual(matrices[1][3::4], (14, 20, 30))

    def test_hybrid_gait_keeps_upper_body_on_moving_pelvis(self):
        names = ["Bip01", "Bip01 Pelvis", "Bip01 Spine", "Bip01 L Thigh"]
        parents = [-1, 0, 1, 1]
        bone_bytes = len(names) * assetc.STUDIO_BONE_SIZE
        animation_bytes = len(names) * assetc.STUDIO_ANIM_SIZE
        upper_base = bone_bytes
        gait_base = upper_base + animation_bytes + len(names) * 4
        data = bytearray(gait_base + animation_bytes + len(names) * 4)
        for index, (name, parent) in enumerate(zip(names, parents)):
            bone = index * assetc.STUDIO_BONE_SIZE
            data[bone:bone + 32] = assetc.fixed_name(name, 32)
            struct.pack_into("<i", data, bone + 32, parent)
            struct.pack_into("<6f", data, bone + 88, 1, 1, 1, 1, 1, 1)

        def write_pose(base, values):
            stream = base + animation_bytes
            for index, value in enumerate(values):
                animation = base + index * assetc.STUDIO_ANIM_SIZE
                value_stream = stream + index * 4
                struct.pack_into("<H", data, animation, value_stream - animation)
                data[value_stream:value_stream + 2] = bytes((1, 1))
                struct.pack_into("<h", data, value_stream + 2, value)

        write_pose(upper_base, [0, 10, 100, 20])
        write_pose(gait_base, [1, 2, 200, 3])
        upper = {
            "frames": 1, "blends": 1, "group": 0,
            "animation": upper_base,
        }
        gait = {
            "frames": 1, "blends": 1, "group": 0,
            "animation": gait_base,
        }
        matrices = assetc.studio_hybrid_bone_matrices(
            bytes(data), len(names), 0, upper, 0, gait, 0, "hybrid-test"
        )
        self.assertEqual([matrix[3] for matrix in matrices], [1, 3, 103, 6])

    def test_bomb_sites_include_brush_centers_and_deduplicate_origins(self):
        entities = b"""
        {
        "classname" "func_bomb_target"
        "model" "*1"
        }
        {
        "classname" "info_bomb_target"
        "origin" "100 200 32"
        }
        {
        "classname" "info_bomb_target"
        "origin" "100 200 32"
        }
        """
        models = [
            tuple([0] * 16),
            (-32, 100, 0, 32, 300, 64) + tuple([0] * 10),
        ]
        payload, sites = assetc.compile_bomb_sites(entities, models)
        self.assertEqual(
            sites,
            [
                {"x": 0, "y": 200, "z": 32},
                {"x": 100, "y": 200, "z": 32},
            ],
        )
        self.assertEqual(
            [
                assetc.BOMB_SITE.unpack_from(payload, offset)
                for offset in range(0, len(payload), assetc.BOMB_SITE.size)
            ],
            [(0, 200, 32), (100, 200, 32)],
        )

    def test_animation_chunk_roundtrip(self):
        vertex_count = 3
        frame_stride = vertex_count * assetc.ANM_POSITION.size
        frame_offset = assetc.align(
            assetc.ANM_HEADER.size + assetc.ANM_SEQUENCE.size, 16
        )
        frames = bytes(frame_stride * 2)
        animation = bytearray(frame_offset + len(frames))
        animation[:assetc.ANM_HEADER.size] = assetc.ANM_HEADER.pack(
            b"C15ANM1\0", 1, len(animation), 0x12345678,
            vertex_count, 1, frame_stride,
        )
        animation[
            assetc.ANM_HEADER.size:
            assetc.ANM_HEADER.size + assetc.ANM_SEQUENCE.size
        ] = assetc.ANM_SEQUENCE.pack(
            b"FIRE", 2, 100, 3, 16, 20 * 256, frame_offset
        )
        animation[frame_offset:] = frames
        result = assetc.validate_animation_chunk(bytes(animation))
        self.assertEqual(result["vertices"], vertex_count)
        self.assertEqual(result["sequences"][0]["action"], "FIRE")
        pack = assetc.build_pack([
            assetc.PakAsset(b"ANM0", "anim/test", bytes(animation))
        ])
        deep = assetc.validate_pack(pack, deep=True)
        self.assertEqual(
            deep["entries"][0]["detail"]["frame_stride"], frame_stride
        )

    def test_rgb565_bmp_menu_conversion(self):
        width = 2
        height = 2
        row_bytes = 8
        pixels = (
            b"\xff\x00\x00" + b"\xff\xff\xff" + b"\0\0" +
            b"\x00\x00\xff" + b"\x00\xff\x00" + b"\0\0"
        )
        header = bytearray(54)
        header[:2] = b"BM"
        struct.pack_into("<I", header, 2, len(header) + len(pixels))
        struct.pack_into("<I", header, 10, len(header))
        struct.pack_into("<IiiHHII", header, 14, 40, width, height, 1, 24, 0, row_bytes * height)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "splash.bmp"
            source.write_bytes(header + pixels)
            chunk, detail = assetc.compile_rgb565_bmp(source, 1, 1)
        magic, out_width, out_height, pixel_bytes = (
            assetc.IMG_HEADER.unpack_from(chunk)
        )
        self.assertEqual(magic, b"C15IMG1\0")
        self.assertEqual((out_width, out_height, pixel_bytes), (1, 1, 2))
        self.assertEqual(struct.unpack_from("<H", chunk, assetc.IMG_HEADER.size)[0], 0xF800)
        self.assertEqual(detail["resident_bytes"], 2)

    def test_muzzle_chunk_roundtrip(self):
        width = 3
        height = 3
        frame_count = 2
        anchor_count = 2
        pixels = bytes((width * height + 1) // 2 * frame_count)
        anchors = (
            assetc.MUZZLE_ANCHOR.pack(16, -32, 48) +
            assetc.MUZZLE_ANCHOR.pack(17, -31, 49)
        )
        file_size = assetc.MUZZLE_HEADER.size + len(anchors) + len(pixels)
        chunk = assetc.MUZZLE_HEADER.pack(
            b"MSP1", file_size, width, height, frame_count,
            anchor_count, 12, 0, *([0] * 16)
        ) + anchors + pixels
        detail = assetc.validate_muzzle_chunk(chunk)
        self.assertEqual(detail["frames"], frame_count)
        pack = assetc.build_pack([
            assetc.PakAsset(b"MSP0", "muzzle/test", chunk)
        ])
        deep = assetc.validate_pack(pack, deep=True)
        self.assertEqual(
            deep["entries"][0]["detail"]["anchor_frames"],
            anchor_count,
        )

    def test_historical_sound_bank_roundtrip(self):
        self.assertEqual(
            assetc.SOUND_CUE_SOURCES,
            (
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
            ),
        )
        with tempfile.TemporaryDirectory() as directory:
            cstrike = Path(directory)
            for cue, relative in enumerate(assetc.SOUND_CUE_SOURCES):
                target = cstrike / "sound" / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with wave.open(str(target), "wb") as output:
                    output.setnchannels(1)
                    output.setsampwidth(1)
                    output.setframerate(11025 if cue == 0 else 22050)
                    output.writeframes(bytes([128 + cue, 127 - cue]) * 16)
            chunk, detail = assetc.compile_sound_bank(cstrike)
        self.assertEqual(detail["sample_rate"], 11025)
        self.assertEqual(
            len(detail["cues"]), len(assetc.SOUND_CUE_SOURCES)
        )
        validated = assetc.validate_sound_chunk(chunk)
        self.assertEqual(
            len(validated["cues"]), len(assetc.SOUND_CUE_SOURCES)
        )
        first_offset = validated["cues"][0]["offset"]
        self.assertEqual(validated["cues"][0]["bytes"], 64)
        self.assertEqual(
            struct.unpack_from("<h", chunk, first_offset)[0], 0
        )
        pack = assetc.build_pack([
            assetc.PakAsset(b"SND0", "sound/game", chunk)
        ])
        deep = assetc.validate_pack(pack)
        self.assertEqual(
            deep["entries"][0]["detail"]["sample_rate"], 11025
        )

    def test_objective_zone_and_dynamic_entity_compilation(self):
        entities = b"""
        {
        "classname" "hostage_entity"
        "origin" "10 20 30"
        }
        {
        "classname" "info_hostage_rescue"
        "origin" "100 200 300"
        }
        {
        "classname" "func_buyzone"
        "model" "*1"
        "team" "2"
        }
        {
        "classname" "func_door"
        "model" "*1"
        "targetname" "front_door"
        }
        {
        "classname" "func_button"
        "origin" "32 48 64"
        "target" "front_door"
        }
        """
        models = [
            (0, 0, 0, 0, 0, 0),
            (-16, -32, -8, 16, 32, 72),
        ]
        hostage_data, hostages = assetc.compile_hostages(entities)
        self.assertEqual(len(hostages), 1)
        self.assertEqual(
            assetc.HOSTAGE.unpack(hostage_data), (10, 20, 30)
        )
        rescue_data, rescues = assetc.compile_zones(
            entities, models,
            ("info_hostage_rescue",), "rescue", 128,
        )
        self.assertEqual(len(rescues), 1)
        self.assertEqual(
            assetc.ZONE.unpack(rescue_data),
            (-28, 72, 172, 228, 328, 428, 0, 0),
        )
        buy_data, buys = assetc.compile_zones(
            entities, models, ("func_buyzone",),
            "buy", 160, True,
        )
        self.assertEqual(len(buys), 1)
        self.assertEqual(
            assetc.ZONE.unpack(buy_data),
            (-16, -32, -8, 16, 32, 72, 2, 0),
        )
        dynamic_data, dynamic = assetc.compile_dynamic_entities(
            entities, models
        )
        self.assertEqual(len(dynamic), 2)
        self.assertEqual(
            len(dynamic_data), 2 * assetc.DYNAMIC_ENTITY.size
        )
        door = assetc.DYNAMIC_ENTITY.unpack_from(dynamic_data)
        self.assertEqual(door[0], 1)
        self.assertEqual(door[2], 1)
        self.assertEqual(door[-1], assetc.fnv1a("front_door"))


if __name__ == "__main__":
    unittest.main()
