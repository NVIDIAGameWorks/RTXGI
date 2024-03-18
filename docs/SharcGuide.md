# SHARC Overview
Spatially Hashed Radiance Cache is a technique aimed at improving signal quality and performance in the context of path tracing. The SHARC operates in world space and provides a radiance value at any hit point.
 
## Distribution
SHARC is distributed as a set of shader-only sources; the package is located in [sdk-libraries/sharc][SharcPackage] and contains shader includes and [integration guide][SharcIntegrationGuide].

[SharcPackage]: https://gitlab-master.nvidia.com/rtx/sharc
[SharcIntegrationGuide]: https://gitlab-master.nvidia.com/rtx/sharc/-/blob/master/docs/Integration.md?ref_type=heads.