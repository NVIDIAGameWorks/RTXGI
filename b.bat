@echo off
pushd %CD%
cd build
cmake --build . --config Release
popd
