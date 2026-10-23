#!/usr/bin/env python3
"""Collect surviving dali UEFI ramoops evidence after returning to Android."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess


def parse_evidence(data):
    """Describe logged milestones; framebuffer readback is not visible output."""
    text = data.decode("utf-8", errors="replace")
    # Do not let an older successful boot supply milestones for a newer failure.
    start = text.rfind("DALI: SEC diagnostics v1")
    if start >= 0:
        text = text[start:]
    keys = [{"number": int(n), "scan": int(s, 16), "unicode": int(u, 16)}
            for n, s, u in re.findall(r"DALI: INPUT key=(\d+) scan=([\da-fA-F]+) unicode=([\da-fA-F]+)", text)]
    failures = [line for line in text.splitlines()
                if "ASSERT" in line or "UEFI_DIAGNOSTIC_FAIL" in line or
                "UEFI_STAGE_FAIL" in line or "Exception at" in line or
                ("UNIXNU: driver " in line and ("load failed:" in line or "start failed:" in line))]
    return {
        "diagnostic_pass": "DALI: UEFI_DIAGNOSTIC_PASS" in text,
        "stages": {stage: marker in text for stage, marker in {
            "shim": "DALI: UEFI relocation shim entered", "sec": "DALI: SEC diagnostics v1",
            "mmu": "DALI: SEC MMU and memory HOBs ready", "dxe": "DALI: diagnostic DXE loaded",
            "bds": "DALI: BDS entered"}.items()},
        "io": {
            "gop_published": "DALI: GOP status=Success" in text,
            "text_output_connected": "DALI: text output status=Success" in text,
            "gop_cpu_readback_pass": "DALI: GOP_BLT_PASS" in text,
            "text_input_connected": "DALI: text input status=Success" in text,
            "input_events": keys,
            "keypad_events": [{"scan": int(s, 16), "unicode": int(u, 16)} for s, u in
                              re.findall(r"DALI: keypad event scan=([\da-fA-F]+) unicode=([\da-fA-F]+)", text)],
            "event_wait_pass": "DALI: WaitForEvent timer status=Success" in text,
            "inherited_watchdog_disabled": bool(re.search(r"DALI: inherited RGU WDT [^\r\n]* disabled=1", text)),
            "pmic_samples": [{"raw": int(raw, 16), "time_ms": int(ms)} for raw, ms in
                             re.findall(r"DALI: MT6363 TOPSTATUS=([\da-fA-F]+) time-ms=(\d+)", text)],
            "app_heartbeats": [{"number": int(n), "time_ms": int(ms)} for n, ms in
                               re.findall(r"DALI: app heartbeat=(\d+) time-ms=(\d+)", text)],
            "frame_submission_enabled": "DALI: frame submission enabled;" in text,
            "frame_submissions": [int(n) for n in re.findall(r"DALI: frame submit=(\d+)", text)],
            "frame_completion_samples": re.findall(r"DALI: frame completion sample[^\r\n]*", text),
            "display_visibility": "unverified",
        },
        "failures": failures,
        "unixnu": {
            "payload_filesystem_pass": "DALI: payload filesystem=PASS" in text,
            "firmware_ready": "DALI: UEFI_UNIXNU_READY" in text,
            "started": "UNIXNU: UniXNU " in text,
            "config_loaded": "UNIXNU: config: loaded" in text,
            "menu_shown": "UNIXNU: menu: shown" in text,
            "sample_driver_started": bool(re.search(r"UNIXNU: driver [^\r\n]*SampleDriver\.efi \(config\): started", text)),
            "hello_returned_success": bool(re.search(r"UNIXNU: boot: [^\r\n]*HelloPayload\.efi returned Success", text)),
            "exit_action": "UNIXNU: action: exit" in text,
            "returned_to_firmware": "DALI: UniXNU returned status=Aborted" in text,
        },
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--serial", required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--require-unixnu", action="store_true")
    args = p.parse_args()

    def read(command):
        return subprocess.check_output(["adb", "-s", args.serial, "exec-out",
                                        "su -c " + shlex.quote(command)], timeout=30)

    if read("getprop ro.product.device").strip() != b"dali":
        raise SystemExit("Expected dali")
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    result = {"slot": read("getprop ro.boot.slot_suffix").decode().strip(), "files": [], "diagnostic_pass": False}
    filenames = read("ls /sys/fs/pstore").decode().splitlines()
    combined = b""
    for name in filenames:
        if "/" in name or not name.startswith(("console-ramoops", "dmesg-ramoops", "pmsg-ramoops")):
            continue
        data = read("cat " + shlex.quote("/sys/fs/pstore/" + name))
        (args.output / name).write_bytes(data)
        result["files"].append(name)
        combined += data
    (args.output / "kernel-dmesg.txt").write_bytes(read("dmesg"))
    (args.output / "bootconfig.txt").write_bytes(read("cat /proc/bootconfig"))
    result.update(parse_evidence(combined))
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if not result["diagnostic_pass"]:
        raise SystemExit("No hardware PASS evidence collected; missing logs do not prove UEFI did not execute")
    if result["failures"]:
        raise SystemExit("Core PASS exists, but later failures require inspection")
    if args.require_unixnu and not all(result["unixnu"].values()):
        raise SystemExit("UniXNU UEFI stage is incomplete; inspect the separate milestones")


if __name__ == "__main__":
    main()
