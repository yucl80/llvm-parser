rm -rf build
mkdir build && cd build
cmake -DCMAKE_C_COMPILER=wllvm -DCMAKE_CXX_COMPILER=wllvm++ ..
make -j$(nproc)
