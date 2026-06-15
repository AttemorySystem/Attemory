from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version


def _load_version() -> str:
    try:
        return version("attemory")
    except PackageNotFoundError:
        return "0.0.0+unknown"


__version__ = _load_version()
