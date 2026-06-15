from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from runtime_setup import setup_runtime_package


setup_runtime_package(
    name="attemory-runtime-linux-cuda-cu132",
    module="attemory_runtime_linux_cuda_cu132",
    description="Linux CUDA 13.2 native runtime for attemory",
)
