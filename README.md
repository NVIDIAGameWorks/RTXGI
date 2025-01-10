# RTXGI
![banner](docs/figures/banner.png)
<br />
<div align="center">
    · 
    <a href="CHANGELOG.md">Change Log</a>
    ·
    <a href="docs/QuickStart.md">Quick Start</a>
    ·
    <a href="docs/NrcGuide.md">NRC Guide</a>
    ·
    <a href="docs/SharcGuide.md">SHaRC Guide</a>
    ·
</div>
<br/>

Advances in path tracing techniques have allowed for the capture of lighting data from the environment, enabling the use of indirect illumination in real-time with both improved accuracy and speed. RTXGI SDK implements two such techniques, replacing traditional probe-based irradiance caching with a world-space radiance cache which can be used to sample outgoing radiance each time scene geometry is hit during path tracing. 

These techniques may be combined with a regular path tracing pipeline for the primary rays, sampling cached data only for indirect bounce evaluation. By replacing the whole path trace with a single ray hit evaluation and cache lookup, the cost is reduced with little to no compromise in signal quality, while remaining responsive to change and supporting large-scale dynamic scenes with complex lighting setups. 

RTXGI SDK provides an example integration (DX12 and Vulkan) of two state-of-the-art radiance caching techniques for path tracing - a (currently experimental) AI-based approach known as Neural Radiance Cache (NRC), and Spatially Hashed Radiance Cache (SHaRC). The former requires Tensor Cores while the latter has certain limitations but is currently supported on a wider range of hardware without any vendor-specific requirements. RTXGI SDK also hosts documentation and distribution corresponding to both of these techniques, see [Project Structure][ProjectStructure] section for further details.


## Project structure
|Directory                   |Details                                      |
|----------------------------|---------------------------------------------|
|[/docs][docs]               |_Documentation for showcased tech_           |
|[/external][external]       |_Helper dependencies for the samples_        |
|[/media][media]             |_Assets and scene definitions_               |
|[/samples][samples]         |_Samples showcasing usage of NRC, SHaRC_     |
|[/sdk-libraries][libraries] |_Binaries, src, includes for NRC, SHaRC_     |


## Getting up and running

### Prerequisites
Any DXR GPU for SHaRC **|** NV GPUs ≥ Turing (arch 70) for NRC **|** [CMake v3.24.3][CMake] **|** [Git LFS][LFS] **|** [Vulkan SDK 1.3.268.0][VKSDK] **|** [VS 2022][VS22] **|** Windows SDK ≥ 10.0.20348.0 **|** Driver ≥ 555.85

### Further steps
- [Quick start guide][QuickStart] for building and running the pathtracer example.
- [NRC integration guide][NrcGuide] and the [SHaRC integration guide][SharcGuide] respectively.
- [Change log][ChangeLog] for release information.

## Contact
RTXGI SDK is actively being developed. Please report any issues directly through the GitHub issue tracker, and for any information or suggestions contact us at rtxgi-sdk-support@nvidia.com.

## Citation
Use the following BibTex entry to cite the usage of RTXGI in published research:
```bibtex
@online{RTXGI,
   title   = {{{NVIDIA}}\textregistered{} {RTXGI}},
   author  = {{NVIDIA}},
   year    = 2024,
   url     = {https://github.com/NVIDIAGameWorks/RTXGI},
   urldate = {2024-03-18},
}
```

## License
See [LICENSE.md](LICENSE.md)

## RTXGI v1.x
Version v1.x of RTXGI which includes the DDGI algorithm is located in a separate repo at the following url: https://github.com/NVIDIAGameWorks/RTXGI-DDGI.


[ChangeLog]: CHANGELOG.md
[QuickStart]: docs/QuickStart.md
[SharcGuide]: docs/SharcGuide.md
[NrcGuide]: docs/NrcGuide.md
[ProjectStructure]: #project-structure
[docs]: docs
[external]: external
[media]: media
[samples]: samples/pathtracer
[libraries]: sdk-libraries
[CMake]: https://cmake.org/download/
[LFS]: https://git-lfs.com/
[VKSDK]: https://vulkan.lunarg.com/sdk/home#windows
[VS22]: https://visualstudio.microsoft.com/vs/
