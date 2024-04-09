set(classes
  vtkVulkanInstance
  vtkVulkanDebug
  vtkVulkanPhysicalDevice
  vtkVulkanRenderWindow)

set(headers
  "${CMAKE_CURRENT_BINARY_DIR}/vtkVulkanError.h"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkRenderingVulkanConfigure.h")

set(nowrap_headers
  vtkVulkanDefines.h
  #  vtkCompositeMapperHelper2.h
  #  vtkCompositePolyDataMapper2Internal.h
)

# add opengl support test executable on windows
#if (VTK_USE_WIN32_OPENGL AND (NOT CMAKE_CROSSCOMPILING OR DEFINED CMAKE_CROSSCOMPILING_EMULATOR))
#  vtk_module_add_executable(vtkTestOpenGLVersion vtkTestOpenGLVersion.cxx)
#  set_target_properties(vtkTestOpenGLVersion
#    PROPERTIES
#    WIN32_EXECUTABLE TRUE)
#  target_link_libraries(vtkTestOpenGLVersion PRIVATE opengl32 VTK::CommonCore)
#  list(APPEND headers vtkTestOpenGLVersion.h)
#endif ()

set(sources)
set(private_headers)
set(private_classes)

set(shader_files)
set(public_shader_files)
unset(shader_h_files)
foreach (file IN LISTS shader_files)
  vtk_encode_string(
    INPUT "${file}"
    EXPORT_SYMBOL "VTKRENDERINGVULKAN2_NO_EXPORT"
    EXPORT_HEADER "vtkRenderingVulkanModule.h"
    HEADER_OUTPUT header
    SOURCE_OUTPUT source)
  list(APPEND sources
    "${source}")
  list(APPEND private_headers
    "${header}")
endforeach ()

foreach (file IN LISTS public_shader_files)
  vtk_encode_string(
    INPUT "${file}"
    EXPORT_SYMBOL "VTKRENDERINGVULKAN_EXPORT"
    EXPORT_HEADER "vtkRenderingOpenGL2Module.h"
    HEADER_OUTPUT header
    SOURCE_OUTPUT source
    ABI_MANGLE_HEADER "vtkABINamespace.h"
    ABI_MANGLE_SYMBOL_BEGIN "VTK_ABI_NAMESPACE_BEGIN"
    ABI_MANGLE_SYMBOL_END "VTK_ABI_NAMESPACE_END")
  list(APPEND sources
    "${source}")
  list(APPEND nowrap_headers
    "${header}")
endforeach ()

# setup factory overrides
set(vulkan_overrides
  #  Actor
  #  CompositePolyDataMapperDelegator
  #  BillboardTextActor3D
  #  Camera
  #  LabeledContourMapper
  #  HardwareSelector
  #  ImageMapper
  #  ImageSliceMapper
  #  Glyph3DMapper
  #  HyperTreeGridMapper
  #  Light
  #  PointGaussianMapper
  #  PolyDataMapper
  #  PolyDataMapper2D
  #  Property
  #  ShaderProperty
  #  Uniforms
  #  Renderer
  #  RenderTimerLog
  #  Skybox
  #  TextActor
  #  TextActor3D
  #  TextMapper
  #  Texture
)

# Now to see about the GPU info list.
list(APPEND classes vtkDummyGPUInfoList)
vtk_object_factory_declare(
  BASE vtkGPUInfoList
  OVERRIDE vtkDummyGPUInfoList)


get_property(_vtk_object_factory_overrides
  DIRECTORY
  PROPERTY _vtk_object_factory_overrides)

foreach (vulkan_override IN LISTS vulkan_overrides)
  vtk_object_factory_declare(
    BASE "vtk${vulkan_override}"
    OVERRIDE "vtkVulkan${vulkan_override}")
endforeach ()

get_property(_vtk_object_factory_overrides
  DIRECTORY
  PROPERTY _vtk_object_factory_overrides)

vtk_object_factory_configure(
  SOURCE_FILE vtk_object_factory_source
  HEADER_FILE vtk_object_factory_header
  EXPORT_MACRO "VTKRENDERINGVULKAN_EXPORT")

message(STATUS ${vtk_object_factory_source})

set(VTK_REPORT_VULKAN_ERRORS ON CACHE BOOL "Enable Vulkan error check and report")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkVulkanError.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkVulkanError.h")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkRenderingVulkanConfigure.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkRenderingVulkanConfigure.h")

vtk_module_add_module(VTK::RenderingVulkan
  CLASSES ${classes}
  SOURCES ${sources} ${vtk_object_factory_source}
  HEADERS ${headers}
  NOWRAP_HEADERS ${nowrap_headers}
  PRIVATE_HEADERS ${private_headers} ${vtk_object_factory_header})

vtk_add_test_mangling(VTK::RenderingVulkan)


