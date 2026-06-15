from __future__ import annotations

from importlib.resources import files
from pathlib import Path


def runtime_info() -> dict[str, str]:
    root = files("attemory_runtime_macos_metal")
    return {
        "name": "macos-metal",
        "server_binary": str(Path(str(root / "bin" / "attemory_server"))),
        "lib_dir": str(Path(str(root / "lib"))),
        "metadata_dir": str(Path(str(root / "metadata"))),
    }
