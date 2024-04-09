
vtk_module_third_party_external(
    PACKAGE             Vulkan
    TARGETS             Vulkan::Vulkan
    INCLUDE_DIRS        VULKAN_INCLUDE_DIR
    LIBRARIES           VULKAN_LIBRARIES)

if(MSVC)
  vtk_module_compile_options(Vulkan::Vulkan
    INTERFACE "/utf-8"
  )
endif ()