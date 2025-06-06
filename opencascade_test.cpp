// Simple test to demonstrate the OpenCASCADE BRepPrimAPI_MakeBox fix
#include <iostream>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <TopoDS_Shape.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <Precision.hxx>

int main() {
    std::cout << "Testing OpenCASCADE primitive creation..." << std::endl;
    std::cout << "Precision::Confusion(): " << Precision::Confusion() << std::endl;
    
    // Create a box using the working approach
    double width = 2.0, height = 1.5, depth = 1.0;
    
    BRepPrimAPI_MakeBox boxMaker(width, height, depth);
    std::cout << "BRepPrimAPI_MakeBox constructor IsDone: " << (boxMaker.IsDone() ? "YES" : "NO") << std::endl;
    
    // The key fix: call Build() if not done
    if (!boxMaker.IsDone()) {
        std::cout << "Calling Build()..." << std::endl;
        boxMaker.Build();
        std::cout << "After Build() IsDone: " << (boxMaker.IsDone() ? "YES" : "NO") << std::endl;
    }
    
    if (boxMaker.IsDone()) {
        TopoDS_Shape box = boxMaker.Shape();
        
        // Calculate volume
        GProp_GProps props;
        BRepGProp::VolumeProperties(box, props);
        double volume = props.Mass();
        
        std::cout << "Box created successfully!" << std::endl;
        std::cout << "Expected volume: " << (width * height * depth) << std::endl;
        std::cout << "Actual volume: " << volume << std::endl;
    } else {
        std::cout << "Box creation failed!" << std::endl;
    }
    
    // Test sphere
    BRepPrimAPI_MakeSphere sphereMaker(1.0);
    if (!sphereMaker.IsDone()) {
        sphereMaker.Build();
    }
    
    if (sphereMaker.IsDone()) {
        std::cout << "Sphere created successfully!" << std::endl;
    } else {
        std::cout << "Sphere creation failed!" << std::endl;
    }
    
    return 0;
}