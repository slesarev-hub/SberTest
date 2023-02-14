#!/bin/bash

rm -rf build
mkdir build
cd build

cmake ..

# cmake -DCMAKE_BUILD_TYPE=DEBUG \
#       -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
#        ..

make -j