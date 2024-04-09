// Little Vulkan Engine (LVE)
// Created by Fujun Xue (meidofuku@hotmail.com) on 2024/4/9.
//

#include "vtkVulkanActor.h"

// #include "vtkDepthPeelingPass.h"
// #include "vtkDualDepthPeelingPass.h"
#include "vtkInformation.h"
#include "vtkInformationIntegerKey.h"
#include "vtkMapper.h"
#include "vtkMatrix3x3.h"
#include "vtkMatrix4x4.h"
#include "vtkObjectFactory.h"
// #include "vtkOpenGLError.h"
// #include "vtkOpenGLPolyDataMapper.h"
// #include "vtkOpenGLRenderWindow.h"
// #include "vtkOpenGLRenderer.h"
// #include "vtkOpenGLState.h"
#include "vtkProperty.h"
#include "vtkRenderWindow.h"
#include "vtkTransform.h"

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkVulkanActor);

void vtkVulkanActor::PrintSelf(ostream& os, vtkIndent indent)
{
  vtkActor::PrintSelf(os, indent);
}
void vtkVulkanActor::Render(vtkRenderer* ren, vtkMapper* mapper)
{
  vtkActor::Render(ren, mapper);
}
void vtkVulkanActor::GetKeyMatrices(vtkMatrix4x4*& WCVCMatrix, vtkMatrix3x3*& normalMatrix) {}
vtkInformationIntegerKey* vtkVulkanActor::GLDepthMaskOverride()
{
  return nullptr;
}
vtkVulkanActor::vtkVulkanActor() {}
vtkVulkanActor::~vtkVulkanActor() {}

VTK_ABI_NAMESPACE_BEGIN
