#!/usr/bin/env python3
"""Package dali UniXNU UEFI using the verified boot-v4 / legacy-LZ4 format."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tomllib

from fdt import Fdt


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--backup", type=Path, required=True)
    p.add_argument("--mode", choices=["DEBUG"], default="DEBUG")
    p.add_argument("--mu", type=Path, required=True)
    p.add_argument("--unixnu", type=Path, required=True, help="reviewed generic UniXNU checkout")
    p.add_argument("--device-repo", type=Path, required=True, help="pinned UniXNU-Dali checkout")
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    mu = args.mu.resolve()
    unixnu = args.unixnu.resolve()
    device_repo = args.device_repo.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((args.backup / "manifest.json").read_text())
    board = tomllib.loads((device_repo / "board.toml").read_text())
    firmware = board.get("firmware", {})
    uefi_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    unixnu_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=unixnu, text=True).strip()
    if firmware.get("uefi_commit") != uefi_commit or firmware.get("unixnu_commit") != unixnu_commit:
        raise SystemExit("UEFI/UniXNU checkout differs from the device board.toml commit pins")
    if not manifest.get("complete") or manifest.get("product") != "dali":
        raise SystemExit("A complete verified dali backup is required")
    expected_ramoops = {"mem_address": 0xAEC00000, "mem_size": 0x410000, "record_size": 4096,
                        "console_size": 0x200000, "pmsg_size": 0x200000, "ftrace_size": 4096, "ecc": 0, "mem_type": 0}
    if manifest.get("ramoops") != expected_ramoops:
        raise SystemExit("Ramoops parameters differ or were not backed up: review the console layout before packaging")
    for name in ("boot_b.img", "runtime.dtb"):
        data = (args.backup / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != manifest["files"][name]["sha256"]:
            raise SystemExit("Backup checksum mismatch: " + name)
    tree = Fdt((args.backup / "runtime.dtb").read_bytes())
    layout = tree.validate()
    io_layout = tree.validate_io()
    boot = (args.backup / "boot_b.img").read_bytes()
    if boot[:8] != b"ANDROID!" or struct.unpack_from("<I", boot, 40)[0] != 4:
        raise SystemExit("Expected boot header v4")
    if boot[4096:4100] != bytes.fromhex("02214c18"):
        raise SystemExit("Expected the observed legacy LZ4 kernel compression")
    config = tomllib.loads((root / "Resources/Configs/dali.toml").read_text())
    fd_base = config["uefi_fd"]["base"]
    fd_size = config["uefi_fd"]["size"]
    if (fd_base, fd_size) != (0xC1000000, 0x4000000):
        raise SystemExit("Dali FD address/size differs from the reviewed 64 MiB arena layout")
    fd = mu / f"Build/daliPkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd"
    if fd.stat().st_size != fd_size:
        raise SystemExit("Unexpected dali FD size")
    shim_obj, shim_elf, shim = out / "DaliShim.o", out / "DaliShim.elf", out / "DaliShim.bin"
    subprocess.run(["clang", "--target=aarch64-none-elf", "-c", str(mu / "BootShim/DaliShim.S"),
                    "-o", str(shim_obj), f"-DFD_BASE={fd_base:#x}", f"-DFD_SIZE={fd_size:#x}"], check=True)
    subprocess.run(["ld.lld", "-m", "aarch64elf", "--image-base=0", "-Ttext=0", "-e", "_start",
                    str(shim_obj), "-o", str(shim_elf)], check=True)
    subprocess.run(["llvm-objcopy", "-O", "binary", str(shim_elf), str(shim)], check=True)
    if shim.stat().st_size != 4096:
        raise SystemExit("Unexpected shim size or unresolved literal pool")
    raw = shim.read_bytes() + fd.read_bytes()
    if raw[56:60] != b"ARM\x64" or struct.unpack_from("<Q", raw, 16)[0] != len(raw):
        raise SystemExit("Invalid ARM64 Image header")
    (out / "Image").write_bytes(raw)
    compressed = subprocess.check_output(["lz4", "-l", "-c"], input=raw)
    if subprocess.check_output(["lz4", "-d", "-c"], input=compressed) != raw:
        raise SystemExit("LZ4 round trip failed")
    # Keep the original boot-v4 metadata and command line; only replace the kernel.
    header = bytearray(boot[:4096])
    struct.pack_into("<I", header, 8, len(compressed))
    struct.pack_into("<I", header, 12, 0)  # no generic ramdisk; init_boot remains separate
    struct.pack_into("<I", header, 1580, 0)  # old GKI boot signature does not cover this image
    image = bytes(header) + compressed
    image += bytes((-len(image)) % 4096)
    limit = manifest["files"]["boot_b.img"]["bytes"]
    if len(image) > limit:
        raise SystemExit("Boot image exceeds test boot partition")
    target = out / "UniXNU-dali-uefi-boot.img"
    target.write_bytes(image)
    record = {"experimental": True, "target": "dali", "mode": args.mode,
              "image": str(target), "bytes": len(image), "sha256": hashlib.sha256(image).hexdigest(),
              "fd_base": fd_base, "fd_size": fd_size,
              "fd_sha256": hashlib.sha256(fd.read_bytes()).hexdigest(), "layout": layout, "io_layout": io_layout,
              "upstream_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=mu, text=True).strip(),
              "backup_manifest": str((args.backup / "manifest.json").resolve()),
              "unixnu_commit": unixnu_commit,
              "device_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=device_repo, text=True).strip(),
              "uefi_commit": uefi_commit,
              "mu_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=mu, text=True).strip(),
              "payload": json.loads((out / "payload.json").read_text())}
    (out / "manifest.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
