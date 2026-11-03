"""Package the Saves NRO. Never includes Atmosphere components or private data."""
from pathlib import Path
import hashlib
import json
import zipfile
from publish_update import prepare
from verify_nro import verify, VERSION

ROOT = Path(__file__).resolve().parent.parent
BASE = "https://raw.githubusercontent.com/ANE-JV/ninmagic-saves/main"

def main():
    dist = ROOT / "dist"
    binary = dist / "NinMagic-Saves.nro"
    report = verify(binary)
    publication = dist / "publication"
    prepared = prepare(binary, BASE, publication)
    payload = {
        "switch/NinMagicSaves/NinMagic-Saves.nro": binary.read_bytes(),
        "switch/NinMagicSaves/LEEME.txt": (ROOT / "README.md").read_bytes(),
        "switch/NinMagicSaves/licencias/GPL-2.0.txt": (ROOT / "LICENSE").read_bytes(),
        "switch/NinMagicSaves/licencias/REBOOT-NOTICE.txt": (ROOT / "third_party/REBOOT-NOTICE.txt").read_bytes(),
        "switch/NinMagicSaves/licencias/FONT-LICENSE.txt": (ROOT / "romfs/FONT-LICENSE.txt").read_bytes(),
        "config/NinMagicSaves/update-url.txt": (BASE + "/update.txt\n").encode("ascii"),
    }
    for notice in (ROOT / "third_party").glob("*-LICENSE.txt"):
        payload["switch/NinMagicSaves/licencias/" + notice.name] = notice.read_bytes()
    package = publication / f"versions/v{VERSION}/NinMagic-Saves-{VERSION}.zip"
    if not package.exists():
        with zipfile.ZipFile(package, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(payload.items()):
                archive.writestr(name, data)
    with zipfile.ZipFile(package) as archive:
        if archive.testzip() or len(archive.namelist()) != len(payload) or set(archive.namelist()) != set(payload):
            raise ValueError("Package inventory or CRC mismatch")
        for name, data in payload.items():
            if archive.read(name) != data:
                raise ValueError("Published package already differs; increment version: " + name)
    report.update({"package": package.name, "package_sha256": hashlib.sha256(package.read_bytes()).hexdigest(), "entries": sorted(payload), "manifest": prepared["manifest_url"], "public_channel_verified": False, "console_installed": False})
    (dist / "validation.json").write_text(json.dumps(report,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    print(json.dumps(report,indent=2,ensure_ascii=False))

if __name__ == "__main__":
    main()
