import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[5]
CLI_DIR = REPO_ROOT / "tools" / "cli"
ANDROID_DIR = REPO_ROOT / "platform" / "android"


def run_dir() -> Path:
    """Per-checkout runtime dir (pids, logs). Created on demand."""
    path = CLI_DIR / ".run"
    path.mkdir(parents=True, exist_ok=True)
    return path


def branch() -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "branch", "--show-current"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() or None
