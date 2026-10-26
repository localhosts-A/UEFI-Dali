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
RESET_METHODS = ("off", "psci-cold", "psci-warm", "aee")
XNU_UFS_RESOURCE_WORDS = (0x31534655494c4144, 1, 0x16810000, 0x2A00, 0x168A0000, 0x1000,
                          0x16800000, 0x10000, 0x10000000, 0x800, 0, 0x1000000000,
                          1, 7, 14, 7)


def xnu_ufs_resources(adt, live_fdt):
    """Return the ADT resources-v1 bytes after binding them to the saved live FDT."""
    if not isinstance(adt, bytes) or not 0 < len(adt) <= 4 * 1024 * 1024:
        raise ValueError("Apple DT must be nonempty and bounded")
    if not isinstance(live_fdt, bytes) or not 40 <= len(live_fdt) <= 1024 * 1024:
        raise ValueError("live FDT must be nonempty and bounded")
    nodes = {}
    cursor = 0

    def visit(parent, depth):
        nonlocal cursor
        if depth > 32 or cursor + 8 > len(adt) or len(nodes) >= 8192:
            raise ValueError("Apple DT tree exceeds its bound")
        property_count, child_count = struct.unpack_from("<II", adt, cursor)
        cursor += 8
        if property_count > 256 or child_count > 256:
            raise ValueError("Apple DT node exceeds its bound")
        properties = {}
        for _ in range(property_count):
            if cursor + 36 > len(adt):
                raise ValueError("truncated Apple DT property")
            raw_key, size = struct.unpack_from("<32sI", adt, cursor)
            cursor += 36
            key_bytes = raw_key.split(b"\0", 1)[0]
            if not key_bytes or size > len(adt) - cursor:
                raise ValueError("invalid Apple DT property bounds")
            key = key_bytes.decode("ascii")
            if key in properties:
                raise ValueError("duplicate Apple DT property")
            properties[key] = adt[cursor:cursor + size]
            padded = (size + 3) & ~3
            if padded > len(adt) - cursor:
                raise ValueError("truncated Apple DT property padding")
            cursor += padded
        name_data = properties.get("name", b"")
        if not name_data.endswith(b"\0") or name_data.count(b"\0") != 1:
            raise ValueError("invalid Apple DT node name")
        name = name_data[:-1].decode("ascii")
        if not name or "/" in name or name in (".", ".."):
            raise ValueError("invalid Apple DT node path component")
        path = (parent.rstrip("/") + "/" + name) if parent else "/" + name
        if path in nodes:
            raise ValueError("duplicate Apple DT node")
        nodes[path] = properties
        for _ in range(child_count):
            visit(path, depth + 1)

    visit("", 0)
    if cursor != len(adt):
        raise ValueError("trailing Apple DT bytes")
    path = "/device-tree/arm-io/dali-ufs"
    props = nodes.get(path)
    if props is None or props.get("compatible") != b"localhosts,dali-ufs\0":
        raise ValueError("Apple DT lacks the explicit Dali UFS provider node")
    record = props.get("resources-v1", b"")
    expected = struct.pack("<16Q", *XNU_UFS_RESOURCE_WORDS)
    if (len(record) != len(expected) + 32 or record[:len(expected)] != expected or
            record[-32:] != hashlib.sha256(live_fdt).digest()):
        raise ValueError("Apple DT UFS resources differ from the pinned live FDT contract")
    return record


def validate_xnu_ufs_readonly(config, adt, live_fdt, token, auto_fastboot, reset_method, has_ram_root):
    if (not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{32}", token) or auto_fastboot or
            reset_method is not None or has_ram_root):
        raise ValueError("XNU UFS read-only needs a fresh token, manual recovery, no reset method and no RAM root")
    entries = [entry for entry in config.get("entry", [])
               if entry.get("type") == "xnu" and entry.get("path") == "Payloads/kernel.macho"]
    if len(entries) != 1 or entries[0].get("initrd"):
        raise ValueError("XNU UFS read-only requires one source kernel without an initrd")
    words = entries[0].get("args", "").split()
    required = {"dali-earlyboot": "dali-earlyboot=1", "dali-auto-fastboot": "dali-auto-fastboot=0",
                "dali-ufs-stop-after-diagnostics": "dali-ufs-stop-after-diagnostics=1",
                "dali-ufs-readonly": "dali-ufs-readonly=1", "dali-run": "dali-run=" + token}
    actual = {key: [word for word in words if word.split("=", 1)[0] == key] for key in required}
    if any(actual[key] != [value] for key, value in required.items()):
        raise ValueError("source XNU boot arguments do not bind read-only UFS/manual recovery to this token")
    forbidden = ("dali-pid1", "dali-reset-method", "dali-reset-delay", "dali-ufs-scratch-write")
    if any(word.split("=", 1)[0] in forbidden for word in words):
        raise ValueError("XNU UFS read-only cannot enable PID1, reset or scratch writes")
    return xnu_ufs_resources(adt, live_fdt)


def validate_xnu_ufs_bundle(payload, token, adt, live_fdt, config_bytes, recovery_bytes, kernel_bytes):
    """Validate the actual FAT members against the combined read-only profile."""
    if not isinstance(payload, dict) or not isinstance(payload.get("files"), dict):
        raise ValueError("XNU UFS payload manifest has no staged-file inventory")
    expected_members = {
        "EFI/UniXNU/unixnu.toml": config_bytes,
        "EFI/UniXNU/dali-boot.conf": recovery_bytes,
        "EFI/UniXNU/Payloads/kernel.macho": kernel_bytes,
        "EFI/UniXNU/Payloads/kernel.macho.adt": adt,
    }
    for name, data in expected_members.items():
        item = payload["files"].get(name)
        if (not isinstance(data, bytes) or not isinstance(item, dict) or
                item.get("bytes") != len(data) or
                item.get("sha256") != hashlib.sha256(data).hexdigest()):
            raise ValueError("actual XNU UFS payload member differs from its manifest: " + name)
    if (payload.get("xnu_kernel_sha256") != hashlib.sha256(kernel_bytes).hexdigest() or
            payload.get("xnu_adt_sha256") != hashlib.sha256(adt).hexdigest() or
            payload.get("xnu_ramdisk_sha256") is not None or
            payload.get("xnu_trust_cache_sha256") is not None or
            payload.get("physical_storage") is not False or payload.get("readonly") is not True or
            payload.get("runtime_trust_cache_provider") is not False):
        raise ValueError("XNU UFS payload is not the source-only read-only bundle")
    try:
        config = tomllib.loads(config_bytes.decode("utf-8"))
    except (UnicodeError, tomllib.TOMLDecodeError) as error:
        raise ValueError("actual XNU UFS boot arguments are invalid: " + str(error)) from None
    resources = validate_xnu_ufs_readonly(config, adt, live_fdt, token, False, None, False)
    profile = payload.get("xnu_ufs_read_only")
    fdt_sha256 = hashlib.sha256(live_fdt).hexdigest()
    if (not isinstance(profile, dict) or profile.get("enabled") is not True or
            profile.get("run_token") != token or profile.get("write_authorized") is not False or
            profile.get("physical_ufs_qualified") is not False or
            profile.get("resources_sha256") != hashlib.sha256(resources).hexdigest() or
            profile.get("resources_fdt_sha256") != fdt_sha256 or
            profile.get("live_fdt_sha256") != fdt_sha256):
        raise ValueError("XNU UFS resource record does not bind the actual ADT and live FDT")
    recovery = payload.get("recovery")
    if (not isinstance(recovery, dict) or set(recovery) !=
            {"auto_fastboot", "watchdog_timeout_seconds", "run_token"} or
            recovery.get("auto_fastboot") is not False or recovery.get("run_token") != token or
            type(recovery.get("watchdog_timeout_seconds")) is not int or
            not 2 <= recovery["watchdog_timeout_seconds"] <= 31):
        raise ValueError("actual Dali recovery config does not bind this token and manual no-reset policy")
    expected_recovery = (
        "version = 1\n"
        "auto_fastboot = false\n"
        f"watchdog_timeout_seconds = {recovery['watchdog_timeout_seconds']}\n"
        f"run_token = {token}\n"
    ).encode("ascii")
    if recovery_bytes != expected_recovery:
        raise ValueError("actual Dali recovery config differs from the exact manual no-reset policy")
    return resources


def qualification_config(config_text, method, token, auto_fastboot, has_ram_root=False):
    if method not in RESET_METHODS or auto_fastboot or has_ram_root or not token or \
            not re.fullmatch(r"[0-9a-f]{32}", token):
        raise ValueError("reset qualification needs a valid method, fresh token, no RAM root and disabled legacy recovery")
    config_text = config_with_xnu_argument(config_text, "dali-reset-method", method)
    return config_with_xnu_argument(config_text, "dali-reset-delay", "5")


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
    return config_with_xnu_argument(config_text, "dali-run", token)


def config_with_xnu_argument(config_text, name, value):
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
        lambda match: match.group(1) + match.group(2) + " " + name + "=" + value + match.group(3),
        block,
        count=1,
    )
    if count != 1 or name + "=" in block:
        raise ValueError("could not safely add a fresh run token/recovery policy to the XNU boot arguments")
    blocks[index] = replaced
    return "[[entry]]".join(blocks)


def validate_ram_root(config, ramdisk, trust, adt, auto_fastboot):
    """Require the explicit RAM-root handoff, without claiming runtime trust."""
    entries = [entry for entry in config.get("entry", []) if entry.get("type") == "xnu"]
    if len(entries) != 1 or entries[0].get("initrd") != "Payloads/root.img":
        raise ValueError("RAM-root config must have one XNU entry with initrd Payloads/root.img")
    words = entries[0].get("args", "").split()
    if words.count("rd=md0") != 1 or words.count("dali-pid1=1") != 1 or auto_fastboot:
        raise ValueError("PID1 requires rd=md0, dali-pid1=1 and a disabled firmware recovery watchdog")
    if len(ramdisk) < 32 or len(ramdisk) > 32 * 1024 * 1024 or \
            struct.unpack_from("<4I", ramdisk)[::3] != (0xFEEDFACF, 2) or \
            struct.unpack_from("<I", ramdisk, 4)[0] != 0x0100000C:
        raise ValueError("the minimal RAM root must be a bounded own arm64 MH_EXECUTE")
    # This profile stages exactly one own v2 module; DarwinBoot validates it again.
    if len(trust) != 56 or struct.unpack_from("<3I", trust) != (1, 8, 2) or \
            struct.unpack_from("<I", trust, 28)[0] != 1 or trust[52:] != bytes((2, 0, 0, 0)):
        raise ValueError("PID1 needs one complete SHA256 v2 test trust-cache entry")
    found = {}
    def node(cursor, parent):
        if cursor + 8 > len(adt):
            raise ValueError("truncated Apple DT node")
        properties, children = struct.unpack_from("<II", adt, cursor)
        cursor += 8
        if properties > 256 or children > 256:
            raise ValueError("unbounded Apple DT node")
        values = {}
        for _ in range(properties):
            if cursor + 36 > len(adt):
                raise ValueError("truncated Apple DT property")
            key, length = struct.unpack_from("<32sI", adt, cursor)
            cursor += 36
            if length > len(adt) - cursor:
                raise ValueError("Apple DT property outside input")
            values[key.rstrip(b"\0")] = adt[cursor:cursor + length]
            cursor += (length + 3) & ~3
        name = values.get(b"name", b"").rstrip(b"\0").decode("ascii")
        path = parent + "/" + name
        if path == "/device-tree/chosen/memory-map":
            found.update(values)
        for _ in range(children):
            cursor = node(cursor, path)
        return cursor
    if node(0, "") != len(adt) or any(found.get(name) != bytes(16) for name in (b"RAMDisk", b"TrustCache")):
        raise ValueError("RAM-root Apple DT must contain zero 16-byte RAMDisk and TrustCache handoff slots")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--efi-dir", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--xnu-kernel", type=Path)
    parser.add_argument("--xnu-adt", type=Path)
    parser.add_argument("--xnu-ramdisk", type=Path, help="owned minimal Mach-O RAM root")
    parser.add_argument("--xnu-trust-cache", type=Path, help="explicit own one-entry test bundle")
    parser.add_argument("--run-token", help="unique 32-hex token for one XNU hardware cycle")
    parser.add_argument("--xnu-ufs-readonly", action="store_true",
                        help="bind source XNU physical UFS read-only to the live FDT and fresh run token")
    parser.add_argument("--xnu-ufs-live-fdt", type=Path,
                        help="verified raw runtime FDT used to generate the UFS ADT resource node")
    parser.add_argument("--size-mib", type=int)
    parser.add_argument("--auto-fastboot", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--reset-method", choices=RESET_METHODS,
                        help="opt-in reset qualification: no competing firmware watchdogs, 5-second post-checkpoint delay")
    parser.add_argument("--watchdog-timeout-seconds", type=int, default=31,
                        help="MT6991 RGU single-shot timeout (2..31 seconds)")
    args = parser.parse_args()
    if bool(args.xnu_kernel) != bool(args.xnu_adt):
        parser.error("--xnu-kernel and --xnu-adt must be supplied together")
    if bool(args.xnu_ramdisk) != bool(args.xnu_trust_cache) or (args.xnu_ramdisk and not args.xnu_kernel):
        parser.error("RAM root and test trust cache must be supplied together with a kernel and ADT")
    if args.xnu_ramdisk and not args.run_token:
        parser.error("a RAM-root hardware image requires a fresh run token")
    if args.run_token and not re.fullmatch(r"[0-9a-f]{32}", args.run_token):
        parser.error("--run-token must be 32 lowercase hexadecimal characters")
    if args.run_token and not args.xnu_kernel:
        parser.error("--run-token requires --xnu-kernel")
    if args.reset_method is not None and (not args.xnu_kernel or not args.run_token):
        parser.error("--reset-method requires a source kernel/ADT and fresh token")
    if args.xnu_ufs_readonly and (not args.xnu_kernel or not args.run_token or not args.xnu_ufs_live_fdt or
                                  args.auto_fastboot or args.reset_method is not None or args.xnu_ramdisk):
        parser.error("XNU UFS read-only requires source XNU/ADT, matching live FDT, fresh token, manual recovery, no reset or RAM root")
    if args.xnu_ufs_live_fdt and not args.xnu_ufs_readonly:
        parser.error("--xnu-ufs-live-fdt is only valid for the explicit XNU UFS read-only profile")
    if not 2 <= args.watchdog_timeout_seconds <= 31:
        parser.error("the MT6991 RGU supports a verified single-shot timeout of 2..31 seconds")
    config = tomllib.loads(args.config.read_text())
    has_xnu_entry = any(entry.get("type") == "xnu" and entry.get("path") == "Payloads/kernel.macho"
                        for entry in config.get("entry", []))
    if bool(args.xnu_kernel) != has_xnu_entry:
        parser.error("the payload config must contain exactly the XNU entry matching the supplied kernel")
    config_text = args.config.read_text()
    if has_xnu_entry:
        try:
            config_text = config_with_xnu_argument(config_text, "dali-auto-fastboot", "1" if args.auto_fastboot else "0")
            if args.xnu_ufs_readonly:
                config_text = config_with_xnu_argument(config_text, "dali-ufs-readonly", "1")
                config_text = config_with_xnu_argument(config_text, "dali-ufs-stop-after-diagnostics", "1")
            if args.reset_method is not None:
                config_text = qualification_config(config_text, args.reset_method, args.run_token,
                                                   args.auto_fastboot, args.xnu_ramdisk is not None)
        except (ValueError, tomllib.TOMLDecodeError) as error:
            parser.error(str(error))
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
    live_fdt_bytes = None
    xnu_ufs_record = None
    ramdisk_bytes = None
    trust_bytes = None
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
        if args.xnu_ufs_readonly:
            live_fdt_bytes = args.xnu_ufs_live_fdt.read_bytes()
            try:
                xnu_ufs_record = validate_xnu_ufs_readonly(
                    tomllib.loads(config_text), adt_bytes, live_fdt_bytes, args.run_token,
                    args.auto_fastboot, args.reset_method, args.xnu_ramdisk is not None)
            except (ValueError, UnicodeError, struct.error) as error:
                parser.error(str(error))
    if args.xnu_ramdisk:
        ramdisk_bytes = args.xnu_ramdisk.read_bytes()
        trust_bytes = args.xnu_trust_cache.read_bytes()
        try:
            validate_ram_root(config, ramdisk_bytes, trust_bytes, adt_bytes, args.auto_fastboot)
        except (ValueError, UnicodeError, struct.error) as error:
            parser.error(str(error))
    elif any(entry.get("initrd") for entry in config.get("entry", [])):
        parser.error("the payload config requests an initrd but no RAM root was supplied")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dali-fat-") as temp:
        stage = Path(temp) / "stage"
        base = stage / "EFI/UniXNU"
        base.mkdir(parents=True)
        (base / "unixnu.toml").write_text(config_text)
        for name, folder in (("HelloPayload", "Payloads"), ("SampleDriver", "Drivers")):
            (base / folder).mkdir(exist_ok=True)
            shutil.copy2(args.efi_dir / (name + ".efi"), base / folder / (name + ".efi"))
        recovery = (f"version = {2 if args.reset_method is not None else 1}\n"
                    f"auto_fastboot = {str(args.auto_fastboot).lower()}\n"
                    f"watchdog_timeout_seconds = {args.watchdog_timeout_seconds}\n"
                    f"run_token = {args.run_token or 'none'}\n")
        if args.reset_method is not None:
            recovery += f"reset_method = {args.reset_method}\nreset_delay_seconds = 5\n"
        (base / "dali-boot.conf").write_text(recovery)
        if kernel_bytes is not None:
            (base / "Payloads/kernel.macho").write_bytes(kernel_bytes)
            (base / "Payloads/kernel.macho.adt").write_bytes(adt_bytes)
        if ramdisk_bytes is not None:
            (base / "Payloads/root.img").write_bytes(ramdisk_bytes)
            (base / "Payloads/kernel.macho.trustcache").write_bytes(trust_bytes)
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
              "xnu_ramdisk_sha256": hashlib.sha256(ramdisk_bytes).hexdigest() if ramdisk_bytes is not None else None,
              "xnu_trust_cache_sha256": hashlib.sha256(trust_bytes).hexdigest() if trust_bytes is not None else None,
              "runtime_trust_cache_provider": False,
              "xnu_run_token": args.run_token}
    if xnu_ufs_record is not None:
        record["xnu_ufs_read_only"] = {
            "enabled": True, "run_token": args.run_token, "write_authorized": False,
            "physical_ufs_qualified": False, "resources_sha256": hashlib.sha256(xnu_ufs_record).hexdigest(),
            "resources_fdt_sha256": xnu_ufs_record[-32:].hex(),
            "live_fdt_sha256": hashlib.sha256(live_fdt_bytes).hexdigest()}
    else:
        record["xnu_ufs_read_only"] = {"enabled": False, "write_authorized": False,
                                      "physical_ufs_qualified": False}
    if args.reset_method is not None:
        record["recovery"].update(reset_method=args.reset_method, reset_delay_seconds=5,
                                  watchdog_enabled=False, firmware_reset_deadline_enabled=False)
    args.output.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
