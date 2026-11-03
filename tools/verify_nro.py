"""Validate a compiled Saves NRO, identity, and the exact prepared assets."""
from pathlib import Path
import argparse
import hashlib
import json
import struct

ROOT = Path(__file__).resolve().parent.parent
TITLE = "NinMagic Saves"
VERSION = "0.2.1"

def require(value, message):
    if not value:
        raise ValueError(message)

def region(data, offset, size):
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset, "Out of bounds resource")
    return data[offset:offset + size]

def text(data):
    require(b"\0" in data, "Unterminated identity")
    return data.split(b"\0", 1)[0].decode("utf-8")

def verify(path, expected_version=VERSION):
    data = Path(path).read_bytes()
    require(0x80 < len(data) <= 16 * 1024 * 1024, "Invalid NRO size")
    require(data[0x10:0x14] == b"NRO0", "Invalid NRO header")
    code_size = struct.unpack_from("<I", data, 0x18)[0]
    require(code_size >= 0x80, "Invalid code size")
    end = 0
    for i in range(3):
        offset, size = struct.unpack_from("<II", data, 0x20 + i * 8)
        require(size and offset >= end and offset + size <= code_size and (i or offset == 0), "Invalid code segments")
        end = offset + size
    header = struct.unpack("<II6Q", region(data, code_size, 56))
    require(header[:2] == (0x54455341, 0), "Invalid ASET header")
    end, assets = 56, []
    for i in range(3):
        offset, size = header[2 + i * 2:4 + i * 2]
        require(size and offset >= end, "Empty or overlapping resources")
        assets.append(region(data, code_size + offset, size))
        end = offset + size
    icon, nacp, romfs = assets
    require(icon == (ROOT / "assets/icon.jpg").read_bytes(), "Wrong app icon")
    require(len(nacp) == 0x4000, "Invalid NACP size")
    for i in range(16):
        title, author = text(nacp[i*0x300:i*0x300+0x200]), text(nacp[i*0x300+0x200:(i+1)*0x300])
        require((title, author) in ((TITLE, "NinMagic"), ("", "")) and (i or title), "Wrong NRO identity")
    require(text(nacp[0x3060:0x3070]) == expected_version, "Wrong NRO version")
    h = struct.unpack("<10Q", region(romfs, 0, 80))
    require(h[0] == 80, "Invalid RomFS header")
    directory = struct.unpack("<6I", region(romfs, h[3], 24))
    require(directory[2] == 0xffffffff and directory[5] == 0, "Unexpected nested assets")
    meta = region(romfs, h[7], h[8])
    offset, visited, files = directory[3], set(), {}
    while offset != 0xffffffff:
        require(offset not in visited, "Cyclic RomFS metadata")
        visited.add(offset)
        parent, next_file, content_offset, size, _, name_size = struct.unpack("<IIQQII", region(meta, offset, 32))
        name = region(meta, offset+32, name_size).decode("utf-8")
        require(parent == 0 and name not in files and name in {"font.bin", "logo.rgba", "FONT-LICENSE.txt"}, "Unexpected runtime asset")
        content = region(romfs, h[9]+content_offset, size)
        require(content == (ROOT / "romfs" / name).read_bytes(), "Runtime asset mismatch: " + name)
        files[name] = {"bytes": size, "sha256": hashlib.sha256(content).hexdigest()}
        offset = next_file
    require(set(files) == {"font.bin", "logo.rgba", "FONT-LICENSE.txt"}, "Missing runtime assets")
    require(files["logo.rgba"]["bytes"] == 256*256*4, "Wrong logo dimensions")
    return {"title": TITLE, "version": expected_version, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "romfs": files, "hardware_tested": False, "workflow": "export in CFW sysMMC, reboot, import in emuMMC", "direct_physical_read_from_emummc": False}

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("nro", type=Path)
    p.add_argument("--expected-version", default=VERSION)
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    report = json.dumps(verify(args.nro, args.expected_version), ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(report, encoding="utf-8")
    print(report)
