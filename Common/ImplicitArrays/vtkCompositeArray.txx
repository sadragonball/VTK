#include "vtkCompositeArray.h"

#include "vtkDataArray.h"

//-----------------------------------------------------------------------
namespace vtk
{
VTK_ABI_NAMESPACE_BEGIN
template <typename T>
vtkSmartPointer<vtkCompositeArray<T>> ConcatenateDataArrays(
  const std::vector<vtkDataArray*>& arrays)
{
  if (arrays.size() == 0)
  {
    return nullptr;
  }
  if (arrays.size() == 1)
  {
    vtkNew<vtkCompositeArray<T>> composite;
    composite->SetBackend(std::make_shared<vtkCompositeImplicitBackend<T>>(arrays[0], nullptr));
    composite->SetNumberOfComponents(arrays[0]->GetNumberOfComponents());
    composite->SetNumberOfTuples(arrays[0]->GetNumberOfTuples());
    return composite;
  }
  vtkIdType nComps = arrays[0]->GetNumberOfComponents();
  for (auto arr : arrays)
  {
    if (arr->GetNumberOfComponents() != nComps)
    {
      vtkErrorWithObjectMacro(nullptr, "Number of components of all the arrays are not equal");
      return nullptr;
    }
  }
  std::vector<vtkSmartPointer<vtkDataArray>> lifetimeBuffer;
  lifetimeBuffer.assign(arrays.begin(), arrays.end());
  std::vector<vtkSmartPointer<vtkCompositeArray<T>>> newComps;
  while (lifetimeBuffer.size() != 1)
  {
    newComps.clear();
    for (int i = 0; i < static_cast<int>(lifetimeBuffer.size() - 1); i += 2)
    {
      vtkNew<vtkCompositeArray<T>> composite;
      composite->SetBackend(
        std::make_shared<vtkCompositeImplicitBackend<T>>(lifetimeBuffer[i], lifetimeBuffer[i + 1]));
      composite->SetNumberOfComponents(lifetimeBuffer[i]->GetNumberOfComponents());
      composite->SetNumberOfTuples(
        lifetimeBuffer[i]->GetNumberOfTuples() + lifetimeBuffer[i + 1]->GetNumberOfTuples());
      newComps.emplace_back(composite);
    }
    if (lifetimeBuffer.size() % 2 != 0)
    {
      vtkNew<vtkCompositeArray<T>> composite;
      composite->SetBackend(
        std::make_shared<vtkCompositeImplicitBackend<T>>(newComps.back(), lifetimeBuffer.back()));
      composite->SetNumberOfComponents(lifetimeBuffer.back()->GetNumberOfComponents());
      composite->SetNumberOfTuples(
        newComps.back()->GetNumberOfTuples() + lifetimeBuffer.back()->GetNumberOfTuples());
      newComps.back() = composite;
    }
    lifetimeBuffer.assign(newComps.begin(), newComps.end());
  }
  return newComps[0];
}
VTK_ABI_NAMESPACE_END
}