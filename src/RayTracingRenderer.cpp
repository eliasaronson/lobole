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
    m_rtFunctions.vkCmdBuildAccelerationStructuresKHR =
        (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(m_device, "vkCmdBuildAccelerationStructuresKHR");
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

VkDeviceAddress RayTracingRenderer::GetBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(m_device, &info);
}

uint32_t RayTracingRenderer::AddGeometry(const std::vector<Vertex3D>& vertices,
                                        const std::vector<uint32_t>& indices,
                                        uint32_t materialIndex) {
    auto geometry = std::make_unique<RTGeometry>();
    geometry->vertexCount = static_cast<uint32_t>(vertices.size());
    geometry->indexCount = static_cast<uint32_t>(indices.size());
    geometry->materialIndex = materialIndex;

    // Vertex + index buffers usable both as acceleration-structure build input
    // and as storage buffers the closest-hit shader reads by index. Host-visible
    // is fine for an on-demand offline render (no per-frame churn).
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VkDeviceSize vSize = vertices.size() * sizeof(Vertex3D);
    geometry->vertexBuffer =
        CreateBuffer(vSize, usage, props, geometry->vertexMemory);
    void* vmap = nullptr;
    vkMapMemory(m_device, geometry->vertexMemory, 0, vSize, 0, &vmap);
    memcpy(vmap, vertices.data(), static_cast<size_t>(vSize));
    vkUnmapMemory(m_device, geometry->vertexMemory);
    geometry->vertexAddress = GetBufferDeviceAddress(geometry->vertexBuffer);

    VkDeviceSize iSize = indices.size() * sizeof(uint32_t);
    geometry->indexBuffer =
        CreateBuffer(iSize, usage, props, geometry->indexMemory);
    void* imap = nullptr;
    vkMapMemory(m_device, geometry->indexMemory, 0, iSize, 0, &imap);
    memcpy(imap, indices.data(), static_cast<size_t>(iSize));
    vkUnmapMemory(m_device, geometry->indexMemory);
    geometry->indexAddress = GetBufferDeviceAddress(geometry->indexBuffer);

    uint32_t geometryId = m_nextGeometryId++;
    m_geometries.push_back(std::move(geometry));
    return geometryId;
}

void RayTracingRenderer::DestroyAccelerationStructure(RTAccelerationStructure& as) {
    if (as.handle != VK_NULL_HANDLE)
        m_rtFunctions.vkDestroyAccelerationStructureKHR(m_device, as.handle, nullptr);
    if (as.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(m_device, as.buffer, nullptr);
    if (as.memory != VK_NULL_HANDLE)
        vkFreeMemory(m_device, as.memory, nullptr);
    as = {};
}

void RayTracingRenderer::ClearGeometry() {
    if (!m_isInitialized) return;
    vkDeviceWaitIdle(m_device);
    DestroyAccelerationStructure(m_topLevelAS);
    for (auto& g : m_geometries) {
        DestroyAccelerationStructure(g->blas);
        if (g->vertexBuffer) vkDestroyBuffer(m_device, g->vertexBuffer, nullptr);
        if (g->vertexMemory) vkFreeMemory(m_device, g->vertexMemory, nullptr);
        if (g->indexBuffer) vkDestroyBuffer(m_device, g->indexBuffer, nullptr);
        if (g->indexMemory) vkFreeMemory(m_device, g->indexMemory, nullptr);
    }
    m_geometries.clear();
    m_nextGeometryId = 1;
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

    // A buffer used for a device address (e.g. the SBT) must have its memory
    // allocated with the matching flag, or vkBindBufferMemory is invalid.
    VkMemoryAllocateFlagsInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        allocInfo.pNext = &flagsInfo;
    }

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

VkCommandBuffer RayTracingRenderer::BeginSingleTimeCommands() {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = m_commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &alloc, &cb);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    return cb;
}

void RayTracingRenderer::EndSingleTimeCommands(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cb);
}

bool RayTracingRenderer::CreateBottomLevelAS() {
    // One BLAS per geometry (body). Built device-side from the vertex/index
    // buffers uploaded in AddGeometry.
    for (auto& geom : m_geometries) {
        VkAccelerationStructureGeometryKHR asGeom{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        asGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        asGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = asGeom.geometry.triangles;
        tri.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; // position is the first vec3
        tri.vertexData.deviceAddress = geom->vertexAddress;
        tri.vertexStride = sizeof(Vertex3D);
        tri.maxVertex = geom->vertexCount - 1;
        tri.indexType = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress = geom->indexAddress;

        uint32_t primCount = geom->indexCount / 3;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &asGeom;

        VkAccelerationStructureBuildSizesInfoKHR sizeInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        m_rtFunctions.vkGetAccelerationStructureBuildSizesKHR(
            m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
            &primCount, &sizeInfo);

        geom->blas.buffer = CreateBuffer(
            sizeInfo.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, geom->blas.memory);

        VkAccelerationStructureCreateInfoKHR createInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.buffer = geom->blas.buffer;
        createInfo.size = sizeInfo.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        m_rtFunctions.vkCreateAccelerationStructureKHR(m_device, &createInfo,
                                                       nullptr, &geom->blas.handle);

        VkDeviceMemory scratchMem = VK_NULL_HANDLE;
        VkBuffer scratch = CreateBuffer(
            sizeInfo.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchMem);

        buildInfo.dstAccelerationStructure = geom->blas.handle;
        buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(scratch);

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = primCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        VkCommandBuffer cb = BeginSingleTimeCommands();
        m_rtFunctions.vkCmdBuildAccelerationStructuresKHR(cb, 1, &buildInfo,
                                                          &pRange);
        EndSingleTimeCommands(cb);

        vkDestroyBuffer(m_device, scratch, nullptr);
        vkFreeMemory(m_device, scratchMem, nullptr);

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addrInfo.accelerationStructure = geom->blas.handle;
        geom->blas.deviceAddress =
            m_rtFunctions.vkGetAccelerationStructureDeviceAddressKHR(m_device,
                                                                     &addrInfo);
    }
    std::cout << "Ray Tracing: built " << m_geometries.size() << " BLAS"
              << std::endl;
    return true;
}

bool RayTracingRenderer::CreateTopLevelAS() {
    // One instance per body, referencing its BLAS. Geometry is already in world
    // space, so the instance transform is identity.
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    for (size_t i = 0; i < m_geometries.size(); ++i) {
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform.matrix[0][0] = 1.0f;
        inst.transform.matrix[1][1] = 1.0f;
        inst.transform.matrix[2][2] = 1.0f;
        inst.instanceCustomIndex = static_cast<uint32_t>(i);
        inst.mask = 0xFF;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = m_geometries[i]->blas.deviceAddress;
        instances.push_back(inst);
    }

    VkDeviceSize instSize =
        instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    VkDeviceMemory instMem = VK_NULL_HANDLE;
    VkBuffer instBuf = CreateBuffer(
        instSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        instMem);
    void* map = nullptr;
    vkMapMemory(m_device, instMem, 0, instSize, 0, &map);
    memcpy(map, instances.data(), static_cast<size_t>(instSize));
    vkUnmapMemory(m_device, instMem);

    VkAccelerationStructureGeometryKHR asGeom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    asGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    asGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    asGeom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    asGeom.geometry.instances.arrayOfPointers = VK_FALSE;
    asGeom.geometry.instances.data.deviceAddress = GetBufferDeviceAddress(instBuf);

    uint32_t primCount = static_cast<uint32_t>(instances.size());

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &asGeom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_rtFunctions.vkGetAccelerationStructureBuildSizesKHR(
        m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
        &primCount, &sizeInfo);

    m_topLevelAS.buffer = CreateBuffer(
        sizeInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_topLevelAS.memory);

    VkAccelerationStructureCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    createInfo.buffer = m_topLevelAS.buffer;
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    m_rtFunctions.vkCreateAccelerationStructureKHR(m_device, &createInfo, nullptr,
                                                   &m_topLevelAS.handle);

    VkDeviceMemory scratchMem = VK_NULL_HANDLE;
    VkBuffer scratch = CreateBuffer(
        sizeInfo.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchMem);

    buildInfo.dstAccelerationStructure = m_topLevelAS.handle;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(scratch);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

    VkCommandBuffer cb = BeginSingleTimeCommands();
    m_rtFunctions.vkCmdBuildAccelerationStructuresKHR(cb, 1, &buildInfo, &pRange);
    EndSingleTimeCommands(cb);

    vkDestroyBuffer(m_device, scratch, nullptr);
    vkFreeMemory(m_device, scratchMem, nullptr);
    vkDestroyBuffer(m_device, instBuf, nullptr);
    vkFreeMemory(m_device, instMem, nullptr);

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addrInfo.accelerationStructure = m_topLevelAS.handle;
    m_topLevelAS.deviceAddress =
        m_rtFunctions.vkGetAccelerationStructureDeviceAddressKHR(m_device,
                                                                 &addrInfo);
    std::cout << "Ray Tracing: built TLAS over " << instances.size()
              << " instances" << std::endl;
    return true;
}