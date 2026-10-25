"""Materialize the pinned Mu_Basecore plus repository patches for source tests."""
from contextlib import contextmanager
from pathlib import Path
import subprocess
import tempfile

try:
    from verify_mu_patches import PATCH_NAMES
except ImportError:
    from .verify_mu_patches import PATCH_NAMES


@contextmanager
def patched_mu_source(repo_root):
    repo_root = Path(repo_root).resolve()
    mu = repo_root / "Mu_Basecore"
    with tempfile.TemporaryDirectory(prefix="dali-mu-source-") as temporary:
        checkout = Path(temporary) / "Mu_Basecore"
        subprocess.run(
            ["git", "-C", str(mu), "worktree", "add", "--detach", str(checkout), "HEAD"],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        try:
            for patch_name in PATCH_NAMES:
                subprocess.run(
                    ["git", "apply", str(repo_root / "Resources" / "MuPatches" / patch_name)],
                    cwd=checkout,
                    check=True,
                    stdout=subprocess.DEVNULL,
                )
            yield checkout
        finally:
            subprocess.run(
                ["git", "-C", str(mu), "worktree", "remove", "--force", str(checkout)],
                check=True,
                stdout=subprocess.DEVNULL,
            )
