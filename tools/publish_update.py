#!/usr/bin/env python3
"""Prepare a verified, versioned NRO and HTTPS update manifest; never uploads.

Example:
  python tools/publish_update.py dist/NinMagic-Saves.nro --base-url \
    https://raw.githubusercontent.com/ANE-JV/ninmagic-saves/main \
    --output dist/publication
The versioned binary is immutable: a different build needs a new NACP version.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from urllib.parse import urlsplit

VERSION = re.compile(r"(?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,4})\Z")
MAX_NRO = 16 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def semver(value):
    require(VERSION.fullmatch(value) is not None, "Expected numeric major.minor.patch")
    parts = tuple(map(int, value.split(".")))
    require(all(part <= 65535 for part in parts), "Version component exceeds limit")
    return parts


def https(url):
    split = urlsplit(url)
    require(len(url) <= 2048 and split.scheme == "https" and split.hostname and not split.username and not split.password and not split.fragment and not any(ord(c) <= 32 or ord(c) == 127 or c == "\\" for c in url), "A valid HTTPS URL without credentials or fragment is required")
    return url


def inspect_nro(binary):
    require(0x80 + 56 + 0x4000 <= len(binary) <= MAX_NRO, "NRO size exceeds limits")
    require(binary[0x10:0x14] == b"NRO0", "Missing NRO0 header")
    code = struct.unpack_from("<I", binary, 0x18)[0]
    require(0x80 <= code <= len(binary) - 56, "Invalid NRO code size")
    end = 0
    for index in range(3):
        offset, size = struct.unpack_from("<II", binary, 0x20 + index * 8)
        require(size and offset >= end and offset + size <= code and (index or offset == 0), "Invalid NRO segment")
        end = offset + size
    aset = struct.unpack_from("<II6Q", binary, code)
    require(aset[:2] == (0x54455341, 0), "Unsupported ASET header")
    end, nacp = 56, b""
    for index in range(3):
        offset, size = aset[2 + index * 2:4 + index * 2]
        require(size and offset >= end and offset + size <= len(binary) - code, "ASET resource outside bounds or overlapping")
        end = offset + size
        if index == 1:
            require(size == 0x4000, "Unexpected NACP size")
            nacp = binary[code + offset:code + offset + size]
    def text(data):
        require(b"\0" in data, "Unterminated NACP field")
        return data.split(b"\0", 1)[0].decode("utf-8")
    for language in range(16):
        title = text(nacp[language * 0x300:language * 0x300 + 0x200])
        author = text(nacp[language * 0x300 + 0x200:(language + 1) * 0x300])
        require((title, author) in (("NinMagic Saves", "NinMagic"), ("", "")) and (language or title), "Wrong application title or author")
    version = text(nacp[0x3060:0x3070])
    semver(version)
    return version


def prepare(nro: Path, base_url: str, output: Path) -> dict:
    require(nro.is_file() and not nro.is_symlink(), "NRO must be a regular file")
    binary = nro.read_bytes()
    version = inspect_nro(binary)
    digest = hashlib.sha256(binary).hexdigest()
    base_url = https(base_url.rstrip("/"))
    relative = f"versions/v{version}/NinMagic-Saves.nro"
    url, manifest_url = https(base_url + "/" + relative), https(base_url + "/update.txt")
    target = output / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        require(target.is_file() and not target.is_symlink() and hashlib.sha256(target.read_bytes()).hexdigest() == digest, "This published version already has a different binary; increment NACP version")
    else:
        with target.open("xb") as stream:
            stream.write(binary)
    require(target.read_bytes() == binary, "Versioned NRO copy mismatch")
    manifest = f"NINMAGIC-SAVES-UPDATE-1\nversion={version}\nurl={url}\nsha256={digest}\nsize={len(binary)}\n"
    previous = output / "update.txt"
    if previous.exists():
        require(previous.is_file() and not previous.is_symlink(), "Manifest path is not a regular file")
        old = dict(line.split("=", 1) for line in previous.read_text("ascii").splitlines()[1:])
        require(semver(old["version"]) <= semver(version), "Refusing to publish an older version")
        require(old["version"] != version or old["sha256"] == digest, "Same version already references another binary")
    pending = output / "update.pending"
    pending.write_text(manifest, encoding="ascii", newline="\n")
    pending.replace(previous)
    config = output / "device-config/config/NinMagicSaves/update-url.txt"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text(manifest_url + "\n", encoding="ascii", newline="\n")
    report = {"version": version, "sha256": digest, "bytes": len(binary), "manifest_url": manifest_url, "nro_url": url, "files_to_publish": [relative, "update.txt"], "device_configuration": str(config), "uploaded": False}
    (output / "publication-verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nro", type=Path)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.nro, args.base_url, args.output), indent=2))


if __name__ == "__main__":
    main()
