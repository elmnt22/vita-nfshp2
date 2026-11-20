# Vulkan build dependency

The retained C headers are the unmodified Khronos Vulkan-Headers v1.3.290
`include/**/*.h` files. Their upstream licenses are retained alongside them.
The Windows AMD64 `lib/x64/vulkan-1.lib` is an import library generated with
MSVC `lib.exe /machine:x64 /Brepro` from the unmodified Vulkan-Loader v1.3.290
`vulkan-1.def`. It contains loader symbol imports, not a Vulkan driver.

`dependency-lock.json` records the download locations, original archive,
per-file SHA256 and generated import library. Normal VS2022 and CMake builds
use these files directly, without Python, network access or VULKAN_SDK.
`scripts/vendor_vulkan.py` is the explicit maintenance/restore command.

At runtime the application uses the graphics driver's system `vulkan-1.dll`.
The renderer checks Vulkan version/device capabilities through its existing
initialization. Driver binaries and the Vulkan SDK are not redistributed.
The project's checked-in SPIR-V arrays do not require a shader compiler for
a normal application build.

Upstream:
- https://github.com/KhronosGroup/Vulkan-Headers/tree/v1.3.290
- https://github.com/KhronosGroup/Vulkan-Loader/tree/v1.3.290
- https://docs.vulkan.org/guide/latest/loader.html
