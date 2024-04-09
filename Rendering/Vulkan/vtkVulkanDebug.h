#pragma once
#include "vtkVulkanDefines.h"
VTK_ABI_NAMESPACE_BEGIN
bool setupDebugUtils(VkInstance instance, VkDebugUtilsMessageSeverityFlagsEXT messageSeverity,
  VkDebugUtilsMessageTypeFlagsEXT messageType, uint32_t IgnoreMessageCount,
  const char* const* ppIgnoreMessageNames, void* pUserData);

void freeDebug(VkInstance instance);

const char* vkResultToString(VkResult errorCode);

// template <typename VulkanObjectType>
// void setVulkanObjectName(VkDevice device, VulkanObjectType vkObject, const char* name);
VTK_ABI_NAMESPACE_END