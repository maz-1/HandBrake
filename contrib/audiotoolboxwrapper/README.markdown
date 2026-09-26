# Windows Apple AAC support

This fork uses [AudioToolboxWrapper](https://github.com/maz-1/AudioToolboxWrapper/tree/34559baa33baa9706b7db02f93457e0d45951e16)
to expose Apple's AAC and HE-AAC encoders on Windows. Enable it when configuring
the native library and CLI, for example from a Linux cross-build environment:

```sh
./configure --cross=x86_64-w64-mingw32 --enable-audiotoolboxwrapper
make -C build -j$(nproc)
```

The Windows x64 workflow enables this option. Other builds keep it disabled by
default. macOS continues to use the system AudioToolbox framework.

At runtime, install the matching Apple Application Support components supplied
with iTunes, or provide `CoreAudioToolbox.dll` and its dependencies in a location
supported by the wrapper (including `QTfiles64` next to the executable for x64).
The GUI and CLI expose `ca_aac` and `ca_haac` only when their converters can be
created. Preset import keeps an available Apple encoder and falls back to
FFmpeg AAC if it is unavailable. Apple DLLs are not bundled by this project.

After building, check `HandBrakeCLI.exe --help` for both encoders and encode a
short source with `-E ca_aac`, then with `-E ca_haac`. Also check a clean Windows
environment without Apple DLLs: startup should succeed and neither encoder
should be listed. In the GUI, save and reload a preset with each Apple encoder
and verify that the selection and fallback encoder survive.

The wrapper supplies the AAC 7.1 layout tag required by the current encoder.
The build sets a CMake policy compatibility floor. The wrapper has no pkg-config
metadata, so the CLI links it explicitly together with its Windows system
dependencies.
