#!/bin/sh

# Generate protobuf files
echo "Generating protobuf files..."
protoc --cpp_out=. cast_channel.proto

rm -rf build
mkdir build
cd build
cmake ..
make
