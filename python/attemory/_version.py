from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version
from pathlib import Path


def _load_source_tree_version() -> str | None:
    version_file = Path(__file__).resolve().parents[2] / "VERSION"
    try:
        value = version_file.read_text(encoding="utf-8").splitlines()[0].strip()
    except (IndexError, OSError):
        return None
    return value or None


def _load_version() -> str:
    try:
        return version("attemory")
    except PackageNotFoundError:
        return _load_source_tree_version() or "0.0.0+unknown"


__version__ = _load_version()
