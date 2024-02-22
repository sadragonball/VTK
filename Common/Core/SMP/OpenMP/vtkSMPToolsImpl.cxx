// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "SMP/Common/vtkSMPToolsImpl.h"
#include "SMP/OpenMP/vtkSMPToolsImpl.txx"

#include <cstdlib> // For std::getenv()
#include <omp.h>
#include <stack> // For std::stack

namespace vtk
{
namespace detail
{
namespace smp
{
VTK_ABI_NAMESPACE_BEGIN
static int specifiedNumThreadsOMP; // Default initialized to zero
static std::stack<int>* threadIdStack;

//------------------------------------------------------------------------------
// Must NOT be initialized. Default initialization to zero is necessary.
unsigned int vtkSMPToolsImplOpenMPInitializeCount;

//------------------------------------------------------------------------------
vtkSMPToolsImplOpenMPInitialize::vtkSMPToolsImplOpenMPInitialize()
{
  if (++vtkSMPToolsImplOpenMPInitializeCount == 1)
  {
    threadIdStack = new std::stack<int>;
  }
}

//------------------------------------------------------------------------------
vtkSMPToolsImplOpenMPInitialize::~vtkSMPToolsImplOpenMPInitialize()
{
  if (--vtkSMPToolsImplOpenMPInitializeCount == 0)
  {
    delete threadIdStack;
    threadIdStack = nullptr;
  }
}

//------------------------------------------------------------------------------
template <>
void vtkSMPToolsImpl<BackendType::OpenMP>::Initialize(int numThreads)
{
  const int maxThreads = omp_get_max_threads();
  if (numThreads == 0)
  {
    const char* vtkSmpNumThreads = std::getenv("VTK_SMP_MAX_THREADS");
    if (vtkSmpNumThreads)
    {
      numThreads = std::atoi(vtkSmpNumThreads);
    }
    else if (specifiedNumThreadsOMP)
    {
      specifiedNumThreadsOMP = 0;
      omp_set_num_threads(maxThreads);
    }
  }
#pragma omp single
  if (numThreads > 0)
  {
    numThreads = std::min(numThreads, maxThreads);
    specifiedNumThreadsOMP = numThreads;
    omp_set_num_threads(numThreads);
  }
}

//------------------------------------------------------------------------------
int GetNumberOfThreadsOpenMP()
{
  return specifiedNumThreadsOMP ? specifiedNumThreadsOMP : omp_get_max_threads();
}

//------------------------------------------------------------------------------
bool GetSingleThreadOpenMP()
{
  return threadIdStack->top() == omp_get_thread_num();
}

//------------------------------------------------------------------------------
template <>
int vtkSMPToolsImpl<BackendType::OpenMP>::GetEstimatedNumberOfThreads()
{
  return GetNumberOfThreadsOpenMP();
}

//------------------------------------------------------------------------------
template <>
int vtkSMPToolsImpl<BackendType::OpenMP>::GetEstimatedDefaultNumberOfThreads()
{
  return omp_get_max_threads();
}

//------------------------------------------------------------------------------
template <>
bool vtkSMPToolsImpl<BackendType::OpenMP>::GetSingleThread()
{
  return GetSingleThreadOpenMP();
}

//------------------------------------------------------------------------------
void vtkSMPToolsImplForOpenMP(vtkIdType first, vtkIdType last, vtkIdType grain,
  ExecuteFunctorPtrType functorExecuter, void* functor, bool nestedActivated)
{
  if (grain <= 0)
  {
    vtkIdType estimateGrain = (last - first) / (GetNumberOfThreadsOpenMP() * 4);
    grain = (estimateGrain > 0) ? estimateGrain : 1;
  }

  omp_set_nested(nestedActivated);

#pragma omp single
  threadIdStack->emplace(omp_get_thread_num());

#pragma omp parallel for schedule(runtime)
  for (vtkIdType from = first; from < last; from += grain)
  {
    functorExecuter(functor, from, grain, last);
  }

#pragma omp single
  threadIdStack->pop();
}

VTK_ABI_NAMESPACE_END
} // namespace smp
} // namespace detail
} // namespace vtk
