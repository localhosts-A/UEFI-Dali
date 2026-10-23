#!/usr/bin/env python3
"""Run the real dali shim/SEC under QEMU and extract its RAM console.

This is a SEC/MMU smoke test only: QEMU does not emulate MT6991's GIC addresses.
No phone is accessed. A full diagnostic PASS still requires real hardware.
"""
import argparse
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dtb", type=Path, required=True)
    p.add_argument("--exception-level", type=int, choices=[1, 2], default=2)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = root / "out"
    with tempfile.TemporaryDirectory(prefix="dali-qemu-") as tmp:
        tmp = Path(tmp)
        entry = ".text\n.global _start\n_start:\n"
        if args.exception_level == 1:
            entry += ("mov x2, #0x80000000\nmsr hcr_el2, x2\nmov x2, #3\nmsr cnthctl_el2, x2\n"
                      "msr cntvoff_el2, xzr\nmsr cptr_el2, xzr\nmsr sctlr_el1, xzr\n"
                      "mov x2, #0x3c5\nmsr spsr_el2, x2\nadr x2, 1f\nmsr elr_el2, x2\neret\n1:\n")
        entry += "ldr x0, =0x88000000\nldr x1, =0x80080000\nbr x1\n.ltorg\n"
        (tmp / "entry.S").write_text(entry)
        subprocess.run(["clang", "--target=aarch64-none-elf", "-c", str(tmp / "entry.S"), "-o", str(tmp / "entry.o")], check=True)
        subprocess.run(["ld.lld", "-m", "aarch64elf", "--image-base=0", "-Ttext=0", "-e", "_start", str(tmp / "entry.o"), "-o", str(tmp / "entry.elf")], check=True)
        subprocess.run(["llvm-objcopy", "-O", "binary", str(tmp / "entry.elf"), str(tmp / "entry.bin")], check=True)
        qmp_path = tmp / "qmp"
        command = ["qemu-system-aarch64", "-M", "virt,virtualization=on,gic-version=3", "-cpu", "cortex-a76",
                   "-smp", "1", "-m", "4G", "-display", "none", "-serial", "none", "-monitor", "none",
                   "-qmp", f"unix:{qmp_path},server=on,wait=off", "-S",
                   "-device", f"loader,file={tmp / 'entry.bin'},addr=0x80000000,cpu-num=0,force-raw=on",
                   "-device", f"loader,file={out / 'Image'},addr=0x80080000,force-raw=on",
                   "-device", f"loader,file={args.dtb.resolve()},addr=0x88000000,force-raw=on"]
        with (out / "qemu-stderr.log").open("wb") as errors:
            process = subprocess.Popen(command, stdout=errors, stderr=errors)
            try:
                for _ in range(100):
                    if qmp_path.exists():
                        break
                    if process.poll() is not None:
                        raise RuntimeError("QEMU exited; see qemu-stderr.log")
                    time.sleep(0.05)
                with socket.socket(socket.AF_UNIX) as sock:
                    sock.settimeout(15)
                    sock.connect(str(qmp_path))
                    stream = sock.makefile("rwb")
                    stream.readline()

                    def qmp(name, **arguments):
                        stream.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
                        stream.flush()
                        while True:
                            response = json.loads(stream.readline())
                            if "return" in response:
                                return response["return"]
                            if "error" in response:
                                raise RuntimeError(response["error"])

                    qmp("qmp_capabilities")
                    qmp("cont")
                    time.sleep(3)
                    qmp("stop")
                    prefix = f"qemu-el{args.exception_level}"
                    (out / (prefix + "-registers.txt")).write_text(qmp("human-monitor-command", **{"command-line": "info registers"}))
                    memory = out / (prefix + "-console.bin")
                    response = qmp("human-monitor-command", **{"command-line": f'pmemsave 0xaec0f000 0x200000 "{memory}"'})
                    if response:
                        raise RuntimeError(response)
                    qmp("quit")
                raw = memory.read_bytes()
                signature, start, size = struct.unpack_from("<III", raw)
                if signature != 0x43474244 or size > len(raw) - 12 or start > size:
                    raise RuntimeError("No valid RAM console; inspect qemu-registers.txt")
                text = (raw[12 + start:12 + size] + raw[12:12 + start]).decode(errors="replace")
                (out / (prefix + "-console.log")).write_text(text)
                print("\n".join(line for line in text.splitlines() if "DALI:" in line))
                if (f"DALI: EL{args.exception_level} " not in text or
                        "DALI: SEC MMU and memory HOBs ready" not in text or
                        "DALI: entering DXE core" not in text):
                    raise SystemExit("SEC/MMU smoke did not reach DXE entry")
                print(f"PASS: EL{args.exception_level} shim → SEC → MMU/HOB → DXE entry (not MT6991 peripheral validation)")
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
