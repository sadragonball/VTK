#include "vtkVulkanPhysicalDevice.h"

#include "vtkObjectFactory.h"
#include "vtkVulkanInstance.h"

#include <cassert>
#include <stdexcept>

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkVulkanPhysicalDevice);

vtkVulkanPhysicalDevice::~vtkVulkanPhysicalDevice() {}

vtkVulkanPhysicalDevice::vtkVulkanPhysicalDevice() {}

void vtkVulkanPhysicalDevice::InitializePhysicalDevice()
{
  if (PhysicalDeviceHandle)
  {
    vtkErrorMacro("Physical device can't be null");
  }
  vkGetPhysicalDeviceProperties(PhysicalDeviceHandle, &DeviceProperties);
  vkGetPhysicalDeviceFeatures(PhysicalDeviceHandle, &DeviceFeatures);
  vkGetPhysicalDeviceMemoryProperties(PhysicalDeviceHandle, &DeviceMemoryProperties);
  vtkTypeUInt32 queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDeviceHandle, &queueFamilyCount, nullptr);
  QueueFamilyProperties.resize(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(
    PhysicalDeviceHandle, &queueFamilyCount, QueueFamilyProperties.data());

  vtkTypeUInt32 extensionCount = 0;
  vkEnumerateDeviceExtensionProperties(PhysicalDeviceHandle, nullptr, &extensionCount, nullptr);
  if (extensionCount > 0)
  {
    SupportedExtensions.resize(extensionCount);
    auto res = vkEnumerateDeviceExtensionProperties(
      PhysicalDeviceHandle, nullptr, &extensionCount, SupportedExtensions.data());
    if (res != VK_SUCCESS)
    {
      std::runtime_error("Failed to get supported extensions");
    }
  }
}

vtkTypeUInt32 vtkVulkanPhysicalDevice::FindQueueFamily(VkQueueFlags flags) const
{
  VkQueueFlags flagsOpt = flags;
  if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
  {
    flags = flags & (~VK_QUEUE_TRANSFER_BIT);
    flagsOpt = flags | VK_QUEUE_TRANSFER_BIT;
  }
  static constexpr vtkTypeUInt32 invalidFamilyIndex = std::numeric_limits<uint32_t>::max();
  vtkTypeUInt32 familyIndex = invalidFamilyIndex;

  for (vtkTypeUInt32 i = 0; i < QueueFamilyProperties.size(); ++i)
  {
    const auto& props = QueueFamilyProperties[i];
    if (props.queueFlags == flags || props.queueFlags == flagsOpt)
    {
      familyIndex = i;
      break;
    }
  }

  if (familyIndex == invalidFamilyIndex)
  {
    for (vtkTypeUInt32 i = 0; i < QueueFamilyProperties.size(); ++i)
    {
      const auto& props = QueueFamilyProperties[i];
      if ((props.queueFlags & flags) == flags)
      {
        familyIndex = i;
        break;
      }
    }
  }

  if (familyIndex != invalidFamilyIndex)
  {
    if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
    {
#ifdef _DEBUG
      const auto& props = QueueFamilyProperties[familyIndex];
      // Queues supporting graphics and/or compute operations must report (1,1,1)
      // in minImageTransferGranularity, meaning that there are no additional restrictions
      // on the granularity of image transfer operations for these queues (4.1).
      assert(props.minImageTransferGranularity.width == 1 &&
        props.minImageTransferGranularity.height == 1 &&
        props.minImageTransferGranularity.depth == 1);
#endif
    }
  }
  else
  {
    std::runtime_error("Faild to find suitable queue family!");
  }

  return familyIndex;
}

bool vtkVulkanPhysicalDevice::IsExtensionSupported(const char* extensionName) const
{
  for (const auto& extension : SupportedExtensions)
  {
    if (strcmp(extension.extensionName, extensionName) == 0)
    {
      return true;
    }
  }

  return false;
}

bool vtkVulkanPhysicalDevice::CheckPresentSupport(
  vtkTypeUInt32 queueFamilyIndex, VkSurfaceKHR VkSurface) const
{
  VkBool32 presentSupport = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(
    this->PhysicalDeviceHandle, queueFamilyIndex, VkSurface, &presentSupport);
  return presentSupport;
}

vtkTypeUInt32 vtkVulkanPhysicalDevice::GetMemoryTypeIndex(
  vtkTypeUInt32 memoryTypeBitsRequirement, VkMemoryPropertyFlags requiredProperties) const
{
  // not for now
  return 0;
}
void vtkVulkanPhysicalDevice::PrintSelf(ostream& os, vtkIndent indent)
{
  vtkObject::PrintSelf(os, indent);
}
VTK_ABI_NAMESPACE_END