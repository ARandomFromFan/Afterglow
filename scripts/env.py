"""Local paths: scripts/.env (KEY=VALUE, see .env.example), then the environment, then repo-relative defaults."""
import os
import pathlib

REPO = pathlib.Path(__file__).resolve().parent.parent

_env_file = REPO / "scripts" / ".env"
if _env_file.exists():
    for _line in _env_file.read_text().splitlines():
        _line = _line.strip()
        if _line and not _line.startswith("#") and "=" in _line:
            _k, _v = _line.split("=", 1)
            os.environ.setdefault(_k.strip(), os.path.expanduser(_v.strip().strip('"\'')))


def _path(key, default):
    return os.path.expanduser(os.environ.get(key) or str(default))


WORK_DIR = _path("WORK_DIR", REPO / "work")
GAME_DIR = _path("GAME_DIR", REPO / "game")  # holds exefs/ and romfs/
# sys.path entries for the lifter: ours first, then the vendored a64-kit
LIFTER_PATHS = [str(REPO / d) for d in ("lifter", "third_party/a64-kit/lift", "third_party/a64-kit/hos", "third_party/a64-kit/tools")]
