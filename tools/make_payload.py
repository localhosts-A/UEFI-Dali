#!/usr/bin/env python3
"""Create and verify the read-only FAT payload embedded in the Dali firmware volume."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import tomllib


MAX_XNU_PAYLOAD_MIB = 24
MAX_FV_PAYLOAD_MIB = 48


def payload_volume_size_mib(requested, has_xnu):
    size_mib = requested if requested is not None else (MAX_XNU_PAYLOAD_MIB if has_xnu else 1)
    if not 1 <= size_mib <= MAX_FV_PAYLOAD_MIB:
        raise ValueError("the embedded FAT volume must fit within the 64 MiB Dali firmware volume")
    if has_xnu and size_mib > MAX_XNU_PAYLOAD_MIB:
        raise ValueError("a Dali XNU payload FAT volume must be at most 24 MiB; 48 MiB failed the DXE contiguous allocation")
    return size_mib


def config_with_run_token(config_text, token):
    if not re.fullmatch(r"[0-9a-f]{32}", token):
        raise ValueError("run token must be 32 lowercase hexadecimal characters")
    blocks = config_text.split("[[entry]]")
    matches = []
    for index, block in enumerate(blocks[1:], start=1):
        entry = tomllib.loads("[[entry]]" + block).get("entry", [{}])[0]
        if entry.get("type") == "xnu" and entry.get("path") == "Payloads/kernel.macho":
            matches.append(index)
    if len(matches) != 1:
        raise ValueError("run token requires exactly one Dali source XNU entry")
    index = matches[0]
    block = blocks[index]
    pattern = re.compile(r'(?m)^(args\s*=\s*")([^"]*)("\s*)$')
    replaced, count = pattern.subn(
        lambda match: match.group(1) + match.group(2) + " dali-run=" + token + match.group(3),
        block,
        count=1,
    )
    if count != 1 or "dali-run=" in block:
        raise ValueError("could not safely add a fresh run token to the XNU boot arguments")
    blocks[index] = replaced
    return "[[entry]]".join(blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--efi-dir", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--xnu-kernel", type=Path)
    parser.add_argument("--xnu-adt", type=Path)
    parser.add_argument("--run-token", help="unique 32-hex token for one XNU hardware cycle")
    parser.add_argument("--size-mib", type=int)
    parser.add_argument("--auto-fastboot", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--watchdog-timeout-seconds", type=int, default=31,
                        help="MT6991 RGU single-shot timeout (2..31 seconds)")
    args = parser.parse_args()
    if bool(args.xnu_kernel) != bool(args.xnu_adt):
        parser.error("--xnu-kernel and --xnu-adt must be supplied together")
    if args.run_token and not re.fullmatch(r"[0-9a-f]{32}", args.run_token):
        parser.error("--run-token must be 32 lowercase hexadecimal characters")
    if args.run_token and not args.xnu_kernel:
        parser.error("--run-token requires --xnu-kernel")
    if not 2 <= args.watchdog_timeout_seconds <= 31:
        parser.error("the MT6991 RGU supports a verified single-shot timeout of 2..31 seconds")
    config = tomllib.loads(args.config.read_text())
    has_xnu_entry = any(entry.get("type") == "xnu" and entry.get("path") == "Payloads/kernel.macho"
                        for entry in config.get("entry", []))
    if bool(args.xnu_kernel) != has_xnu_entry:
        parser.error("the payload config must contain exactly the XNU entry matching the supplied kernel")
    config_text = args.config.read_text()
    if args.run_token:
        try:
            config_text = config_with_run_token(config_text, args.run_token)
        except (ValueError, tomllib.TOMLDecodeError) as error:
            parser.error(str(error))
    try:
        size_mib = payload_volume_size_mib(args.size_mib, args.xnu_kernel is not None)
    except ValueError as error:
        parser.error(str(error))
    kernel_bytes = None
    adt_bytes = None
    if args.xnu_kernel:
        kernel_bytes = args.xnu_kernel.read_bytes()
        if len(kernel_bytes) < 32:
            parser.error("source-built XNU Mach-O is truncated")
        magic, cpu, _, filetype, commands, command_bytes, *_ = struct.unpack_from("<8I", kernel_bytes)
        if (magic, cpu, filetype) != (0xFEEDFACF, 0x0100000C, 2) or commands == 0 or 32 + command_bytes > len(kernel_bytes):
            parser.error("source-built XNU must be a complete arm64 MH_EXECUTE Mach-O")
        offset = 32
        for _ in range(commands):
            if offset + 8 > 32 + command_bytes:
                parser.error("source-built XNU has a truncated load command")
            _, command_size = struct.unpack_from("<II", kernel_bytes, offset)
            if command_size < 8 or offset + command_size > 32 + command_bytes:
                parser.error("source-built XNU has an invalid load command size")
            offset += command_size
        if offset != 32 + command_bytes:
            parser.error("source-built XNU load-command table size is inconsistent")
        adt_bytes = args.xnu_adt.read_bytes()
        if not adt_bytes:
            parser.error("Apple device tree is empty")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dali-fat-") as temp:
        stage = Path(temp) / "stage"
        base = stage / "EFI/UniXNU"
        base.mkdir(parents=True)
        (base / "unixnu.toml").write_text(config_text)
        for name, folder in (("HelloPayload", "Payloads"), ("SampleDriver", "Drivers")):
            (base / folder).mkdir(exist_ok=True)
            shutil.copy2(args.efi_dir / (name + ".efi"), base / folder / (name + ".efi"))
        recovery = ("version = 1\n"
                    f"auto_fastboot = {str(args.auto_fastboot).lower()}\n"
                    f"watchdog_timeout_seconds = {args.watchdog_timeout_seconds}\n"
                    f"run_token = {args.run_token or 'none'}\n")
        (base / "dali-boot.conf").write_text(recovery)
        if kernel_bytes is not None:
            (base / "Payloads/kernel.macho").write_bytes(kernel_bytes)
            (base / "Payloads/kernel.macho.adt").write_bytes(adt_bytes)
        image = args.output.resolve()
        image.write_bytes(bytes(size_mib * 1024 * 1024))
        subprocess.run(["mformat", "-i", str(image), "-v", "DALIUEFI", "-N", "0x44414c49",
                        "-T", str(size_mib * 2048), "::"], check=True)
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
    record = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "size_mib": size_mib,
              "files": files, "physical_storage": False, "readonly": True,
              "recovery": {"auto_fastboot": args.auto_fastboot,
                           "watchdog_timeout_seconds": args.watchdog_timeout_seconds,
                           "run_token": args.run_token or "none"},
              "xnu_kernel_sha256": hashlib.sha256(kernel_bytes).hexdigest() if kernel_bytes is not None else None,
              "xnu_adt_sha256": hashlib.sha256(adt_bytes).hexdigest() if adt_bytes is not None else None,
              "xnu_run_token": args.run_token}
    args.output.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
