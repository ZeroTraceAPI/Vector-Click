# Building Vector Click

This folder contains the source files and build configuration needed to build Vector Click.

The normal supported build environment is 64-bit Windows 10 or Windows 11. The project uses C++20 and CMake 3.24 or newer.

The commands below assume your terminal is open in the `Source Files` folder.

## Recommended Windows Build

### Requirements

- Visual Studio 2022 or Visual Studio 2022 Build Tools
- The **Desktop development with C++** workload
- A Windows 10 or Windows 11 SDK
- CMake 3.24 or newer

Visual Studio Code is optional. If you use it, the Microsoft C / C++ and CMake Tools extensions can provide a convenient editor and build interface, but they are not required.

### Command Line

Open an **x64 Native Tools Command Prompt for VS 2022**, change to the `Source Files` folder, and run:

```bat
cmake --preset windows-x64-release
cmake --build --preset build-windows-x64-release
ctest --preset test-windows-x64-release
```

The Release executable is normally written to:

```text
build/windows-x64-release/Release/Vector Click.exe
```

For a Debug build, use the corresponding `windows-x64-debug`, `build-windows-x64-debug`, and `test-windows-x64-debug` presets.

### Visual Studio Code

1. Open the `Source Files` folder.
2. Select the `windows-x64-release` CMake configure preset.
3. Configure the project.
4. Build with the `build-windows-x64-release` preset.
5. Run the `test-windows-x64-release` test preset.

## Optional Zig Build

The repository also includes a Linux-to-Windows x64 build route using Zig. This route is useful for reproducible cross-builds and does not replace the recommended native Windows build.

### Requirements

- Linux x86_64
- Zig **0.16.0** exactly
- CMake 3.24 or newer
- Ninja
- Python 3

Set `VECTORCLICK_ZIG` to the Zig 0.16.0 executable and run:

```sh
VECTORCLICK_ZIG=/absolute/path/to/zig-0.16.0/zig \
    ./Tools/Zig/build-zig.sh
```

The executable is written to:

```text
build/zig-0.16.0-release/Vector Click.exe
```

The first Zig build can take longer because Zig may need to prepare its C++ and MinGW-compatible runtime cache. Later builds with the same Zig installation and cache are normally faster.

The helper uses the included `Tools/Zig` toolchain files, targets `x86_64-windows-gnu`, builds the Release configuration, and normalizes the unsigned PE timestamp for reproducible same-source builds.

## Platform-Neutral Tests

On a non-Windows development system, the platform-neutral Core and presentation tests can be built with the host compiler:

```sh
cmake -S . -B build/native-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/native-tests
ctest --test-dir build/native-tests --output-on-failure
```

These tests do not replace native Windows testing of the application, input backends, hotkeys, target handling, capture exclusion, window behavior, or other Windows-specific functionality.

## Build Differences

MSVC and Zig use different compiler and runtime implementations, so their executables are not expected to have identical file sizes or SHA-256 hashes.

When comparing two builds for size or reproducibility, use the same source, compiler family, exact compiler version, target, build configuration, resource route, and linker policy.

Vector Click release builds are not intended to use executable packing or runtime compression.

## Troubleshooting

- Start from a new build directory after changing compiler families or major toolchain versions.
- Do not reuse a Zig build directory for an MSVC build, or an MSVC build directory for Zig.
- Confirm that the selected compiler and Windows SDK match the target architecture.
- If CMake reports a stale cache after moving the source folder, delete the affected `build` subfolder and configure again.
- If the Zig helper rejects the compiler version, point `VECTORCLICK_ZIG` to the exact Zig 0.16.0 executable.

For contribution expectations and project policy, see the repository-level `CONTRIBUTING.md` file.
