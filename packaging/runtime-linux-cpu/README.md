# attemory-runtime-linux-cpu

Native Linux CPU runtime package for `attemory`.

Expected layout:

```text
attemory_runtime_linux_cpu/
  bin/attemory_server
  lib/libattemory-core.so
  lib/libllama.so
  lib/libggml*.so
  metadata/build-info.json
```

This package is only a packaging boundary. Build scripts or CI should populate
`bin/` and `lib/` before building the wheel.

Collector example:

```bash
python packaging/collect_runtime.py \
  --variant linux-cpu \
  --prefix build-linux-cpu \
  --runtime-dir ~/work/memory/attemory-core/build-linux-cpu/lib \
  --runtime-dir ~/work/memory/attemory-core/build-linux-cpu/bin \
  --clean
```

If `attemory_server` is not under `PREFIX/bin`, pass it explicitly:

```bash
python packaging/collect_runtime.py \
  --variant linux-cpu \
  --server-binary build-linux-cpu/bin/attemory_server \
  --runtime-dir ~/work/memory/attemory-core/build-linux-cpu/lib \
  --runtime-dir ~/work/memory/attemory-core/build-linux-cpu/bin \
  --clean
```

The Linux CPU collector rejects CUDA dependencies by default. This is intended:
do not publish a CUDA-linked build as `attemory-runtime-linux-cpu`.

The native `attemory_server` should be linked with:

```text
RPATH=$ORIGIN/../lib
```

so users do not need to set `LD_LIBRARY_PATH`.
