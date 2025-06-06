#!/bin/bash

# Create build directory for shaders
mkdir -p build/shaders

# Compile vertex shader
glslc src/shaders/mesh.vert -o build/shaders/mesh_vert.spv

# Compile fragment shader  
glslc src/shaders/mesh.frag -o build/shaders/mesh_frag.spv

echo "Shaders compiled successfully!"