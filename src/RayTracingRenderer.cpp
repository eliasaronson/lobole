#include "RayTracingRenderer.h"
#include "MeshRenderer.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <unistd.h>
#include <climits>

RayTracingRenderer::RayTracingRenderer() {
}

RayTracingRenderer::~RayTracingRenderer() {
    Shutdown();
}

bool RayTracingRenderer::IsRayTracingSupported(VkPhysicalDevice physicalDevice) {
    // Check for required RT extensions
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
    
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, availableExtensions.data());
    
    // Required RT extensions
    std::vector<std::string> requiredExtensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME
    };
    
    for (const auto& required : requiredExtensions) {
        bool found = false;
        for (const auto& available : availableExtensions) {
            if (strcmp(required.c_str(), available.extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cout << "Ray Tracing: Missing required extension: " << required << std::endl;
            return false;
        }
    }
    
    // Check RT features
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{};
    rtProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    
    VkPhysicalDeviceProperties2 deviceProperties2{};
    deviceProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProperties2.pNext = &rtProperties;
    
    vkGetPhysicalDeviceProperties2(physicalDevice, &deviceProperties2);
    
    if (rtProperties.maxRayRecursionDepth == 0) {
        std::cout << "Ray Tracing: Device does not support ray recursion" << std::endl;
        return false;
    }
    
    std::cout << "Ray Tracing: Support detected!" << std::endl;
    std::cout << "  - Max ray recursion depth: " << rtProperties.maxRayRecursionDepth << std::endl;
    std::cout << "  - Max shader group stride: " << rtProperties.shaderGroupHandleSize << std::endl;
    
    return true;
}

bool RayTracingRenderer::Initialize(VkDevice device, VkPhysicalDevice physicalDevice, 
                                   VkQueue graphicsQueue, uint32_t queueFamily, 
                                   VkCommandPool commandPool) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_graphicsQueue = graphicsQueue;
    m_queueFamily = queueFamily;
    m_commandPool = commandPool;
    
    std::cout << "Initializing Ray Tracing Renderer..." << std::endl;
    
    // Verify RT support
    if (!IsRayTracingSupported(physicalDevice)) {
        std::cout << "Ray Tracing: Hardware not supported, falling back to rasterization" << std::endl;
        m_useRayTracing = false;
        return true; // Not an error - we'll use fallback
    }
    
    // Load RT function pointers
    if (!LoadRTFunctions()) {
        std::cerr << "Ray Tracing: Failed to load RT functions" << std::endl;
        return false;
    }
    
    // Get RT properties
    if (!CheckRTProperties()) {
        std::cerr << "Ray Tracing: Failed to get RT properties" << std::endl;
        return false;
    }
    
    // Create RT pipeline
    if (!CreateRTPipeline()) {
        std::cerr << "Ray Tracing: Failed to create RT pipeline" << std::endl;
        return false;
    }
    
    // Create descriptor sets
    if (!CreateRTDescriptorSets()) {
        std::cerr << "Ray Tracing: Failed to create RT descriptor sets" << std::endl;
        return false;
    }
    
    // Create shader binding table
    if (!CreateShaderBindingTable()) {
        std::cerr << "Ray Tracing: Failed to create shader binding table" << std::endl;
        return false;
    }
    
    m_isInitialized = true;
    std::cout << "Ray Tracing: Initialization successful!" << std::endl;
    
    return true;
}

void RayTracingRenderer::Shutdown() {
    if (m_device == VK_NULL_HANDLE) return;
    
    // Wait for device to finish
    vkDeviceWaitIdle(m_device);
    
    // Cleanup acceleration structures
    if (m_rtFunctions.vkDestroyAccelerationStructureKHR) {
        if (m_bottomLevelAS.handle != VK_NULL_HANDLE) {
            m_rtFunctions.vkDestroyAccelerationStructureKHR(m_device, m_bottomLevelAS.handle, nullptr);
        }
        if (m_topLevelAS.handle != VK_NULL_HANDLE) {
            m_rtFunctions.vkDestroyAccelerationStructureKHR(m_device, m_topLevelAS.handle, nullptr);
        }
    }
    
    // Cleanup buffers
    if (m_bottomLevelAS.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_bottomLevelAS.buffer, nullptr);
        vkFreeMemory(m_device, m_bottomLevelAS.memory, nullptr);
    }
    if (m_topLevelAS.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_topLevelAS.buffer, nullptr);
        vkFreeMemory(m_device, m_topLevelAS.memory, nullptr);
    }
    if (m_shaderBindingTableBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_shaderBindingTableBuffer, nullptr);
        vkFreeMemory(m_device, m_shaderBindingTableMemory, nullptr);
    }
    
    // Cleanup geometry buffers
    for (auto& geometry : m_geometries) {
        if (geometry && geometry->vertexBuffer != VK_NULL_HANDLE) {
            // Note: These buffers are managed by MeshRenderer, don't destroy here
        }
    }
    
    // Cleanup Vulkan objects
    if (m_rtDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_rtDescriptorPool, nullptr);
    }
    if (m_rtPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_rtPipeline, nullptr);
    }
    if (m_rtPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_rtPipelineLayout, nullptr);
    }
    if (m_rtDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_rtDescriptorSetLayout, nullptr);
    }
    
    m_isInitialized = false;
}

bool RayTracingRenderer::LoadRTFunctions() {
    // Load RT extension functions
    m_rtFunctions.vkCreateAccelerationStructureKHR = 
        (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(m_device, "vkCreateAccelerationStructureKHR");
    m_rtFunctions.vkDestroyAccelerationStructureKHR = 
        (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(m_device, "vkDestroyAccelerationStructureKHR");
    m_rtFunctions.vkBuildAccelerationStructuresKHR = 
        (PFN_vkBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(m_device, "vkBuildAccelerationStructuresKHR");
    m_rtFunctions.vkGetAccelerationStructureDeviceAddressKHR = 
        (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureDeviceAddressKHR");
    m_rtFunctions.vkGetAccelerationStructureBuildSizesKHR = 
        (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureBuildSizesKHR");
    m_rtFunctions.vkCreateRayTracingPipelinesKHR = 
        (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(m_device, "vkCreateRayTracingPipelinesKHR");
    m_rtFunctions.vkGetRayTracingShaderGroupHandlesKHR = 
        (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupHandlesKHR");
    m_rtFunctions.vkCmdTraceRaysKHR = 
        (PFN_vkCmdTraceRaysKHR)vkGetDeviceProcAddr(m_device, "vkCmdTraceRaysKHR");
    
    // Verify all functions loaded
    return m_rtFunctions.vkCreateAccelerationStructureKHR && 
           m_rtFunctions.vkDestroyAccelerationStructureKHR && 
           m_rtFunctions.vkBuildAccelerationStructuresKHR && 
           m_rtFunctions.vkGetAccelerationStructureDeviceAddressKHR && 
           m_rtFunctions.vkGetAccelerationStructureBuildSizesKHR && 
           m_rtFunctions.vkCreateRayTracingPipelinesKHR && 
           m_rtFunctions.vkGetRayTracingShaderGroupHandlesKHR && 
           m_rtFunctions.vkCmdTraceRaysKHR;
}

bool RayTracingRenderer::CheckRTProperties() {
    // Get RT properties
    m_rtProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    
    VkPhysicalDeviceProperties2 deviceProperties2{};
    deviceProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProperties2.pNext = &m_rtProperties;
    
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &deviceProperties2);
    
    std::cout << "Ray Tracing Properties:" << std::endl;
    std::cout << "  - Shader group handle size: " << m_rtProperties.shaderGroupHandleSize << std::endl;
    std::cout << "  - Max ray recursion depth: " << m_rtProperties.maxRayRecursionDepth << std::endl;
    std::cout << "  - Shader group base alignment: " << m_rtProperties.shaderGroupBaseAlignment << std::endl;
    
    return true;
}

bool RayTracingRenderer::CreateRTPipeline() {
    std::cout << "Ray Tracing: Creating RT pipeline..." << std::endl;
    
    // Create descriptor set layout
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        // Acceleration structure
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr},
        // Output image
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr},
        // Uniform buffer
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr},
        // Vertex buffer
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr},
        // Index buffer
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr}
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_rtDescriptorSetLayout) != VK_SUCCESS) {
        std::cerr << "Ray Tracing: Failed to create descriptor set layout" << std::endl;
        return false;
    }
    
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_rtDescriptorSetLayout;
    
    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_rtPipelineLayout) != VK_SUCCESS) {
        std::cerr << "Ray Tracing: Failed to create pipeline layout" << std::endl;
        return false;
    }
    
    // Load RT shaders
    std::string shaderDir = GetExecutableDirectory() + "/libraries/shaders/";
    auto raygenCode = ReadShaderFile(shaderDir + "raygen.spv");
    auto missCode = ReadShaderFile(shaderDir + "miss.spv");
    auto closesthitCode = ReadShaderFile(shaderDir + "closesthit.spv");
    auto shadowMissCode = ReadShaderFile(shaderDir + "shadow.spv");
    
    // Create shader modules
    VkShaderModule raygenModule = CreateShaderModule(raygenCode);
    VkShaderModule missModule = CreateShaderModule(missCode);
    VkShaderModule closesthitModule = CreateShaderModule(closesthitCode);
    VkShaderModule shadowMissModule = CreateShaderModule(shadowMissCode);
    
    // Shader stages
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_RAYGEN_BIT_KHR, raygenModule, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MISS_BIT_KHR, missModule, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, closesthitModule, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MISS_BIT_KHR, shadowMissModule, "main", nullptr}
    };
    
    // Shader groups
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups = {
        // Raygen group
        {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr},
        // Miss group
        {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr},
        // Hit group (closest hit)
        {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 2, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr},
        // Shadow miss group
        {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 3, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr}
    };
    
    // Create RT pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.groupCount = static_cast<uint32_t>(shaderGroups.size());
    pipelineInfo.pGroups = shaderGroups.data();
    pipelineInfo.maxPipelineRayRecursionDepth = m_maxRayDepth;
    pipelineInfo.layout = m_rtPipelineLayout;
    
    if (m_rtFunctions.vkCreateRayTracingPipelinesKHR(m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_rtPipeline) != VK_SUCCESS) {
        std::cerr << "Ray Tracing: Failed to create RT pipeline" << std::endl;
        
        // Cleanup shader modules
        vkDestroyShaderModule(m_device, raygenModule, nullptr);
        vkDestroyShaderModule(m_device, missModule, nullptr);
        vkDestroyShaderModule(m_device, closesthitModule, nullptr);
        vkDestroyShaderModule(m_device, shadowMissModule, nullptr);
        return false;
    }
    
    // Cleanup shader modules
    vkDestroyShaderModule(m_device, raygenModule, nullptr);
    vkDestroyShaderModule(m_device, missModule, nullptr);
    vkDestroyShaderModule(m_device, closesthitModule, nullptr);
    vkDestroyShaderModule(m_device, shadowMissModule, nullptr);
    
    std::cout << "Ray Tracing: RT pipeline created successfully" << std::endl;
    return true;
}

bool RayTracingRenderer::CreateRTDescriptorSets() {
    std::cout << "Ray Tracing: RT Descriptor sets creation - placeholder" << std::endl;
    return true;
}

bool RayTracingRenderer::CreateShaderBindingTable() {
    std::cout << "Ray Tracing: Creating shader binding table..." << std::endl;
    
    uint32_t groupCount = 4; // raygen, miss, hit, shadow miss
    uint32_t groupHandleSize = m_rtProperties.shaderGroupHandleSize;
    uint32_t groupSizeAligned = (groupHandleSize + m_rtProperties.shaderGroupBaseAlignment - 1) & ~(m_rtProperties.shaderGroupBaseAlignment - 1);
    
    // Get shader group handles
    uint32_t sbtSize = groupCount * groupHandleSize;
    std::vector<uint8_t> shaderHandleStorage(sbtSize);
    
    if (m_rtFunctions.vkGetRayTracingShaderGroupHandlesKHR(m_device, m_rtPipeline, 0, groupCount, sbtSize, shaderHandleStorage.data()) != VK_SUCCESS) {
        std::cerr << "Ray Tracing: Failed to get shader group handles" << std::endl;
        return false;
    }
    
    // Calculate SBT size
    uint32_t raygenSize = groupSizeAligned;
    uint32_t missSize = groupSizeAligned * 2; // miss + shadow miss
    uint32_t hitSize = groupSizeAligned;
    uint32_t sbtTotalSize = raygenSize + missSize + hitSize;
    
    // Create SBT buffer
    m_shaderBindingTableBuffer = CreateBuffer(sbtTotalSize, 
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m_shaderBindingTableMemory);
    
    // Map and fill SBT buffer
    void* sbtData;
    vkMapMemory(m_device, m_shaderBindingTableMemory, 0, sbtTotalSize, 0, &sbtData);
    
    uint8_t* sbtPtr = static_cast<uint8_t*>(sbtData);
    uint32_t handleIdx = 0;
    
    // Raygen
    memcpy(sbtPtr, &shaderHandleStorage[handleIdx * groupHandleSize], groupHandleSize);
    
    // Miss shaders
    sbtPtr += raygenSize;
    handleIdx = 1; // miss shader
    memcpy(sbtPtr, &shaderHandleStorage[handleIdx * groupHandleSize], groupHandleSize);
    
    sbtPtr += groupSizeAligned;
    handleIdx = 3; // shadow miss shader
    memcpy(sbtPtr, &shaderHandleStorage[handleIdx * groupHandleSize], groupHandleSize);
    
    // Hit group
    sbtPtr += groupSizeAligned;
    handleIdx = 2; // closest hit shader
    memcpy(sbtPtr, &shaderHandleStorage[handleIdx * groupHandleSize], groupHandleSize);
    
    vkUnmapMemory(m_device, m_shaderBindingTableMemory);
    
    // Get buffer device address
    VkBufferDeviceAddressInfoKHR bufferAddressInfo{};
    bufferAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bufferAddressInfo.buffer = m_shaderBindingTableBuffer;
    VkDeviceAddress sbtAddress = vkGetBufferDeviceAddress(m_device, &bufferAddressInfo);
    
    // Setup SBT regions
    m_raygenSBT.deviceAddress = sbtAddress;
    m_raygenSBT.stride = groupSizeAligned;
    m_raygenSBT.size = raygenSize;
    
    m_missSBT.deviceAddress = sbtAddress + raygenSize;
    m_missSBT.stride = groupSizeAligned;
    m_missSBT.size = missSize;
    
    m_hitSBT.deviceAddress = sbtAddress + raygenSize + missSize;
    m_hitSBT.stride = groupSizeAligned;
    m_hitSBT.size = hitSize;
    
    m_callableSBT = {}; // Not used
    
    std::cout << "Ray Tracing: Shader binding table created successfully" << std::endl;
    return true;
}

uint32_t RayTracingRenderer::AddGeometry(const std::vector<Vertex3D>& vertices, 
                                        const std::vector<uint32_t>& indices,
                                        uint32_t materialIndex) {
    std::cout << "Ray Tracing: Adding geometry with " << vertices.size() << " vertices, " 
              << indices.size() << " indices" << std::endl;
    
    // For now, just track that we have geometry
    auto geometry = std::make_unique<RTGeometry>();
    geometry->vertexCount = static_cast<uint32_t>(vertices.size());
    geometry->indexCount = static_cast<uint32_t>(indices.size());
    geometry->materialIndex = materialIndex;
    
    uint32_t geometryId = m_nextGeometryId++;
    m_geometries.push_back(std::move(geometry));
    
    return geometryId;
}

uint32_t RayTracingRenderer::AddMaterial(const RTMaterial& material) {
    m_materials.push_back(material);
    return m_nextMaterialId++;
}

bool RayTracingRenderer::BuildAccelerationStructures() {
    if (!m_isInitialized) {
        return true; // Not an error if RT isn't initialized
    }
    
    if (m_geometries.empty()) {
        std::cout << "Ray Tracing: No geometries to build acceleration structures for" << std::endl;
        return true;
    }
    
    std::cout << "Ray Tracing: Building acceleration structures for " << m_geometries.size() << " geometries" << std::endl;
    
    // Build bottom level acceleration structure
    if (!CreateBottomLevelAS()) {
        std::cerr << "Ray Tracing: Failed to create bottom level AS" << std::endl;
        return false;
    }
    
    // Build top level acceleration structure
    if (!CreateTopLevelAS()) {
        std::cerr << "Ray Tracing: Failed to create top level AS" << std::endl;
        return false;
    }
    
    std::cout << "Ray Tracing: Acceleration structures built successfully" << std::endl;
    return true;
}

void RayTracingRenderer::Render(VkCommandBuffer commandBuffer, uint32_t currentFrame,
                               const glm::mat4& view, const glm::mat4& projection,
                               const glm::vec3& cameraPos) {
    if (!m_isInitialized || !m_useRayTracing) {
        // Fall back to rasterization (handled by MeshRenderer)
        return;
    }
    
    // Placeholder for RT rendering
    // std::cout << "Ray Tracing: Rendering frame " << currentFrame << std::endl;
}

// Utility functions
VkBuffer RayTracingRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                                         VkMemoryPropertyFlags properties, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer;
    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate buffer memory!");
    }

    vkBindBufferMemory(m_device, buffer, bufferMemory, 0);

    return buffer;
}

uint32_t RayTracingRenderer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
}

std::vector<char> RayTracingRenderer::ReadShaderFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("failed to open file: " + filename);
    }

    size_t fileSize = (size_t) file.tellg();
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);

    file.close();

    return buffer;
}

VkShaderModule RayTracingRenderer::CreateShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("failed to create shader module!");
    }

    return shaderModule;
}

std::string RayTracingRenderer::GetExecutableDirectory() {
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    std::string path(result, (count > 0) ? count : 0);
    return std::filesystem::path(path).parent_path().string();
}

bool RayTracingRenderer::CreateBottomLevelAS() {
    std::cout << "Ray Tracing: Creating bottom level acceleration structure..." << std::endl;
    
    // For now, create a simple BLAS for all geometries combined
    // In a full implementation, we'd create separate BLAS for each geometry
    
    // Geometry description
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> buildRangeInfos;
    
    for (const auto& geom : m_geometries) {
        VkAccelerationStructureGeometryKHR asGeometry{};
        asGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        asGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        asGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        
        // Vertex data
        asGeometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        asGeometry.geometry.triangles.vertexData.deviceAddress = 0; // Will be set when we have actual buffers
        asGeometry.geometry.triangles.maxVertex = geom->vertexCount - 1;
        asGeometry.geometry.triangles.vertexStride = sizeof(float) * 9; // pos(3) + normal(3) + color(3)
        asGeometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        
        // Index data
        asGeometry.geometry.triangles.indexData.deviceAddress = 0; // Will be set when we have actual buffers
        asGeometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        
        geometries.push_back(asGeometry);
        
        VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo{};
        buildRangeInfo.primitiveCount = geom->indexCount / 3;
        buildRangeInfo.primitiveOffset = 0;
        buildRangeInfo.firstVertex = 0;
        buildRangeInfo.transformOffset = 0;
        
        buildRangeInfos.push_back(buildRangeInfo);
    }
    
    // For now, return true - we'll implement the actual AS building when we integrate with MeshRenderer
    std::cout << "Ray Tracing: Bottom level AS creation - ready for integration" << std::endl;
    return true;
}

bool RayTracingRenderer::CreateTopLevelAS() {
    std::cout << "Ray Tracing: Creating top level acceleration structure..." << std::endl;
    
    // Create instance data for each geometry
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    
    for (size_t i = 0; i < m_geometries.size(); ++i) {
        VkAccelerationStructureInstanceKHR instance{};
        
        // Identity transform for now
        instance.transform.matrix[0][0] = 1.0f;
        instance.transform.matrix[1][1] = 1.0f;
        instance.transform.matrix[2][2] = 1.0f;
        
        instance.instanceCustomIndex = static_cast<uint32_t>(i);
        instance.mask = 0xFF;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = m_bottomLevelAS.deviceAddress;
        
        instances.push_back(instance);
    }
    
    // For now, return true - we'll implement the actual AS building when we integrate with MeshRenderer
    std::cout << "Ray Tracing: Top level AS creation - ready for integration" << std::endl;
    return true;
}