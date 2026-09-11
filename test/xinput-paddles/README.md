# Offline XInput paddle tests

This standalone CMake project builds the canonical decoder, reader, identity,
GATT, service, broker, WGI, trace, runtime, mapping, and pipeline tests in `SDL/test`.
The mapping suite registers its empty-field regression as a separate CTest case.

Use an x64 Native Tools command prompt with native MSVC, the C++ workload,
Windows SDK 10.0.26100.0 or newer, CMake 3.22 or newer, and Ninja. From the SDL
checkout root:

```bat
cmake -S test/xinput-paddles -B build/paddle-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/paddle-tests --parallel 8
ctest --test-dir build/paddle-tests --output-on-failure
```

For AddressSanitizer, use a separate build directory in the same developer
prompt so the sanitizer runtime DLL is on PATH:

```bat
cmake -S test/xinput-paddles -B build/paddle-tests-asan -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_XINPUT_PADDLE_TEST_ASAN=ON
cmake --build build/paddle-tests-asan --parallel 8
ctest --test-dir build/paddle-tests-asan --output-on-failure
```

Visual Studio generators also work. Supply `-A x64` when configuring,
`--config Release` when building, and `-C Release` when running CTest. ASan
supports Release and RelWithDebInfo. It excludes Debug's incompatible `/RTC1`.

Tests use C11 or C++20, `/W4 /WX`, and C++ exception handling. The mapping
target and the pipeline's C bridge suppress C4100 for unused callback parameters
in the included upstream `SDL_gamepad.c`. Other warnings remain errors, so a
new toolset can require a compatibility correction. The sidecar adds no global
SDK warning suppressions. The archive and tests share CMake's default MSVC CRT
selection, which is `/MD` in Release unless explicitly overridden.

Mapping and pipeline link a real `SDL3-static` archive built in this project's build tree.
Its separate cache forces shared SDL, the production paddle supplement, HIDAPI
joystick acquisition, and libusb off. Virtual joysticks stay on. The HIDAPI
subsystem stays on because this tree makes virtual joysticks depend on it.
`HAVE_GAMEINPUT_LIB=OFF` preserves optional dynamic GameInput loading.
Upstream test registration and examples stay off. The normal SDL build and
`test/CMakeLists.txt` are unaffected.

CTest uses fixed offline commands, including the runtime test without `--live`.
Mapping and pipeline disable hardware-backend hints at override priority before SDL_Init
and require an empty initial device list. The other tests use injected inputs
or compile-time test guards. No probe executables are built or registered.
ASan instruments the test targets and their directly compiled implementation
files. For mapping and pipeline, that includes the real gamepad implementation
in each C bridge. The rest of the SDL static archive is not instrumented.
