#pragma once
#include "vtkVulkanDefines.h"

#include "vtkObject.h"
#include "vtkRenderingVulkanConfigure.h"
#include "vtkRenderingVulkanModule.h" // For export macro
#include "vtkType.h"                  // for ivar
#include "vtkWrappingHints.h"         // For VTK_MARSHALAUTO

#include <vector>

VTK_ABI_NAMESPACE_BEGIN
class VulkanInstance;
class VTKRENDERINGVULKAN_EXPORT VTK_MARSHALAUTO vtkVulkanPhysicalDevice : public vtkObject
{
public:
  //  struct CreateInfo
  //  {
  //    const VulkanInstance& Instance;
  //    const VkPhysicalDevice PhysicalDeviceHandle;
  //    bool LogExtensions = false;
  //  };
  //  static std::unique_ptr<VulkanPhysicalDevice> Create(const CreateInfo& info);
  static vtkVulkanPhysicalDevice* New();
  vtkTypeMacro(vtkVulkanPhysicalDevice, vtkObject);
  void PrintSelf(std::ostream& os, vtkIndent indent) override;

protected:
  vtkVulkanPhysicalDevice();
  ~vtkVulkanPhysicalDevice();

  vtkGetMacro(PhysicalDeviceHandle, VkPhysicalDevice);
  vtkSetMacro(PhysicalDeviceHandle, VkPhysicalDevice);

  void InitializePhysicalDevice();

  vtkTypeUInt32 FindQueueFamily(VkQueueFlags flags) const;

  bool IsExtensionSupported(const char* extensionName) const;

  bool CheckPresentSupport(vtkTypeUInt32 queueFamilyIndex, VkSurfaceKHR VkSurface) const;

  vtkTypeUInt32 GetMemoryTypeIndex(
    vtkTypeUInt32 memoryTypeBitsRequirement, VkMemoryPropertyFlags requiredProperties) const;

private:
  VkPhysicalDevice PhysicalDeviceHandle;
  VkPhysicalDeviceProperties DeviceProperties = {};
  VkPhysicalDeviceFeatures DeviceFeatures = {};
  VkPhysicalDeviceMemoryProperties DeviceMemoryProperties = {};
  // ExtensionFeatures                    extFeatures            = {};
  // ExtensionProperties                  extProperties          = {};
  std::vector<VkQueueFamilyProperties> QueueFamilyProperties;
  std::vector<VkExtensionProperties> SupportedExtensions;
};
VTK_ABI_NAMESPACE_END