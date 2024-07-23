# RTXGI SDK Change Log

## 2.2.0

### RTXGI
- Bug fix for the `brdf.h` PDF calculation and epsilon size.
- Addition of global surface properties override for roughness and metalness values.
- DLL signing verification implementation for the NRC integration.
- Auto-enablement of detected raytracing-capable hardware when using the pathtracer sample.
- Documentation update.

### SHaRC
- Update to version 1.3.1.0.
- The radiance cache now relies on frame-based accumulation. The user should provide the amount of frames for accumulation to the `SharcResolveEntry()` invocation as well as the amount of frames to keep the records which do not receive any updates in the cache before they get evicted. Frame limits can be tied to FPS. This change also improves responsiveness of the cache to the lighting changes.
- Robust accumulation which works better with high sample count
- Documentation updates, including debugging tips and parameters tweaking
- Misc bug fixes

### NRC
- Update to v0.12.1
- Update dependencies including CUDA Toolkit to v12.5.1.
- Modifications to `Nrc.hlsli` to comply with Slang requirements for global variables and macro defines.
- Addition of debug visualization of the cache as part of the `ResolveModes`.
- Removal of `queryVertexIndex` debug mechanism in favour of the Resolve pass approach.
- Addition of DLL signing verification capabilities.
- Bug fix for allowing the context to be recreated internally on scene bounds change.
- Documentation update.

## 2.1.0

### Fixed issues
- Internal fix for NRC to allow it to run on NVIDIA 20xx series GPUs
- Window resizing for the pathtracer sample

### API changes
- NRC's `CountersData` buffer is now of type `StructuredBuffer`
- SHARC's `VoxelData` buffers are now of type `StructuredBuffer`
- SHARC modifications to improve GLSL compatibility

### Misc. changes
- Readability improvements for the code sample and documentation
- Update to dependencies:
    - NRD version in use is [v4.6.1](https://github.com/NVIDIAGameWorks/RayTracingDenoiser/tree/db4f66f301406344211d86463d9f3ba43e74412a)
    - Donut version in use is [e053410](https://github.com/NVIDIAGameWorks/donut/tree/e05341011f82ca72dd0d37adc8ef9235ef5607b3)

## 2.0.0
Initial release.