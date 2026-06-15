from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from runtime_setup import setup_runtime_package


setup_runtime_package(
    name="attemory-runtime-linux-cpu",
    module="attemory_runtime_linux_cpu",
    description="Linux CPU native runtime for attemory",
)
