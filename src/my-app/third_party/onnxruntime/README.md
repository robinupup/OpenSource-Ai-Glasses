# onnxruntime (cross-compiled for RV1106)

Minimal, op-reduced ONNX Runtime v1.14.1 for:

- target: armv7-a + NEON + VFPv4, uClibc-ng 1.0.50 (arm-rockchip831-linux-uclibcgnueabihf)
- runtime features: **extended minimal build** (loads `.ort` format), FusedConv enabled
- model footprint on glass (load + infer): **~3.8 MB RSS**
- inference: **~9.7 ms per 32 ms frame** on single Cortex-A7 @ ~1.2 GHz (Silero VAD v5)

## Files

| path | purpose |
|---|---|
| `include/onnxruntime_c_api.h` | ORT C API header (only C API is used; no C++) |
| `lib/libonnxruntime.so.1.14.1` | stripped shared library, 2.4 MB |
| `lib/libonnxruntime.so` | symlink → `.1.14.1` |
| `toolchain-rv1106.cmake` | cmake toolchain used to cross-compile |

## Deploying to the device

`make deploy-ort` (defined in `my-app/Makefile`) pushes the `.so` and the
`silero_vad.ort` model to the glass:

```
/oem/usr/lib/libonnxruntime.so.1.14.1
/oem/usr/lib/libonnxruntime.so          -> libonnxruntime.so.1.14.1
/oem/etc/silero_vad.ort
```

The device already ships `libstdc++.so.6`, `libgcc_s.so.1`, uClibc-ng and
`ld-uClibc.so.1` — no extra runtime dependencies are needed.

## Rebuilding from source (if you ever need to)

```bash
# Host prerequisites (user-level install is fine, no sudo needed):
pip install --user onnx onnxruntime==1.14.1 flatbuffers cmake protobuf==3.20.2
# plus a host-side protoc 3.20.2 binary in $PATH.

git clone -b v1.14.1 --recursive --depth 1 --shallow-submodules \
    https://github.com/microsoft/onnxruntime.git
cd onnxruntime
# Apply the two tiny patches described in ../build-ort/PATCHES.md:
#   - tools/ci_build/build.py:   remove trailing empty cmake arg (arm path)
#   - onnxruntime/core/common/cpuid_info.cc: guard cpuinfo usage
#   - onnxruntime/core/providers/cpu/nn/string_normalizer.cc: add __UCLIBC__
./build.sh --config Release --arm \
    --cmake_path $(which cmake) --ctest_path $(which ctest) \
    --path_to_protoc_exe $(which protoc) \
    --cmake_extra_defines \
        CMAKE_TOOLCHAIN_FILE=$(pwd)/../my-app/third_party/onnxruntime/toolchain-rv1106.cmake \
        onnxruntime_ENABLE_CPUINFO=OFF \
    --parallel $(nproc) --minimal_build extended --disable_ml_ops \
    --disable_exceptions --skip_tests --build_shared_lib \
    --include_ops_by_config $(pwd)/../my-app/models/silero_required_ops.config \
    --build_dir build
```

Then convert `silero_vad.onnx` (full) to `silero_vad.ort` (minimal) with the
same ORT version on the host:

```bash
python -m onnxruntime.tools.convert_onnx_models_to_ort \
    --optimization_style Fixed --target_platform arm \
    my-app/models/silero_vad.onnx
```

The resulting `silero_vad.ort` is what gets loaded on-device; `.onnx` is kept
next to it only for debugging / reconversion.
