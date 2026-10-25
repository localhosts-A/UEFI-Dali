#!/usr/bin/env python3
"""Accept only a clean Mu_Basecore tree or the exact repository-owned patch set."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


PATCH_NAMES = (
    "AsmMacroLib.patch",
    "Auth-Service.patch",
    "Boot-Manager.patch",
    "ExitBootServices-Trace.patch",
    "Timer.patch",
    "Usb-Bus.patch",
    "Xhci-Native-Safety.patch",
)


def git(root, *args, env=None):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True, env=env).strip()


def patch_paths(path):
    return set(re.findall(r"(?m)^diff --git a/(\S+) b/\S+$", path.read_text()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mu", required=True, type=Path)
    parser.add_argument("--patch-dir", required=True, type=Path)
    args = parser.parse_args()
    mu = args.mu.resolve()
    patch_dir = args.patch_dir.resolve()
    patches = [patch_dir / name for name in PATCH_NAMES]
    if not (mu / ".git").exists() or any(not patch.is_file() for patch in patches):
        raise SystemExit("pinned Mu_Basecore or required patch files are missing")

    expected_paths = set().union(*(patch_paths(patch) for patch in patches))
    if not expected_paths:
        raise SystemExit("the Mu_Basecore patch set has no file records")
    status = git(mu, "status", "--porcelain", "--untracked-files=normal", "--ignore-submodules=all")
    changed = set(git(mu, "diff", "--name-only", "HEAD", "--ignore-submodules=all").splitlines())
    untracked = [line for line in status.splitlines() if line.startswith("?? ")]
    if untracked:
        raise SystemExit(f"Mu_Basecore has untracked non-build files: {untracked}")
    if not changed:
        print("PASS: Mu_Basecore tracked worktree is clean")
        return
    if changed != expected_paths:
        raise SystemExit(f"Mu_Basecore has changes outside the known patch set: {sorted(changed ^ expected_paths)}")

    with tempfile.TemporaryDirectory(prefix="dali-mu-index-") as temporary:
        index_path = Path(temporary) / "index"
        env = dict(os.environ, GIT_INDEX_FILE=str(index_path))
        subprocess.run(["git", "-C", str(mu), "read-tree", "HEAD"], env=env, check=True)
        for patch in patches:
            subprocess.run(["git", "-C", str(mu), "apply", "--cached", str(patch)], env=env, check=True)
        mismatches = []
        for relative in sorted(expected_paths):
            expected_blob = subprocess.check_output(
                ["git", "-C", str(mu), "rev-parse", ":" + relative], text=True, env=env).strip()
            actual_blob = git(mu, "hash-object", relative)
            if actual_blob != expected_blob:
                mismatches.append(relative)
        if mismatches:
            raise SystemExit(f"Mu_Basecore does not exactly match the reviewed patch set: {mismatches}")
    print("PASS: Mu_Basecore contains exactly the pinned repository patch set")


if __name__ == "__main__":
    main()
