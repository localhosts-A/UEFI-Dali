#!/usr/bin/env python3
"""Read-only, verified host backups for dali bring-up. Never writes to the phone."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
from datetime import datetime, timezone


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    adb = ["adb", "-s", args.serial, "exec-out"]

    def read(command, root=True):
        remote = "su -c " + shlex.quote(command) if root else command
        return subprocess.check_output(adb + [remote], timeout=120)

    product = read("getprop ro.product.device", False).decode().strip()
    if product != "dali" or b"uid=0(" not in read("id"):
        raise SystemExit("Expected a rooted dali; refusing to back up another target")
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    os.chmod(args.output, 0o700)
    manifest = {
        "schema": 1, "serial": args.serial, "product": product,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "slot_suffix": read("getprop ro.boot.slot_suffix", False).decode().strip(),
        "fingerprint": read("getprop ro.build.fingerprint", False).decode().strip(),
        "files": {},
    }

    def save_manifest():
        (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    save_manifest()
    names = [name + "_" + slot for name in (
        "boot", "init_boot", "vendor_boot", "dtbo", "vbmeta", "vbmeta_system",
        "vbmeta_vendor", "lk",
    ) for slot in "ab"] + ["misc", "boot_para"]
    for name in names:
        block = "/dev/block/by-name/" + name
        size = int(read("blockdev --getsize64 " + shlex.quote(block)))
        before = read("sha256sum " + shlex.quote(block)).decode().split()[0]
        target = args.output / (name + ".img")
        partial = target.with_suffix(".partial")
        remote = "su -c " + shlex.quote("cat " + shlex.quote(block))
        with partial.open("xb") as output:
            os.chmod(partial, 0o600)
            subprocess.run(adb + [remote], stdout=output, check=True, timeout=180)
            output.flush()
            os.fsync(output.fileno())
        with partial.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        after = read("sha256sum " + shlex.quote(block)).decode().split()[0]
        if partial.stat().st_size != size or digest != before or digest != after:
            raise SystemExit("Backup changed or verification failed: " + name)
        partial.rename(target)
        manifest["files"][target.name] = {"bytes": size, "sha256": digest, "source": block}
        save_manifest()
        print(f"verified {name}: {size} bytes {digest}", flush=True)
    for source, filename in (
        ("/sys/firmware/fdt", "runtime.dtb"),
        ("/proc/bootconfig", "bootconfig.txt"),
        ("/proc/cmdline", "cmdline.txt"),
        ("/proc/config.gz", "kernel-config.gz"),
    ):
        data = read("cat " + shlex.quote(source))
        target = args.output / filename
        target.write_bytes(data)
        os.chmod(target, 0o600)
        manifest["files"][filename] = {
            "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "source": source,
        }
        save_manifest()
    manifest["complete"] = True
    keys = ("mem_address", "mem_size", "record_size", "console_size", "pmsg_size", "ftrace_size", "ecc", "mem_type")
    manifest["ramoops"] = {key: int(read("cat /sys/module/ramoops/parameters/" + key).strip(), 0) for key in keys}
    save_manifest()
    print("Backup complete: " + str(args.output.resolve()))


if __name__ == "__main__":
    main()
