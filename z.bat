@echo off

IF "%1" == "clean" (
    call d.bat
)

call b.bat
zip -9 rtxgi-ori.zip ./bin/*.exe ./bin/NRC_Vulkan.dll  ./bin/NRC_D3D12.dll  ./bin/nvrtc-builtins64_125.dll ./bin/NRD.dll
"C:\Program Files\7-Zip\7z.exe" a rtxgi-ori.zip bin/shaders/ -mmt -mx9
"C:\Program Files\7-Zip\7z.exe" d rtxgi-ori.zip bin/shaders/framework/spirv/passes/ -mmt -mx9
move /Y rtxgi-ori.zip i:\