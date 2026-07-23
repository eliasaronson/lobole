#include "render/Viewport.h"
#include "render/Camera.h"
#include "render/Scene.h"
#include "render/MeshRenderer.h"
#include <iostream>

Viewport::Viewport()
    : m_size(800, 600)
    , m_position(0, 0)
    , m_mouseOver(false)
    , m_focused(false)
    , m_device(VK_NULL_HANDLE)
    , m_queue(VK_NULL_HANDLE)
    , m_queueFamily(0)
    , m_colorImage(VK_NULL_HANDLE)
    , m_colorImageMemory(VK_NULL_HANDLE)
    , m_colorImageView(VK_NULL_HANDLE)
    , m_depthImage(VK_NULL_HANDLE)
    , m_depthImageMemory(VK_NULL_HANDLE)
    , m_depthImageView(VK_NULL_HANDLE)
    , m_renderPass(VK_NULL_HANDLE)
    , m_framebuffer(VK_NULL_HANDLE)
    , m_commandPool(VK_NULL_HANDLE)
    , m_commandBuffer(VK_NULL_HANDLE)
    , m_descriptorSet(VK_NULL_HANDLE)
    , m_showGrid(true)
    , m_gridSize(1.0f)
    , m_gridSubdivisions(10) {
}

Viewport::~Viewport() {
    Shutdown();
}

bool Viewport::Initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, uint32_t queueFamily, VkCommandPool commandPool) {
    m_device = device;
    m_queue = queue;
    m_queueFamily = queueFamily;
    
    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        std::cerr << "Failed to create command pool for viewport" << std::endl;
        return false;
    }
    
    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    if (vkAllocateCommandBuffers(device, &allocInfo, &m_commandBuffer) != VK_SUCCESS) {
        std::cerr << "Failed to allocate command buffer for viewport" << std::endl;
        return false;
    }
    
    CreateFramebuffer();
    return true;
}

void Viewport::Shutdown() {
    if (m_device != VK_NULL_HANDLE) {
        DestroyFramebuffer();
        
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }
    }
}

void Viewport::Update(float deltaTime) {
    // Update viewport state
}

void Viewport::Render(Camera* camera, Scene* scene) {
    if (!camera || !scene) return;
    
    // Begin command buffer recording
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
    
    // Begin render pass
    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {(uint32_t)m_size.x, (uint32_t)m_size.y};
    
    VkClearValue clearValues[2] = {};
    clearValues[0].color = {0.2f, 0.2f, 0.2f, 1.0f}; // Background color
    clearValues[1].depthStencil = {1.0f, 0};
    
    renderPassInfo.clearValueCount = 2;
    renderPassInfo.pClearValues = clearValues;
    
    vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // Set viewport
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = m_size.x;
    viewport.height = m_size.y;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {(uint32_t)m_size.x, (uint32_t)m_size.y};
    vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
    
    // Render grid
    if (m_showGrid) {
        RenderGrid(camera);
    }
    
    // Render scene objects
    scene->Render();
    
    // End render pass
    vkCmdEndRenderPass(m_commandBuffer);
    
    // End command buffer
    vkEndCommandBuffer(m_commandBuffer);
    
    // Submit command buffer
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;
    
    vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue); // Wait for completion
}

void Viewport::RenderUI() {
    ImGui::Begin("3D Viewport");
    
    m_focused = ImGui::IsWindowFocused();
    m_mouseOver = ImGui::IsWindowHovered();
    
    // Get available content region
    ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    
    // Resize framebuffer if needed
    if (contentRegion.x != m_size.x || contentRegion.y != m_size.y) {
        SetSize((int)contentRegion.x, (int)contentRegion.y);
    }
    
    // Display the rendered viewport as an image
    if (m_descriptorSet != VK_NULL_HANDLE) {
        ImGui::Image((ImTextureID)m_descriptorSet, m_size);
    }
    
    // Viewport controls
    if (ImGui::BeginPopupContextWindow()) {
        ImGui::Checkbox("Show Grid", &m_showGrid);
        ImGui::SliderFloat("Grid Size", &m_gridSize, 0.1f, 10.0f);
        ImGui::SliderInt("Grid Subdivisions", &m_gridSubdivisions, 5, 50);
        ImGui::EndPopup();
    }
    
    ImGui::End();
}

void Viewport::SetSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    
    m_size.x = (float)width;
    m_size.y = (float)height;
    
    ResizeFramebuffer(width, height);
}

void Viewport::CreateFramebuffer() {
    // This is a placeholder - full Vulkan framebuffer creation would be more complex
    // For now, we'll create basic structures
}

void Viewport::DestroyFramebuffer() {
    if (m_device == VK_NULL_HANDLE) return;
    
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    
    // Destroy image views and images
    if (m_colorImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_colorImageView, nullptr);
        m_colorImageView = VK_NULL_HANDLE;
    }
    
    if (m_depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_depthImageView, nullptr);
        m_depthImageView = VK_NULL_HANDLE;
    }
    
    if (m_colorImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_colorImage, nullptr);
        m_colorImage = VK_NULL_HANDLE;
    }
    
    if (m_depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    
    if (m_colorImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_colorImageMemory, nullptr);
        m_colorImageMemory = VK_NULL_HANDLE;
    }
    
    if (m_depthImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_depthImageMemory, nullptr);
        m_depthImageMemory = VK_NULL_HANDLE;
    }
}

void Viewport::ResizeFramebuffer(int width, int height) {
    // Destroy existing framebuffer
    DestroyFramebuffer();
    
    // Recreate with new size
    CreateFramebuffer();
}

void Viewport::RenderGrid(Camera* camera) {
    // Placeholder for grid rendering
    // This would involve creating vertex buffers for grid lines and rendering them
}