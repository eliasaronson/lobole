#include "render/RayTracingRenderer.h"
#include "render/StillOutlines.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "core/Conventions.h"

#include "render/MeshRenderer.h"
#include "render/VulkanCommon.h"
#include "thirdparty/stb_image_write.h" // declarations only; impl lives in Screenshot.cpp
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

RayTracingRenderer::RayTracingRenderer() {}

RayTracingRenderer::~RayTracingRenderer() { Shutdown(); }

bool RayTracingRenderer::IsRayTracingSupported(
    VkPhysicalDevice physicalDevice) {
  // Check for required RT extensions
  uint32_t extensionCount;
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                       nullptr);

  std::vector<VkExtensionProperties> availableExtensions(extensionCount);
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                       availableExtensions.data());

  // Required RT extensions
  std::vector<std::string> requiredExtensions = {
      VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
      VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
      VK_KHR_RAY_QUERY_EXTENSION_NAME,
      VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
      VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};

  for (const auto& required : requiredExtensions) {
    bool found = false;
    for (const auto& available : availableExtensions) {
      if (strcmp(required.c_str(), available.extensionName) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      LOG_INFO("Ray Tracing: Missing required extension: {}", required);
      return false;
    }
  }

  // Check RT features
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{};
  rtProperties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

  VkPhysicalDeviceProperties2 deviceProperties2{};
  deviceProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  deviceProperties2.pNext = &rtProperties;

  vkGetPhysicalDeviceProperties2(physicalDevice, &deviceProperties2);

  if (rtProperties.maxRayRecursionDepth == 0) {
    LOG_INFO("Ray Tracing: Device does not support ray recursion");
    return false;
  }

  LOG_INFO("Ray Tracing: Support detected!");
  LOG_INFO(" - Max ray recursion depth: {}", rtProperties.maxRayRecursionDepth);
  LOG_INFO(" - Max shader group stride: {}",
           rtProperties.shaderGroupHandleSize);

  return true;
}

bool RayTracingRenderer::Initialize(VkDevice device,
                                    VkPhysicalDevice physicalDevice,
                                    VkQueue graphicsQueue, uint32_t queueFamily,
                                    VkCommandPool commandPool) {
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_graphicsQueue = graphicsQueue;
  m_queueFamily = queueFamily;
  m_commandPool = commandPool;

  LOG_INFO("Initializing Ray Tracing Renderer...");

  // Verify RT support
  if (!IsRayTracingSupported(physicalDevice)) {
    LOG_INFO(
        "Ray Tracing: Hardware not supported, falling back to rasterization");
    m_useRayTracing = false;
    return true; // Not an error - we'll use fallback
  }

  // Load RT function pointers
  if (!LoadRTFunctions()) {
    LOG_ERROR("Ray Tracing: Failed to load RT functions");
    return false;
  }

  // Get RT properties
  if (!CheckRTProperties()) {
    LOG_ERROR("Ray Tracing: Failed to get RT properties");
    return false;
  }

  // Create RT pipeline
  if (!CreateRTPipeline()) {
    LOG_ERROR("Ray Tracing: Failed to create RT pipeline");
    return false;
  }

  // Create descriptor sets
  if (!CreateRTDescriptorSets()) {
    LOG_ERROR("Ray Tracing: Failed to create RT descriptor sets");
    return false;
  }

  // Create shader binding table
  if (!CreateShaderBindingTable()) {
    LOG_ERROR("Ray Tracing: Failed to create shader binding table");
    return false;
  }

  m_isInitialized = true;
  LOG_INFO("Ray Tracing: Initialization successful!");

  return true;
}

void RayTracingRenderer::Shutdown() {
  if (m_device == VK_NULL_HANDLE)
    return;

  // Wait for device to finish
  vkDeviceWaitIdle(m_device);

  // Cleanup any in-flight offline-render resources
  DestroyStillResources();
  m_stillActive = false;

  // The scene: the top-level AS, and every geometry's BLAS + vertex/index
  // buffers. This was an empty loop with a note saying MeshRenderer owned
  // these — it does not. AddGeometry allocates the vertex/index buffers here
  // and CreateBottomLevelAS allocates the BLAS here, so both leaked on every
  // exit; ASan put the number on it (~108 KB of host allocations still live,
  // and a BLAS per body of device memory behind them).
  //
  // ClearGeometry() FIRST and alone: it is the one owner of the TLAS and the
  // per-geometry resources, and it nulls what it frees. The by-hand pass that
  // used to sit here destroyed the TLAS without clearing the handles, so
  // adding this call after it would have freed each one twice.
  ClearGeometry();

  // The legacy single-BLAS member, which predates the per-geometry ones and
  // is not part of m_geometries.
  DestroyAccelerationStructure(m_bottomLevelAS);

  if (m_shaderBindingTableBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(m_device, m_shaderBindingTableBuffer, nullptr);
    vkFreeMemory(m_device, m_shaderBindingTableMemory, nullptr);
    m_shaderBindingTableBuffer = VK_NULL_HANDLE;
    m_shaderBindingTableMemory = VK_NULL_HANDLE;
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
      (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(
          m_device, "vkCreateAccelerationStructureKHR");
  m_rtFunctions.vkDestroyAccelerationStructureKHR =
      (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(
          m_device, "vkDestroyAccelerationStructureKHR");
  m_rtFunctions.vkBuildAccelerationStructuresKHR =
      (PFN_vkBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(
          m_device, "vkBuildAccelerationStructuresKHR");
  m_rtFunctions.vkCmdBuildAccelerationStructuresKHR =
      (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(
          m_device, "vkCmdBuildAccelerationStructuresKHR");
  m_rtFunctions.vkGetAccelerationStructureDeviceAddressKHR =
      (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(
          m_device, "vkGetAccelerationStructureDeviceAddressKHR");
  m_rtFunctions.vkGetAccelerationStructureBuildSizesKHR =
      (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(
          m_device, "vkGetAccelerationStructureBuildSizesKHR");
  m_rtFunctions.vkCreateRayTracingPipelinesKHR =
      (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(
          m_device, "vkCreateRayTracingPipelinesKHR");
  m_rtFunctions.vkGetRayTracingShaderGroupHandlesKHR =
      (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetDeviceProcAddr(
          m_device, "vkGetRayTracingShaderGroupHandlesKHR");
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
  m_rtProperties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

  VkPhysicalDeviceProperties2 deviceProperties2{};
  deviceProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  deviceProperties2.pNext = &m_rtProperties;

  vkGetPhysicalDeviceProperties2(m_physicalDevice, &deviceProperties2);

  LOG_INFO("Ray Tracing Properties:");
  LOG_INFO(" - Shader group handle size: {}",
           m_rtProperties.shaderGroupHandleSize);
  LOG_INFO(" - Max ray recursion depth: {}",
           m_rtProperties.maxRayRecursionDepth);
  LOG_INFO(" - Shader group base alignment: {}",
           m_rtProperties.shaderGroupBaseAlignment);

  return true;
}

bool RayTracingRenderer::CreateRTPipeline() {
  LOG_INFO("Ray Tracing: Creating RT pipeline...");

  // Create descriptor set layout. Bindings are shared by the whole offline
  // pipeline; per-body vertex/index data is reached through buffer-reference
  // addresses in the GeometryInfo SSBO (binding 3), not per-body descriptors.
  const VkShaderStageFlags allRT =
      VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
      VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
  std::vector<VkDescriptorSetLayoutBinding> bindings = {
      // 0: TLAS
      {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
       VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
       nullptr},
      // 1: rgba32f accumulation image
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
       nullptr},
      // 2: camera + light UBO
      {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allRT, nullptr},
      // 3: GeometryInfo[] (per-body vertex/index addresses + material index).
      //    The any-hit stage reads it too, to spare the ground plane a cut.
      {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
       nullptr},
      // 4: RTMaterial[]. Any-hit reads it too: a shadow ray passes glass.
      {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
       nullptr},
      // 5/6/7: IBL irradiance / prefilter / BRDF LUT
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
       nullptr},
      {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
       nullptr},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr},
      // 8: full-res environment HDR (background + near-mirror reflections;
      //    the prefilter chain's base mip is too low-res for either)
      {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
       nullptr},
      // 9: rgba32ui outline G-buffer (raygen writes it on the aux launch)
      {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
       nullptr}};

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();

  if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                  &m_rtDescriptorSetLayout) != VK_SUCCESS) {
    LOG_ERROR("Ray Tracing: Failed to create descriptor set layout");
    return false;
  }

  // Create pipeline layout. Push constant carries the per-sample RNG seed data
  // to the raygen stage (sampleIndex + frameSeed).
  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  pushRange.offset = 0;
  // {uint sampleIndex; uint frameSeed; uint auxPass; uint auxRow0;
  //  uint auxHeight;} -- the last two place an outline strip in the image.
  pushRange.size = 5 * sizeof(uint32_t);

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &m_rtDescriptorSetLayout;
  pipelineLayoutInfo.pushConstantRangeCount = 1;
  pipelineLayoutInfo.pPushConstantRanges = &pushRange;

  if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr,
                             &m_rtPipelineLayout) != VK_SUCCESS) {
    LOG_ERROR("Ray Tracing: Failed to create pipeline layout");
    return false;
  }

  VkShaderModule raygenModule =
      vkutil::LoadShaderModule(m_device, "raygen.spv");
  VkShaderModule missModule = vkutil::LoadShaderModule(m_device, "miss.spv");
  VkShaderModule closesthitModule =
      vkutil::LoadShaderModule(m_device, "closesthit.spv");
  VkShaderModule shadowMissModule =
      vkutil::LoadShaderModule(m_device, "shadow.spv");
  VkShaderModule sectionAnyHitModule =
      vkutil::LoadShaderModule(m_device, "section.spv");
  if (raygenModule == VK_NULL_HANDLE || missModule == VK_NULL_HANDLE ||
      closesthitModule == VK_NULL_HANDLE ||
      shadowMissModule == VK_NULL_HANDLE ||
      sectionAnyHitModule == VK_NULL_HANDLE) {
    LOG_ERROR("Ray Tracing: failed to load RT shaders");
    for (VkShaderModule m : {raygenModule, missModule, closesthitModule,
                             shadowMissModule, sectionAnyHitModule})
      vkDestroyShaderModule(m_device, m, nullptr);
    return false;
  }

  // Shader stages
  std::vector<VkPipelineShaderStageCreateInfo> shaderStages = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_RAYGEN_BIT_KHR, raygenModule, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_MISS_BIT_KHR, missModule, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, closesthitModule, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_MISS_BIT_KHR, shadowMissModule, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_ANY_HIT_BIT_KHR, sectionAnyHitModule, "main", nullptr}};

  // Shader groups
  std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups = {
      // Raygen group
      {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
       VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR,
       VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr},
      // Miss group
      {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
       VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, VK_SHADER_UNUSED_KHR,
       VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr},
      // Hit group (closest hit + the section any-hit). Filling the any-hit
      // slot that was already here leaves the GROUP COUNT alone, so the
      // shader binding table's strides and regions are untouched.
      {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
       VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
       VK_SHADER_UNUSED_KHR, 2, 4, VK_SHADER_UNUSED_KHR, nullptr},
      // Shadow miss group
      {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
       VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 3, VK_SHADER_UNUSED_KHR,
       VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr}};

  // Create RT pipeline
  VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
  pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
  pipelineInfo.pStages = shaderStages.data();
  pipelineInfo.groupCount = static_cast<uint32_t>(shaderGroups.size());
  pipelineInfo.pGroups = shaderGroups.data();
  pipelineInfo.maxPipelineRayRecursionDepth = m_maxRayDepth;
  pipelineInfo.layout = m_rtPipelineLayout;

  if (m_rtFunctions.vkCreateRayTracingPipelinesKHR(
          m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
          &m_rtPipeline) != VK_SUCCESS) {
    LOG_ERROR("Ray Tracing: Failed to create RT pipeline");

    // Cleanup shader modules
    vkDestroyShaderModule(m_device, raygenModule, nullptr);
    vkDestroyShaderModule(m_device, missModule, nullptr);
    vkDestroyShaderModule(m_device, closesthitModule, nullptr);
    vkDestroyShaderModule(m_device, shadowMissModule, nullptr);
    vkDestroyShaderModule(m_device, sectionAnyHitModule, nullptr);
    return false;
  }

  // Cleanup shader modules
  vkDestroyShaderModule(m_device, raygenModule, nullptr);
  vkDestroyShaderModule(m_device, missModule, nullptr);
  vkDestroyShaderModule(m_device, closesthitModule, nullptr);
  vkDestroyShaderModule(m_device, shadowMissModule, nullptr);
  vkDestroyShaderModule(m_device, sectionAnyHitModule, nullptr);

  LOG_INFO("Ray Tracing: RT pipeline created successfully");
  return true;
}

bool RayTracingRenderer::CreateRTDescriptorSets() {
  // The offline pipeline allocates its descriptor pool + set per render in
  // BeginStill (the set binds render-sized, per-scene resources), so there is
  // nothing to create at init time. Kept for the Initialize() call sequence.
  return true;
}

bool RayTracingRenderer::CreateShaderBindingTable() {
  LOG_INFO("Ray Tracing: Creating shader binding table...");

  uint32_t groupCount = 4; // raygen, miss, hit, shadow miss
  uint32_t groupHandleSize = m_rtProperties.shaderGroupHandleSize;
  uint32_t groupSizeAligned =
      (groupHandleSize + m_rtProperties.shaderGroupBaseAlignment - 1) &
      ~(m_rtProperties.shaderGroupBaseAlignment - 1);

  // Get shader group handles
  uint32_t sbtSize = groupCount * groupHandleSize;
  std::vector<uint8_t> shaderHandleStorage(sbtSize);

  if (m_rtFunctions.vkGetRayTracingShaderGroupHandlesKHR(
          m_device, m_rtPipeline, 0, groupCount, sbtSize,
          shaderHandleStorage.data()) != VK_SUCCESS) {
    LOG_ERROR("Ray Tracing: Failed to get shader group handles");
    return false;
  }

  // Calculate SBT size
  uint32_t raygenSize = groupSizeAligned;
  uint32_t missSize = groupSizeAligned * 2; // miss + shadow miss
  uint32_t hitSize = groupSizeAligned;
  uint32_t sbtTotalSize = raygenSize + missSize + hitSize;

  // Create SBT buffer
  m_shaderBindingTableBuffer =
      CreateBuffer(sbtTotalSize,
                   VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   m_shaderBindingTableMemory);

  // Map and fill SBT buffer
  void* sbtData;
  vkMapMemory(m_device, m_shaderBindingTableMemory, 0, sbtTotalSize, 0,
              &sbtData);

  uint8_t* sbtPtr = static_cast<uint8_t*>(sbtData);
  uint32_t handleIdx = 0;

  // Raygen
  memcpy(sbtPtr, &shaderHandleStorage[handleIdx * groupHandleSize],
         groupHandleSize);

  // Miss shaders
  sbtPtr += raygenSize;
  handleIdx = 1; // miss shader
  memcpy(sbtPtr, &shaderHandleStorage[handleIdx * groupHandleSize],
         groupHandleSize);

  sbtPtr += groupSizeAligned;
  handleIdx = 3; // shadow miss shader
  memcpy(sbtPtr, &shaderHandleStorage[handleIdx * groupHandleSize],
         groupHandleSize);

  // Hit group
  sbtPtr += groupSizeAligned;
  handleIdx = 2; // closest hit shader
  memcpy(sbtPtr, &shaderHandleStorage[handleIdx * groupHandleSize],
         groupHandleSize);

  vkUnmapMemory(m_device, m_shaderBindingTableMemory);

  // Get buffer device address
  VkBufferDeviceAddressInfoKHR bufferAddressInfo{};
  bufferAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  bufferAddressInfo.buffer = m_shaderBindingTableBuffer;
  VkDeviceAddress sbtAddress =
      vkGetBufferDeviceAddress(m_device, &bufferAddressInfo);

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

  LOG_INFO("Ray Tracing: Shader binding table created successfully");
  return true;
}

VkDeviceAddress RayTracingRenderer::GetBufferDeviceAddress(VkBuffer buffer) {
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = buffer;
  return vkGetBufferDeviceAddress(m_device, &info);
}

uint32_t RayTracingRenderer::AddGeometry(const std::vector<Vertex3D>& vertices,
                                         const std::vector<uint32_t>& indices,
                                         uint32_t materialIndex,
                                         uint32_t flags) {
  auto geometry = std::make_unique<RTGeometry>();
  geometry->vertexCount = static_cast<uint32_t>(vertices.size());
  geometry->indexCount = static_cast<uint32_t>(indices.size());
  geometry->materialIndex = materialIndex;
  geometry->flags = flags;

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

void RayTracingRenderer::DestroyAccelerationStructure(
    RTAccelerationStructure& as) {
  if (as.handle != VK_NULL_HANDLE)
    m_rtFunctions.vkDestroyAccelerationStructureKHR(m_device, as.handle,
                                                    nullptr);
  if (as.buffer != VK_NULL_HANDLE)
    vkDestroyBuffer(m_device, as.buffer, nullptr);
  if (as.memory != VK_NULL_HANDLE)
    vkFreeMemory(m_device, as.memory, nullptr);
  as = {};
}

void RayTracingRenderer::ClearGeometry() {
  if (!m_isInitialized)
    return;
  vkDeviceWaitIdle(m_device);
  DestroyAccelerationStructure(m_topLevelAS);
  for (auto& g : m_geometries) {
    DestroyAccelerationStructure(g->blas);
    if (g->vertexBuffer)
      vkDestroyBuffer(m_device, g->vertexBuffer, nullptr);
    if (g->vertexMemory)
      vkFreeMemory(m_device, g->vertexMemory, nullptr);
    if (g->indexBuffer)
      vkDestroyBuffer(m_device, g->indexBuffer, nullptr);
    if (g->indexMemory)
      vkFreeMemory(m_device, g->indexMemory, nullptr);
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
    LOG_INFO("Ray Tracing: No geometries to build acceleration structures for");
    return true;
  }

  LOG_INFO("Ray Tracing: Building acceleration structures for {} geometries",
           m_geometries.size());

  // Build bottom level acceleration structure
  if (!CreateBottomLevelAS()) {
    LOG_ERROR("Ray Tracing: Failed to create bottom level AS");
    return false;
  }

  // Build top level acceleration structure
  if (!CreateTopLevelAS()) {
    LOG_ERROR("Ray Tracing: Failed to create top level AS");
    return false;
  }

  LOG_INFO("Ray Tracing: Acceleration structures built successfully");
  return true;
}

void RayTracingRenderer::Render(VkCommandBuffer commandBuffer,
                                uint32_t currentFrame, const glm::mat4& view,
                                const glm::mat4& projection,
                                const glm::vec3& cameraPos) {
  if (!m_isInitialized || !m_useRayTracing) {
    // Fall back to rasterization (handled by MeshRenderer)
    return;
  }

  // Placeholder for RT rendering (per-frame; would be LOG_TRACE if enabled).
}

// ---- Offline still render -----------------------------------------------

namespace {
// std430/glm mirror of the closest-hit GeometryInfo. The two device addresses
// become buffer_reference values (8 bytes each) on the GPU. Explicit padding to
// 32 bytes so the SSBO stride is unambiguous on both sides.
struct GeometryInfoGPU {
  VkDeviceAddress vertexAddress;
  VkDeviceAddress indexAddress;
  uint32_t materialIndex;
  uint32_t flags; // RTGeometryFlags
  uint32_t pad0;
  uint32_t pad1;
};

// Camera + light UBO, matching the layout declared in the RT shaders.
struct RTUniforms {
  glm::mat4 viewInverse;
  glm::mat4 projInverse;
  glm::vec4 viewPos;
  glm::vec4 lightDir;   // xyz = travel direction, w = intensity
  glm::vec4 lightColor; // rgb = color, w unused
  glm::vec4 ambientColor;
  glm::vec4 params; // x = prefilter mips, y = ray far, z = AO radius
  // The viewport's fill and rim (conv::kFillLight / kRimLight). Must stay
  // byte-identical to the block in raygen.rgen / miss.rmiss / closesthit.rchit.
  glm::vec4 fillDir; // xyz = travel direction, w = intensity
  glm::vec4 fillColor;
  glm::vec4 rimDir;
  glm::vec4 rimColor;
  // Section planes (ClipPlanes.h's convention: xyz = world normal, w =
  // distance, a hit removed when dot(n, p) > w; an all-zero slot disabled).
  glm::vec4 clipPlanes[kMaxClipPlanes];
  // x = how many of them are live. Kept as a count so the hot path can skip
  // the whole test with one compare instead of four length checks per ray.
  // y = 1 when some material transmits: shadow rays must then run any-hit,
  // which is where they pass through glass.
  glm::vec4 clipInfo;
};

glm::vec3 ACESFilm(const glm::vec3& x) {
  const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
  glm::vec3 num = x * (a * x + b);
  glm::vec3 den = x * (c * x + d) + e;
  return glm::clamp(num / den, glm::vec3(0.0f), glm::vec3(1.0f));
}
} // namespace

void RayTracingRenderer::TransitionImage(VkCommandBuffer cb, VkImage image,
                                         VkImageLayout oldLayout,
                                         VkImageLayout newLayout,
                                         VkPipelineStageFlags srcStage,
                                         VkPipelineStageFlags dstStage,
                                         VkAccessFlags srcAccess,
                                         VkAccessFlags dstAccess) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
}

bool RayTracingRenderer::BeginStill(const RTStillDesc& desc) {
  if (!m_isInitialized || !m_useRayTracing) {
    LOG_ERROR("Ray Tracing: BeginStill with no RT support");
    return false;
  }
  if (m_topLevelAS.handle == VK_NULL_HANDLE || m_geometries.empty()) {
    LOG_ERROR("Ray Tracing: BeginStill with no acceleration structures");
    return false;
  }
  if (m_stillActive)
    EndStill();

  m_stillWidth = desc.width;
  m_stillHeight = desc.height;
  m_stillSeed = desc.seed;

  // --- Accumulation image (rgba32f, storage + transfer src for readback) ---
  VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imgInfo.imageType = VK_IMAGE_TYPE_2D;
  imgInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  imgInfo.extent = {desc.width, desc.height, 1};
  imgInfo.mipLevels = 1;
  imgInfo.arrayLayers = 1;
  imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | // readback
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // clear-to-zero
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(m_device, &imgInfo, nullptr, &m_stillImage) != VK_SUCCESS) {
    LOG_ERROR("Ray Tracing: failed to create still image");
    return false;
  }
  if (!vkutil::AllocateImageMemory(m_device, m_physicalDevice, m_stillImage,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   m_stillImageMemory)) {
    LOG_ERROR("Ray Tracing: failed to allocate still image memory");
    vkDestroyImage(m_device, m_stillImage, nullptr);
    m_stillImage = VK_NULL_HANDLE;
    return false;
  }

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = m_stillImage;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  vkCreateImageView(m_device, &viewInfo, nullptr, &m_stillImageView);

  // --- Outline G-buffer ---
  m_stillOutlines = desc.outlines;
  m_stillOutlineColor = desc.outlineColor;
  m_stillOutlineWidth = desc.outlineWidth;
  // The G-buffer is supersampled 4x4 so the edge mask averages down into
  // coverage with 17 levels, not a hard 1 px staircase, and it is traced in
  // STRIPS of rows so its memory does not grow with the still: a whole-image
  // G-buffer is 16 bytes a ray, which is why this used to be 2x2 up to ~2 Mpx
  // and 1x1 above (docs/rendering/still-render.md#outlines-on-a-traced-still).
  // The scale drops only if 4 x the width passes the device's largest image.
  {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    m_auxScale = 4;
    while (m_auxScale > 1 &&
           desc.width * m_auxScale > props.limits.maxImageDimension2D)
      --m_auxScale;
    constexpr uint64_t kAuxStripTexels = 4ull * 1024ull * 1024ull; // 64 MB
    const uint32_t aw = desc.width * m_auxScale;
    const uint32_t ah = desc.height * m_auxScale;
    const uint32_t halo = static_cast<uint32_t>(
        stilloutline::HaloRows(desc.outlineWidth, m_auxScale));
    const uint32_t outRows = std::max<uint32_t>(
        1, static_cast<uint32_t>(kAuxStripTexels / aw) / m_auxScale);
    m_auxStripRows = std::min(ah, outRows * m_auxScale + 2 * halo);
  }
  // The image is created either way. Binding 9 must be written even with
  // outlines off (an unwritten descriptor is undefined behaviour, not a
  // no-op), and it has to be a uint view to match the shader's rgba32ui
  // qualifier — the accumulation image's float view cannot stand in. With
  // outlines off it is a 1x1 placeholder that no launch ever writes.
  {
    VkImageCreateInfo auxInfo = imgInfo;
    auxInfo.format = VK_FORMAT_R32G32B32A32_UINT;
    auxInfo.extent =
        m_stillOutlines ? VkExtent3D{desc.width * m_auxScale, m_auxStripRows, 1}
                        : VkExtent3D{1, 1, 1};
    if (vkCreateImage(m_device, &auxInfo, nullptr, &m_auxImage) != VK_SUCCESS ||
        !vkutil::AllocateImageMemory(m_device, m_physicalDevice, m_auxImage,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                     m_auxImageMemory)) {
      LOG_ERROR("Ray Tracing: failed to create the outline G-buffer");
      if (m_auxImage != VK_NULL_HANDLE)
        vkDestroyImage(m_device, m_auxImage, nullptr);
      m_auxImage = VK_NULL_HANDLE;
      EndStill();
      return false;
    }
    VkImageViewCreateInfo auxView = viewInfo;
    auxView.image = m_auxImage;
    auxView.format = VK_FORMAT_R32G32B32A32_UINT;
    vkCreateImageView(m_device, &auxView, nullptr, &m_auxImageView);
  }

  // --- Camera + light UBO ---
  RTUniforms u{};
  u.viewInverse = glm::inverse(desc.view);
  u.projInverse = glm::inverse(desc.proj);
  u.viewPos = glm::vec4(desc.cameraPos, 1.0f);
  // Match the raster primary key light (conv::kKeyLightDir, toward the
  // light); store the travel direction (negated) so the shader recovers the
  // toward-light vector as -lightDir.
  glm::vec3 toLight = glm::length(desc.keyLightDir) > 1e-6f
                          ? glm::normalize(desc.keyLightDir)
                          : conv::kKeyLightDir;
  u.lightDir = glm::vec4(-toLight, 1.8f);
  u.lightColor = glm::vec4(glm::vec3(conv::kKeyLight), 1.0f);
  u.ambientColor = glm::vec4(0.15f, 0.17f, 0.2f, 1.0f);
  // Fill and rim carry the shared rig's COLOURS and intensities; their
  // directions come from the caller, which swings the whole rig off the
  // camera axis (MakeStillDesc). Travel direction is the negated toward-light
  // vector, matching lightDir above.
  const glm::vec3 toFill = glm::length(desc.fillLightDir) > 1e-6f
                               ? glm::normalize(desc.fillLightDir)
                               : conv::kFillLightDir;
  const glm::vec3 toRim = glm::length(desc.rimLightDir) > 1e-6f
                              ? glm::normalize(desc.rimLightDir)
                              : conv::kRimLightDir;
  u.fillDir = glm::vec4(-toFill, conv::kFillLight.a);
  u.fillColor = glm::vec4(glm::vec3(conv::kFillLight), 1.0f);
  u.rimDir = glm::vec4(-toRim, conv::kRimLight.a);
  u.rimColor = glm::vec4(glm::vec3(conv::kRimLight), 1.0f);
  // Section planes, compacted so the shader's count is the loop bound.
  int live = 0;
  for (const glm::vec4& pl : desc.clipPlanes)
    if (glm::length(glm::vec3(pl)) > 1e-6f)
      u.clipPlanes[live++] = pl;
  for (int i = live; i < kMaxClipPlanes; ++i)
    u.clipPlanes[i] = glm::vec4(0.0f);
  const bool glass =
      std::any_of(m_materials.begin(), m_materials.end(),
                  [](const RTMaterial& m) { return m.transmission > 0.0f; });
  u.clipInfo =
      glm::vec4(static_cast<float>(live), glass ? 1.0f : 0.0f, 0.0f, 0.0f);
  m_stillHasClip = live > 0;
  u.params = glm::vec4(static_cast<float>(desc.prefilterMips), desc.rayFar,
                       desc.aoRadius, desc.studioBackdrop ? 1.0f : 0.0f);

  const VkMemoryPropertyFlags hostVisible =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  m_stillUBO =
      CreateBuffer(sizeof(RTUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   hostVisible, m_stillUBOMemory);
  void* ubomap = nullptr;
  vkMapMemory(m_device, m_stillUBOMemory, 0, sizeof(RTUniforms), 0, &ubomap);
  memcpy(ubomap, &u, sizeof(RTUniforms));
  vkUnmapMemory(m_device, m_stillUBOMemory);

  // --- GeometryInfo[] SSBO (per-body buffer addresses + material index) ---
  std::vector<GeometryInfoGPU> geomInfos;
  geomInfos.reserve(m_geometries.size());
  for (const auto& g : m_geometries) {
    GeometryInfoGPU gi{};
    gi.vertexAddress = g->vertexAddress;
    gi.indexAddress = g->indexAddress;
    gi.materialIndex = g->materialIndex;
    gi.flags = g->flags;
    geomInfos.push_back(gi);
  }
  VkDeviceSize geomSize = geomInfos.size() * sizeof(GeometryInfoGPU);
  m_stillGeomInfo = CreateBuffer(geomSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 hostVisible, m_stillGeomInfoMemory);
  void* gmap = nullptr;
  vkMapMemory(m_device, m_stillGeomInfoMemory, 0, geomSize, 0, &gmap);
  memcpy(gmap, geomInfos.data(), static_cast<size_t>(geomSize));
  vkUnmapMemory(m_device, m_stillGeomInfoMemory);

  // --- RTMaterial[] SSBO (std430 stride == sizeof(RTMaterial) == 80) ---
  std::vector<RTMaterial> mats = m_materials;
  if (mats.empty()) {
    RTMaterial def{};
    def.baseColor = glm::vec3(0.8f);
    def.metallic = 0.1f;
    def.roughness = 0.5f;
    def.specular = glm::vec3(0.5f);
    mats.push_back(def);
  }
  VkDeviceSize matSize = mats.size() * sizeof(RTMaterial);
  m_stillMaterials = CreateBuffer(matSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  hostVisible, m_stillMaterialsMemory);
  void* mmap = nullptr;
  vkMapMemory(m_device, m_stillMaterialsMemory, 0, matSize, 0, &mmap);
  memcpy(mmap, mats.data(), static_cast<size_t>(matSize));
  vkUnmapMemory(m_device, m_stillMaterialsMemory);

  // --- Descriptor pool + set ---
  std::vector<VkDescriptorPoolSize> poolSizes = {
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, // accumulation + outline G-buffer
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
  };
  VkDescriptorPoolCreateInfo poolInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr,
                             &m_rtDescriptorPool) != VK_SUCCESS) {
    LOG_ERROR("Ray Tracing: failed to create descriptor pool");
    EndStill();
    return false;
  }
  VkDescriptorSetAllocateInfo setAlloc{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  setAlloc.descriptorPool = m_rtDescriptorPool;
  setAlloc.descriptorSetCount = 1;
  setAlloc.pSetLayouts = &m_rtDescriptorSetLayout;
  if (vkAllocateDescriptorSets(m_device, &setAlloc, &m_rtDescriptorSet) !=
      VK_SUCCESS) {
    LOG_ERROR("Ray Tracing: failed to allocate descriptor set");
    EndStill();
    return false;
  }

  // --- Descriptor writes ---
  VkWriteDescriptorSetAccelerationStructureKHR asWrite{
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  asWrite.accelerationStructureCount = 1;
  asWrite.pAccelerationStructures = &m_topLevelAS.handle;

  VkDescriptorImageInfo imageInfo{};
  imageInfo.imageView = m_stillImageView;
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorBufferInfo uboInfo{m_stillUBO, 0, sizeof(RTUniforms)};
  VkDescriptorBufferInfo geomBufInfo{m_stillGeomInfo, 0, geomSize};
  VkDescriptorBufferInfo matBufInfo{m_stillMaterials, 0, matSize};

  VkDescriptorImageInfo irr{desc.envSampler, desc.irradiance,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo pre{desc.envSampler, desc.prefilter,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo brdf{desc.lutSampler, desc.brdfLut,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo env{desc.envSampler, desc.environment,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

  VkDescriptorImageInfo auxInfo{};
  auxInfo.imageView = m_auxImageView;
  auxInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  std::vector<VkWriteDescriptorSet> writes(
      10, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
  for (auto& w : writes)
    w.dstSet = m_rtDescriptorSet, w.descriptorCount = 1;
  writes[0].dstBinding = 0;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  writes[0].pNext = &asWrite;
  writes[1].dstBinding = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[1].pImageInfo = &imageInfo;
  writes[2].dstBinding = 2;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[2].pBufferInfo = &uboInfo;
  writes[3].dstBinding = 3;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].pBufferInfo = &geomBufInfo;
  writes[4].dstBinding = 4;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[4].pBufferInfo = &matBufInfo;
  writes[5].dstBinding = 5;
  writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[5].pImageInfo = &irr;
  writes[6].dstBinding = 6;
  writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[6].pImageInfo = &pre;
  writes[7].dstBinding = 7;
  writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[7].pImageInfo = &brdf;
  writes[8].dstBinding = 8;
  writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[8].pImageInfo = &env;
  writes[9].dstBinding = 9;
  writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[9].pImageInfo = &auxInfo;
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  // --- Clear the accumulation image to zero, leave it in GENERAL ---
  VkCommandBuffer cb = vkutil::BeginSingleTimeCommands(m_device, m_commandPool);
  TransitionImage(
      cb, m_stillImage, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkClearColorValue clear{};
  clear.float32[0] = clear.float32[1] = clear.float32[2] = clear.float32[3] =
      0.0f;
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cb, m_stillImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       &clear, 1, &range);
  TransitionImage(cb, m_stillImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  // The G-buffer needs no clear (the launch writes every pixel), only the
  // layout a storage image has to be in to be written at all.
  TransitionImage(cb, m_auxImage, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  vkutil::EndSingleTimeCommands(m_device, m_commandPool, m_graphicsQueue, cb);

  m_stillActive = true;
  LOG_INFO("Ray Tracing: BeginStill {}x{} over {} bodies", desc.width,
           desc.height, m_geometries.size());
  return true;
}

bool RayTracingRenderer::RenderBatch(uint32_t firstSample,
                                     uint32_t samplesInBatch) {
  if (!m_stillActive)
    return false;

  VkCommandBuffer cb = vkutil::BeginSingleTimeCommands(m_device, m_commandPool);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          m_rtPipelineLayout, 0, 1, &m_rtDescriptorSet, 0,
                          nullptr);

  for (uint32_t s = 0; s < samplesInBatch; ++s) {
    uint32_t push[5] = {firstSample + s, m_stillSeed, 0u, 0u, 0u};
    vkCmdPushConstants(cb, m_rtPipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                       0, sizeof(push), push);
    m_rtFunctions.vkCmdTraceRaysKHR(cb, &m_raygenSBT, &m_missSBT, &m_hitSBT,
                                    &m_callableSBT, m_stillWidth, m_stillHeight,
                                    1);
    // The accumulation image is read-modify-written each sample: serialize
    // consecutive dispatches so sample s+1 sees sample s's writes.
    if (s + 1 < samplesInBatch) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = m_stillImage;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
    }
  }
  vkutil::EndSingleTimeCommands(m_device, m_commandPool, m_graphicsQueue, cb);
  return true;
}

// Traces the outline G-buffer strip by strip and reduces each strip to
// per-output-pixel edge coverage (render/StillOutlines.h says what counts as
// an edge). Each strip carries a halo of rows above and below, so a seam
// computes exactly what one whole-image pass would.
// Returns an empty vector when outlines are off or a readback fails; the
// caller then writes the picture without them rather than failing the render.
std::vector<float> RayTracingRenderer::TraceOutlineCoverage() {
  if (!m_stillOutlines || m_auxImage == VK_NULL_HANDLE)
    return {};

  const uint32_t s = m_auxScale;
  const uint32_t aw = m_stillWidth * s;
  const uint32_t ah = m_stillHeight * s;
  const uint32_t halo =
      static_cast<uint32_t>(stilloutline::HaloRows(m_stillOutlineWidth, s));
  // Output rows each strip answers for: all of them when the image fits one
  // strip, else what is left of the strip once both halos are taken out.
  const uint32_t perStrip =
      m_auxStripRows >= ah ? m_stillHeight : (m_auxStripRows - 2 * halo) / s;

  const VkDeviceSize bufSize =
      static_cast<VkDeviceSize>(aw) * m_auxStripRows * 16; // rgba32ui
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkBuffer buf = VK_NULL_HANDLE;
  if (!vkutil::CreateReadbackBuffer(m_device, m_physicalDevice, bufSize, buf,
                                    mem))
    return {};

  std::vector<float> coverage(static_cast<size_t>(m_stillWidth) *
                              m_stillHeight);
  bool ok = true;
  for (uint32_t out0 = 0; out0 < m_stillHeight && ok; out0 += perStrip) {
    const uint32_t out1 = std::min(m_stillHeight, out0 + perStrip);
    const uint32_t row0 = out0 * s > halo ? out0 * s - halo : 0;
    const uint32_t row1 = std::min(ah, out1 * s + halo);
    const uint32_t rows = row1 - row0;

    // One primary-ray launch over the strip. Tracing is cheap next to this
    // render's fixed costs, so even 4x4 does not show in the timings.
    VkCommandBuffer cb =
        vkutil::BeginSingleTimeCommands(m_device, m_commandPool);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_rtPipelineLayout, 0, 1, &m_rtDescriptorSet, 0,
                            nullptr);
    uint32_t push[5] = {0u, m_stillSeed, 1u, row0, ah};
    vkCmdPushConstants(cb, m_rtPipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                       0, sizeof(push), push);
    m_rtFunctions.vkCmdTraceRaysKHR(cb, &m_raygenSBT, &m_missSBT, &m_hitSBT,
                                    &m_callableSBT, aw, rows, 1);
    TransitionImage(cb, m_auxImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {aw, rows, 1};
    vkCmdCopyImageToBuffer(cb, m_auxImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &copy);
    // Back to GENERAL for the next strip's launch to write.
    TransitionImage(cb, m_auxImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkutil::EndSingleTimeCommands(m_device, m_commandPool, m_graphicsQueue, cb);

    const auto* texels = static_cast<const stilloutline::Texel*>(
        vkutil::MapReadback(m_device, mem));
    if (texels == nullptr) {
      ok = false;
      break;
    }
    stilloutline::Coverage(
        texels, aw, ah, row0, rows, s, m_stillOutlineWidth, out0, out1,
        coverage.data() + static_cast<size_t>(out0) * m_stillWidth);
    vkUnmapMemory(m_device, mem);
  }
  vkDestroyBuffer(m_device, buf, nullptr);
  vkFreeMemory(m_device, mem, nullptr);
  if (!ok)
    return {};
  return coverage;
}

bool RayTracingRenderer::ReadbackStill(uint32_t totalSamples,
                                       const std::string& pngPath,
                                       std::string* error) {
  if (!m_stillActive) {
    if (error)
      *error = "no active still render";
    return false;
  }
  if (totalSamples == 0)
    totalSamples = 1;

  // Traced before the accumulation readback: both use one-shot submits, and
  // the G-buffer launch must not land between them.
  const std::vector<float> outline = TraceOutlineCoverage();

  const VkDeviceSize pixelBytes = 16; // rgba32f
  VkDeviceSize bufSize =
      static_cast<VkDeviceSize>(m_stillWidth) * m_stillHeight * pixelBytes;
  VkDeviceMemory readMem = VK_NULL_HANDLE;
  VkBuffer readBuf = VK_NULL_HANDLE;
  vkutil::CreateReadbackBuffer(m_device, m_physicalDevice, bufSize, readBuf,
                               readMem);

  VkCommandBuffer cb = vkutil::BeginSingleTimeCommands(m_device, m_commandPool);
  TransitionImage(cb, m_stillImage, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {m_stillWidth, m_stillHeight, 1};
  vkCmdCopyImageToBuffer(cb, m_stillImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         readBuf, 1, &copy);
  vkutil::EndSingleTimeCommands(m_device, m_commandPool, m_graphicsQueue, cb);

  const float* src =
      static_cast<const float*>(vkutil::MapReadback(m_device, readMem));
  std::vector<uint8_t> rgb(static_cast<size_t>(m_stillWidth) * m_stillHeight *
                           3);
  const float invN = 1.0f / static_cast<float>(totalSamples);
  for (size_t i = 0; i < static_cast<size_t>(m_stillWidth) * m_stillHeight;
       ++i) {
    glm::vec3 c(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2]);
    c *= invN;                               // average the accumulated samples
    c = ACESFilm(c);                         // filmic tonemap
    c = glm::pow(c, glm::vec3(1.0f / 2.2f)); // gamma to sRGB
    c *= 1.1f; // the same post-gamma lift mesh.frag applies
    // Edges go on after tonemapping, so the line keeps the colour it was
    // asked for instead of being filmically rolled toward it.
    if (!outline.empty() && outline[i] > 0.0f)
      c = glm::mix(c, m_stillOutlineColor, outline[i]);
    rgb[i * 3 + 0] =
        static_cast<uint8_t>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    rgb[i * 3 + 1] =
        static_cast<uint8_t>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    rgb[i * 3 + 2] =
        static_cast<uint8_t>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  vkUnmapMemory(m_device, readMem);
  vkDestroyBuffer(m_device, readBuf, nullptr);
  vkFreeMemory(m_device, readMem, nullptr);

  int ok = stbi_write_png(pngPath.c_str(), static_cast<int>(m_stillWidth),
                          static_cast<int>(m_stillHeight), 3, rgb.data(),
                          static_cast<int>(m_stillWidth) * 3);
  if (!ok) {
    if (error)
      *error = "stbi_write_png failed for " + pngPath;
    return false;
  }
  LOG_INFO("Ray Tracing: wrote still {} ({} spp)", pngPath, totalSamples);
  return true;
}

void RayTracingRenderer::DestroyStillResources() {
  if (m_device == VK_NULL_HANDLE)
    return;
  if (m_rtDescriptorPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(m_device, m_rtDescriptorPool, nullptr);
    m_rtDescriptorPool = VK_NULL_HANDLE;
    m_rtDescriptorSet = VK_NULL_HANDLE; // freed with the pool
  }
  if (m_stillImageView != VK_NULL_HANDLE) {
    vkDestroyImageView(m_device, m_stillImageView, nullptr);
    m_stillImageView = VK_NULL_HANDLE;
  }
  if (m_stillImage != VK_NULL_HANDLE) {
    vkDestroyImage(m_device, m_stillImage, nullptr);
    vkFreeMemory(m_device, m_stillImageMemory, nullptr);
    m_stillImage = VK_NULL_HANDLE;
    m_stillImageMemory = VK_NULL_HANDLE;
  }
  if (m_auxImageView != VK_NULL_HANDLE) {
    vkDestroyImageView(m_device, m_auxImageView, nullptr);
    m_auxImageView = VK_NULL_HANDLE;
  }
  if (m_auxImage != VK_NULL_HANDLE) {
    vkDestroyImage(m_device, m_auxImage, nullptr);
    vkFreeMemory(m_device, m_auxImageMemory, nullptr);
    m_auxImage = VK_NULL_HANDLE;
    m_auxImageMemory = VK_NULL_HANDLE;
  }
  auto freeBuf = [&](VkBuffer& b, VkDeviceMemory& m) {
    if (b != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, b, nullptr);
      vkFreeMemory(m_device, m, nullptr);
      b = VK_NULL_HANDLE;
      m = VK_NULL_HANDLE;
    }
  };
  freeBuf(m_stillUBO, m_stillUBOMemory);
  freeBuf(m_stillGeomInfo, m_stillGeomInfoMemory);
  freeBuf(m_stillMaterials, m_stillMaterialsMemory);
}

void RayTracingRenderer::EndStill() {
  if (m_device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(m_device);
  DestroyStillResources();
  m_stillActive = false;
}

// Utility functions
// This one KEEPS its throwing shape rather than adopting vkutil's bool.
// The RT path is entered only from render_still / rt_build, both of which
// already report failure to their caller, and the acceleration-structure
// builders below have a dozen call sites whose failure branches nothing here
// can exercise — so the body is shared (including the device-address
// allocate flag, which vkutil applies from the usage bits) while the call
// sites stay as they were. Convert them when the RT path grows a test.
VkBuffer RayTracingRenderer::CreateBuffer(VkDeviceSize size,
                                          VkBufferUsageFlags usage,
                                          VkMemoryPropertyFlags properties,
                                          VkDeviceMemory& bufferMemory) {
  VkBuffer buffer = VK_NULL_HANDLE;
  if (!vkutil::CreateBuffer(m_device, m_physicalDevice, size, usage, properties,
                            buffer, bufferMemory)) {
    throw std::runtime_error("RayTracingRenderer: buffer allocation failed");
  }
  return buffer;
}

bool RayTracingRenderer::CreateBottomLevelAS() {
  // One BLAS per geometry (body). Built device-side from the vertex/index
  // buffers uploaded in AddGeometry.
  for (auto& geom : m_geometries) {
    VkAccelerationStructureGeometryKHR asGeom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    asGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Any-hit may otherwise run twice for one intersection, and a shadow ray
    // crossing glass multiplies its transmittance there — twice would darken
    // the pane's shadow for no reason a reader could find.
    asGeom.flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
    // NOT VK_GEOMETRY_OPAQUE_BIT_KHR. That flag is a property of the
    // acceleration structure and suppresses the any-hit stage outright, so
    // section.rahit never ran and a sectioned still came back uncut -- with
    // no error anywhere, because declaring geometry opaque is a promise the
    // driver is entitled to keep. Opacity is decided per RAY instead
    // (gl_RayFlagsOpaqueEXT in raygen/closesthit, dropped only when a plane
    // is live), which costs nothing when nothing is cut and still lets a cut
    // through when something is.
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
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &asGeom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_rtFunctions.vkGetAccelerationStructureBuildSizesKHR(
        m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
        &primCount, &sizeInfo);

    geom->blas.buffer =
        CreateBuffer(sizeInfo.accelerationStructureSize,
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
    VkBuffer scratch =
        CreateBuffer(sizeInfo.buildScratchSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchMem);

    buildInfo.dstAccelerationStructure = geom->blas.handle;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(scratch);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

    VkCommandBuffer cb =
        vkutil::BeginSingleTimeCommands(m_device, m_commandPool);
    m_rtFunctions.vkCmdBuildAccelerationStructuresKHR(cb, 1, &buildInfo,
                                                      &pRange);
    vkutil::EndSingleTimeCommands(m_device, m_commandPool, m_graphicsQueue, cb);

    vkDestroyBuffer(m_device, scratch, nullptr);
    vkFreeMemory(m_device, scratchMem, nullptr);

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addrInfo.accelerationStructure = geom->blas.handle;
    geom->blas.deviceAddress =
        m_rtFunctions.vkGetAccelerationStructureDeviceAddressKHR(m_device,
                                                                 &addrInfo);
  }
  LOG_INFO("Ray Tracing: built {} BLAS", m_geometries.size());
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
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
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
  asGeom.geometry.instances.data.deviceAddress =
      GetBufferDeviceAddress(instBuf);

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

  m_topLevelAS.buffer =
      CreateBuffer(sizeInfo.accelerationStructureSize,
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
  VkBuffer scratch =
      CreateBuffer(sizeInfo.buildScratchSize,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchMem);

  buildInfo.dstAccelerationStructure = m_topLevelAS.handle;
  buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(scratch);

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = primCount;
  const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

  VkCommandBuffer cb = vkutil::BeginSingleTimeCommands(m_device, m_commandPool);
  m_rtFunctions.vkCmdBuildAccelerationStructuresKHR(cb, 1, &buildInfo, &pRange);
  vkutil::EndSingleTimeCommands(m_device, m_commandPool, m_graphicsQueue, cb);

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
  LOG_INFO("Ray Tracing: built TLAS over {} instances", instances.size());
  return true;
}