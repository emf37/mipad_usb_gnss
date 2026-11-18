#!/usr/bin/env python3
"""Pack a KernelSU module directory into dist/<name>-v<ver>.zip.

Writes entries without path prefixes and sets exec bits (0755) on scripts and
the HAL binary, which Windows zip tools (bsdtar/Explorer) cannot do.

Usage: python tools/pack_module.py [module|module_st]
"""
import pathlib
import stat
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

# module dir argument -> zip basename
ZIP_NAMES = {
    "module": "gnss-usb",
    "module_st": "gnss-usb-st",
}

EXEC_NAMES = {
    "post-fs-data.sh",
    "service.sh",
    "uninstall.sh",
    "customize.sh",
    "action.sh",
    "activate.sh",
    "diagnose.sh",
    "gnss_usb_hal",
}


def main() -> int:
    module_key = sys.argv[1] if len(sys.argv) > 1 else "module"
    module = ROOT / module_key
    if module_key not in ZIP_NAMES or not module.is_dir():
        print(f"unknown/missing module dir: {module_key}", file=sys.stderr)
        return 1

    version = None
    for line in (module / "module.prop").read_text(encoding="utf-8").splitlines():
        if line.startswith("version="):
            version = line.split("=", 1)[1].strip()
            break
    if not version:
        print("module.prop: version= not found", file=sys.stderr)
        return 1

    out_dir = ROOT / "dist"
    out_dir.mkdir(exist_ok=True)
    out = out_dir / f"{ZIP_NAMES[module_key]}-v{version}.zip"
    if out.exists():
        out.unlink()

    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(module.rglob("*")):
            if path.is_dir():
                continue  # dirs are implied by file entries
            rel = path.relative_to(module).as_posix()
            mode = 0o755 if path.name in EXEC_NAMES else 0o644
            zi = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
            zi.external_attr = (stat.S_IFREG | mode) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(zi, path.read_bytes())

    print(f"packed {out} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
