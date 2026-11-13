"""Environment paths for Afterglow recompilation scripts."""
import os
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

def _load_env():
    p = REPO / "scripts" / ".env"
    if not p.is_file():
        return {}
    vars = {}
    with open(p) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                vars[k.strip()] = os.path.expanduser(v.strip())
    return vars

_ENV = _load_env()

def _path(key, default):
    return Path(os.environ.get(key, _ENV.get(key, default)))

WORK_DIR = _path("WORK_DIR", REPO / "work")
GAME_DIR = _path("GAME_DIR", REPO / "game")  # holds exefs/ and romfs/
LIFTER_KIT_DIR = _path("LIFTER_KIT_DIR", REPO / "references" / "lifter-kit")
