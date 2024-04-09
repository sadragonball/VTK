// Little Vulkan Engine (LVE)
// Created by Fujun Xue (meidofuku@hotmail.com) on 2024/4/9.
//

#pragma once

#include "vtkActor.h"
#include "vtkRenderingVulkanModule.h"
#include "vtkVulkanDefines.h"
#include "vtkWrappingHints.h"

VTK_ABI_NAMESPACE_BEGIN
class vtkInformationIntegerKey;
class vtkVulkanRenderer;
class vtkMatrix4x4;
class vtkMatrix3x3;

class VTKRENDERINGVULKAN_EXPORT VTK_MARSHALAUTO vtkVulkanActor : public vtkActor
{
public:
  static vtkVulkanActor* New();
  vtkTypeMacro(vtkVulkanActor, vtkActor);
  void PrintSelf(std::ostream& os, vtkIndent indent) override;

  void Render(vtkRenderer* ren, vtkMapper* mapper) override;

  virtual void GetKeyMatrices(vtkMatrix4x4*& WCVCMatrix, vtkMatrix3x3*& normalMatrix);

  static vtkInformationIntegerKey* GLDepthMaskOverride();

protected:
  vtkVulkanActor();
  ~vtkVulkanActor() override;

  vtkMatrix4x4* MCWCMatrix;
  vtkMatrix3x3* NormalMatrix;
  vtkTransform* NormalTransform;
  vtkTimeStamp KeyMatrixTime;

private:
  VTK_DISABLE_COPY(vtkVulkanActor);
};
VTK_ABI_NAMESPACE_END