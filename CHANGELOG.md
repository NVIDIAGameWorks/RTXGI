# RTXGI SDK Change Log

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