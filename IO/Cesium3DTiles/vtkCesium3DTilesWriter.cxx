/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkCesium3DTilesWriter.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkCesium3DTilesWriter.h"

#include "vtkCellArray.h"
#include "vtkDirectory.h"
#include "vtkImageReader2.h"
#include "vtkInformation.h"
#include "vtkLookupTable.h"
#include "vtkMath.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkPolyDataNormals.h"
#include "vtkRenderWindow.h"
#include "vtkRenderer.h"
#include "vtkTransform.h"
#include "vtkTransformFilter.h"
#include "vtksys/FStream.hxx"
#include <vtkDataObjectTreeIterator.h>
#include <vtkIncrementalOctreeNode.h>
#include <vtkIncrementalOctreePointLocator.h>
#include <vtkJPEGReader.h>
#include <vtkLogger.h>
#include <vtkPNGReader.h>
#include <vtkPolyDataMapper.h>
#include <vtkStringArray.h>
#include <vtkTexture.h>
#include <vtksys/SystemTools.hxx>

#include "TreeInformation.h"

#include <sstream>

using namespace vtksys;

vtkStandardNewMacro(vtkCesium3DTilesWriter);

namespace
{
//------------------------------------------------------------------------------
/**
 * Add building centers to the octree
 */
vtkSmartPointer<vtkIncrementalOctreePointLocator> BuildOctree(
  std::vector<vtkSmartPointer<vtkCompositeDataSet>>& buildings,
  const std::array<double, 6>& wholeBB, int buildingsPerTile)
{
  vtkNew<vtkPoints> points;
  points->SetDataTypeToDouble();
  vtkNew<vtkIncrementalOctreePointLocator> octree;
  octree->SetMaxPointsPerLeaf(buildingsPerTile);
  octree->InitPointInsertion(points, &wholeBB[0]);

  // TreeInformation::PrintBounds("octreeBB", &wholeBB[0]);
  for (size_t i = 0; i < buildings.size(); ++i)
  {
    double bb[6];
    buildings[i]->GetBounds(bb);
    double center[3] = { (bb[0] + bb[1]) / 2.0, (bb[2] + bb[3]) / 2, (bb[4] + bb[5]) / 2 };
    octree->InsertNextPoint(center);
    // std::cout << "insert: " << center[0] << ", " << center[1] << ", " << center[2]
    //           << " number of nodes: " << octree->GetNumberOfNodes() << std::endl;
  }
  return octree;
}

//------------------------------------------------------------------------------
std::array<double, 6> AddBuildingsWithTexture(vtkMultiBlockDataSet* root,
  const std::string& texturePath, const double* fileOffset, bool saveTextures,

  std::vector<vtkSmartPointer<vtkCompositeDataSet>>& buildings, std::array<double, 3>& offset)
{
  std::array<double, 6> wholeBB;
  root->GetBounds(&wholeBB[0]);

  // translate the buildings so that the minimum wholeBB is at 0,0,0
  offset = { { wholeBB[0], wholeBB[2], wholeBB[4] } };
  vtkNew<vtkTransformFilter> f;
  vtkNew<vtkTransform> t;
  t->Identity();
  t->Translate(-offset[0], -offset[1], -offset[2]);
  f->SetTransform(t);
  f->SetInputData(root);
  f->Update();
  vtkMultiBlockDataSet* tr = vtkMultiBlockDataSet::SafeDownCast(f->GetOutputDataObject(0));
  tr->GetBounds(&wholeBB[0]);

  auto buildingIt = vtk::TakeSmartPointer(tr->NewTreeIterator());
  buildingIt->VisitOnlyLeavesOff();
  buildingIt->TraverseSubTreeOff();
  for (buildingIt->InitTraversal(); !buildingIt->IsDoneWithTraversal(); buildingIt->GoToNextItem())
  {
    auto building = vtkMultiBlockDataSet::SafeDownCast(buildingIt->GetCurrentDataObject());
    if (!building)
    {
      buildings.clear();
      return wholeBB;
    }
    buildings.push_back(building);
  }

  std::transform(offset.begin(), offset.end(), fileOffset, offset.begin(), std::plus<double>());

  return wholeBB;
}
};

//------------------------------------------------------------------------------
vtkCesium3DTilesWriter::vtkCesium3DTilesWriter()
{
  this->DirectoryName = nullptr;
  this->TexturePath = nullptr;
  std::fill(this->Origin, this->Origin + 3, 0);
  this->SaveTextures = true;
  this->SaveGLTF = true;
  this->NumberOfBuildingsPerTile = 100;
  this->UTMZone = 1;
  this->UTMHemisphere = 'N';
  this->SrsName = nullptr;
}

//------------------------------------------------------------------------------
vtkCesium3DTilesWriter::~vtkCesium3DTilesWriter()
{
  this->SetDirectoryName(nullptr);
  this->SetTexturePath(nullptr);
}

//------------------------------------------------------------------------------
void vtkCesium3DTilesWriter::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "DirectoryName: " << (this->DirectoryName ? this->DirectoryName : "NONE")
     << indent << "TexturePath: " << (this->TexturePath ? this->TexturePath : "NONE") << endl;
}

//------------------------------------------------------------------------------
int vtkCesium3DTilesWriter::FillInputPortInformation(int port, vtkInformation* info)
{
  if (port == 0)
  {
    info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkMultiBlockDataSet");
  }
  return 1;
}

//------------------------------------------------------------------------------
void vtkCesium3DTilesWriter::WriteData()
{
  {
    {
      auto root = vtkMultiBlockDataSet::SafeDownCast(this->GetInput());
      std::vector<vtkSmartPointer<vtkCompositeDataSet>> buildings;
      std::array<double, 3> offset = { 0, 0, 0 };

      vtkLog(INFO, "Add buildings with texture...");
      auto wholeBB = AddBuildingsWithTexture(
        root, this->TexturePath, this->Origin, this->SaveTextures, buildings, offset);
      std::copy(offset.begin(), offset.end(), this->Origin);
      if (buildings.empty())
      {
        vtkLog(ERROR,
          "No buildings read from the input file. "
          "Maybe buildings are on a different LOD. Try changing --lod parameter.");
        return;
      }
      vtkLog(INFO, "Processing " << buildings.size() << " buildings...");
      vtkDirectory::MakeDirectory(this->DirectoryName);

      vtkSmartPointer<vtkIncrementalOctreePointLocator> octree =
        BuildOctree(buildings, wholeBB, this->NumberOfBuildingsPerTile);
      TreeInformation treeInformation(octree->GetRoot(), octree->GetNumberOfNodes(), buildings,
        offset, this->DirectoryName, this->TexturePath, this->SaveTextures, this->SrsName,
        this->UTMZone, this->UTMHemisphere);
      treeInformation.Compute();
      vtkLog(INFO, "Generating tileset.json for " << octree->GetNumberOfNodes() << " nodes...");
      treeInformation.Generate3DTiles(std::string(this->DirectoryName) + "/tileset.json");

      if (this->SaveGLTF)
      {
        treeInformation.SaveGLTF();
      }
      vtkLog(INFO, "Deleting objects ...");
    }
    vtkLog(INFO, "Deleting rendering objects ...");
  }
  vtkLog(INFO, "Done.");
}
