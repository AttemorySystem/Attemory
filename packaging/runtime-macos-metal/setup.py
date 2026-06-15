from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from runtime_setup import setup_runtime_package


setup_runtime_package(
    name="attemory-runtime-macos-metal",
    module="attemory_runtime_macos_metal",
    description="macOS Metal native runtime for attemory",
    macos_metal=True,
)
