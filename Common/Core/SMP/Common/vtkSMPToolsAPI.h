/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkSMPToolsAPI.h

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#ifndef vtkSMPToolsAPI_h
#define vtkSMPToolsAPI_h

#include "vtkCommonCoreModule.h" // For export macro
#include "vtkNew.h"
#include "vtkObject.h"

#include "SMP/Common/vtkSMPToolsImpl.h"
#if VTK_SMP_ENABLE_SEQUENTIAL
#include "SMP/Sequential/vtkSMPToolsImpl.txx"
#endif
#if VTK_SMP_ENABLE_STDTHREAD
#include "SMP/STDThread/vtkSMPToolsImpl.txx"
#endif
#if VTK_SMP_ENABLE_TBB
#include "SMP/TBB/vtkSMPToolsImpl.txx"
#endif
#if VTK_SMP_ENABLE_OPENMP
#include "SMP/OpenMP/vtkSMPToolsImpl.txx"
#endif

namespace vtk
{
namespace detail
{
namespace smp
{

#if VTK_SMP_DEFAULT_IMPLEMENTATION_SEQUENTIAL
const BackendType DefaultBackend = BackendType::Sequential;
#elif VTK_SMP_DEFAULT_IMPLEMENTATION_STDTHREAD
const BackendType DefaultBackend = BackendType::STDThread;
#elif VTK_SMP_DEFAULT_IMPLEMENTATION_TBB
const BackendType DefaultBackend = BackendType::TBB;
#elif VTK_SMP_DEFAULT_IMPLEMENTATION_OPENMP
const BackendType DefaultBackend = BackendType::OpenMP;
#endif

using vtkSMPToolsDefaultImpl = vtkSMPToolsImpl<DefaultBackend>;

class VTKCOMMONCORE_EXPORT vtkSMPToolsAPI
{
public:
  //--------------------------------------------------------------------------------
  static vtkSMPToolsAPI& GetInstance();

  //--------------------------------------------------------------------------------
  BackendType GetBackendType();

  //--------------------------------------------------------------------------------
  const char* GetBackend();

  //--------------------------------------------------------------------------------
  void SetBackend(const char* type);

  //--------------------------------------------------------------------------------
  void Initialize(int numThreads = 0);

  //--------------------------------------------------------------------------------
  int GetEstimatedNumberOfThreads();

  //--------------------------------------------------------------------------------
  template <typename FunctorInternal>
  void For(vtkIdType first, vtkIdType last, vtkIdType grain, FunctorInternal& fi)
  {
    switch (this->ActivatedBackend)
    {
      case BackendType::Sequential:
        this->SequentialBackend.For(first, last, grain, fi);
        break;
      case BackendType::STDThread:
        this->STDThreadBackend.For(first, last, grain, fi);
        break;
      case BackendType::TBB:
        this->TBBBackend.For(first, last, grain, fi);
        break;
      case BackendType::OpenMP:
        this->OpenMPBackend.For(first, last, grain, fi);
        break;
    }
  }

  //--------------------------------------------------------------------------------
  template <typename InputIt, typename OutputIt, typename Functor>
  void Transform(InputIt inBegin, InputIt inEnd, OutputIt outBegin, Functor& transform)
  {
    switch (this->ActivatedBackend)
    {
      case BackendType::Sequential:
        this->SequentialBackend.Transform(inBegin, inEnd, outBegin, transform);
        break;
      case BackendType::STDThread:
        this->STDThreadBackend.Transform(inBegin, inEnd, outBegin, transform);
        break;
      case BackendType::TBB:
        this->TBBBackend.Transform(inBegin, inEnd, outBegin, transform);
        break;
      case BackendType::OpenMP:
        this->OpenMPBackend.Transform(inBegin, inEnd, outBegin, transform);
        break;
    }
  }

  //--------------------------------------------------------------------------------
  template <typename InputIt1, typename InputIt2, typename OutputIt, typename Functor>
  void Transform(
    InputIt1 inBegin1, InputIt1 inEnd, InputIt2 inBegin2, OutputIt outBegin, Functor& transform)
  {
    switch (this->ActivatedBackend)
    {
      case BackendType::Sequential:
        this->SequentialBackend.Transform(inBegin1, inEnd, inBegin2, outBegin, transform);
        break;
      case BackendType::STDThread:
        this->STDThreadBackend.Transform(inBegin1, inEnd, inBegin2, outBegin, transform);
        break;
      case BackendType::TBB:
        this->TBBBackend.Transform(inBegin1, inEnd, inBegin2, outBegin, transform);
        break;
      case BackendType::OpenMP:
        this->OpenMPBackend.Transform(inBegin1, inEnd, inBegin2, outBegin, transform);
        break;
    }
  }

  //--------------------------------------------------------------------------------
  template <typename Iterator, typename T>
  void Fill(Iterator begin, Iterator end, const T& value)
  {
    switch (this->ActivatedBackend)
    {
      case BackendType::Sequential:
        this->SequentialBackend.Fill(begin, end, value);
        break;
      case BackendType::STDThread:
        this->STDThreadBackend.Fill(begin, end, value);
        break;
      case BackendType::TBB:
        this->TBBBackend.Fill(begin, end, value);
        break;
      case BackendType::OpenMP:
        this->OpenMPBackend.Fill(begin, end, value);
        break;
    }
  }

  //--------------------------------------------------------------------------------
  template <typename RandomAccessIterator>
  void Sort(RandomAccessIterator begin, RandomAccessIterator end)
  {
    switch (this->ActivatedBackend)
    {
      case BackendType::Sequential:
        this->SequentialBackend.Sort(begin, end);
        break;
      case BackendType::STDThread:
        this->STDThreadBackend.Sort(begin, end);
        break;
      case BackendType::TBB:
        this->TBBBackend.Sort(begin, end);
        break;
      case BackendType::OpenMP:
        this->OpenMPBackend.Sort(begin, end);
        break;
    }
  }

  //--------------------------------------------------------------------------------
  template <typename RandomAccessIterator, typename Compare>
  void Sort(RandomAccessIterator begin, RandomAccessIterator end, Compare comp)
  {
    switch (this->ActivatedBackend)
    {
      case BackendType::Sequential:
        this->SequentialBackend.Sort(begin, end, comp);
        break;
      case BackendType::STDThread:
        this->STDThreadBackend.Sort(begin, end, comp);
        break;
      case BackendType::TBB:
        this->TBBBackend.Sort(begin, end, comp);
        break;
      case BackendType::OpenMP:
        this->OpenMPBackend.Sort(begin, end, comp);
        break;
    }
  }

  // disable copying
  vtkSMPToolsAPI(vtkSMPToolsAPI const&) = delete;
  void operator=(vtkSMPToolsAPI const&) = delete;

private:
  //--------------------------------------------------------------------------------
  vtkSMPToolsAPI(){};

  /**
   * Indicate which backend to use.
   */
  BackendType ActivatedBackend = DefaultBackend;

  /**
   * Sequential backend
   */
#if VTK_SMP_ENABLE_SEQUENTIAL
  vtkSMPToolsImpl<BackendType::Sequential> SequentialBackend;
#else
  vtkSMPToolsDefaultImpl SequentialBackend;
#endif

  /**
   * STDThread backend
   */
#if VTK_SMP_ENABLE_STDTHREAD
  vtkSMPToolsImpl<BackendType::STDThread> STDThreadBackend;
#else
  vtkSMPToolsDefaultImpl STDThreadBackend;
#endif

  /**
   * TBB backend
   */
#if VTK_SMP_ENABLE_TBB
  vtkSMPToolsImpl<BackendType::TBB> TBBBackend;
#else
  vtkSMPToolsDefaultImpl TBBBackend;
#endif

  /**
   * TBB backend
   */
#if VTK_SMP_ENABLE_OPENMP
  vtkSMPToolsImpl<BackendType::OpenMP> OpenMPBackend;
#else
  vtkSMPToolsDefaultImpl OpenMPBackend;
#endif
};

} // namespace smp
} // namespace detail
} // namespace vtk

#endif
