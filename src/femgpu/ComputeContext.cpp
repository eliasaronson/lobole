// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "femgpu/ComputeContext.h"

#include "common/DevicePreference.h"

#include <cstring>

namespace gpu {

namespace {

// Ranks a physical device for compute work; higher is better, <0 unusable.
// Discrete beats integrated beats software — but any device that passes the
// requirements (compute queue + float32 buffer atomicAdd) is usable.
struct DevicePick {
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  int score = -1;
  std::string name;
  bool software = false;
};

DevicePick RankDevice(VkPhysicalDevice pd) {
  DevicePick out;

  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qs(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
  bool hasCompute = false;
  uint32_t qfi = 0;
  for (uint32_t q = 0; q < qn; ++q)
    if (qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      hasCompute = true;
      qfi = q;
      break;
    }
  if (!hasCompute)
    return out;

  // The SpMV scatter needs buffer float32 atomicAdd
  // (VK_EXT_shader_atomic_float).
  uint32_t en = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
  std::vector<VkExtensionProperties> exts(en);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, exts.data());
  bool hasAtomicExt = false;
  for (const auto& e : exts)
    if (std::strcmp(e.extensionName,
                    VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME) == 0)
      hasAtomicExt = true;
  if (!hasAtomicExt)
    return out;

  VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomics{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
  VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  feats.pNext = &atomics;
  vkGetPhysicalDeviceFeatures2(pd, &feats);
  if (!atomics.shaderBufferFloat32AtomicAdd)
    return out;

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  int score = 1;
  if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
    score = 2;
  if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    score = 3;

  out.phys = pd;
  out.queueFamily = qfi;
  out.score = score;
  out.name = props.deviceName;
  out.software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
  return out;
}

bool SetError(std::string* error, const std::string& msg) {
  if (error)
    *error = msg;
  return false;
}

} // namespace

std::unique_ptr<ComputeContext>
ComputeContext::Create(std::string* error, const std::string& preferredDevice) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<ComputeContext> {
    if (error)
      *error = m;
    return nullptr;
  };

  std::unique_ptr<ComputeContext> ctx(new ComputeContext());

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "lobole-femgpu";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  if (vkCreateInstance(&ici, nullptr, &ctx->m_instance) != VK_SUCCESS)
    return fail("vkCreateInstance failed (no Vulkan runtime?)");

  uint32_t nd = 0;
  vkEnumeratePhysicalDevices(ctx->m_instance, &nd, nullptr);
  std::vector<VkPhysicalDevice> pds(nd);
  vkEnumeratePhysicalDevices(ctx->m_instance, &nd, pds.data());
  // Rank every device, then let the SHARED rule choose — so a preference set
  // in Preferences means the same thing here as it does for the viewport.
  std::vector<DevicePick> picks;
  std::vector<std::string> names;
  std::vector<int> scores;
  for (VkPhysicalDevice pd : pds) {
    picks.push_back(RankDevice(pd));
    names.push_back(picks.back().name);
    scores.push_back(picks.back().score);
  }
  const int chosen = devicepref::Choose(names, scores, preferredDevice);
  DevicePick best;
  if (chosen >= 0)
    best = picks[static_cast<size_t>(chosen)];
  if (best.score < 0)
    return fail("no Vulkan device with a compute queue and float32 buffer "
                "atomicAdd (VK_EXT_shader_atomic_float)");
  ctx->m_phys = best.phys;
  ctx->m_queueFamily = best.queueFamily;
  ctx->m_deviceName = best.name;
  ctx->m_software = best.software;

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = ctx->m_queueFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomics{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
  atomics.shaderBufferFloat32AtomicAdd = VK_TRUE;
  const char* extNames[] = {VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME};

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &atomics;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = extNames;
  if (vkCreateDevice(ctx->m_phys, &dci, nullptr, &ctx->m_device) != VK_SUCCESS)
    return fail("vkCreateDevice failed");
  vkGetDeviceQueue(ctx->m_device, ctx->m_queueFamily, 0, &ctx->m_queue);

  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = ctx->m_queueFamily;
  if (vkCreateCommandPool(ctx->m_device, &cpi, nullptr, &ctx->m_cmdPool) !=
      VK_SUCCESS)
    return fail("vkCreateCommandPool failed");
  VkCommandBufferAllocateInfo cbai{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = ctx->m_cmdPool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(ctx->m_device, &cbai, &ctx->m_cmd) != VK_SUCCESS)
    return fail("vkAllocateCommandBuffers failed");
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(ctx->m_device, &fci, nullptr, &ctx->m_fence) != VK_SUCCESS)
    return fail("vkCreateFence failed");

  return ctx;
}

ComputeContext::~ComputeContext() {
  if (m_device) {
    vkDeviceWaitIdle(m_device);
    if (m_fence)
      vkDestroyFence(m_device, m_fence, nullptr);
    if (m_cmdPool)
      vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
    vkDestroyDevice(m_device, nullptr);
  }
  if (m_instance)
    vkDestroyInstance(m_instance, nullptr);
}

bool ComputeContext::CreateBuffer(VkDeviceSize size, Buffer& out,
                                  std::string* error) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT; // vkCmdFillBuffer zeroing
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(m_device, &bci, nullptr, &out.buffer) != VK_SUCCESS)
    return SetError(error, "vkCreateBuffer failed");

  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(m_device, out.buffer, &mr);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(m_phys, &mp);
  // Preference order: device-local AND host-visible (resizable BAR, iGPU,
  // software — the SpMV then streams at VRAM bandwidth), else plain
  // host-visible. Fall through on allocation failure too (a small BAR window
  // can run out long before VRAM does).
  const VkMemoryPropertyFlags hostVis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  const VkMemoryPropertyFlags prefs[2] = {
      hostVis | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hostVis};
  for (const VkMemoryPropertyFlags want : prefs) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if (!(mr.memoryTypeBits & (1u << i)) ||
          (mp.memoryTypes[i].propertyFlags & want) != want)
        continue;
      VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      mai.allocationSize = mr.size;
      mai.memoryTypeIndex = i;
      if (vkAllocateMemory(m_device, &mai, nullptr, &out.memory) == VK_SUCCESS)
        break;
    }
    if (out.memory)
      break;
  }
  if (!out.memory) {
    Destroy(out);
    return SetError(error, "vkAllocateMemory failed (out of GPU memory?)");
  }
  vkBindBufferMemory(m_device, out.buffer, out.memory, 0);
  if (vkMapMemory(m_device, out.memory, 0, size, 0, &out.mapped) !=
      VK_SUCCESS) {
    Destroy(out);
    return SetError(error, "vkMapMemory failed");
  }
  out.size = size;
  std::memset(out.mapped, 0, size);
  return true;
}

void ComputeContext::Destroy(Buffer& b) {
  if (b.memory) {
    if (b.mapped)
      vkUnmapMemory(m_device, b.memory);
    vkFreeMemory(m_device, b.memory, nullptr);
  }
  if (b.buffer)
    vkDestroyBuffer(m_device, b.buffer, nullptr);
  b = Buffer{};
}

bool ComputeContext::CreatePipeline(const unsigned char* spirv,
                                    size_t spirvBytes, uint32_t bufferCount,
                                    uint32_t pushBytes, Pipeline& out,
                                    std::string* error) {
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = spirvBytes;
  smci.pCode = reinterpret_cast<const uint32_t*>(spirv); // alignas(4) embed
  if (vkCreateShaderModule(m_device, &smci, nullptr, &out.module) != VK_SUCCESS)
    return SetError(error, "vkCreateShaderModule failed");

  std::vector<VkDescriptorSetLayoutBinding> bindings(bufferCount);
  for (uint32_t i = 0; i < bufferCount; ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dlci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dlci.bindingCount = bufferCount;
  dlci.pBindings = bindings.data();
  if (vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &out.setLayout) !=
      VK_SUCCESS) {
    Destroy(out);
    return SetError(error, "vkCreateDescriptorSetLayout failed");
  }

  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
  VkPipelineLayoutCreateInfo plci{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &out.setLayout;
  plci.pushConstantRangeCount = pushBytes > 0 ? 1 : 0;
  plci.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(m_device, &plci, nullptr, &out.layout) !=
      VK_SUCCESS) {
    Destroy(out);
    return SetError(error, "vkCreatePipelineLayout failed");
  }

  VkComputePipelineCreateInfo cpci{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = out.module;
  cpci.stage.pName = "main";
  cpci.layout = out.layout;
  if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                               &out.pipeline) != VK_SUCCESS) {
    Destroy(out);
    return SetError(error, "vkCreateComputePipelines failed");
  }
  out.bufferCount = bufferCount;
  out.pushBytes = pushBytes;
  return true;
}

void ComputeContext::Destroy(Pipeline& p) {
  if (p.pipeline)
    vkDestroyPipeline(m_device, p.pipeline, nullptr);
  if (p.layout)
    vkDestroyPipelineLayout(m_device, p.layout, nullptr);
  if (p.setLayout)
    vkDestroyDescriptorSetLayout(m_device, p.setLayout, nullptr);
  if (p.module)
    vkDestroyShaderModule(m_device, p.module, nullptr);
  p = Pipeline{};
}

bool ComputeContext::Dispatch(const Pipeline& p,
                              const std::vector<const Buffer*>& buffers,
                              const void* push, uint32_t pushBytes, uint32_t gx,
                              uint32_t gy, std::string* error) {
  if (buffers.size() != p.bufferCount)
    return SetError(error, "dispatch buffer count mismatch");
  if (pushBytes != p.pushBytes)
    return SetError(error, "dispatch push-constant size mismatch");

  // Transient descriptor pool + set per dispatch: trivially correct, and the
  // cost is irrelevant next to the copy-in/copy-out of a GPU0 round trip. The
  // persistent-set restructure belongs to GPU1's on-device loop.
  VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, p.bufferCount};
  VkDescriptorPoolCreateInfo dpci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &dps;
  VkDescriptorPool pool;
  if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &pool) != VK_SUCCESS)
    return SetError(error, "vkCreateDescriptorPool failed");

  VkDescriptorSetAllocateInfo dsai{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &p.setLayout;
  VkDescriptorSet set;
  if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS) {
    vkDestroyDescriptorPool(m_device, pool, nullptr);
    return SetError(error, "vkAllocateDescriptorSets failed");
  }
  std::vector<VkDescriptorBufferInfo> infos(buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffers.size());
  for (size_t i = 0; i < buffers.size(); ++i) {
    infos[i] = {buffers[i]->buffer, 0, buffers[i]->size};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set;
    writes[i].dstBinding = static_cast<uint32_t>(i);
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  vkResetCommandBuffer(m_cmd, 0);
  VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(m_cmd, &bbi);
  vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
  vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1,
                          &set, 0, nullptr);
  if (pushBytes > 0)
    vkCmdPushConstants(m_cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       pushBytes, push);
  vkCmdDispatch(m_cmd, gx, gy, 1);
  vkEndCommandBuffer(m_cmd);

  const bool ok = SubmitAndWait(error);
  vkDestroyDescriptorPool(m_device, pool, nullptr);
  return ok;
}

bool ComputeContext::SubmitAndWait(std::string* error) {
  if (m_lost)
    return SetError(error, "the GPU was lost (VK_ERROR_DEVICE_LOST)");
  if (m_abandoned)
    return SetError(error, "cancelled");
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &m_cmd;
  const bool injected = m_lossCountdown > 0 && --m_lossCountdown == 0;
  VkResult r =
      injected ? VK_ERROR_DEVICE_LOST : vkQueueSubmit(m_queue, 1, &si, m_fence);
  // In 0.1 s slices, never UINT64_MAX: a cancel is seen between them.
  constexpr uint64_t kSliceNs = 100'000'000;
  bool cancelled = false;
  if (r == VK_SUCCESS) {
    do {
      r = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, kSliceNs);
      if (r == VK_TIMEOUT && m_cancel && m_cancel())
        cancelled = true;
    } while (r == VK_TIMEOUT && !cancelled);
  }
  if (r == VK_ERROR_DEVICE_LOST) {
    m_lost = true;
    return SetError(error, "the GPU was lost (VK_ERROR_DEVICE_LOST)");
  }
  if (cancelled) {
    // The submission may still be running: the fence is left as it is, and
    // the context is not used again (a cancelled run ends).
    m_abandoned = true;
    return SetError(error, "cancelled");
  }
  vkResetFences(m_device, 1, &m_fence);
  if (r != VK_SUCCESS)
    return SetError(error, "compute submit/wait failed");
  return true;
}

bool ComputeContext::CreateBoundSet(const Pipeline& p,
                                    const std::vector<const Buffer*>& buffers,
                                    BoundSet& out, std::string* error) {
  if (buffers.size() != p.bufferCount)
    return SetError(error, "bound-set buffer count mismatch");
  VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, p.bufferCount};
  VkDescriptorPoolCreateInfo dpci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &dps;
  if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &out.pool) != VK_SUCCESS)
    return SetError(error, "vkCreateDescriptorPool failed");
  VkDescriptorSetAllocateInfo dsai{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = out.pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &p.setLayout;
  if (vkAllocateDescriptorSets(m_device, &dsai, &out.set) != VK_SUCCESS) {
    Destroy(out);
    return SetError(error, "vkAllocateDescriptorSets failed");
  }
  std::vector<VkDescriptorBufferInfo> infos(buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffers.size());
  for (size_t i = 0; i < buffers.size(); ++i) {
    infos[i] = {buffers[i]->buffer, 0, buffers[i]->size};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = out.set;
    writes[i].dstBinding = static_cast<uint32_t>(i);
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
  return true;
}

void ComputeContext::Destroy(BoundSet& s) {
  if (s.pool)
    vkDestroyDescriptorPool(m_device, s.pool, nullptr);
  s = BoundSet{};
}

bool ComputeContext::BeginBatch(std::string* error) {
  if (m_batchOpen)
    return SetError(error, "a batch is already recording");
  vkResetCommandBuffer(m_cmd, 0);
  VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(m_cmd, &bbi) != VK_SUCCESS)
    return SetError(error, "vkBeginCommandBuffer failed");
  m_batchOpen = true;
  m_batchFirst = true;
  return true;
}

void ComputeContext::BatchBarrier() {
  if (m_batchFirst) {
    m_batchFirst = false;
    return;
  }
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                     VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(
      m_cmd,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      1, &mb, 0, nullptr, 0, nullptr);
}

void ComputeContext::BatchFill(const Buffer& b, VkDeviceSize offset,
                               VkDeviceSize size, uint32_t value) {
  BatchBarrier();
  vkCmdFillBuffer(m_cmd, b.buffer, offset, size, value);
}

void ComputeContext::BatchDispatch(const Pipeline& p, const BoundSet& set,
                                   const void* push, uint32_t pushBytes,
                                   uint32_t gx, uint32_t gy) {
  BatchBarrier();
  vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
  vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1,
                          &set.set, 0, nullptr);
  if (pushBytes > 0)
    vkCmdPushConstants(m_cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       pushBytes, push);
  vkCmdDispatch(m_cmd, gx, gy, 1);
}

bool ComputeContext::SubmitBatch(std::string* error) {
  if (!m_batchOpen)
    return SetError(error, "no batch is recording");
  m_batchOpen = false;
  vkEndCommandBuffer(m_cmd);
  return SubmitAndWait(error);
}

} // namespace gpu
