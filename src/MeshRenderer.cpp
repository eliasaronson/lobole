#include "MeshRenderer.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <limits.h>

// Utility function to get the directory of the current executable
std::string GetExecutableDirectory() {
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count != -1) {
        result[count] = '\0';
        std::filesystem::path execPath(result);
        return execPath.parent_path().string();
    }
    // Fallback to current directory if readlink fails
    return ".";
}

// Vertex3D implementation
VkVertexInputBindingDescription Vertex3D::getBindingDescription() {
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex3D);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return bindingDescription;
}

std::vector<VkVertexInputAttributeDescription> Vertex3D::getAttributeDescriptions() {
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions(3);
    
    // Position
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex3D, position);
    
    // Normal
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex3D, normal);
    
    // Color
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(Vertex3D, color);
    
    return attributeDescriptions;
}

// MeshRenderer implementation
MeshRenderer::MeshRenderer() {
}

MeshRenderer::~MeshRenderer() {
    Shutdown();
}

bool MeshRenderer::Initialize(VkDevice device, VkPhysicalDevice physicalDevice, 
                             VkRenderPass renderPass, VkQueue graphicsQueue, 
                             uint32_t queueFamily, VkCommandPool commandPool) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_renderPass = renderPass;
    m_graphicsQueue = graphicsQueue;
    m_queueFamily = queueFamily;
    m_commandPool = commandPool;
    
    if (!CreateDescriptorSetLayout()) return false;
    if (!CreateGraphicsPipeline()) return false;
    if (!CreateUniformBuffers()) return false;
    if (!CreateDescriptorPool()) return false;
    if (!CreateDescriptorSets()) return false;
    
    return true;
}

void MeshRenderer::Shutdown() {
    if (m_device == VK_NULL_HANDLE) return;
    
    // Cleanup meshes
    for (auto& mesh : m_meshes) {
        if (mesh) {
            DestroyMeshBuffers(mesh.get());
        }
    }
    m_meshes.clear();
    
    // Cleanup uniform buffers
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (m_uniformBuffers[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_uniformBuffers[i], nullptr);
        }
        if (m_uniformBuffersMemory[i] != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_uniformBuffersMemory[i], nullptr);
        }
    }
    
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }
    
    if (m_graphicsPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
    }
    
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    }
    
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    }
}

uint32_t MeshRenderer::AddMesh(const std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices) {
    auto mesh = std::make_unique<Mesh>();
    mesh->vertices = vertices;
    mesh->indices = indices;
    
    if (!CreateMeshBuffers(mesh.get())) {
        return 0; // Invalid mesh ID
    }
    
    uint32_t meshId = m_nextMeshId++;
    m_meshes.push_back(std::move(mesh));
    
    return meshId;
}

void MeshRenderer::UpdateMesh(uint32_t meshId, const std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices) {
    if (meshId == 0 || meshId > m_meshes.size()) return;
    
    Mesh* mesh = m_meshes[meshId - 1].get();
    if (!mesh) return;
    
    mesh->vertices = vertices;
    mesh->indices = indices;
    mesh->isDirty = true;
    
    UpdateMeshBuffers(mesh);
}

void MeshRenderer::RemoveMesh(uint32_t meshId) {
    if (meshId == 0 || meshId > m_meshes.size()) return;

    if (m_meshes[meshId - 1]) {
        // Ensure the buffers aren't referenced by an in-flight frame before
        // destroying them (rapid resync, e.g. accepting a feature preview,
        // otherwise causes a GPU device-lost).
        vkDeviceWaitIdle(m_device);
        DestroyMeshBuffers(m_meshes[meshId - 1].get());
        m_meshes[meshId - 1].reset();
    }
}

void MeshRenderer::SetMeshTransform(uint32_t meshId, const glm::mat4& transform) {
    if (meshId == 0 || meshId > m_meshes.size()) return;
    
    Mesh* mesh = m_meshes[meshId - 1].get();
    if (mesh) {
        mesh->modelMatrix = transform;
    }
}

void MeshRenderer::SetMeshColor(uint32_t meshId, const glm::vec3& color) {
    if (meshId == 0 || meshId > m_meshes.size()) return;
    
    Mesh* mesh = m_meshes[meshId - 1].get();
    if (mesh) {
        mesh->color = color;
        
        // Update vertex colors
        for (auto& vertex : mesh->vertices) {
            vertex.color = color;
        }
        
        mesh->isDirty = true;
        UpdateMeshBuffers(mesh);
    }
}

void MeshRenderer::UpdateCamera(const glm::mat4& view, const glm::mat4& projection) {
    m_viewMatrix = view;
    m_projectionMatrix = projection;
}

void MeshRenderer::Render(VkCommandBuffer commandBuffer, uint32_t currentFrame) {
    // Update uniform buffer
    UniformBufferObject ubo{};
    ubo.view = m_viewMatrix;
    ubo.proj = m_projectionMatrix;
    
    // Extract camera position from inverse view matrix
    glm::mat4 invView = glm::inverse(m_viewMatrix);
    ubo.viewPos = glm::vec4(invView[3][0], invView[3][1], invView[3][2], 1.0f);
    
    // FreeCAD-style multi-light setup for professional CAD lighting
    
    // Primary directional light (main illumination from upper right)
    ubo.primaryLight.direction = glm::vec4(glm::normalize(glm::vec3(1.0f, 1.5f, 1.0f)), 1.8f); // High intensity
    ubo.primaryLight.color = glm::vec4(1.0f, 0.95f, 0.8f, 1.0f); // Slightly warm white
    
    // Fill light (softer light from opposite direction to reduce harsh shadows)
    ubo.fillLight.direction = glm::vec4(glm::normalize(glm::vec3(-0.5f, -0.8f, -0.5f)), 0.4f); // Lower intensity
    ubo.fillLight.color = glm::vec4(0.8f, 0.85f, 1.0f, 1.0f); // Slightly cool blue fill
    
    // Rim light (side lighting for edge definition)
    ubo.rimLight.direction = glm::vec4(glm::normalize(glm::vec3(0.8f, 0.2f, -0.9f)), 0.6f); // Medium intensity
    ubo.rimLight.color = glm::vec4(1.0f, 1.0f, 0.9f, 1.0f); // Neutral white
    
    // Ambient light (reduced for better contrast)
    ubo.ambientColor = glm::vec4(0.15f, 0.17f, 0.2f, 0.8f); // Cool ambient with intensity
    
    
    // Bind pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
    
    // Set viewport to the central region (excludes the side panels)
    VkViewport viewport{};
    viewport.x = static_cast<float>(m_viewportX);
    viewport.y = static_cast<float>(m_viewportY);
    viewport.width = static_cast<float>(m_viewportWidth);
    viewport.height = static_cast<float>(m_viewportHeight);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {m_viewportX, m_viewportY};
    scissor.extent = {m_viewportWidth, m_viewportHeight};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    // Shared UBO (camera + lights); the model matrix rides per-mesh in push
    // constants so one UBO write serves every draw this frame
    ubo.model = glm::mat4(1.0f);
    memcpy(m_uniformBuffersMapped[currentFrame], &ubo, sizeof(ubo));

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           m_pipelineLayout, 0, 1, &m_descriptorSets[currentFrame], 0, nullptr);

    // Render each mesh
    struct PushConstants {
        glm::mat4 model;
        glm::uvec4 highlight;
    } push;

    for (const auto& mesh : m_meshes) {
        if (!mesh || mesh->vertices.empty()) continue;

        push.model = mesh->modelMatrix;
        push.highlight = mesh->highlight;
        vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

        // Bind vertex buffer
        VkBuffer vertexBuffers[] = {mesh->vertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

        // Bind index buffer
        vkCmdBindIndexBuffer(commandBuffer, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        // Draw
        vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(mesh->indices.size()), 1, 0, 0, 0);
    }
}

void MeshRenderer::SetFaceHighlight(uint32_t meshId, uint32_t firstVertex, uint32_t count) {
    if (meshId == 0 || meshId > m_meshes.size() || !m_meshes[meshId - 1]) return;
    ClearFaceHighlight();
    m_meshes[meshId - 1]->highlight.x = firstVertex;
    m_meshes[meshId - 1]->highlight.y = firstVertex + count;
}

void MeshRenderer::SetFaceHover(uint32_t meshId, uint32_t firstVertex, uint32_t count) {
    if (meshId == 0 || meshId > m_meshes.size() || !m_meshes[meshId - 1]) return;
    ClearFaceHover();
    m_meshes[meshId - 1]->highlight.z = firstVertex;
    m_meshes[meshId - 1]->highlight.w = firstVertex + count;
}

void MeshRenderer::ClearFaceHighlight() {
    for (auto& mesh : m_meshes) {
        if (mesh) { mesh->highlight.x = mesh->highlight.y = 0; }
    }
}

void MeshRenderer::ClearFaceHover() {
    for (auto& mesh : m_meshes) {
        if (mesh) { mesh->highlight.z = mesh->highlight.w = 0; }
    }
}

void MeshRenderer::SetViewport(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    m_viewportX = x;
    m_viewportY = y;
    m_viewportWidth = width;
    m_viewportHeight = height;
}

// Private helper functions
bool MeshRenderer::CreateDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboLayoutBinding;
    
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create descriptor set layout!" << std::endl;
        return false;
    }
    
    return true;
}

bool MeshRenderer::CreateGraphicsPipeline() {
    // Get executable directory and construct shader paths
    std::string execDir = GetExecutableDirectory();
    std::string vertShaderPath = execDir + "/libraries/shaders/mesh_vert.spv";
    std::string fragShaderPath = execDir + "/libraries/shaders/mesh_frag.spv";
    
    // Load shaders
    auto vertShaderCode = ReadFile(vertShaderPath);
    auto fragShaderCode = ReadFile(fragShaderPath);
    
    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        std::cerr << "Failed to load shader files from: " << execDir << "/libraries/shaders/" << std::endl;
        std::cerr << "Vertex shader path: " << vertShaderPath << std::endl;
        std::cerr << "Fragment shader path: " << fragShaderPath << std::endl;
        return false;
    }
    
    VkShaderModule vertShaderModule = CreateShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = CreateShaderModule(fragShaderCode);
    
    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
    
    // Vertex input
    auto bindingDescription = Vertex3D::getBindingDescription();
    auto attributeDescriptions = Vertex3D::getAttributeDescriptions();
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
    
    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // Viewport and scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    
    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    // With the Y-flipped projection, OCCT's outward-oriented triangles are
    // counter-clockwise in framebuffer space (verified empirically; pinned
    // by the "camera-facing triangles are CCW" unit test)
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Depth testing
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    
    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    // Dynamic state
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    
    // Pipeline layout
    // Per-mesh model matrix + highlight ranges via push constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4) + sizeof(glm::uvec4);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout!" << std::endl;
        vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
        vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
        return false;
    }
    
    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;
    
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline) != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline!" << std::endl;
        vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
        vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
        return false;
    }
    
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    
    return true;
}

bool MeshRenderer::CreateUniformBuffers() {
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    
    m_uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    m_uniformBuffersMemory.resize(MAX_FRAMES_IN_FLIGHT);
    m_uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
    
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!CreateBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 
                         m_uniformBuffers[i], m_uniformBuffersMemory[i])) {
            return false;
        }
        
        vkMapMemory(m_device, m_uniformBuffersMemory[i], 0, bufferSize, 0, &m_uniformBuffersMapped[i]);
    }
    
    return true;
}

bool MeshRenderer::CreateDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        std::cerr << "Failed to create descriptor pool!" << std::endl;
        return false;
    }
    
    return true;
}

bool MeshRenderer::CreateDescriptorSets() {
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    allocInfo.pSetLayouts = layouts.data();
    
    m_descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) != VK_SUCCESS) {
        std::cerr << "Failed to allocate descriptor sets!" << std::endl;
        return false;
    }
    
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);
        
        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;
        
        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
    }
    
    return true;
}

VkShaderModule MeshRenderer::CreateShaderModule(const std::vector<char>& code) {
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

std::vector<char> MeshRenderer::ReadFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return {};
    }
    
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();
    
    return buffer;
}

bool MeshRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                               VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        std::cerr << "Failed to create buffer!" << std::endl;
        return false;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);
    
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate buffer memory!" << std::endl;
        return false;
    }
    
    vkBindBufferMemory(m_device, buffer, bufferMemory, 0);
    return true;
}

uint32_t MeshRenderer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    throw std::runtime_error("failed to find suitable memory type!");
}

void MeshRenderer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_commandPool;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
    
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
}

bool MeshRenderer::CreateMeshBuffers(Mesh* mesh) {
    if (!mesh || mesh->vertices.empty()) return false;
    
    VkDeviceSize vertexBufferSize = sizeof(mesh->vertices[0]) * mesh->vertices.size();
    VkDeviceSize indexBufferSize = sizeof(mesh->indices[0]) * mesh->indices.size();
    
    // Create staging buffers
    VkBuffer vertexStagingBuffer;
    VkDeviceMemory vertexStagingBufferMemory;
    CreateBuffer(vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 
                vertexStagingBuffer, vertexStagingBufferMemory);
    
    void* data;
    vkMapMemory(m_device, vertexStagingBufferMemory, 0, vertexBufferSize, 0, &data);
    memcpy(data, mesh->vertices.data(), static_cast<size_t>(vertexBufferSize));
    vkUnmapMemory(m_device, vertexStagingBufferMemory);
    
    // Create vertex buffer
    CreateBuffer(vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mesh->vertexBuffer, mesh->vertexBufferMemory);
    
    CopyBuffer(vertexStagingBuffer, mesh->vertexBuffer, vertexBufferSize);
    
    vkDestroyBuffer(m_device, vertexStagingBuffer, nullptr);
    vkFreeMemory(m_device, vertexStagingBufferMemory, nullptr);
    
    // Create index buffer if indices exist
    if (!mesh->indices.empty()) {
        VkBuffer indexStagingBuffer;
        VkDeviceMemory indexStagingBufferMemory;
        CreateBuffer(indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 
                    indexStagingBuffer, indexStagingBufferMemory);
        
        vkMapMemory(m_device, indexStagingBufferMemory, 0, indexBufferSize, 0, &data);
        memcpy(data, mesh->indices.data(), static_cast<size_t>(indexBufferSize));
        vkUnmapMemory(m_device, indexStagingBufferMemory);
        
        CreateBuffer(indexBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mesh->indexBuffer, mesh->indexBufferMemory);
        
        CopyBuffer(indexStagingBuffer, mesh->indexBuffer, indexBufferSize);
        
        vkDestroyBuffer(m_device, indexStagingBuffer, nullptr);
        vkFreeMemory(m_device, indexStagingBufferMemory, nullptr);
    }
    
    mesh->isDirty = false;
    return true;
}

void MeshRenderer::UpdateMeshBuffers(Mesh* mesh) {
    if (!mesh || !mesh->isDirty) return;
    
    // For simplicity, destroy and recreate buffers
    // In production, you'd want to reuse buffers when possible
    DestroyMeshBuffers(mesh);
    CreateMeshBuffers(mesh);
}

void MeshRenderer::DestroyMeshBuffers(Mesh* mesh) {
    if (!mesh) return;
    
    if (mesh->vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, mesh->vertexBuffer, nullptr);
        mesh->vertexBuffer = VK_NULL_HANDLE;
    }
    if (mesh->vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, mesh->vertexBufferMemory, nullptr);
        mesh->vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (mesh->indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, mesh->indexBuffer, nullptr);
        mesh->indexBuffer = VK_NULL_HANDLE;
    }
    if (mesh->indexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, mesh->indexBufferMemory, nullptr);
        mesh->indexBufferMemory = VK_NULL_HANDLE;
    }
}