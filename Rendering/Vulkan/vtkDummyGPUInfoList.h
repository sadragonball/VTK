#pragma once

#include "vtkGPUInfoList.h"
#include "vtkRenderingVulkanModule.h" // For export macro

class VTKRENDERINGVULKAN_EXPORT vtkDummyGPUInfoListVk : public vtkGPUInfoList
{
public:
  static vtkDummyGPUInfoListVk* New();
  vtkTypeMacro(vtkDummyGPUInfoListVk, vtkGPUInfoList);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  /**
   * Build the list of vtkInfoGPU if not done yet.
   * \post probed: IsProbed()
   */
  void Probe() override;

protected:
  ///@{
  /**
   * Default constructor.
   */
  vtkDummyGPUInfoListVk();
  ~vtkDummyGPUInfoListVk() override;
  ///@}

private:
  vtkDummyGPUInfoListVk(const vtkDummyGPUInfoListVk&) = delete;
  void operator=(const vtkDummyGPUInfoListVk&) = delete;
};
