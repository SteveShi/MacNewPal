#!/usr/bin/env python3
"""
pal_unpack.py - Asset Unpacker Toolkit for PalGame (Sword and Fairy 2000 / 新仙劍奇俠傳)
Supports:
  - all_*.tsw: Multi-frame UI, character, item, map, magic sprite containers (LZO1X + ARLE 0xFFFF)
  - all_*.act: Animation / action metadata archives
  - all.snd: Sound archives with WAV header reconstruction
  - huge.lmf / huge2.lmf: MSF3 map archives and tiled RGB555 background rendering

Strictly READ-ONLY on game data files.
"""

import os
import sys
import json
import struct
import argparse
import time
from typing import Optional, List, Dict, Tuple, Any
from PIL import Image


# ==============================================================================
# LZO1X Decompressor (Pure Python + Opportunistic C Acceleration)
# ==============================================================================

_c_lzo_decompress = None

def _init_c_lzo():
    global _c_lzo_decompress
    import ctypes
    dylib_dir = os.path.dirname(os.path.abspath(__file__))
    dylib_path = os.path.join(dylib_dir, "_lzo_accel.dylib")
    c_src_path = os.path.join(dylib_dir, "_lzo_accel.c")

    if not os.path.exists(dylib_path) and os.path.exists(c_src_path):
        import subprocess
        try:
            subprocess.run(["clang", "-O3", "-shared", "-fPIC", "-o", dylib_path, c_src_path],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

    if os.path.exists(dylib_path):
        try:
            lib = ctypes.CDLL(dylib_path)
            lib.lzo1x_decompress.argtypes = [
                ctypes.c_char_p, ctypes.c_size_t,
                ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t)
            ]
            lib.lzo1x_decompress.restype = ctypes.c_int
            _c_lzo_decompress = lib.lzo1x_decompress
        except Exception:
            _c_lzo_decompress = None

def _lzo1x_decompress_py(src: bytes, allow_truncated: bool = False) -> bytes:
    """Pure Python LZO1X decompressor following the exact state machine in _lzo_accel.c."""
    dst = bytearray()
    ip = 0
    in_len = len(src)
    if in_len == 0:
        return b""

    class _TruncatedLZO(Exception):
        pass

    def read_u8() -> int:
        nonlocal ip
        if ip >= in_len:
            raise _TruncatedLZO()
        b = src[ip]
        ip += 1
        return b

    def read_bytes(n: int) -> bytes:
        nonlocal ip
        if ip + n > in_len:
            if allow_truncated:
                chunk = src[ip:in_len]
                ip = in_len
                raise _TruncatedLZO()
            raise EOFError(f"LZO1X input truncated at {ip}/{in_len} (needed {n} bytes)")
        chunk = src[ip : ip + n]
        ip += n
        return chunk

    def read_ext_len(base: int) -> int:
        nonlocal ip
        if base != 0:
            return base
        total = 0
        while ip < in_len and src[ip] == 0:
            total += 255
            ip += 1
        if ip >= in_len:
            raise _TruncatedLZO()
        val = total + src[ip]
        ip += 1
        return val

    try:
        enter_match = False
        first = read_u8()
        if first > 17:
            t = first - 17
            if t < 4:
                dst.extend(read_bytes(t))
                t = read_u8()
                enter_match = True
            else:
                dst.extend(read_bytes(t))
                t = read_u8()
                if t < 16:
                    m_pos = len(dst) - 1 - 0x800 - (t >> 2) - (read_u8() << 2)
                    dst.append(dst[m_pos])
                    dst.append(dst[m_pos + 1])
                    dst.append(dst[m_pos + 2])
                    t = src[ip - 2] & 3
                    if t != 0:
                        dst.extend(read_bytes(t))
                        t = read_u8()
                        enter_match = True
                else:
                    enter_match = True
        else:
            ip = 0

        while ip < in_len or enter_match:
            if not enter_match:
                t = read_u8()
                if t < 16:
                    if t == 0:
                        t = 15 + read_ext_len(0)
                    dst.extend(read_bytes(t + 3))
                    t = read_u8()
                    if t < 16:
                        m_pos = len(dst) - 1 - 0x800 - (t >> 2) - (read_u8() << 2)
                        dst.append(dst[m_pos])
                        dst.append(dst[m_pos + 1])
                        dst.append(dst[m_pos + 2])
                        t = src[ip - 2] & 3
                        if t == 0:
                            continue
                        dst.extend(read_bytes(t))
                        t = read_u8()

            enter_match = False
            while True:
                if t >= 64:
                    m_pos = len(dst) - 1 - ((t >> 2) & 7) - (read_u8() << 3)
                    m_len = (t >> 5) - 1 + 2
                elif t >= 32:
                    m_len = t & 31
                    if m_len == 0:
                        m_len = 31 + read_ext_len(0)
                    b0 = read_u8()
                    b1 = read_u8()
                    m_pos = len(dst) - 1 - ((b0 | (b1 << 8)) >> 2)
                    m_len += 2
                elif t >= 16:
                    m_pos = len(dst) - ((t & 8) << 11)
                    m_len = t & 7
                    if m_len == 0:
                        m_len = 7 + read_ext_len(0)
                    b0 = read_u8()
                    b1 = read_u8()
                    m_pos -= ((b0 | (b1 << 8)) >> 2)
                    if m_pos == len(dst):
                        return bytes(dst)
                    m_pos -= 0x4000
                    m_len += 2
                else:
                    m_pos = len(dst) - 1 - (t >> 2) - (read_u8() << 2)
                    dst.append(dst[m_pos])
                    dst.append(dst[m_pos + 1])
                    m_len = 0

                for _ in range(m_len):
                    dst.append(dst[m_pos])
                    m_pos += 1

                t = src[ip - 2] & 3
                if t == 0:
                    break
                dst.extend(read_bytes(t))
                if ip >= in_len:
                    break
                t = read_u8()
    except _TruncatedLZO:
        if not allow_truncated:
            raise EOFError(f"LZO1X stream truncated at {ip}/{in_len} (decoded {len(dst)} bytes)")

    return bytes(dst)


def lzo1x_decompress(src: bytes, expected_len: int = 0, allow_truncated: bool = False) -> bytes:
    """
    Decompresses LZO1X compressed data.
    Uses bounds-checked C library if available; falls back to pure Python.
    """
    if not src:
        return b""

    global _c_lzo_decompress
    if _c_lzo_decompress is not None:
        import ctypes
        buf_size = max(expected_len, len(src) * 10, 65536)
        max_buf = 64 * 1024 * 1024
        while buf_size <= max_buf:
            out_buf = ctypes.create_string_buffer(buf_size)
            out_len = ctypes.c_size_t(buf_size)
            res = _c_lzo_decompress(src, len(src), out_buf, ctypes.byref(out_len))
            if res == 0:
                return out_buf.raw[:out_len.value]
            if res == -1 and ((expected_len > 0 and out_len.value == expected_len) or allow_truncated):
                data = out_buf.raw[:out_len.value]
                if allow_truncated and expected_len > 0 and len(data) < expected_len:
                    data = data.ljust(expected_len, b"\x00")
                return data
            if res == -3:
                buf_size *= 2
                continue
            break

    data = _lzo1x_decompress_py(src, allow_truncated=allow_truncated)
    if allow_truncated and expected_len > 0 and len(data) < expected_len:
        data = data.ljust(expected_len, b"\x00")
    return data


# ==============================================================================
# Pixel Format & ARLE Decoders
# ==============================================================================

_RGB555_LUT: Optional[List[bytes]] = None

def _get_rgb555_lut() -> List[bytes]:
    global _RGB555_LUT
    if _RGB555_LUT is None:
        lut: List[bytes] = []
        for c in range(65536):
            r5 = (c >> 10) & 0x1F
            g5 = (c >> 5) & 0x1F
            b5 = c & 0x1F
            lut.append(bytes((
                (r5 << 3) | (r5 >> 2),
                (g5 << 3) | (g5 >> 2),
                (b5 << 3) | (b5 >> 2),
                255,
            )))
        _RGB555_LUT = lut
    return _RGB555_LUT


def rgb555_to_rgba(c: int, alpha: int = 255) -> Tuple[int, int, int, int]:
    """Expands 15-bit RGB555 (0x7C00 / 0x03E0 / 0x001F) to 32-bit RGBA."""
    r5 = (c >> 10) & 0x1F
    g5 = (c >> 5) & 0x1F
    b5 = c & 0x1F
    r8 = (r5 << 3) | (r5 >> 2)
    g8 = (g5 << 3) | (g5 >> 2)
    b8 = (b5 << 3) | (b5 >> 2)
    return (r8, g8, b8, alpha)

KEY_COLOR_TRANSPARENT = 0x319F  # Transparent chroma key
KEY_COLOR_SHADOW      = 0x026B  # Translucent 50% shadow chroma key


def _rgb555_with_keys(c: int) -> Tuple[int, int, int, int]:
    """Converts RGB555 color to RGBA while honoring engine transparent/shadow color keys."""
    if c == KEY_COLOR_TRANSPARENT:
        return (0, 0, 0, 0)
    if c == KEY_COLOR_SHADOW:
        return (0, 0, 0, 128)
    return rgb555_to_rgba(c, 255)


def decode_arle(
    data: bytes,
    width: int,
    height: int,
    bpp: int,
    palette: Optional[List[int]] = None,
    mask_bytes: Optional[bytes] = None,
) -> List[Tuple[int, int, int, int]]:
    """
    Decodes 8-bit or 16-bit ARLE (magic 0xFFFF) pixel stream into RGBA tuples.
    """
    bp = 8  # skip 8-byte ARLE header (magic, width, height, bpp)
    total_pixels = width * height
    pixels: List[Tuple[int, int, int, int]] = []
    data_len = len(data)

    while bp + 2 <= data_len:
        row_marker = struct.unpack_from("<H", data, bp)[0]
        if row_marker == 0:
            break
        bp += 2
        while bp + 2 <= data_len:
            token = struct.unpack_from("<H", data, bp)[0]
            bp += 2
            if token == 0:
                break
            tag = token & 0xC000
            count = token & 0x3FFF
            if tag == 0:
                if bpp == 16:
                    for _ in range(count):
                        if bp + 2 <= data_len:
                            c = struct.unpack_from("<H", data, bp)[0]
                            bp += 2
                            pixels.append(_rgb555_with_keys(c))
                        else:
                            pixels.append((0, 0, 0, 0))
                else:
                    pal = palette or []
                    for _ in range(count):
                        if bp < data_len:
                            idx = data[bp]
                            bp += 1
                            c = pal[idx] if idx < len(pal) else 0
                            pixels.append(_rgb555_with_keys(c))
                        else:
                            pixels.append((0, 0, 0, 0))
            elif tag == 0xC000:
                pixels.extend([(0, 0, 0, 128)] * count)
            else:  # 0x8000 / 0x4000 transparent skip
                pixels.extend([(0, 0, 0, 0)] * count)

    if len(pixels) < total_pixels:
        pixels.extend([(0, 0, 0, 0)] * (total_pixels - len(pixels)))

    if mask_bytes and len(mask_bytes) >= total_pixels:
        for i in range(total_pixels):
            if mask_bytes[i] == 1:
                r, g, b, a = pixels[i]
                pixels[i] = (r, g, b, max(a, 200))

    return pixels[:total_pixels]


# ==============================================================================
# Container Parsers (.tsw, .act, .snd, .lmf)
# ==============================================================================

def decode_name(raw: bytes) -> str:
    """Decodes CP950/Big5 encoded Chinese string, stripping null padding."""
    raw = raw.split(b"\x00", 1)[0]
    for enc in ("cp950", "big5", "gbk", "latin1"):
        try:
            return raw.decode(enc)
        except Exception:
            continue
    return repr(raw)


def sanitize_filename(name: str) -> str:
    """Sanitizes archive entry name for safe filesystem output."""
    return name.replace("/", "_").replace("\\", "_").replace("\x00", "")


def iter_archive_directory(path: str, max_slots: int = 3000):
    """
    Yields (file_handle, file_size, slot_index, name, raw_size, offset, slot_id, raw_record)
    for archives using the standard 0x1C header + fixed 3000-slot (0x1C..0x203BC) 44-byte directory.

    Note: hdr[24:28] (raw_cnt) is often stale/under-counted in .act and .snd archives
    (e.g. all_map1.act has raw_cnt=64 while entries sit at slots 1000..1500; all.snd has
    raw_cnt=664 while valid entries continue to slot 930), so we always scan the full
    3000-slot TOC table bounded by EOF.
    """
    with open(path, "rb") as f:
        f.seek(0, 2)
        fsize = f.tell()
        f.seek(0)
        hdr = f.read(28)
        if len(hdr) < 28:
            return
        max_possible = min(max_slots, max(0, (fsize - 0x1C) // 44))

        for i in range(max_possible):
            entry_off = 0x1C + i * 44
            f.seek(entry_off)
            rec = f.read(44)
            if len(rec) < 44:
                break
            name = decode_name(rec[:20])
            size_raw, off, slot_id = struct.unpack("<3I", rec[20:32])
            yield f, fsize, i, name, size_raw, off, slot_id, rec


class TswArchive:
    """Parser for PalGame sprite archives (all_*.tsw)."""

    def __init__(self, path: str):
        self.path = path
        self.filename = os.path.basename(path)
        self.slots: List[Dict[str, Any]] = []
        self._parse_slots()

    def _parse_slots(self):
        for f, fsize, i, name, size, off, slot_id, _ in iter_archive_directory(self.path):
            if off == 0 or size == 0 or off >= fsize or off + 12 > fsize:
                continue
            f.seek(off)
            chdr = f.read(12)
            if len(chdr) < 12:
                continue
            magic, nframes, bpp, off0 = struct.unpack("<4H", chdr[4:12])
            if magic != 0xABCD:
                continue
            self.slots.append({
                "slot_index": i,
                "id": slot_id,
                "name": name,
                "offset": off,
                "size": size,
                "nframes": nframes,
                "bpp": bpp,
                "off0": off0,
            })

    def extract_entry(self, slot_meta: Dict[str, Any], out_dir: str, frame_limit: Optional[int] = None) -> List[str]:
        """Extracts frames for a slot as PNG files."""
        os.makedirs(out_dir, exist_ok=True)
        saved_paths = []
        off = slot_meta["offset"]
        nframes = slot_meta["nframes"]
        bpp = slot_meta["bpp"]
        off0 = slot_meta["off0"]
        name = sanitize_filename(slot_meta["name"])
        slot_id = slot_meta["id"]

        with open(self.path, "rb") as f:
            palette: List[int] = []
            if bpp == 8:
                f.seek(off + 12)
                pal_bytes = f.read(512)
                palette = list(struct.unpack("<256H", pal_bytes))
                frame_table_base = off + off0 + 0x200
            else:
                frame_table_base = off + off0

            max_frames = nframes if frame_limit is None else min(nframes, frame_limit)
            for fr in range(max_frames):
                f.seek(frame_table_base + fr * 36)
                fhdr = f.read(36)
                if len(fhdr) < 36:
                    break
                f_off, c_sz1, u_sz1, _, c_sz2, u_sz2 = struct.unpack("<6I", fhdr[:24])
                w, h = struct.unpack("<2H", fhdr[0x20:0x24])

                if w == 0 or h == 0 or c_sz1 == 0:
                    continue

                f.seek(off + f_off)
                decomp1 = lzo1x_decompress(f.read(c_sz1), u_sz1)
                if not decomp1:
                    continue

                decomp2 = None
                if c_sz2 > 0:
                    f.seek(off + f_off + c_sz1)
                    decomp2 = lzo1x_decompress(f.read(c_sz2), u_sz2, allow_truncated=True)

                magic = struct.unpack("<H", decomp1[:2])[0] if len(decomp1) >= 2 else 0
                if magic == 0xFFFF:
                    pixels = decode_arle(decomp1, w, h, bpp, palette=palette, mask_bytes=decomp2)
                else:
                    words = struct.unpack(f"<{min(len(decomp1)//2, w*h)}H", decomp1[:w*h*2])
                    pixels = [rgb555_to_rgba(c, 255) for c in words]
                    if len(pixels) < w * h:
                        pixels.extend([(0, 0, 0, 0)] * (w * h - len(pixels)))

                img = Image.new("RGBA", (w, h))
                img.putdata(pixels)
                out_name = f"{slot_id:04d}_{name}_fr{fr:02d}.png"
                out_path = os.path.join(out_dir, out_name)
                img.save(out_path)
                saved_paths.append(out_path)

        return saved_paths


# 2-byte little-endian opcodes in all_*.act action bytecode.
# Format per opcode: (mnemonic, arg_count_u16, description)
ACT_OPCODES: Dict[int, Tuple[str, int, str]] = {
    0x4544: ("DE", 0, "delay_frame_end"),
    0x4F56: ("VO", 0, "end_sequence"),
    0x4F32: ("2O", 0, "secondary_end_or_yield"),
    0x4E4F: ("ON", 0, "visible_on"),
    0x5649: ("IV", 0, "flag_bit_0_visible"),
    0x534D: ("SM", 0, "shadow_or_mirror_mode"),
    0x434C: ("LC", 0, "clear_rotation_scale_2"),
    0x4148: ("HA", 0, "blend_flag_8"),
    0x414D: ("MA", 0, "blend_flag_4"),
    0x414E: ("NA", 0, "blend_flag_2c"),
    0x544E: ("NT", 1, "set_tsw_slot_id"),
    0x5246: ("FR", 1, "set_frame_delay_base"),
    0x524F: ("OR", 1, "set_frame_override"),
    0x5344: ("DS", 1, "set_duration_ticks"),
    0x5041: ("AP", 1, "set_frame_index_and_step"),
    0x5859: ("YX", 2, "set_anchor_xy"),
    0x5748: ("HW", 2, "set_hitbox_or_size_wh"),
    0x4F58: ("XO", 1, "set_offset_x"),
    0x4F59: ("YO", 1, "set_offset_y"),
    0x4F41: ("AO", 1, "set_alpha_or_opacity"),
    0x5756: ("VW", 1, "set_wait_or_var"),
    0x5145: ("EQ", 1, "set_param_10"),
    0x4154: ("TA", 1, "set_tilt_or_tag"),
    0x4145: ("EA", 1, "set_param_9"),
    0x4158: ("XA", 1, "set_aux_x"),
    0x4159: ("YA", 1, "set_aux_y"),
    0x5457: ("TW", 1, "set_tween_byte"),
    0x5343: ("CS", 1, "trigger_sound_effect"),
    0x4753: ("SG", 1, "set_sprite_group_flag"),
    0x4C44: ("LD", 1, "set_blend_depth"),
    0x4C50: ("LP", 3, "loop_range_and_count"),
    0x4347: ("GC", 2, "color_green_mod"),
    0x4342: ("BC", 2, "color_blue_mod"),
    0x4352: ("RC", 2, "color_red_mod"),
    0x464C: ("LF", 7, "light_flash_rgb_params"),
}


class ActArchive:
    """Parser & disassembler for PalGame animation metadata archives (all_*.act)."""

    def __init__(self, path: str):
        self.path = path
        self.filename = os.path.basename(path)
        self.slots: List[Dict[str, Any]] = []
        self._parse_slots()

    def _parse_slots(self):
        for f, fsize, i, name, size, off, slot_id, _ in iter_archive_directory(self.path):
            if off == 0 or size < 2 or off + size > fsize:
                continue
            f.seek(off)
            cnt_bytes = f.read(2)
            sub_count = struct.unpack("<H", cnt_bytes)[0] if len(cnt_bytes) == 2 else 0
            self.slots.append({
                "slot_index": i,
                "id": slot_id,
                "name": name,
                "offset": off,
                "size": size,
                "sub_action_count": sub_count,
            })

    @staticmethod
    def disassemble_stream(buf: bytes) -> Dict[str, Any]:
        """Disassemble a single .act action bytecode stream."""
        if len(buf) < 2:
            return {"header_u16": None, "instructions": [], "clean": False, "unparsed_bytes": len(buf)}
        header_u16 = struct.unpack_from("<H", buf, 0)[0]
        pos = 2
        insns: List[Dict[str, Any]] = []
        clean = True
        while pos + 2 <= len(buf):
            op_off = pos
            code = struct.unpack_from("<H", buf, pos)[0]
            pos += 2
            if code == 0:
                continue
            spec = ACT_OPCODES.get(code)
            if spec is None:
                clean = False
                pos -= 2
                break
            mnem, nargs, desc = spec
            if pos + nargs * 2 > len(buf):
                clean = False
                pos -= 2
                break
            args = list(struct.unpack_from(f"<{nargs}h", buf, pos)) if nargs else []
            pos += nargs * 2
            insns.append({
                "offset": op_off,
                "op": struct.pack("<H", code).decode("latin1"),
                "code": f"0x{code:04x}",
                "args": args,
                "desc": desc,
            })
        return {
            "header_u16": header_u16,
            "instructions": insns,
            "clean": clean and (pos == len(buf)),
            "unparsed_bytes": len(buf) - pos,
        }

    def parse_slot_actions(self, slot_meta: Dict[str, Any]) -> Dict[str, Any]:
        """Read and decode all sub-action streams inside one .act slot."""
        with open(self.path, "rb") as f:
            f.seek(slot_meta["offset"])
            sub = f.read(slot_meta["size"])

        cnt = struct.unpack_from("<H", sub, 0)[0] if len(sub) >= 2 else 0
        max_cnt = max(0, (len(sub) - 2) // 4)
        cnt = min(cnt, max_cnt)
        offs = list(struct.unpack_from(f"<{cnt}I", sub, 2)) if cnt else []

        unique_valid = sorted({o for o in offs if 2 + cnt * 4 <= o < len(sub)})
        next_end = {
            o: (unique_valid[idx + 1] if idx + 1 < len(unique_valid) else len(sub))
            for idx, o in enumerate(unique_valid)
        }

        decoded_by_off: Dict[int, Dict[str, Any]] = {}
        for o in unique_valid:
            decoded_by_off[o] = self.disassemble_stream(sub[o : next_end[o]])

        actions: List[Dict[str, Any]] = []
        clean_streams = 0
        for act_idx, o in enumerate(offs):
            if o in decoded_by_off:
                d = decoded_by_off[o]
                if d["clean"]:
                    clean_streams += 1
                actions.append({
                    "action_index": act_idx,
                    "offset": o,
                    "byte_length": next_end[o] - o,
                    **d,
                })

        return {
            "id": slot_meta["id"],
            "slot_index": slot_meta["slot_index"],
            "name": slot_meta["name"],
            "sub_action_slots": cnt,
            "non_empty_actions": len(actions),
            "clean_actions": clean_streams,
            "actions": actions,
        }

    def extract_entry(self, slot_meta: Dict[str, Any], out_dir: str) -> str:
        """Decode and save one .act slot to a structured UTF-8 JSON file."""
        os.makedirs(out_dir, exist_ok=True)
        decoded = self.parse_slot_actions(slot_meta)
        name = sanitize_filename(slot_meta["name"])
        out_path = os.path.join(out_dir, f"{slot_meta['id']:05d}_{name}.json")
        with open(out_path, "w", encoding="utf-8") as f_out:
            json.dump(decoded, f_out, ensure_ascii=False, indent=2)
        return out_path


class SndArchive:
    """Parser for PalGame sound archives (all.snd)."""

    def __init__(self, path: str):
        self.path = path
        self.entries: List[Dict[str, Any]] = []
        self._parse_entries()

    def _parse_entries(self):
        for _, fsize, i, name, size_raw, off, slot_id, rec in iter_archive_directory(self.path):
            size = size_raw & 0x03FFFFFF
            flag = rec[0x20] & 3
            if off == 0 or size == 0 or off + size > fsize:
                continue
            self.entries.append({
                "slot_index": i,
                "id": slot_id,
                "name": name,
                "offset": off,
                "size": size,
                "flag": flag,
            })

    def extract_entry(self, entry: Dict[str, Any], out_dir: str) -> str:
        """Extracts a sound entry and reconstructs standard 44-byte WAV header."""
        os.makedirs(out_dir, exist_ok=True)
        off = entry["offset"]
        size = entry["size"]
        flag = entry["flag"]
        name = sanitize_filename(entry["name"])
        slot_id = entry["id"]

        with open(self.path, "rb") as f:
            f.seek(off)
            raw = f.read(size)

        out_path = os.path.join(out_dir, f"{slot_id:04d}_{name}.wav")

        if flag == 1:
            with open(out_path, "wb") as f_out:
                f_out.write(raw)
        else:
            if len(raw) < 18:
                raise ValueError(f"Sound entry {slot_id} ({name}) too small ({len(raw)} bytes)")
            fmt_chunk = raw[:16]
            pcm_data = raw[18:]
            data_len = len(pcm_data)
            header = (
                b"RIFF" +
                struct.pack("<I", data_len + 36) +
                b"WAVEfmt " +
                struct.pack("<I", 16) +
                fmt_chunk +
                b"data" +
                struct.pack("<I", data_len)
            )
            with open(out_path, "wb") as f_out:
                f_out.write(header + pcm_data)

        return out_path


class LmfArchive:
    """Parser for PalGame MSF3 map archives (huge.lmf / huge2.lmf)."""

    def __init__(self, path: str):
        self.path = path
        self.filename = os.path.basename(path)
        self.maps: List[Dict[str, Any]] = []
        self._parse_maps()

    def _parse_maps(self):
        with open(self.path, "rb") as f:
            f.seek(0, 2)
            fsize = f.tell()
            f.seek(0)
            idx_off = struct.unpack("<I", f.read(4))[0]
            if idx_off >= fsize:
                return
            n_entries = (fsize - idx_off) // 16
            f.seek(idx_off)
            for i in range(n_entries):
                rec = f.read(16)
                if len(rec) < 16:
                    break
                map_off, map_size, map_id, unk = struct.unpack("<4I", rec)
                if map_off >= fsize or map_size == 0:
                    continue
                self.maps.append({
                    "map_index": i,
                    "id": map_id,
                    "offset": map_off,
                    "size": map_size,
                    "unk": unk,
                })

    def get_map_metadata(self, map_meta: Dict[str, Any]) -> Dict[str, Any]:
        """Reads MSF3 map header details."""
        map_off = map_meta["offset"]
        with open(self.path, "rb") as f:
            f.seek(map_off)
            magic = f.read(4).decode("latin1", "replace")
            hdr = f.read(0x400)
            w_tiles, h_tiles, tile_sz, num_tiles, bg_type = struct.unpack("<5H", hdr[0x100:0x10A])
            bg_chunk_off = struct.unpack("<I", hdr[0x38:0x3C])[0]
            return {
                "id": map_meta["id"],
                "magic": magic,
                "offset": map_off,
                "size": map_meta["size"],
                "w_tiles": w_tiles,
                "h_tiles": h_tiles,
                "tile_size": tile_sz,
                "pixel_width": w_tiles * tile_sz,
                "pixel_height": h_tiles * tile_sz,
                "num_tiles": num_tiles,
                "bg_type": bg_type,
                "bg_chunk_offset": bg_chunk_off,
            }

    def render_map_background(self, map_meta: Dict[str, Any], out_png_path: str) -> bool:
        """Decompresses MSF3 tile pool and assembles map background via TileMap into RGBA PNG."""
        map_off = map_meta["offset"]
        with open(self.path, "rb") as f:
            f.seek(map_off)
            if f.read(4) != b"MSF3":
                return False
            hdr = f.read(0x400)
            w_tiles, h_tiles, tile_sz, num_tiles, bg_type = struct.unpack("<5H", hdr[0x100:0x10A])
            bg_chunk_off = struct.unpack("<I", hdr[0x38:0x3C])[0]
            if bg_chunk_off == 0 or tile_sz == 0 or w_tiles == 0 or h_tiles == 0:
                return False

            # TileMap array at MSF3 + 0x118: u32 entries whose low u16 indexes the tile pool
            layer_tiles = w_tiles * h_tiles
            layers = max(bg_type, 1)
            f.seek(map_off + 0x118)
            tm_bytes = f.read(layer_tiles * layers * 4)
            if len(tm_bytes) < layer_tiles * 4:
                return False
            tile_map = [val & 0xFFFF for val in struct.unpack(f"<{len(tm_bytes)//4}I", tm_bytes)]

            f.seek(map_off + bg_chunk_off)
            bg_hdr = f.read(0x400)
            block_size = struct.unpack("<I", bg_hdr[4:8])[0]

            decomp_all = bytearray()
            k = 0
            while 0x10 + k * 8 + 8 <= len(bg_hdr):
                b_off, b_size = struct.unpack("<2I", bg_hdr[0x10 + k * 8 : 0x18 + k * 8])
                if b_off == 0 and b_size == 0:
                    break
                f.seek(map_off + b_off)
                decomp_all.extend(lzo1x_decompress(f.read(b_size), block_size))
                k += 1

            tile_bytes = tile_sz * tile_sz * 2
            if len(decomp_all) < tile_bytes:
                return False

            pw = w_tiles * tile_sz
            ph = h_tiles * tile_sz
            os.makedirs(os.path.dirname(os.path.abspath(out_png_path)), exist_ok=True)
            base_root, base_ext = os.path.splitext(out_png_path)
            lut = _get_rgb555_lut()
            num_avail_tiles = len(decomp_all) // tile_bytes
            tile_rows_cache: Dict[int, List[bytes]] = {}
            row_stride_bytes = pw * 4
            tile_row_bytes = tile_sz * 4

            for layer in range(layers):
                start_idx = layer * layer_tiles
                if start_idx + layer_tiles > len(tile_map):
                    break
                rgba = bytearray(pw * ph * 4)
                mv = memoryview(rgba)
                for ty in range(h_tiles):
                    row_off = start_idx + ty * w_tiles
                    y_base = ty * tile_sz
                    for tx in range(w_tiles):
                        tid = tile_map[row_off + tx]
                        if tid >= num_avail_tiles:
                            continue
                        t_rows = tile_rows_cache.get(tid)
                        if t_rows is None:
                            words = struct.unpack_from(f"<{tile_sz * tile_sz}H", decomp_all, tid * tile_bytes)
                            t_rows = [
                                b"".join(lut[w] for w in words[py * tile_sz : (py + 1) * tile_sz])
                                for py in range(tile_sz)
                            ]
                            tile_rows_cache[tid] = t_rows
                        x_off_bytes = tx * tile_row_bytes
                        for py in range(tile_sz):
                            dst_off = (y_base + py) * row_stride_bytes + x_off_bytes
                            mv[dst_off : dst_off + tile_row_bytes] = t_rows[py]

                img = Image.frombytes("RGBA", (pw, ph), bytes(rgba))
                target_path = out_png_path if layer == 0 else f"{base_root}_layer{layer}{base_ext}"
                img.save(target_path)

            return True


# ==============================================================================
# CLI Commands
# ==============================================================================

def cmd_manifest(args):
    game_dir = args.game_dir
    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)
    print(f"[*] Generating JSON manifests for game assets at: {game_dir}")
    print(f"[*] Manifest output directory: {out_dir}")

    summary = {
        "source_dir": game_dir,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "files": {},
    }

    # 1. Process .tsw files
    tsw_names = ["all_char.tsw", "all_item.tsw", "all_magic.tsw", "all_map1.tsw", "all_map2.tsw", "all_sys.tsw"]
    for tsw_name in tsw_names:
        p = os.path.join(game_dir, tsw_name)
        if not os.path.exists(p):
            continue
        tsw = TswArchive(p)
        manifest_path = os.path.join(out_dir, f"{tsw_name}.json")
        with open(manifest_path, "w", encoding="utf-8") as f_out:
            json.dump({
                "archive": tsw_name,
                "file_size": os.path.getsize(p),
                "valid_slots_count": len(tsw.slots),
                "slots": tsw.slots,
            }, f_out, ensure_ascii=False, indent=2)
        summary["files"][tsw_name] = {
            "valid_slots": len(tsw.slots),
            "manifest": manifest_path,
        }
        print(f"  [+] {tsw_name}: {len(tsw.slots)} valid slots -> {os.path.basename(manifest_path)}")

    # 2. Process .act files
    act_names = ["all_char.act", "all_item.act", "all_magic.act", "all_map1.act", "all_map2.act", "all_sys.act"]
    for act_name in act_names:
        p = os.path.join(game_dir, act_name)
        if not os.path.exists(p):
            continue
        act = ActArchive(p)
        manifest_path = os.path.join(out_dir, f"{act_name}.json")
        with open(manifest_path, "w", encoding="utf-8") as f_out:
            json.dump({
                "archive": act_name,
                "file_size": os.path.getsize(p),
                "valid_slots_count": len(act.slots),
                "slots": act.slots,
            }, f_out, ensure_ascii=False, indent=2)
        summary["files"][act_name] = {
            "valid_slots": len(act.slots),
            "manifest": manifest_path,
        }
        print(f"  [+] {act_name}: {len(act.slots)} valid slots -> {os.path.basename(manifest_path)}")

    # 3. Process all.snd
    snd_path = os.path.join(game_dir, "all.snd")
    if os.path.exists(snd_path):
        snd = SndArchive(snd_path)
        manifest_path = os.path.join(out_dir, "all.snd.json")
        with open(manifest_path, "w", encoding="utf-8") as f_out:
            json.dump({
                "archive": "all.snd",
                "file_size": os.path.getsize(snd_path),
                "valid_entries_count": len(snd.entries),
                "entries": snd.entries,
            }, f_out, ensure_ascii=False, indent=2)
        summary["files"]["all.snd"] = {
            "valid_entries": len(snd.entries),
            "manifest": manifest_path,
        }
        print(f"  [+] all.snd: {len(snd.entries)} valid audio entries -> {os.path.basename(manifest_path)}")

    # 4. Process huge.lmf & huge2.lmf
    for lmf_name in ["huge.lmf", "huge2.lmf"]:
        p = os.path.join(game_dir, lmf_name)
        if not os.path.exists(p):
            continue
        lmf = LmfArchive(p)
        manifest_path = os.path.join(out_dir, f"{lmf_name}.json")
        with open(manifest_path, "w", encoding="utf-8") as f_out:
            json.dump({
                "archive": lmf_name,
                "file_size": os.path.getsize(p),
                "maps_count": len(lmf.maps),
                "maps": lmf.maps,
            }, f_out, ensure_ascii=False, indent=2)
        summary["files"][lmf_name] = {
            "maps_count": len(lmf.maps),
            "manifest": manifest_path,
        }
        print(f"  [+] {lmf_name}: {len(lmf.maps)} maps -> {os.path.basename(manifest_path)}")

    summary_path = os.path.join(out_dir, "manifest_summary.json")
    with open(summary_path, "w", encoding="utf-8") as f_out:
        json.dump(summary, f_out, ensure_ascii=False, indent=2)
    print(f"[✓] Manifest generation complete: {summary_path}")


def cmd_extract_tsw(args):
    game_dir = args.game_dir
    target_file = args.file
    if not os.path.isabs(target_file):
        target_file = os.path.join(game_dir, target_file)

    if not os.path.exists(target_file):
        print(f"[-] Error: file not found: {target_file}", file=sys.stderr)
        sys.exit(1)

    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)
    tsw = TswArchive(target_file)
    print(f"[*] Opened {os.path.basename(target_file)} ({len(tsw.slots)} slots)")

    # Filter slots
    filtered_slots = tsw.slots
    if args.id is not None:
        filtered_slots = [s for s in filtered_slots if s["id"] == args.id or s["slot_index"] == args.id]
    if args.name is not None:
        filtered_slots = [s for s in filtered_slots if args.name.lower() in s["name"].lower()]
    if args.limit is not None and args.limit > 0:
        filtered_slots = filtered_slots[:args.limit]

    print(f"[*] Extracting {len(filtered_slots)} slot(s) to: {out_dir}")
    extracted_count = 0
    total_frames = 0
    t0 = time.time()

    for s in filtered_slots:
        paths = tsw.extract_entry(s, out_dir, frame_limit=args.frame)
        total_frames += len(paths)
        extracted_count += 1
        if len(filtered_slots) <= 30 or extracted_count % 10 == 0:
            print(f"  [+] Extracted slot {s['id']:04d} ({s['name']}): {len(paths)} frames")

    t1 = time.time()
    print(f"[✓] Extracted {total_frames} frame(s) across {extracted_count} slot(s) in {t1 - t0:.2f}s")


def cmd_extract_snd(args):
    game_dir = args.game_dir
    target_file = os.path.join(game_dir, "all.snd")
    if not os.path.exists(target_file):
        print(f"[-] Error: all.snd not found at: {target_file}", file=sys.stderr)
        sys.exit(1)

    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)
    snd = SndArchive(target_file)
    print(f"[*] Opened all.snd ({len(snd.entries)} sound entries)")

    filtered = snd.entries
    if args.id is not None:
        filtered = [e for e in filtered if e["id"] == args.id or e["slot_index"] == args.id]
    if args.name is not None:
        filtered = [e for e in filtered if args.name.lower() in e["name"].lower()]
    if args.limit is not None and args.limit > 0:
        filtered = filtered[:args.limit]

    print(f"[*] Extracting {len(filtered)} sound file(s) to: {out_dir}")
    t0 = time.time()
    count = 0
    for e in filtered:
        out_path = snd.extract_entry(e, out_dir)
        count += 1
        if len(filtered) <= 30 or count % 20 == 0:
            print(f"  [+] Extracted: {os.path.basename(out_path)} ({e['size']} bytes)")

    t1 = time.time()
    print(f"[✓] Extracted {count} sound file(s) in {t1 - t0:.2f}s")


def cmd_extract_lmf(args):
    game_dir = args.game_dir
    target_file = args.file
    if not os.path.isabs(target_file):
        target_file = os.path.join(game_dir, target_file)

    if not os.path.exists(target_file):
        print(f"[-] Error: LMF file not found: {target_file}", file=sys.stderr)
        sys.exit(1)

    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)
    lmf = LmfArchive(target_file)
    print(f"[*] Opened {os.path.basename(target_file)} ({len(lmf.maps)} maps)")

    filtered = lmf.maps
    if args.id is not None:
        filtered = [m for m in filtered if m["id"] == args.id]
    if args.limit is not None and args.limit > 0:
        filtered = filtered[:args.limit]

    print(f"[*] Processing {len(filtered)} map(s) to: {out_dir}")
    t0 = time.time()
    count = 0
    for m in filtered:
        meta = lmf.get_map_metadata(m)
        json_path = os.path.join(out_dir, f"map_{meta['id']:04d}.json")
        with open(json_path, "w", encoding="utf-8") as f_out:
            json.dump(meta, f_out, indent=2)

        png_path = os.path.join(out_dir, f"map_{meta['id']:04d}.png")
        ok = lmf.render_map_background(m, png_path)
        count += 1
        status = f"rendered ({meta['pixel_width']}x{meta['pixel_height']})" if ok else "no background"
        print(f"  [+] Map {meta['id']:04d}: {status} -> {os.path.basename(json_path)}")

    t1 = time.time()
    print(f"[✓] Finished processing {count} map(s) in {t1 - t0:.2f}s")


def cmd_extract_act(args):
    game_dir = args.game_dir
    out_root = args.out_dir
    act_names = (
        ["all_char.act", "all_item.act", "all_magic.act", "all_map1.act", "all_map2.act", "all_sys.act"]
        if args.file == "all"
        else [args.file]
    )

    total_slots = 0
    total_actions = 0
    total_dirty = 0
    t0 = time.time()
    for act_name in act_names:
        target_file = act_name if os.path.isabs(act_name) else os.path.join(game_dir, act_name)
        if not os.path.exists(target_file):
            print(f"[-] Error: file not found: {target_file}", file=sys.stderr)
            sys.exit(1)
        act = ActArchive(target_file)
        filtered = act.slots
        if args.id is not None:
            filtered = [s for s in filtered if s["id"] == args.id or s["slot_index"] == args.id]
        if args.limit is not None and args.limit > 0:
            filtered = filtered[:args.limit]

        sub_out = os.path.join(out_root, os.path.splitext(os.path.basename(target_file))[0])
        os.makedirs(sub_out, exist_ok=True)
        index_entries = []
        file_actions = 0
        file_dirty = 0
        for s in filtered:
            out_path = act.extract_entry(s, sub_out)
            with open(out_path, "r", encoding="utf-8") as f_in:
                decoded = json.load(f_in)
            n_act = decoded["non_empty_actions"]
            n_dirty = n_act - decoded["clean_actions"]
            file_actions += n_act
            file_dirty += n_dirty
            index_entries.append({
                "id": s["id"],
                "slot_index": s["slot_index"],
                "name": s["name"],
                "sub_action_slots": decoded["sub_action_slots"],
                "non_empty_actions": n_act,
                "clean_actions": decoded["clean_actions"],
                "file": os.path.basename(out_path),
            })

        with open(os.path.join(sub_out, "_index.json"), "w", encoding="utf-8") as f_idx:
            json.dump({
                "archive": os.path.basename(target_file),
                "slot_count": len(index_entries),
                "total_actions": file_actions,
                "unclean_actions": file_dirty,
                "slots": index_entries,
            }, f_idx, ensure_ascii=False, indent=2)

        total_slots += len(index_entries)
        total_actions += file_actions
        total_dirty += file_dirty
        print(f"  [+] {os.path.basename(target_file)}: {len(index_entries)} slots, {file_actions} actions, unclean={file_dirty} -> {sub_out}")

    print(f"[✓] Extracted {total_slots} .act slots ({total_actions} action streams, unclean={total_dirty}) in {time.time() - t0:.2f}s")


# ==============================================================================
# Main Entry Point
# ==============================================================================

def main():
    _init_c_lzo()

    default_game_dir = "/Users/steve/Downloads/PalGame"
    default_unpacked_dir = "/Users/steve/Downloads/PalGame_unpacked"

    parser = argparse.ArgumentParser(
        description="PalGame Asset Unpacker Toolkit (Sword and Fairy 2000)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--game-dir", default=default_game_dir, help="Path to PalGame directory")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # Subcommand: manifest
    p_man = subparsers.add_parser("manifest", help="Generate JSON manifests for all archives")
    p_man.add_argument("--out-dir", default=os.path.join(default_unpacked_dir, "manifests"),
                       help="Output directory for JSON manifests")

    # Subcommand: extract-tsw
    p_tsw = subparsers.add_parser("extract-tsw", help="Extract sprite frames from .tsw archives to RGBA PNG")
    p_tsw.add_argument("--file", default="all_sys.tsw", help=".tsw filename or full path")
    p_tsw.add_argument("--out-dir", default=os.path.join(default_unpacked_dir, "extracted", "tsw"),
                       help="Output directory for PNGs")
    p_tsw.add_argument("--id", type=int, default=None, help="Slot ID filter")
    p_tsw.add_argument("--name", default=None, help="Slot name substring filter")
    p_tsw.add_argument("--limit", type=int, default=None, help="Maximum entries to extract")
    p_tsw.add_argument("--frame", type=int, default=None, help="Limit to N frames per slot")

    # Subcommand: extract-act
    p_act = subparsers.add_parser("extract-act", help="Disassemble and export all_*.act animation scripts to JSON")
    p_act.add_argument("--file", default="all", help=".act filename, full path, or 'all'")
    p_act.add_argument("--out-dir", default=os.path.join(default_unpacked_dir, "extracted", "act"),
                       help="Output directory for decoded .act JSONs")
    p_act.add_argument("--id", type=int, default=None, help="Slot ID filter")
    p_act.add_argument("--limit", type=int, default=None, help="Maximum slots to extract")

    # Subcommand: extract-snd
    p_snd = subparsers.add_parser("extract-snd", help="Extract and reconstruct WAV files from all.snd")
    p_snd.add_argument("--out-dir", default=os.path.join(default_unpacked_dir, "extracted", "snd"),
                       help="Output directory for WAV files")
    p_snd.add_argument("--id", type=int, default=None, help="Entry ID filter")
    p_snd.add_argument("--name", default=None, help="Entry name substring filter")
    p_snd.add_argument("--limit", type=int, default=None, help="Maximum sound files to extract")

    # Subcommand: extract-lmf
    p_lmf = subparsers.add_parser("extract-lmf", help="Extract map metadata and render background PNGs")
    p_lmf.add_argument("--file", default="huge.lmf", help="LMF filename (huge.lmf or huge2.lmf)")
    p_lmf.add_argument("--out-dir", default=os.path.join(default_unpacked_dir, "extracted", "lmf"),
                       help="Output directory for map data")
    p_lmf.add_argument("--id", type=int, default=None, help="Map ID filter")
    p_lmf.add_argument("--limit", type=int, default=None, help="Maximum maps to extract")

    args = parser.parse_args()

    if args.command == "manifest":
        cmd_manifest(args)
    elif args.command == "extract-tsw":
        cmd_extract_tsw(args)
    elif args.command == "extract-act":
        cmd_extract_act(args)
    elif args.command == "extract-snd":
        cmd_extract_snd(args)
    elif args.command == "extract-lmf":
        cmd_extract_lmf(args)


if __name__ == "__main__":
    main()
