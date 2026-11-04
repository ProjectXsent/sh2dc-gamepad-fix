cmake -S . -B build -A Win32 -DSDL3_DIR=./ThirdParty/SDL3-VC/cmake
cmake --build build --config Release
pause