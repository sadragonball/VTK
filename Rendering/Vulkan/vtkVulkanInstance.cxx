#include "vtkVulkanInstance.h"
#include "vtkObjectFactory.h"

#include "vtkVulkanDebug.h"

#include <array>
#include <iostream>
#include <stdexcept>

#ifdef VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
#endif

namespace
{
// clang-format off
static constexpr std::array<const char*, 1> ValidationLayerNames =
{
    // Unified validation layer used on Desktop and Mobile platforms
    "VK_LAYER_KHRONOS_validation"
};
// clang-format on

bool EnumerateInstanceExtensions(
  const char* LayerName, std::vector<VkExtensionProperties>& Extensions)
{
  vtkTypeUInt32 extensionCount = 0;

  if (vkEnumerateInstanceExtensionProperties(LayerName, &extensionCount, nullptr) != VK_SUCCESS)
    return false;

  Extensions.resize(extensionCount);
  if (vkEnumerateInstanceExtensionProperties(LayerName, &extensionCount, Extensions.data()) !=
    VK_SUCCESS)
  {
    Extensions.clear();
    return false;
  }
  // VERIFY(ExtensionCount == Extensions.size(),
  //        "The number of extensions written by vkEnumerateInstanceExtensionProperties is not
  //        consistent " "with the count returned in the first call. This is a Vulkan loader bug.");

  return true;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
  VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
  VkDebugUtilsMessageTypeFlagsEXT messageType,
  const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{

  if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
  {
    std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
  }
  return VK_FALSE;
}

} // namespace

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkVulkanInstance);

vtkVulkanInstance::vtkVulkanInstance()
{
  // Enumerate available layers
  vtkTypeUInt32 layerCount = 0;
  auto res = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
  layers.resize(layerCount);
  vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

  // Enumerate available instance extensions
  std::vector<const char*> instanceExtensions;

  if (!EnumerateInstanceExtensions(nullptr, availableExtensions))
  {
    throw std::runtime_error("Failed to enumerate instance extensions!");
  }

  // Extension check

  if (isExtensionAvailable(VK_KHR_SURFACE_EXTENSION_NAME))
  {
    instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  }

#ifndef NDEBUG
  if (isExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
  {
    instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }
#endif

#if defined(VK_USE_PLATFORM_WIN32_KHR)
  instanceExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif

  std::vector<const char*> instanceLayers;

  // layer check
#ifndef NDEBUG
  for (const auto* layerName : ValidationLayerNames)
  {
    //~0U is NULL
    vtkTypeUInt32 layerVer = ~0U;
    if (!isLayerAvailable(layerName, layerVer))
    {
      std::runtime_error("Validation layers requested, but not available!");
    }
    instanceLayers.push_back(layerName);
  }
#endif

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "VT STUDIO";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName = "VT ENGINE";
  appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.apiVersion = this->version;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = (vtkTypeUInt32)instanceExtensions.size();
  createInfo.ppEnabledExtensionNames = instanceExtensions.data();
  createInfo.enabledLayerCount = (vtkTypeUInt32)instanceLayers.size();
  createInfo.ppEnabledLayerNames = instanceLayers.data();

#ifndef NDEBUG

  // For tracing vkCreateInstance
  VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};

  debugCreateInfo = {};
  debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

  debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

  debugCreateInfo.pfnUserCallback = DebugCallback;
  debugCreateInfo.pUserData = nullptr; // Optional

  createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
#else
  createInfo.pNext = nullptr;
#endif

  if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create instance!");
  }

  enabledExtensions = std::move(instanceExtensions);

  setupDebugUtils(instance,
    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
    VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
    0, nullptr, nullptr);
}

vtkVulkanInstance::~vtkVulkanInstance()
{
  freeDebug(instance);
  vkDestroyInstance(instance, nullptr);
}

bool vtkVulkanInstance::isDeviceSuitable(VkPhysicalDevice device) const
{
  const auto IsGraphicsAndComputeQueueSupported = [](VkPhysicalDevice dev)
  {
    vtkTypeUInt32 queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
    // VERIFY_EXPR(QueueFamilyCount > 0);
    std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, queueFamilyProperties.data());
    // VERIFY_EXPR(QueueFamilyCount == QueueFamilyProperties.size());

    // If an implementation exposes any queue family that supports graphics operations,
    // at least one queue family of at least one physical device exposed by the implementation
    // must support both graphics and compute operations.
    for (const auto& QueueFamilyProps : queueFamilyProperties)
    {
      if ((QueueFamilyProps.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
        (QueueFamilyProps.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
      {
        return true;
      }
    }
    return false;
  };

  return IsGraphicsAndComputeQueueSupported(device);
}

bool vtkVulkanInstance::isLayerAvailable(const char* layerName, vtkTypeUInt32& Version) const
{
  for (const auto& Layer : layers)
  {
    if (strcmp(Layer.layerName, layerName) == 0)
    {
      Version = Layer.specVersion;
      return true;
    }
  }
  return false;
}

bool vtkVulkanInstance::isExtensionAvailable(const char* extensionName) const
{
  for (auto& extension : availableExtensions)
  {
    if (strcmp(extension.extensionName, extensionName) == 0)
    {
      return true;
    }
  }
  return false;
}

bool vtkVulkanInstance::isExtensionEnabled(const char* extensionName) const
{
  for (auto& extension : enabledExtensions)
  {
    if (strcmp(extension, extensionName) == 0)
    {
      return true;
    }
  }

  return false;
}

void vtkVulkanInstance::pickPhysicalDevice()
{
  vtkTypeUInt32 deviceCount = 0;
  vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

  if (deviceCount == 0)
  {
    throw std::runtime_error("Failed to find GPUs with Vulkan support!");
  }

  std::vector<VkPhysicalDevice> devices((size_t)deviceCount);
  vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

  for (const auto& device : devices)
  {
    if (isDeviceSuitable(device))
    {
      this->physicalDevice = device;
      break;
    }
  }

  if (physicalDevice == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Failed to find a suitable physical device!");
  }
}
void vtkVulkanInstance::PrintSelf(ostream& os, vtkIndent indent)
{
  vtkObject::PrintSelf(os, indent);
}
VTK_ABI_NAMESPACE_END