# Capture Viewer

A fork of [CaptureKVM](https://github.com/PaulFreund/CaptureKVM) that focuses on being a simple low-latency viewer for HDMI capture devices on Windows. This fork is is compatible with any DirectShow capture source for video, and audio on devices that expose audio through the video filter (like the GC573) by default, but audio can be monitored from any audio input device. It's designed for streaming/remote play on the Switch 2, but will work with any device.

You can use this viewer to play through the preview, stream to Discord, or use it as the video source for a [custom remote play setup 😉](https://www.youtube.com/watch?v=r0OW_0BuXs4) on consoles.

## Requirements

- Windows 10 or newer.
- A D3D12 capable GPU.
- An HDMI capture device with a DirectShow filter, such as the AVerMedia GC573, Elgato 4K60 Pro MK.2, or most UVC-compliant devices.
- A capture card that can provide RGB8 uncompressed (XRGB in OBS) video frames is ideal, however NV12 (YUV420) sources are also supported.

## Build Prerequisites

- CMake 3.20 or newer.
- Microsoft Visual Studio 2019 or newer with the MSVC x64 toolchain.

## Building

1. Configure and build:

    ```powershell
    cmake -S . -B build -G "Visual Studio 18 2026" -A x64
    cmake --build build --config Release
    ```

2. Run the viewer from the generated `Release` (or `Debug`) output directory.

## Windows x64 builds with GitHub Actions

This fork includes the **Build Windows x64** workflow, which runs on pushes to
`master` and can also be started from **Actions > Build Windows x64 > Run workflow**.
It uses the `windows-2022` runner, Visual Studio 2022, CMake and the x64 Release
configuration. The MSVC runtime is linked statically for portable use.

After a successful run, download **CaptureViewer-Windows-x64** from its Artifacts
section (GitHub sign-in required). Extract the ZIP and run `viewer.exe` on Windows
11 x64. Press `M` for settings or `F11` for fullscreen. Artifacts are retained for
90 days; rerun the workflow when a fresh download is needed.

The package includes licenses, usage instructions, source/build information and
a SHA-256 checksum. CI checks the executable's x64 PE headers; actual capture,
audio, latency and high-frame-rate operation require testing with capture hardware.

## Runtime behaviour

- Press `M` at any time to open an in-window settings menu. Device choices and feature toggles persist in `settings.json` beside the executable.
  - The menu allows you to select the audio and video capture devices, adjust window sizing and display options, and adjust video capture settings like resolution, frame rate, and pixel format.
- The app enumerates the available capture devices through DirectShow, builds a graph with the Sample Grabber filter, and streams frames into the renderer without Sample Grabber buffering.
- BGRA frames are uploaded directly, NV12 frames stay planar and are converted in a D3D12 pixel shader while drawing.
- CPU-side double buffering keeps the capture callback decoupled from the render loop while maintaining low latency.

## Differences from upstream

- KVM features like input and audio forwarding are removed.
- Audio monitoring is supported from any input device, not just the GC573.
- Windowing options like borderless, fullscreen, etc.
- Support for the uncompressed RGB8 format.
- V-sync can be enabled.
- Refresh rate can be set independently of the resolution.
- Prevents sleep while the viewer is running.
- Audio monitoring can output to multiple devices at once.

## License

CaptureKVM is released under the [MIT License](LICENSE).
