#!/usr/bin/env python3
"""Create and verify the small UEFI-only FAT test volume; no Apple inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--efi-dir", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dali-fat-") as temp:
        stage = Path(temp) / "stage"
        base = stage / "EFI/UniXNU"
        base.mkdir(parents=True)
        shutil.copy2(args.config, base / "unixnu.toml")
        for name, folder in (("HelloPayload", "Payloads"), ("SampleDriver", "Drivers")):
            (base / folder).mkdir(exist_ok=True)
            shutil.copy2(args.efi_dir / (name + ".efi"), base / folder / (name + ".efi"))
        image = args.output.resolve()
        image.write_bytes(bytes(0x100000))
        subprocess.run(["mformat", "-i", str(image), "-v", "DALIUEFI", "-N", "0x44414c49", "::"], check=True)
        subprocess.run(["mcopy", "-s", "-i", str(image), str(stage / "EFI"), "::/"], check=True)
        files = {}
        for file in sorted(stage.rglob("*")):
            if not file.is_file():
                continue
            relative = file.relative_to(stage).as_posix()
            data = file.read_bytes()
            actual = subprocess.check_output(["mtype", "-i", str(image), "::/" + relative])
            if data != actual:
                raise SystemExit("FAT readback differs: " + relative)
            files[relative] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    data = args.output.read_bytes()
    sectors = struct.unpack_from("<H", data, 19)[0] or struct.unpack_from("<I", data, 32)[0]
    if data[510:512] != b"\x55\xaa" or struct.unpack_from("<H", data, 11)[0] != 512 or sectors * 512 != len(data):
        raise SystemExit("Invalid FAT BPB")
    record = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "files": files,
              "physical_storage": False, "readonly": True}
    args.output.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
