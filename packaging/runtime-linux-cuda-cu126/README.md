# attemory-runtime-linux-cuda-cu126

Native Linux CUDA 12.6 runtime package for `attemory`.

Expected layout:

```text
attemory_runtime_linux_cuda_cu126/
  bin/attemory_server
  lib/libattemory-core.so
  lib/libllama.so
  lib/libggml*.so
  metadata/build-info.json
```

This package is only a packaging boundary. Build scripts or CI should populate
`bin/` and `lib/` before building the wheel.

CUDA toolkit runtime libraries may be bundled by `auditwheel`. NVIDIA driver
libraries such as `libcuda.so` and `libnvidia-ml.so` must stay external and come
from the user's installed driver.

Collector example:

```bash
python packaging/collect_runtime.py \
  --variant linux-cuda-cu126 \
  --prefix build-linux-cuda-cu126 \
  --runtime-dir /path/to/attemory-core-sdk/lib \
  --runtime-dir /usr/local/cuda/lib64 \
  --clean
```

The native `attemory_server` should be linked with:

```text
RPATH=$ORIGIN/../lib
```

so users do not need to set `LD_LIBRARY_PATH`.
