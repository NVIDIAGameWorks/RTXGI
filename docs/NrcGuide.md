# NRC Integration Guide
<p align="center">
    <img src="figures/nrc_guidebanner.png" alt>
    <em><small>Figure 1. Comparison of signal quality using a pathtracer at eight bounces and one sample per pixel. LHS: raw pathtracer output. Centre: pathtracer output terminating early in the NRC. RHS: Accumulated result of the pathtracer without relying on the NRC.</small></em>
</p>

## Introduction
The Neural Radiance Cache is an AI technique aimed at improving signal quality and potentially performance in the context of pathtracing. The NRC operates in world space and predicts **radiance** at any point in the virtual world using path-traced live-trained data.
 
 The NRC is designed to support dynamic scenes and to be independent of scene data such as materials, BRDFs, geometry, etc. As a result, NRC does not depend on any precomputations or manual parameter adjustments. The key to this property is continuous adaptation of the underlying neural network – it is always updated by tracing a certain amount of training paths at full length which optimizes the network to fit the current state of the scene. Moreover, NRC learns radiance, and as a result works well for glossy reflections, as opposed to irradiance caching techniques which cater to diffuse lighting. The NRC library supports D3D12 and Vulkan rendering APIs while relying on Tensor Cores to train the neural network each frame.

 The NRC library is currently in experimental mode and is being actively developed.

### Algorithm Overview
The process of tracing paths to their full length (until they reach a light source or exit the scene) is expensive and introduces significant noise. To mitigate these drawbacks, the paths are terminated early (shortened) by querying the radiance cache for the amount of light that should be injected into the ends of these short paths (illustrated in *Figure 2.*).

To avoid further limitations such as a precomputation step, only supporting diffuse materials, or introducing light leakage due to misalignment of geometry, The NRC algorithm is agnostic to material definition and light setup. It relies on a neural network that is trained while rendering and takes the position and direction as inputs then returns the predicted radiance leaving that position in the specified direction.

![NrcPathtracing][NrcPathtracing]
<p align="center"> 
    <em><small>Figure 2. Enhancing signal quality with radiance caching. Top LHS: paths are typically traced until they reach a light source or exit the scene - expensive and noisy. Bottom LHS: shorter paths are obtained by terminating into the NRC. RHS: Query and training points during pathtracing.</small></em>
</p>

The workflow is to first run a path tracer at lower-than-target resolution to write training radiance for the neural network. We refer to this pass as the _update_ pass. This is followed by a second, full-resolution, _query_ pathtracer pass, where query points where we want to read predicted radiance are created. Next, the neural network predicts radiance which is read at the queried points during a resolve pass. To generate the training data, the NRC library internally propagates the predicted data backwards along the training path such that each vertex of that training path will get an estimate of reflected light - this will be used to train and optimize the network so it makes accurate predictions.

#### Path termination heuristic
Instead of tracing a ray or a path to get incident radiance at a given point, we query the radiance cache to obtain an accurate estimate of the radiance, effectively terminating paths early in the cache. This improves the quality and potentially the performance. 
![NrcTermination][NrcTermination]
<p align="center"> 
    <em><small>Figure 3. Termination heuristic employed when querying the cache - the paths are shortened when the path spread(r) is larger than a custom threshold (t).</small></em>
</p>

The decision to terminate the path early into the cache is informed by a path spread (ray cone) heuristic. The cone approximates the ray and spreads along the path as it intersects surface with various material properties. The interaction with a diffuse surface will significantly increase the cone's radius, whereas smooth/specular surfaces do not contribute as much to the cone's spread. When the radius of the cone reaches a user-specified threshold, the path is safe to terminate early in the cache. This allows the application to control the trade-off between bias and noise.


> 📗 **Background materials**
>
>- *[Real-time Neural Radiance Caching for Path Tracing][SiggraphPaper], SIGGRAPH 2021.*
>- *[Advancing Real-Time Path Tracing with the Neural Radiance Cache][GTC2], GTC 2023.*


## Distribution
The NRC library is a binary distribution. The content resides in [sdk-libraries/nrc][NRCPackage] and accounts for binaries per API, shader includes, and headers.

|Directory          |Files                                        |
|-------------------|---------------------------------------------|
|[/bin][nrcbin]     | NRC_D3D12.dll, NRC_Vulkan.dll, CUDA dlls    |
|[/include][nrcinc] | Nrc.hlsli, NrcD3d12.h, NrVk.h, misc helpers |
|[/lib][nrclib]     | NRC_D3D12.lib, NRC_Vulkan.lib               |


## API Overview
Assuming the application already has a pathtracer and associated requirements such as an acceleration structure (AS) in place, the most relevant parts for implementing NRC can be conceptually divided into two categories - the CPU-side API and the shader-side API.

**CPU-side API.** The [`NrcD3d12.h`][NrcD3d12h] and the Vulkan counterpart, [`NrcVk.h`][NrcVkh], expose the following functionality:
- Initialization and (re)configuration of the NRC context
- Options regarding resource memory management (app-managed or internally managed by the NRC library)
- Invocations to the underlying neural network to:
  - Create query data - points where we want the network to infer radiance values.
  - Train the network on data collected in the pathtracer. This optimizes the network and allows it to predict radiance.
- Invocation of a Resolve pass. This is a compute pass internal to the NRC library that resolves radiance at the queried points since this cannot be done in-line in the pathtracer. Additionally, this method is used for debug visualization.

**Shader-side API.** The application's pathtracer will rely on the [`Nrc.hlsli`][Nrchlsli], [`NrcHelpers.hlsli`][Nrchlsli], as well an [`NrcStructures.h`][NrcStruct] to declare and write training and query data to NRC's resources.

# Integration Steps

![NrcIntegWorkflow][NrcIntegWorkflow]
<p align="center">
    <em><small>Figure 4. An overview of the integration steps. Initialization is carried out before the render loop, the (re)configuration step is only invoked when context settings change, whilst the remaining tasks occur per-frame. The update and query passes rely on the application's provided pathtracer. If the signal is split per BRDF, the application can define a custom resolve pass instead of relying on the NRC library in-built resolve.</small></em>
</p>

## At Load-time
### Inclusion of library contents
At this stage, the application will include the headers inside the provided **/include**, as well as link against prebuilt binaries available in the same package (see **/bin** and **/lib**). The Resolve pass and kernels used internally by the NRC library come packaged as part of the binary and as such it is not necessary to provide or explicitly load external shader files.

## At Run-time
> 💡 **Optional integration class.** 
> 
> *The application can invoke the public NRC functions in-place, where they are required, or can opt for the creation of an integration class to encapsulate the functionality (as seen in [samples/pathtracer/NrcIntegration.h][SampleNrcIntegh]). The latter approach is used in the pathtracer example for ease of readability and demonstration of the library's functionality; it is not enforced as a best-practice.*


### At the start of the application (only once)

#### Step 1 - Initialization and memory management mode selection
This is typically achieved in the application's `Init()` step. First, set up NRC's `GlobalSettings`. Here the application can hook a logger callback for intercepting NRC library messages as well as specifying which memory management mode should be used (consult the following section for further details).

Initialize the library by invoking:
```cpp
nrc::Status status = nrc::d3d12::Initialize(globalSettings);
```

Next, create an NRC context:
```cpp
status = nrc::d3d12::Context::Create(nativeDevice5, m_nrcContext);
``` 

> ❗ At this point, the neural network has not yet been created. This is achieved in the configuration step.

**Selection of memory management.** The NRC library caters for two approaches when it comes to managing its buffers, controlled via the `globalSettings.enableGPUMemoryAllocation` switch is for. When the SDK manages the buffers internally, this flag should be enabled first at initialization time. After that, using the helper function `GetBuffers()`, the application will only create views (_handles_) to these buffers. They are required during path tracing and in a scenario where a custom resolve pass is necessary (more on this in the Resolve section).
If buffers are managed on the application side, the flag will be disabled, and the buffers will be created by the application during the configuration step using `GetBuffersAllocationInfo(const ContextSettings& contextSettings, BuffersAllocationInfo& outBuffersAllocationInfo)` to inform properties such as element size and stride, which types of views are allowed, etc.

> 💡 [NrcIntegration.cpp][SampleNrcIntegc] illustrates both approaches depending on the state of the flag. This is available for D3D12 and Vulkan implementations. 

### (Re)configuration (on a per-need basis)
#### Step 2 - Configure
This is expected to occur infrequently when constituent parts of the `ContextSettings` have changed. E.g. on level load, or when the screen resolution changes. This reloads the neural network configuration and may require buffers to be reallocated.

Considerations when using Configure:
- `Configure(const ContextSettings& contextSettings, const Buffers* buffers = nullptr)` should be called at least once if this was the first time NRC was initialized. This is required as `Configure` performs memory allocations.

- If `enableGPUMemoryAllocation` is switched off and app-side memory management is preferred, then it is at this point when the app-side managed buffers will be passed to `Configure`. 

- If `Configure` is a part of the render loop, it could cause a hitch if any of the context settings have changed and a tear-down is required.

### Per frame setup

#### Step 3 - NRC constants update
The NRC library differentiates between settings that seldomly change (`ContextSettings`) and settings that change per-frame (`FrameSettings`).

At the start of the frame, invoke:
```cpp
// This function populates internal data and clears the `Counter` buffer.
Status BeginFrame(ID3D12GraphicsCommandList4* cmdList, const FrameSettings& frameSettings);
```

Next, populate the `NrcConstants` structure by calling:
```cpp
Status PopulateShaderConstants(NrcConstants& outConstants) const;
```

The `NrcConstants` structure is intended to be passed to the pathtracer inside a constant buffer. It can be included as part of an existing constant buffer for the pathtracer. Its content is derived from `FrameSettings` and `ContextSettings`.


#### Step 4 - Pathtracer setup
The NRC requires two pathtracer passes - one for updating (writing path data for training the NN), and one for querying (creating query points where the NN will predict radiance).

1) The update pass runs at lower-than-target resolution. It is recommended to rely on the auxiliary function to obtain this resolution and set it on the `ContextSettings` early on:
    ```cpp
    nrc_uint2 computeIdealTrainingDimensions(nrc_uint2 const& frameDimensions, float avgTrainingVerticesPerPath = 0.f)
    ```
    The dispatch size of this pass has to match the `contextSettings.trainingDimensions`.

2) The query pass dispatch size is equal to the `contextSettings.frameDimensions`.

> 💡 The update pass is independent of the query pass - this offers a potential performance benefit.

There are several approaches to a pathtracer's modus operandi. The pathtracer project in RTXGI SDK illustrates a trivial path tracer that does _not_ start from the G-Buffer.

Below is another variant which shows how NRC is integrated if the primary ray is reconstructed from the G-Buffer.
```cpp
// Prepare NRC buffers: queryPathInfo, trainingPathInfo, trainingPathVertices, queryRadianceParams, countersData, debugTrainingPathInfo.

void RayGenFunc() 
{
    // Load data from G-Buffer...

    // Flag G-Buffer miss to write it to NRC later

    // Only 1 SPP during NRC Update pass
    const uint samplesPerPixel = NrcIsUpdateMode() ? 1 : nrcConstants.samplesPerPixel;

    // Prepare NRC context
    NrcBuffers nrcBuffers = {queryPathInfo, trainingPathInfo, ...};
    NrcContext nrcContext = NrcCreateContext(nrcConstants, nrcBuffers, DispatchRaysIndex().xy); 
    
    for (int sampleIndex = 0; sampleIndex < samplesPerPixel; sampleIndex++) 
    {     
        // Initialize NRC data for path and sample index traced in this thread
        NrcSetSampleIndex(nrcContext, sampleIndex);
        NrcPathState nrcPathState = NrcCreatePathState(rand(rngState));

        if (NrcIsUpdateMode()) {/*Add random offset to pixel's coords...*/}
        
        if(flagGbufferMiss) 
        {
            NrcUpdateOnMiss(nrcPathState);
            break;
        }
        else
        {
            NrcSurfaceAttributes surfaceAttributes = gBufferData;
            NrcProgressState nrcProgressState = NrcUpdateOnHit(...); // Update NRC state on hit
            if (nrcProgressState == NrcProgressState::TerminateImmediately) 
                break;
            NrcSetBrdfPdf(nrcPathState, brdfPdf)
        }

        // Prepare Payload and other data...
        
        for (int bounce = 1; bounce < gData.maxPathVertices; bounce++) 
        {                 
            TraceRay(...);

            if (!payload.hasHit()) NrcUpdateOnMiss(nrcPathState); // Handle miss
            
            // Decode material properties...  

            NrcSurfaceAttributes surfaceAttributes = decodedMaterial; // Passed to NrcUpdateOnHit
         
            NrcProgressState nrcProgressState = NrcUpdateOnHit(...); // Update NRC state on hit
            if (nrcProgressState == NrcProgressState::TerminateImmediately) break;

            // Account for emissives and evaluate NEE with RIS...

            // Terminate loop early on last bounce (don't sample BRDF)
            if (bounce == gData.maxPathVertices - 1) 
            {
                NrcSetDebugPathTerminationReason(...); 
                break;
            }

            // Terminate loop after emissives and direct light if CreateQuery requests delayed termination. 
            // If direct lighting isn't cached (radianceCacheDirect is false)
            // add direct lighting on hit where we query NRC before terminating the loop.
            if (nrcProgressState == NrcProgressState::TerminateAfterDirectLighting) break;
            
            // Sample BRDF to generate the next ray and run MIS...
            
            if(!evaluateCombinedBRDF(...)
                NrcSetDebugPathTerminationReason(nrcPathState, BRDFAbsorption);

            NrcSetBrdfPdf(nrcPathState, brdfPdf);
        } // End of path
        NrcWriteFinalPathInfo(nrcContext, nrcPathState, throughput, radiance);
      } // End of SPP loop
}
``` 

> 💡 If the application's pathtracer carries out the work in Closest-Hit Shader (CHS) then the `NrcPathState` has to be packed inside the payload and communicated between stages.

#### Step 5 - Query and Train
Once the pathtracer passes complete, the neural network predicts radiance at the query points. Internally, the NRC library propagates the radiance based on the predicted values and the path data. This is immediately followed by the training of the network which relies on the training data from the propagation. This optimizes the network to produce accurate radiace predictions.

All the aforementioned steps are achieved in a single exposed API call:
```cpp
Status QueryAndTrain(ID3D12GraphicsCommandList4* cmdList, float* trainingLossPtr)
```

#### Step 6 - The resolve pass
The final radiance is not obtained in-line in the pathtracer. As such, a separate pass is required to compute the final result. The NRC library exposes an API call to an in-built resolve pass which assumes the signal is combined. This pass takes the predicted radiance from the query records, modulates by the throughput of the path, and adds the result to the final image.
```cpp
Status Resolve(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* outputBuffer);
```

If the application relies on a split signal, then the library call can be skipped in favor of defining a custom compute pass, for example:
```cpp
void CustomResolve(int3 DispatchThreadID : SV_DispatchThreadID)
{
	const uint2 launchIndex = DispatchThreadID.xy;
	if(any(launchIndex >= screenResolution)) 
	    return;

    const uint sampleIndex = 0;
    const uint samplesPerPixel = 1;

    const uint pathIndex = NrcGetPathInfoIndex(screenResolution, launchIndex, sampleIndex, samplesPerPixel);
    const NrcQueryPathInfo path = NrcUnpackQueryPathInfo(nrcQueryPathInfo[pathIndex]);

    if (path.queryBufferIndex < 0xFFFFFFFF)
    {
		float3 radiance = NrcUnpackRadiance(nrcQueryRadiance[path.queryBufferIndex], radianceUnpackMultiplier) * path.prefixThroughput;
		uint uBrdfType = brdfTypeTarget[launchIndex];

		if(uBrdfType == BRDF_SPECULAR)
			specularPathTracingTarget[launchIndex] += float4(radiance, 0.0f);
		if(uBrdfType == BRDF_DIFFUSE)
            diffusePathTracingTarget[launchIndex] += float4(radiance, 0.0f);
    }
}
```
In the above scenario, the pathtracer uses a probabilistic selection of BRDF type per path and writes diffuse and specular results to two separate buffers (`specularPathTracingTarget` and `diffusePathTracingTarget`). In order for the custom resolve pass to work in this case, the BRDF selection is recorded in the pathtracer via the `brdfTypeTarget`. This intermediate buffer informs what radiance to unpack and to which output buffer to write it to.

> 💡 The in-built resolve pass can be used for visually debugging the NRC. See the [Debugging][DebugMethods] section for details.


#### Step 7 - End of frame  
This is when `EndFrame(ID3D12CommandQueue* cmdQueue)` is invoked once the command list has been submitted.The command queue must be the same one that was used to execute all the previous command lists.

### Application Shutdown (only once)
Any created contexts must be destroyed:

```cpp
Status Destroy(Context& context)
```

And finally, the NRC library itself must be shutdown:
```cpp
void Shutdown()
```

# Debugging

## Debug visualizations
The NRC library provides several ways to visually inspect the quality of the cache data and narrow down potential integration errors. This can be achieved at the pathtracer level and at the resolve pass level.

### Direct visualization of the cache
This is achieved in the path tracer by always querying vertex index 0. The pathtracer sample in RTXGI showcases this usage. In the UI "Enable Termination Heuristic" should be disabled and the "Query Vertex Index" selection should be set to 0. When inspecting the direct visualization of the cache, indirect signal as well as defined shadows should be present. Boiling-like artifacts are expected as the NRC is not intended to be _used_ in this way, only previewed for troubleshooting.

![DebugCacheVis][DebugCacheVis]
<p align="center">
    <em><small>Figure 5. Direct visualization of the cache - querying at vertex index 0.</small></em>
</p>

> 💡 This debug view should match the pathtracer's intensity closely. If it does not match the intensity or looks flat and lacks details (such as shadows), it could indicate that the radiance is not correctly recorded in the pathtracer passes, or that the update pass does not trace a small fraction to the full length.

### Resolve pass debug visualization 
Similar to the path tracer debug views, the in-built Resolve pass caters for several types of cache data visualizations including a result of the query, a training bounce heatmap, and training radiance (raw and smoothed) output. The training bounce heatmap is similar to the pathtracer bounce visualization and ensures that during the update pass, paths are traced on average to four bounces and a  fraction (~16th of the paths) at full length. The visualization of the training radiances at the primary path vertex, should match closely, when accumulated/smoothed, to the ground-truth path tracer output (when the NRC is disabled).

![DebugAll][DebugAll]
<p align="center">
    <em><small>Figure 6. Resolve pass debug modes. LHS: query results. Centre: Training bounces heatmap (Red encodes two bounces, yellow-three, green-four, white-eight). RHS: Smoothed training radiance for the primary ray. </small></em>
</p>


## Nsight Graphics Frame Debugging
During the integration it is important to ensure that the NRC buffers are correctly written to by the application's pathtracer. This can be achieved with Nsight Graphics frame capture in conjunction with custom structure definitions for ease of readability.

![DebugNsight][DebugNsight]
<p align="center">
    <em><small>Figure 7. Visualizing NRC buffers in Nsight Graphics Frame Capture. This illustrates the path tracing update pass with a focus on the buffer holding NrcRadianceParameters.  </small></em>
</p>

The Structured Memory Configuration feature in Nsight comes in handy for inspecting the contents of NRC buffers regardless of packing. It's noteworthy that the neural network calls will not be available in Nsight Graphics Frame Debugging at the present time.

# Glossary of resource names and estimated sizes

|Resource               |Element Size |Total Count  |Total Allocation |
|-----------------------|-------------|-------------|-----------------|
|Counter                | 4           | 8           | 8               | 
|QueryPathInfo          | 8           | 2073600     | 16588800        |
|TrainingPathInfo       | 8           | 42196       | 337568          |
|TrainingPathVertices   | 48          | 337568      | 16203264        |
|TrainingRadiance       | 12          | 337568      | 4050816         |
|TrainingRadianceParams | 56          | 337568      | 18903808        |
|QueryRadiance          | 12          | 2117844     | 25414128        |
|QueryRadianceParams    | 56          | 2117844     | 118599264       |
|DebugTrainingRadiance  | 24          | 42196       | 1012704         |
|Total (MB)             |             |             | 191.793         |
<p align="center">
    <em><small>Table 1. Expected values at frame dimensions 1920 x 1080, with a training resolution of 274 x 154, maximum path length of eight bounces, one SPP.</small></em>
</p>

[NrcPackage]: ../sdk-libraries/nrc
[NrcIntegration]: ../samples/pathtracer/NrcIntegration.h
[SiggraphPaper]: https://research.nvidia.com/publication/2021-06_real-time-neural-radiance-caching-path-tracing
[GTC1]: https://www.nvidia.com/en-us/on-demand/session/gtcspring21-e31307/
[GTC2]: https://www.nvidia.com/en-us/on-demand/session/gtcspring23-s51967/
[nrcbin]: ../sdk-libraries/nrc/bin
[nrcinc]: ../sdk-libraries/nrc/include
[nrclib]: ../sdk-libraries/nrc/lib
[SampleNrcIntegh]: ../samples/pathtracer/NrcIntegration.h
[SampleNrcIntegc]: ../samples/pathtracer/NrcIntegration.cpp
[NrcPathtracing]: figures/nrc_intropath.png
[NrcTermination]: figures/nrc_introtermination.svg
[NrcIntegWorkflow]: figures/nrc_integworkflow.svg
[DebugMethods]: #debugging
[DebugCacheVis]: figures/nrc_debugcachevis.gif
[DebugAll]: figures/nrc_debugresolve.png
[DebugNsight]: figures/nrc_debugnsight.png
[NrcD3d12h]: ../sdk-libraries/nrc/include/NrcD3d12.h
[NrcVkh]: ../sdk-libraries/nrc/include/NrcVk.h
[Nrchlsli]: ../sdk-libraries/nrc/include/Nrc.hlsli
[NrcHelp]: ../sdk-libraries/nrc/include/NrcHelpers.hlsli
[NrcStruct]: ../sdk-libraries/nrc/include/NrcStructures.h