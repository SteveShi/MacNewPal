#!/usr/bin/env python3
"""Read-only parser and JSON exporter for PalGame (新仙劍奇俠傳 / SWD2k) .dat files.

Parses:
  1. 0x7F-tagged entity/script databases (ghero, gmon, glevel, gitem, gmagic, gmap, GFight1..4).
  2. namex.dat CP950 string table (7,100 entries).
  3. talk1..8.dat Talk.cpp VM bytecode scripts (all 1..0x402 opcodes, dialogues, choices, Bink videos).
  4. maps.dat (map title table + slot inventory) & path1..10.dat (waypoint/path archive inventory).

Strictly read-only against --game-dir; writes UTF-8 JSON outputs only inside --out-dir.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import struct
from typing import Any

MZ_HEADER_SIZE = 0x200

# Verified Talk.cpp VM fixed-length instruction byte sizes (from 0x401eb5.c).
# Keyed by (raw_word & 0x3FFF).
TALK_FIXED_BYTES: dict[int, int] = {
    0x07: 2, 0x08: 4, 0x09: 2, 0x0A: 6, 0x0B: 6, 0x0C: 8, 0x0D: 4, 0x0E: 4,
    0x0F: 6, 0x10: 8, 0x11: 8, 0x12: 4, 0x13: 2,
    0x15: 8, 0x16: 8, 0x19: 4, 0x1A: 4, 0x1B: 14, 0x1C: 6,
    0x1D: 6, 0x1E: 6, 0x1F: 6, 0x20: 10, 0x21: 10,
    0x22: 4, 0x23: 8, 0x24: 8, 0x25: 2, 0x26: 4, 0x27: 4, 0x28: 8,
    0x2A: 2, 0x2B: 2, 0x2C: 6, 0x2D: 6,
    0x2E: 4, 0x2F: 4, 0x30: 4, 0x31: 4,
    0x32: 10, 0x33: 2, 0x34: 16, 0x35: 2, 0x36: 6, 0x37: 4, 0x38: 4, 0x39: 4, 0x3A: 10, 0x3B: 4,
    0x3C: 2, 0x3D: 2, 0x3E: 18, 0x40: 2, 0x41: 4, 0x42: 16, 0x43: 4, 0x44: 4, 0x45: 4,
    0x46: 10, 0x47: 6, 0x48: 4, 0x49: 8, 0x4A: 2, 0x4B: 4, 0x4C: 6,
    0x4D: 6, 0x4E: 4, 0x4F: 16, 0x50: 2, 0x51: 10, 0x52: 6, 0x53: 16, 0x54: 6, 0x55: 10, 0x56: 10,
    0x58: 4, 0x5B: 4, 0x5C: 4, 0x5D: 4, 0x5E: 2, 0x5F: 2,
    0x61: 2, 0x62: 4, 0x63: 4, 0x64: 6, 0x65: 4,
    0x66: 6, 0x67: 6, 0x68: 6, 0x69: 2, 0x6A: 4, 0x6B: 6, 0x6C: 6,
    0x6E: 6, 0x6F: 6, 0x70: 2, 0x71: 6, 0x72: 8, 0x73: 4, 0x75: 6,
    0x76: 4, 0x77: 4, 0x78: 10, 0x79: 2, 0x7A: 2, 0x7B: 10,
    0x7C: 2, 0x7E: 10, 0x7F: 10, 0x80: 6,
    0x81: 8, 0x82: 8, 0x83: 6, 0x84: 8, 0x86: 10, 0x87: 2,
    0x88: 6, 0x89: 2, 0x8A: 14, 0x8B: 4, 0x8C: 6,
    0x8D: 6, 0x8E: 8, 0x8F: 2, 0x90: 4, 0x91: 6, 0x92: 6,
    0x93: 2, 0x94: 2, 0x95: 2, 0x96: 6, 0x97: 10, 0x98: 2,
    0x99: 10, 0x9A: 4, 0x9B: 2, 0x9C: 2, 0x9D: 8, 0x9E: 2, 0x9F: 2, 0xA0: 2,
    0xA1: 4, 0xA2: 4, 0xA3: 8, 0xA4: 8, 0xA5: 10, 0xA6: 10, 0xA7: 8, 0xA8: 8,
    0xAE: 6, 0xAF: 2, 0xB0: 2, 0xB1: 2, 0xB2: 4, 0xB3: 10, 0xB4: 2,
    0xB5: 8, 0xB6: 8, 0xB7: 8, 0xB8: 12, 0xB9: 12,
    0xBF: 4, 0xC0: 2, 0xC1: 12, 0xC3: 8, 0xC4: 2, 0xC5: 8,
    0xC6: 8, 0xC7: 8, 0xC8: 12, 0xC9: 12, 0xCA: 12, 0xCC: 12, 0xCD: 4, 0xCE: 4, 0xCF: 6,
    0xD0: 6, 0xD1: 4, 0xD2: 2, 0xD3: 2, 0xD4: 2,
    0xD5: 14, 0xD6: 2, 0xD7: 4, 0xD8: 10, 0xD9: 6, 0xDA: 10, 0xDC: 10,
    0xDD: 16, 0xDE: 16, 0xDF: 8, 0xE0: 8, 0xE1: 6,
    0xE2: 16, 0xE3: 8, 0xE4: 6, 0xE5: 6, 0xE6: 16, 0xE7: 16, 0xE8: 10,
    0xE9: 2,
    0x309: 4, 0x30A: 4, 0x30B: 6, 0x30C: 12, 0x30D: 4,
    0x30E: 8, 0x30F: 8, 0x310: 6, 0x311: 4, 0x312: 2,
    0x400: 2, 0x401: 2, 0x402: 2,
}

DIALOGUE_HEADER_BYTES: dict[int, int] = {
    1: 6, 2: 6,
    3: 14, 4: 14, 5: 14, 6: 14,
    0x59: 10, 0x5A: 10,
}

# Confirmed property IDs on 0x7f201001 (ghero/gmon/glevel) from decompiled engine functions.
CONFIRMED_CHAR_PROPS: dict[int, str] = {
    4: "max_lv",
    5: "lv",
    9: "prev_exp",
    10: "next_exp",
    11: "cur_exp",
    13: "lv_up_hp_delta",
    16: "max_hp",
    17: "cur_hp",
    19: "lv_up_mp_delta",
    22: "max_mp",
    23: "cur_mp",
    25: "lv_up_atk_delta",
    26: "temp_atk",
    29: "atk",
    31: "lv_up_mag_delta",
    32: "temp_mag",
    35: "mag",
    37: "lv_up_def_delta",
    38: "temp_def",
    41: "def",
    43: "lv_up_spd_delta",
    44: "temp_spd",
    47: "spd",
    49: "lv_up_luck_delta",
    50: "temp_luck",
    53: "luck",
    56: "temp_poison_res",
    59: "poison_res",
    62: "temp_wind_res",
    65: "wind_res",
    68: "temp_lightning_res",
    71: "lightning_res",
    74: "temp_water_res",
    77: "water_res",
    80: "temp_fire_res",
    83: "fire_res",
    86: "temp_earth_res",
    89: "earth_res",
    92: "temp_monster_mag",
    95: "monster_mag",
    101: "flee_hp_pct",
    107: "rage_chance_pct",
    125: "monster_category",
    180: "ai_action_id",
    181: "ai_target_override",
    182: "sprite_or_type",
    187: "ai_phase1_script",
    188: "ai_phase2_script",
    189: "ai_phase3_script",
    190: "reward_exp",
    203: "steal_gold",
    204: "steal1_chance",
    205: "steal1_qty",
    206: "steal1_item_id",
    207: "drop1_or_steal2_chance",
    208: "drop1_or_steal2_qty",
    209: "drop1_or_steal2_item_id",
    210: "drop2_chance",
    211: "drop2_qty",
    212: "drop2_item_id",
    219: "gourd_soul_value",
    220: "initial_magic_id",
    221: "initial_weapon_id",
}

CONFIRMED_ITEM_PROPS: dict[int, str] = {
    0: "target_scope",
    1: "field_talk_script",
    2: "battle_fight_script",
    3: "equip_bonus_script",
    4: "effect_or_anim_id",
    6: "reusable_flag",
    7: "use_count_temp",
    8: "equip_slot_mask",
    9: "price",
}

CONFIRMED_MAGIC_PROPS: dict[int, str] = {
    0: "target_scope",
    1: "field_talk_script",
    2: "battle_fight_script",
    5: "mp_cost",
    6: "max_uses",
    8: "user_category_mask",
}

# All 88 GFight VM opcodes dispatched by 0x4bdce0, keyed by (tag & 0xFF00FFFF).
COMBAT_OPCODES: dict[int, str] = {
    0x7F00FFFF: "battle_end_return",
    0x7F00F002: "dialogue_or_banner_str",
    0x7F002001: "wait_ticks",
    0x7F002002: "clear_action_queue",
    0x7F002003: "set_actor_pos_type",
    0x7F002004: "spawn_effect_or_motion",
    0x7F002005: "summon_or_transform_actor",
    0x7F002006: "jump_or_select_target",
    0x7F002007: "resolve_hit_guard_and_damage",
    0x7F002008: "check_battle_flag",
    0x7F002009: "check_end_turn",
    0x7F00200A: "move_actor_step",
    0x7F00200B: "modify_actor_prop",
    0x7F00200C: "camera_or_ui_mode_on",
    0x7F00200D: "camera_or_ui_mode_off",
    0x7F00200E: "battle_win",
    0x7F00200F: "battle_lose",
    0x7F002010: "set_global_battle_var",
    0x7F002011: "apply_status_or_element",
    0x7F002012: "nop_advance",
    0x7F002013: "play_effect_sequence",
    0x7F002014: "set_script_temp_reg",
    0x7F002015: "set_atk_mode_and_power",
    0x7F002016: "enqueue_delayed_action",
    0x7F002017: "cancel_delayed_action",
    0x7F002018: "update_effect_param",
    0x7F002019: "animate_sprite_sequence",
    0x7F00201A: "try_capture_or_steal_item",
    0x7F00201B: "shake_or_flash_screen",
    0x7F00201C: "check_story_flag_branch",
    0x7F00201D: "push_status_flag",
    0x7F00201E: "modify_status_turns",
    0x7F00201F: "apply_status_with_chance",
    0x7F002020: "set_status_flag_if",
    0x7F002021: "set_screen_tint_pair",
    0x7F002022: "check_actor_undead_branch",
    0x7F002023: "set_overlay_param",
    0x7F002024: "set_group_status_flag",
    0x7F002025: "spawn_projectile_or_trail",
    0x7F002026: "check_group_status_branch",
    0x7F00F027: "play_sound_id_str",
    0x7F002028: "gfx_cmd_0",
    0x7F002029: "gfx_cmd_1arg",
    0x7F00202A: "gfx_cmd_a",
    0x7F00202B: "gfx_cmd_b",
    0x7F00202C: "gfx_cmd_c",
    0x7F00202D: "gfx_cmd_d",
    0x7F00202E: "gfx_cmd_e",
    0x7F00202F: "gfx_draw_7args",
    0x7F002030: "gfx_palette_2args",
    0x7F002031: "gfx_check_ready",
    0x7F002032: "gfx_load_res",
    0x7F002033: "gfx_blit_rect",
    0x7F002034: "gfx_scroll_offset",
    0x7F002035: "gfx_free_res",
    0x7F00F036: "show_floating_text_str",
    0x7F002037: "clear_floating_text",
    0x7F002038: "audio_set_param",
    0x7F002039: "audio_check_done",
    0x7F00203A: "add_inventory_or_buff",
    0x7F00203B: "remove_buff",
    0x7F00203C: "check_actor_alive_branch",
    0x7F00203D: "kill_or_remove_actor",
    0x7F00203E: "sync_actor_frame_step",
    0x7F00203F: "reset_actor_facing",
    0x7F002040: "advance_script_step",
    0x7F002041: "consume_or_grant_item",
    0x7F002042: "learn_magic_or_skill",
    0x7F002043: "set_actor_layer",
    0x7F002044: "replace_equipped_weapon",
    0x7F002045: "steal_or_drop_trigger",
    0x7F002046: "play_sfx_by_id",
    0x7F002047: "battle_escape_win",
    0x7F002048: "spawn_extra_actor_4args",
    0x7F002049: "spawn_combo_effect_7args",
    0x7F00204A: "check_queue_empty_end",
    0x7F00204B: "apply_equip_stat_mod",
    0x7F00204C: "play_bgm_by_id",
    0x7F00204D: "register_counter_trigger",
    0x7F00204E: "register_reflect_trigger",
    0x7F00204F: "register_death_trigger",
    0x7F002050: "set_actor_hp_limit",
    0x7F002051: "set_or_tick_loop_counter",
    0x7F002052: "check_equip_item_branch",
    0x7F002053: "enqueue_post_action",
    0x7F002054: "clear_temp_props",
    0x7F002055: "check_equip_slot_branch",
    0x7F002056: "check_item_exists_branch",
}

TAG7F_DB_FILES = (
    "ghero.dat",
    "gmon.dat",
    "glevel.dat",
    "gitem.dat",
    "gmagic.dat",
    "gmap.dat",
    "GFight1.dat",
    "GFight2.dat",
    "GFight3.dat",
    "GFight4.dat",
)

TEXT_CTRL_RE = re.compile(r"%(?:C\d+|S\d+|G\d+|K)")


def _decode_cp950(raw: bytes) -> str:
    return raw.decode("cp950", errors="replace")


def _clean_dialogue_text(raw_text: str) -> str:
    """Convert %N newlines to \\n and strip Talk UI formatting tags (%C, %S, %G, %K)."""
    text = raw_text.replace("%N", "\n")
    return TEXT_CTRL_RE.sub("", text)


def _format_scalar(u: int) -> int | dict[str, str]:
    """Decode a 32-bit word inside a 0x7F data stream: 0x7Exxxxxx -> var_ref, >=0x80000000 -> int32."""
    if 0x7E000000 <= u < 0x7F000000:
        return {"var_ref": f"0x{u:08x}"}
    if u >= 0x80000000:
        return u - 0x100000000
    return u


def _write_json(path: pathlib.Path, obj: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _read_mz_payload(path: pathlib.Path) -> bytes:
    data = path.read_bytes()
    if len(data) < MZ_HEADER_SIZE or data[:2] != b"MZ":
        raise ValueError(f"{path} is missing expected 0x200 MZ header")
    return data[MZ_HEADER_SIZE:]


class Tag7fDatabase:
    """Parser for 0x7F-tagged binary databases (ghero/gmon/glevel/gitem/gmagic/gmap/GFight*)."""

    @staticmethod
    def parse_file(path: pathlib.Path) -> tuple[list[list[dict[str, Any]]], int]:
        payload = _read_mz_payload(path)
        pos = 0
        records: list[list[dict[str, Any]]] = []
        cur: list[dict[str, Any]] | None = None

        while pos + 4 <= len(payload):
            tag = struct.unpack_from("<I", payload, pos)[0]
            pos += 4
            if tag == 0x7F00FFFF:
                if cur is not None:
                    records.append(cur)
                    cur = None
                continue
            if (tag >> 24) != 0x7F:
                raise ValueError(f"{path.name}: unexpected non-0x7F tag 0x{tag:08x} at payload+0x{pos-4:x}")

            is_str = ((tag & 0xFFFF) >= 0xF000) and (((tag & 0xF0000) >> 16) != 0)
            if is_str:
                end = payload.find(b"%%", pos)
                if end == -1:
                    raise ValueError(f"{path.name}: unterminated %% string for tag 0x{tag:08x} at 0x{pos:x}")
                val: Any = _decode_cp950(payload[pos:end])
                pos = end + 2
            else:
                vals: list[int | dict[str, str]] = []
                while pos + 4 <= len(payload):
                    u = struct.unpack_from("<I", payload, pos)[0]
                    if (u >> 24) == 0x7F:
                        break
                    vals.append(_format_scalar(u))
                    pos += 4
                val = vals

            if cur is None:
                cur = []
            cur.append({"tag": f"0x{tag:08x}", "value": val})

        if cur is not None or pos != len(payload):
            raise ValueError(
                f"{path.name}: unparsed trailing data (pos={pos}, payload_len={len(payload)}, open_record={cur is not None})"
            )
        return records, len(payload)


def _normalize_7f_records(filename: str, records: list[list[dict[str, Any]]]) -> dict[str, Any]:
    """Produce normalized views alongside raw TLV fields for each 0x7F database."""
    stem = pathlib.Path(filename).stem
    base: dict[str, Any] = {"file": filename, "record_count": len(records)}

    if filename in ("ghero.dat", "gmon.dat"):
        entities = []
        for rec in records:
            ent: dict[str, Any] = {"id": None, "name": None, "confirmed_stats": {}, "raw_props": {}}
            extra_fields = []
            for field in rec:
                tag, val = field["tag"], field["value"]
                if tag == "0x7f101000" and isinstance(val, list) and len(val) == 1:
                    ent["id"] = val[0]
                elif tag == "0x7ff2f000" and isinstance(val, str):
                    ent["name"] = val
                elif tag == "0x7f201001" and isinstance(val, list) and len(val) == 2 and isinstance(val[0], int):
                    pid, pval = val
                    ent["raw_props"][str(pid)] = pval
                    if pid in CONFIRMED_CHAR_PROPS:
                        ent["confirmed_stats"][CONFIRMED_CHAR_PROPS[pid]] = pval
                else:
                    extra_fields.append(field)
            if extra_fields:
                ent["extra_fields"] = extra_fields
            entities.append(ent)
        key = "heroes" if filename == "ghero.dat" else "monsters"
        base[key] = entities
        return base

    if filename in ("gitem.dat", "gmagic.dat", "glevel.dat"):
        entities = []
        scripts = []
        for rec in records:
            tags = {f["tag"]: f["value"] for f in rec}
            if "0x7f100000" in tags or "0x7f101000" in tags:
                id_tag = "0x7f100000" if "0x7f100000" in tags else "0x7f101000"
                ent_id = tags[id_tag][0] if isinstance(tags[id_tag], list) and tags[id_tag] else None
                props: dict[str, Any] = {}
                other: list[dict[str, Any]] = []
                for f in rec:
                    if f["tag"] in ("0x7f200001", "0x7f201001") and isinstance(f["value"], list) and len(f["value"]) == 2:
                        props[str(f["value"][0])] = f["value"][1]
                    elif f["tag"] not in (id_tag, "0x7ff2f000"):
                        other.append(f)
                item_obj: dict[str, Any] = {"id": ent_id}
                if "0x7ff2f000" in tags:
                    item_obj["name"] = tags["0x7ff2f000"]
                item_obj["raw_props"] = props
                if other:
                    item_obj["extra_fields"] = other
                entities.append(item_obj)
            else:
                sid = tags.get("0x7f102000", [None])[0] if isinstance(tags.get("0x7f102000"), list) else None
                scripts.append({"script_id": sid, "fields": rec})
        key = {"gitem": "items", "gmagic": "magics", "glevel": "templates"}[stem]
        base[key] = entities
        base["scripts"] = scripts
        return base

    if filename == "gmap.dat":
        zones = []
        for rec in records:
            zid = None
            entries = []
            extra = []
            for f in rec:
                if f["tag"] == "0x7f103000" and isinstance(f["value"], list) and f["value"]:
                    zid = f["value"][0]
                elif f["tag"] == "0x7ff03002":
                    entries.append(f["value"])
                else:
                    extra.append(f)
            zone: dict[str, Any] = {"zone_id": zid, "entries": entries}
            if extra:
                zone["extra_fields"] = extra
            zones.append(zone)
        base["encounter_zones"] = zones
        return base

    # GFight1..4.dat
    fight_scripts = []
    for rec in records:
        sid = None
        for f in rec:
            if f["tag"] == "0x7f102000" and isinstance(f["value"], list) and f["value"]:
                sid = f["value"][0]
                break
        fight_scripts.append({"script_id": sid, "fields": rec})
    base["scripts"] = fight_scripts
    return base


class NamexArchive:
    """Parser for namex.dat (CP950 name table indexed 0..7099)."""

    SLOT_COUNT = 7100

    @classmethod
    def parse_file(cls, path: pathlib.Path) -> dict[str, Any]:
        payload = _read_mz_payload(path)
        offs = struct.unpack_from(f"<{cls.SLOT_COUNT}I", payload, 0)
        names: dict[str, str] = {}
        for idx, off in enumerate(offs):
            if off >= len(payload):
                continue
            end = payload.find(b"%Q", off)
            if end == -1:
                continue
            val = _decode_cp950(payload[off:end])
            if val:
                names[str(idx)] = val
        return {
            "file": path.name,
            "slot_count": cls.SLOT_COUNT,
            "non_empty_count": len(names),
            "names": names,
        }


class TalkDisassembler:
    """Exact byte-level disassembler for Talk.cpp VM scripts in talk1..8.dat."""

    @staticmethod
    def _table_slots(talk_index: int) -> int:
        # talk1.dat & talk4.dat store 701 slots (table bytes 0xAF4); others store 1000 slots (0xFA0).
        return 701 if talk_index in (1, 4) else 1000

    @classmethod
    def disassemble_chunk(cls, chunk: bytes) -> tuple[list[dict[str, Any]], bool]:
        """Disassemble a script chunk across all conditional branches until EOF."""
        insns: list[dict[str, Any]] = []
        pos = 0
        n = len(chunk)

        while pos + 2 <= n:
            start = pos
            raw = struct.unpack_from("<H", chunk, pos)[0]
            if raw == 0xFFFF:
                insns.append({"offset": start, "op": "0xffff", "kind": "end_branch"})
                pos += 2
                continue

            op = raw & 0x3FFF
            if op in DIALOGUE_HEADER_BYTES:
                hdr_bytes = DIALOGUE_HEADER_BYTES[op]
                if pos + hdr_bytes > n:
                    return insns, False
                speaker_id = struct.unpack_from("<H", chunk, pos + 2)[0]
                header_words = list(struct.unpack_from(f"<{(hdr_bytes - 4) // 2}H", chunk, pos + 4))
                end = chunk.find(b"%Q", pos + hdr_bytes)
                if end == -1:
                    return insns, False
                raw_text = _decode_cp950(chunk[pos + hdr_bytes : end])
                insns.append({
                    "offset": start,
                    "op": f"0x{op:02x}",
                    "raw_word": f"0x{raw:04x}",
                    "kind": "dialogue",
                    "speaker_id": speaker_id,
                    "box_header_u16": header_words,
                    "raw_text": raw_text,
                    "text": _clean_dialogue_text(raw_text),
                })
                pos = end + 2

            elif op == 0x7D:
                end = chunk.find(b"%Q", pos + 2)
                if end == -1:
                    return insns, False
                raw_text = _decode_cp950(chunk[pos + 2 : end])
                insns.append({
                    "offset": start,
                    "op": "0x7d",
                    "raw_word": f"0x{raw:04x}",
                    "kind": "choice_text",
                    "raw_text": raw_text,
                    "text": _clean_dialogue_text(raw_text),
                })
                pos = end + 2

            elif op in (0xAA, 0xAB, 0xAC, 0xAD):
                if raw & 1:
                    end = chunk.find(b"%Q", pos + 2)
                    if end == -1:
                        return insns, False
                    raw_text = _decode_cp950(chunk[pos + 2 : end])
                    insns.append({
                        "offset": start,
                        "op": f"0x{op:02x}",
                        "raw_word": f"0x{raw:04x}",
                        "kind": "banner_text",
                        "raw_text": raw_text,
                        "text": _clean_dialogue_text(raw_text),
                    })
                    pos = end + 2
                else:
                    insns.append({"offset": start, "op": f"0x{op:02x}", "raw_word": f"0x{raw:04x}", "kind": "banner_clear"})
                    pos += 2

            elif op in (0x60, 0xC2):
                p = pos + 2
                flags = ""
                if op == 0x60:
                    if p < n and chunk[p] == 0x25:
                        flags += "%"
                        p += 1
                    if p < n and chunk[p] == 0x2A:
                        flags += "*"
                        p += 1
                end = chunk.find(b"%Q", p)
                if end == -1:
                    return insns, False
                filename = _decode_cp950(chunk[p:end])
                insns.append({
                    "offset": start,
                    "op": f"0x{op:02x}",
                    "raw_word": f"0x{raw:04x}",
                    "kind": "video",
                    "flags": flags,
                    "filename": filename,
                })
                pos = end + 2

            elif op in (0x14, 0xA9):
                if pos + 4 > n:
                    return insns, False
                hdr = struct.unpack_from("<H", chunk, pos + 2)[0]
                cnt = hdr & 0x3FFF
                step = 6 if op == 0x14 else 12
                end_pos = pos + 4 + cnt * step
                if end_pos > n:
                    return insns, False
                args = list(struct.unpack_from(f"<{1 + cnt * (step // 2)}H", chunk, pos + 2))
                insns.append({"offset": start, "op": f"0x{op:02x}", "raw_word": f"0x{raw:04x}", "args_u16": args})
                pos = end_pos

            elif op in (0x17, 0x18):
                p = pos + 2
                flags_list = []
                while p + 2 <= n:
                    w = struct.unpack_from("<H", chunk, p)[0]
                    p += 2
                    if w == 0xFF00:
                        break
                    flags_list.append(w)
                if p + 4 > n:
                    return insns, False
                jump_off = struct.unpack_from("<I", chunk, p)[0]
                insns.append({
                    "offset": start,
                    "op": f"0x{op:02x}",
                    "raw_word": f"0x{raw:04x}",
                    "kind": "flag_branch",
                    "flags": flags_list,
                    "jump_target": f"0x{jump_off:x}",
                })
                pos = p + 4

            elif op in (0x29, 0x57):
                p = pos + 2
                targets = []
                while p + 4 <= n:
                    t = struct.unpack_from("<I", chunk, p)[0]
                    p += 4
                    if t == 0xFF00FF00:
                        break
                    targets.append(f"0x{t:x}")
                insns.append({
                    "offset": start,
                    "op": f"0x{op:02x}",
                    "raw_word": f"0x{raw:04x}",
                    "kind": "jump_table",
                    "targets": targets,
                })
                pos = p

            elif op == 0x3F:
                if pos + 4 > n:
                    return insns, False
                p = pos + 4
                path_words = []
                while p + 2 <= n:
                    w = struct.unpack_from("<H", chunk, p)[0]
                    p += 2
                    if w == 0xFF00:
                        break
                    path_words.append(w)
                insns.append({
                    "offset": start,
                    "op": "0x3f",
                    "raw_word": f"0x{raw:04x}",
                    "arg0": struct.unpack_from("<H", chunk, pos + 2)[0],
                    "words": path_words,
                })
                pos = p

            elif op in (0x6D, 0x74):
                if pos + 4 > n:
                    return insns, False
                cnt = struct.unpack_from("<H", chunk, pos + 2)[0]
                stride = 2 if op == 0x6D else 6
                end_pos = pos + 4 + cnt * stride
                if end_pos > n:
                    return insns, False
                args = list(struct.unpack_from(f"<{1 + cnt * (stride // 2)}H", chunk, pos + 2))
                insns.append({"offset": start, "op": f"0x{op:02x}", "raw_word": f"0x{raw:04x}", "args_u16": args})
                pos = end_pos

            elif op == 0x85:
                p = pos + 2
                words = []
                while p + 2 <= n:
                    w = struct.unpack_from("<H", chunk, p)[0]
                    p += 2
                    if w == 0:
                        break
                    words.append(w)
                insns.append({"offset": start, "op": "0x85", "raw_word": f"0x{raw:04x}", "words": words})
                pos = p

            elif op == 0xCB:
                if pos + 6 > n:
                    return insns, False
                hdr = list(struct.unpack_from("<2H", chunk, pos + 2))
                p = pos + 6
                targets = []
                while p + 4 <= n:
                    t = struct.unpack_from("<I", chunk, p)[0]
                    p += 4
                    if t == 0xFF00FF00:
                        break
                    targets.append(f"0x{t:x}")
                insns.append({
                    "offset": start,
                    "op": "0xcb",
                    "raw_word": f"0x{raw:04x}",
                    "kind": "switch_var",
                    "var": hdr,
                    "targets": targets,
                })
                pos = p

            elif op in TALK_FIXED_BYTES:
                size = TALK_FIXED_BYTES[op]
                if pos + size > n:
                    return insns, False
                n_words = (size - 2) // 2
                args = list(struct.unpack_from(f"<{n_words}H", chunk, pos + 2)) if n_words > 0 else []
                insn: dict[str, Any] = {"offset": start, "op": f"0x{op:02x}", "raw_word": f"0x{raw:04x}"}
                if args:
                    insn["args_u16"] = args
                insns.append(insn)
                pos += size

            else:
                return insns, False

        return insns, (pos == n)

    @classmethod
    def parse_file(cls, path: pathlib.Path, talk_index: int) -> dict[str, Any]:
        payload = _read_mz_payload(path)
        n_slots = cls._table_slots(talk_index)
        data_start = n_slots * 4
        offs = struct.unpack_from(f"<{n_slots}I", payload, 0)
        valid_offs = sorted({v for v in offs if v != 0xFFFFFFFF and data_start <= v < len(payload)} | {len(payload)})
        next_map = {valid_offs[i]: valid_offs[i + 1] for i in range(len(valid_offs) - 1)}

        scripts: list[dict[str, Any]] = []
        clean_count = 0
        dialogue_count = 0

        for slot_id, off in enumerate(offs):
            if off == 0xFFFFFFFF or off < data_start or off >= len(payload):
                continue
            end = next_map[off]
            chunk = payload[off:end]
            if chunk == b"\x02\x04\xff\xff":
                continue

            insns, clean = cls.disassemble_chunk(chunk)
            if clean:
                clean_count += 1

            dialogues = []
            choices = []
            videos = []
            banners = []
            for ins in insns:
                kind = ins.get("kind")
                if kind == "dialogue":
                    dialogues.append({
                        "offset": ins["offset"],
                        "speaker_id": ins["speaker_id"],
                        "raw_text": ins["raw_text"],
                        "text": ins["text"],
                    })
                elif kind == "choice_text":
                    choices.append({"offset": ins["offset"], "raw_text": ins["raw_text"], "text": ins["text"]})
                elif kind == "video":
                    videos.append({"offset": ins["offset"], "flags": ins["flags"], "filename": ins["filename"]})
                elif kind == "banner_text":
                    banners.append({"offset": ins["offset"], "raw_text": ins["raw_text"], "text": ins["text"]})

            dialogue_count += len(dialogues)
            talk_id = (talk_index - 1) * 1000 + slot_id
            entry: dict[str, Any] = {
                "slot_id": slot_id,
                "talk_id": talk_id,
                "offset": f"0x{off:x}",
                "byte_length": len(chunk),
                "clean_disassembly": clean,
                "dialogues": dialogues,
                "instructions": insns,
            }
            if choices:
                entry["choices"] = choices
            if videos:
                entry["videos"] = videos
            if banners:
                entry["banners"] = banners
            scripts.append(entry)

        return {
            "file": path.name,
            "talk_bank": talk_index,
            "slot_count": n_slots,
            "non_stub_scripts": len(scripts),
            "clean_disassembly_count": clean_count,
            "dialogue_count": dialogue_count,
            "scripts": scripts,
        }


def parse_maps_dat(path: pathlib.Path) -> dict[str, Any]:
    """Parse maps.dat (u32 magic=100 + 25x u32 offsets, slot 24 sentinel 0xFFFFFFFF).

    Verified slots from disassembly (0x41d335 / binary inspection):
      - slot 1: map_id -> Music/<filename> string table
      - slot 16: quest/objective CP950 descriptions
      - slot 19: u16 map_id + CP950 map title table terminated by %Q
    """
    payload = _read_mz_payload(path)
    magic = struct.unpack_from("<I", payload, 0)[0]
    offs = struct.unpack_from("<25I", payload, 4)
    sorted_offs = sorted({v for v in offs if v < len(payload)} | {len(payload)})
    next_map = {sorted_offs[i]: sorted_offs[i + 1] for i in range(len(sorted_offs) - 1)}

    slots = []
    for idx, off in enumerate(offs):
        seg = payload[off : next_map[off]] if off < len(payload) else b""
        slots.append({"slot": idx, "offset": f"0x{off:x}", "byte_length": len(seg)})

    def _read_u32_ptr_strings(slot_idx: int) -> list[dict[str, Any]]:
        s_off = offs[slot_idx]
        if s_off >= len(payload):
            return []
        first_ptr = struct.unpack_from("<I", payload, s_off)[0]
        if first_ptr == 0:
            # Find first non-zero pointer in header table to bound pointer array size
            ptrs = []
            pos = s_off
            while pos + 4 <= len(payload):
                p = struct.unpack_from("<I", payload, pos)[0]
                if p != 0 and (p < s_off or p >= len(payload)):
                    break
                ptrs.append(p)
                pos += 4
                if p != 0 and pos >= p:
                    break
            first_nonzero = next((p for p in ptrs if p > 0), len(payload))
            cnt = max(0, (first_nonzero - s_off) // 4)
        else:
            cnt = max(0, (first_ptr - s_off) // 4)
        arr = struct.unpack_from(f"<{min(cnt, 2000)}I", payload, s_off) if cnt else ()
        out = []
        for idx, p in enumerate(arr):
            if p == 0 or p >= len(payload):
                continue
            end = payload.find(b"%Q", p)
            if end == -1:
                z = payload.find(b"\x00", p)
                end = z if z != -1 else len(payload)
            val = _decode_cp950(payload[p:end])
            if val:
                out.append({"index": idx, "text": val})
        return out

    # Slot 19: u16 map_id + CP950 title terminated by %Q
    s19_off = offs[19]
    s19 = payload[s19_off : next_map[s19_off]] if s19_off < len(payload) else b""
    p = 0
    map_titles = []
    while p + 2 <= len(s19):
        mid = struct.unpack_from("<H", s19, p)[0]
        if mid == 0xFFFF:
            break
        end = s19.find(b"%Q", p + 2)
        if end == -1:
            break
        map_titles.append({"map_id": mid, "name": _decode_cp950(s19[p + 2 : end])})
        p = end + 2

    return {
        "file": path.name,
        "magic": magic,
        "payload_bytes": len(payload),
        "slots": slots,
        "map_title_count": len(map_titles),
        "map_titles": map_titles,
        "quest_entries": _read_u32_ptr_strings(16),
    }


def parse_path_dat(path: pathlib.Path) -> dict[str, Any]:
    """Parse path1..10.dat waypoint/route archives (magic=100 + variable slot offset table)."""
    payload = _read_mz_payload(path)
    info: dict[str, Any] = {"file": path.name, "payload_bytes": len(payload)}
    if len(payload) == 4 and struct.unpack_from("<I", payload, 0)[0] == 0x64:
        info["format"] = "empty_magic_header"
        info["slot_count"] = 0
        return info

    if len(payload) >= 8 and struct.unpack_from("<I", payload, 0)[0] == 0x64:
        o0 = struct.unpack_from("<I", payload, 4)[0]
        if o0 >= 8 and (o0 - 4) % 4 == 0 and o0 <= len(payload):
            n_slots = (o0 - 4) // 4
            offs = struct.unpack_from(f"<{n_slots}I", payload, 4)
            valid_offs = sorted({v for v in offs if o0 <= v < len(payload)} | {len(payload)})
            next_map = {valid_offs[i]: valid_offs[i + 1] for i in range(len(valid_offs) - 1)}
            non_empty = []
            for idx, off in enumerate(offs):
                if o0 <= off < len(payload):
                    seg = payload[off : next_map[off]]
                    non_empty.append((idx, off, len(seg)))
            info["format"] = f"u32_table_{n_slots}_slots"
            info["slot_count"] = n_slots
            info["non_empty_count"] = len(non_empty)
            info["sample_slots"] = [
                {
                    "slot": idx,
                    "offset": f"0x{off:x}",
                    "byte_length": ln,
                    "head_hex": payload[off : off + min(16, ln)].hex(),
                }
                for idx, off, ln in non_empty[:8]
            ]
            return info

    info["format"] = "raw_path_stream"
    info["raw_hex"] = payload.hex(" ")
    return info


def _annotate_db_doc(fname: str, doc: dict[str, Any]) -> None:
    """Attach confirmed item/magic property names and GFight opcode names in-place."""
    if fname == "gitem.json":
        for rec in doc.get("items", []):
            raw = rec.get("raw_props", {})
            rec["confirmed_props"] = {
                name: raw[str(pid)]
                for pid, name in CONFIRMED_ITEM_PROPS.items()
                if str(pid) in raw
            }
    elif fname == "gmagic.json":
        for rec in doc.get("magics", []):
            raw = rec.get("raw_props", {})
            rec["confirmed_props"] = {
                name: raw[str(pid)]
                for pid, name in CONFIRMED_MAGIC_PROPS.items()
                if str(pid) in raw
            }

    for sc in doc.get("scripts", []):
        for fld in sc.get("fields", []):
            tag_str = fld.get("tag")
            if isinstance(tag_str, str) and tag_str.startswith("0x"):
                masked = int(tag_str, 16) & 0xFF00FFFF
                if masked in COMBAT_OPCODES:
                    fld["opcode_name"] = COMBAT_OPCODES[masked]


def cmd_export_db(game_dir: pathlib.Path, out_dir: pathlib.Path) -> dict[str, Any]:
    db_out = out_dir / "db"
    summary: dict[str, Any] = {}
    for fname in TAG7F_DB_FILES:
        src = game_dir / fname
        records, payload_len = Tag7fDatabase.parse_file(src)
        doc = _normalize_7f_records(fname, records)
        doc["payload_bytes"] = payload_len
        out_name = f"{pathlib.Path(fname).stem}.json"
        _annotate_db_doc(out_name, doc)
        out_path = db_out / out_name
        _write_json(out_path, doc)
        summary[fname] = {
            "record_count": len(records),
            "payload_bytes": payload_len,
            "output": str(out_path),
        }
    return summary


def cmd_export_scripts(game_dir: pathlib.Path, out_dir: pathlib.Path) -> dict[str, Any]:
    scripts_out = out_dir / "scripts"
    summary: dict[str, Any] = {}

    namex_doc = NamexArchive.parse_file(game_dir / "namex.dat")
    namex_path = scripts_out / "namex.json"
    _write_json(namex_path, namex_doc)
    summary["namex.dat"] = {
        "slot_count": namex_doc["slot_count"],
        "non_empty_count": namex_doc["non_empty_count"],
        "output": str(namex_path),
    }

    all_dialogues: list[dict[str, Any]] = []
    talk_summary: dict[str, Any] = {}
    for idx in range(1, 9):
        fname = f"talk{idx}.dat"
        talk_doc = TalkDisassembler.parse_file(game_dir / fname, idx)
        talk_path = scripts_out / f"talk{idx}.json"
        _write_json(talk_path, talk_doc)
        talk_summary[fname] = {
            "slot_count": talk_doc["slot_count"],
            "non_stub_scripts": talk_doc["non_stub_scripts"],
            "clean_disassembly_count": talk_doc["clean_disassembly_count"],
            "dialogue_count": talk_doc["dialogue_count"],
            "output": str(talk_path),
        }
        for s in talk_doc["scripts"]:
            if s["dialogues"] or s.get("choices") or s.get("videos") or s.get("banners"):
                all_dialogues.append({
                    "talk_id": s["talk_id"],
                    "file": fname,
                    "slot_id": s["slot_id"],
                    "dialogues": s["dialogues"],
                    **({"choices": s["choices"]} if "choices" in s else {}),
                    **({"videos": s["videos"]} if "videos" in s else {}),
                    **({"banners": s["banners"]} if "banners" in s else {}),
                })

    all_dlg_path = scripts_out / "all_dialogues.json"
    _write_json(all_dlg_path, {
        "total_scripts_with_text": len(all_dialogues),
        "total_dialogue_lines": sum(x["dialogue_count"] for x in talk_summary.values()),
        "entries": all_dialogues,
    })
    summary["talk"] = talk_summary
    summary["all_dialogues"] = str(all_dlg_path)

    maps_doc = parse_maps_dat(game_dir / "maps.dat")
    maps_path = scripts_out / "maps_index.json"
    _write_json(maps_path, maps_doc)
    summary["maps.dat"] = {"map_title_count": maps_doc["map_title_count"], "output": str(maps_path)}

    paths_docs = [parse_path_dat(game_dir / f"path{i}.dat") for i in range(1, 11)]
    paths_path = scripts_out / "path_summary.json"
    _write_json(paths_path, {"files": paths_docs})
    summary["paths"] = {"file_count": len(paths_docs), "output": str(paths_path)}

    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description="Read-only PalGame .dat DB & Talk VM exporter")
    parser.add_argument("command", choices=("export-db", "export-scripts", "export-all"))
    parser.add_argument("--game-dir", type=pathlib.Path, default=pathlib.Path("/Users/steve/Downloads/PalGame"))
    parser.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("/Users/steve/Downloads/PalGame_unpacked/db_export"))
    args = parser.parse_args()

    out: dict[str, Any] = {}
    if args.command in ("export-db", "export-all"):
        out["databases"] = cmd_export_db(args.game_dir, args.out_dir)
    if args.command in ("export-scripts", "export-all"):
        out["scripts"] = cmd_export_scripts(args.game_dir, args.out_dir)

    summary_path = args.out_dir / "db_summary.json"
    _write_json(summary_path, out)
    print(json.dumps(out, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
