cmake -S . -B build -G "Visual Studio 16 2019" -A Win32 -DSDL3_DIR=./ThirdParty/SDL3-VC/cmake
cmake --build build --config Release
pause