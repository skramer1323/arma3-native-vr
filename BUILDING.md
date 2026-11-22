# Building Arma 3 Native VR

Use Windows x64, Visual Studio 2022 Build Tools with **Desktop development with C++** and a Windows SDK, CMake 3.24 or later, Git and Python 3. The OpenXR loader uses Python during the build. On first configure, CMake fetches pinned versions of OpenXR SDK Source and MinHook. Dear ImGui is included in the repository.

Open **Developer PowerShell for VS 2022** with the x64 tools selected. Run these commands from the project directory.

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build --target Arma3NativeVR Arma3NativeVRFreeTrack Arma3NativeVRTests
ctest --test-dir build --output-on-failure
```

`build/dist` contains `dinput8.dll`, `FreeTrackClient64.dll` and `arma3_native_vr.ini`. A binary distribution also includes the user guide, the project [LICENSE](LICENSE) and the third-party license texts under `licenses/`.

For an offline build, pass the paths to checkouts of the exact revisions in `CMakeLists.txt` using `-DFETCHCONTENT_SOURCE_DIR_OPENXR` and `-DFETCHCONTENT_SOURCE_DIR_MINHOOK`. If CMake cannot find Python, point `-DPython3_EXECUTABLE` to your Python executable.

The preview builds with DLSS disabled. To build the experimental path locally, explicitly set `-DARMA_WITH_DLSS=ON` and `-DARMA_DLSS_SDK_ROOT` to a checkout of the [official NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS). Review the [NVIDIA RTX SDKs License](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt) before distributing a DLSS-enabled build.
