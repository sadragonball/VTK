//
// Created by Fujun Xue(meidofuku@hotmail.com) on 2024/4/9.
//
#pragma once

#include "vtkRect.h" // for vtkRecti
#include "vtkRenderWindow.h"
#include "vtkRenderingVulkanModule.h" // For export macro
#include "vtkType.h"                  // for ivar
#include "vtkVulkanDefines.h"
#include "vtkWrappingHints.h" // For VTK_MARSHALAUTO
#include <map>                // for ivar
#include <set>                // for ivar
#include <string>             // for ivar

VTK_ABI_NAMESPACE_BEGIN
class vtkVulkanRenderDevice;
class VTKRENDERINGVULKAN_EXPORT VTK_MARSHALAUTO vtkVulkanRenderWindow : public vtkRenderWindow
{
public:
  vtkTypeMacro(vtkVulkanRenderWindow, vtkRenderWindow);
  void PrintSelf(std::ostream& os, vtkIndent indent) override;

  void Start() override;

  void Frame() override;

  const char* GetRenderingBackend() override;

  void End() override;

  void Render() override;

  void ReleaseGraphicsResources(vtkWindow*) override;

  vtkSetMacro(FramebufferFlipY, bool);
  vtkGetMacro(FramebufferFlipY, bool);

protected:
  bool FramebufferFlipY;

private:
  VTK_DISABLE_COPY(vtkVulkanRenderWindow);
  vtkVulkanRenderDevice* RenderDevice;
};
VTK_ABI_NAMESPACE_END
