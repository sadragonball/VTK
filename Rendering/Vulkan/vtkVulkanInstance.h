#pragma once
#include "vtkVulkanDefines.h"

#include "vtkRenderingVulkanConfigure.h"
#include "vtkRenderingVulkanModule.h" // For export macro
#include "vtkType.h"                  // for ivar
#include "vtkWrappingHints.h"         // For VTK_MARSHALAUTO

#include "vtkObject.h"

#include <memory>
#include <vector>

VTK_ABI_NAMESPACE_BEGIN

class VTKRENDERINGVULKAN_EXPORT VTK_MARSHALAUTO vtkVulkanInstance : public vtkObject
{
public:
  static vtkVulkanInstance* New();
  vtkTypeMacro(vtkVulkanInstance, vtkObject);
  void PrintSelf(std::ostream& os, vtkIndent indent) override;

  void pickPhysicalDevice();

  bool isLayerAvailable(const char* layerName, vtkTypeUInt32& Version) const;
  bool isExtensionAvailable(const char* extensionName) const;
  bool isExtensionEnabled(const char* extensionName) const;

  bool isDeviceSuitable(VkPhysicalDevice device) const;

  inline VkInstance getVkInstance() const { return instance; }

  inline VkPhysicalDevice getVkPhysicalDevice() const { return physicalDevice; }

  std::vector<const char*> getEnabledExtensions() { enabledExtensions; }

protected:
  explicit vtkVulkanInstance();
  ~vtkVulkanInstance();

private:
  VTK_DISABLE_COPY_MOVE(vtkVulkanInstance);

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice;
  vtkTypeUInt32 version = VK_API_VERSION_1_1;

  std::vector<VkLayerProperties> layers;
  std::vector<VkExtensionProperties> availableExtensions;
  std::vector<const char*> enabledExtensions;
};

VTK_ABI_NAMESPACE_END