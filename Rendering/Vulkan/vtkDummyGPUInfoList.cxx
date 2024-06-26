/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkDummyGPUInfoListVk.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkDummyGPUInfoList.h"

#include "vtkGPUInfoListArray.h"

#include "vtkObjectFactory.h"
#include <cassert>

vtkStandardNewMacro(vtkDummyGPUInfoListVk);

//------------------------------------------------------------------------------
// Description:
// Build the list of vtkInfoGPU if not done yet.
// \post probed: IsProbed()
void vtkDummyGPUInfoListVk::Probe()
{
  if (!this->Probed)
  {
    this->Probed = true;
    this->Array = new vtkGPUInfoListArray;
    this->Array->v.resize(0); // no GPU.
  }
  assert("post: probed" && this->IsProbed());
}

//------------------------------------------------------------------------------
vtkDummyGPUInfoListVk::vtkDummyGPUInfoListVk() = default;

//------------------------------------------------------------------------------
vtkDummyGPUInfoListVk::~vtkDummyGPUInfoListVk() = default;

//------------------------------------------------------------------------------
void vtkDummyGPUInfoListVk::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
