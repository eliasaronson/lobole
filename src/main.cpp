#include "Application.h"
#include <iostream>

int main(int argc, char** argv) {
    Application app;
    
    if (!app.Initialize()) {
        std::cerr << "Failed to initialize application" << std::endl;
        return -1;
    }
    
    app.Run();
    app.Shutdown();
    
    return 0;
}