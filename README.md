# RTXGI
![banner](docs/figures/banner.png)
<br />
<div align="center">
    · 
    <a href="docs/QuickStart.md">Quick Start</a>
    ·
    <a href="docs/NrcGuide.md">NRC Guide</a>
    ·
    <a href="docs/SharcGuide.md">SHARC Guide</a>
    ·
</div>
<br/>

Advances in path tracing techniques have allowed for the capture of lighting data from the environment, enabling the use of indirect illumination in real-time with both improved accuracy and speed. RTXGI SDK implements two such techniques, replacing traditional probe-based irradiance caching with a world-space radiance cache which can be used to sample outgoing radiance each time scene geometry is hit during path tracing. 

These techniques may be combined with a regular path tracing pipeline for the primary rays, sampling cached data only for indirect bounce evaluation. By replacing the whole path trace with a single ray hit evaluation and cache lookup, the cost is reduced with little to no compromise in signal quality, while remaining responsive to change and supporting large-scale dynamic scenes with complex lighting setups. 

RTXGI SDK provides an example integration (DX12 and Vulkan) of two state-of-the-art radiance caching techniques for path tracing - a (currently experimental) AI-based approach known as Neural Radiance Cache (NRC), and Spatially Hashed Radiance Cache (SHARC). The former requires Tensor Cores while the latter has certain limitations but is currently supported on a wider range of hardware without any vendor-specific requirements. RTXGI SDK also hosts documentation and distribution corresponding to both of these techniques, see [Project Structure][ProjectStructure] section for further details.


## Project structure
|Directory                   |Details                                      |
|----------------------------|---------------------------------------------|
|[/docs][docs]               |_Documentation for showcased tech_           |
|[/donut][donut]             |_Framework used for the samples_             |
|[/external][external]       |_Helper dependencies for the samples_        |
|[/media][media]             |_Assets and scene definitions_               |
|[/samples][samples]         |_Samples showcasing usage of NRC, SHARC_     |
|[/sdk-libraries][libraries] |_Binaries, src, includes for NRC, SHARC_     |


## Getting up and running

### Prerequisites
Min 20xx - SHARC **|** Min 30xx - NRC **|** [CMake v3.24.3][CMake] **|** [Git LFS][LFS] **|** [Vulkan SDK 1.3.268.0][VKSDK] **|** [VS 2022][VS22] **|** Windows SDK ≥ 10.0.20348.0

### Further steps
- [Quick start guide][QuickStart] for building and running the pathtracer example.
- [NRC integration guide][NrcGuide] and the [SHARC integration guide][SharcGuide] respectively.

## Contact
RTXGI SDK is actively being developed. Please report any issues directly through the GitHub issue tracker, and for any information or suggestions contact us at rtxgi-sdk-support@nvidia.com.

## License
See [LICENSE.md](LICENSE.md)


[QuickStart]: docs/QuickStart.md
[SharcGuide]: docs/SharcGuide.md
[NrcGuide]: docs/NrcGuide.md
[ProjectStructure]: #project-structure
[docs]: docs
[donut]: donut
[external]: external
[media]: media
[samples]: samples/pathtracer
[libraries]: sdk-libraries
[CMake]: https://cmake.org/download/
[LFS]: https://git-lfs.com/
[VKSDK]: https://vulkan.lunarg.com/sdk/home#windows
[VS22]: https://visualstudio.microsoft.com/vs/
