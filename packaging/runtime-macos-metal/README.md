# attemory-runtime-macos-metal

This package contains the macOS arm64 Metal native runtime for `attemory`.

It is a platform runtime package and is not intended to be installed directly
by most users. Install it through:

```bash
uv tool install "attemory[mcp,macos-metal]"
```

Builds must be produced on macOS arm64 with:

```text
MACOSX_DEPLOYMENT_TARGET=11.0
CMAKE_OSX_ARCHITECTURES=arm64
ATMCORE_ENABLE_METAL=ON
ATMCORE_ENABLE_CUDA=OFF
GGML_METAL=ON
```

Populate the package tree with:

```bash
python -B packaging/collect_runtime.py \
  --variant macos-metal \
  --server-binary build-macos-metal/bin/attemory_server \
  --runtime-dir ~/work/memory/attemory-core/build-macos-metal/lib \
  --runtime-dir ~/work/memory/attemory-core/build-macos-metal/bin \
  --clean \
  --patch-rpath always
```

The collector uses `otool`, `install_name_tool`, and ad-hoc `codesign` to make
the copied binaries relocatable inside the wheel.
