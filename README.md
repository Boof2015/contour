# Contour

A Windows system-wide parametric equalizer in early development.

Contour uses an ordered stack of EQ layers. Each layer can target one channel,
any combination of channels, or All. Imported and manually entered filters use
the same editable model.

The native foundation includes an audio processing object (APO), eight PEQ
filter types, text import, saved configurations, live updates with crossfades,
and independent DSP and APO tests. The application UI and production installer
are still to come.

## Build

Requires Windows 11 x64, CMake 3.24 or later, Visual Studio 2022 C++ Build Tools,
and a Windows SDK.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
```

Building and running the tests does not attach Contour to an audio device or
play sound. Deployment recovery tests use disposable per-user registry keys.
The deployment tools are for development experiments; they are not a production
installation path.
