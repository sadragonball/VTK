/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkIOSSModel.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkIOSSModel.h"

#include "vtkArrayDispatch.h"
#include "vtkCellData.h"
#include "vtkDataArrayRange.h"
#include "vtkDataArraySelection.h"
#include "vtkDataAssembly.h"
#include "vtkDataAssemblyUtilities.h"
#include "vtkDummyController.h"
#include "vtkIOSSUtilities.h"
#include "vtkIOSSWriter.h"
#include "vtkIdTypeArray.h"
#include "vtkInformation.h"
#include "vtkMultiProcessController.h"
#include "vtkObjectFactory.h"
#include "vtkPartitionedDataSet.h"
#include "vtkPartitionedDataSetCollection.h"
#include "vtkPointData.h"
#include "vtkSMPTools.h"
#include "vtkSmartPointer.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnstructuredGrid.h"

#include <vtksys/MD5.h>

// Ioss includes
#include <vtk_ioss.h>
// clang-format off
#include VTK_IOSS(Ioss_Assembly.h)
#include VTK_IOSS(Ioss_DatabaseIO.h)
#include VTK_IOSS(Ioss_EdgeBlock.h)
#include VTK_IOSS(Ioss_EdgeSet.h)
#include VTK_IOSS(Ioss_ElementBlock.h)
#include VTK_IOSS(Ioss_ElementSet.h)
#include VTK_IOSS(Ioss_ElementTopology.h)
#include VTK_IOSS(Ioss_FaceBlock.h)
#include VTK_IOSS(Ioss_FaceSet.h)
#include VTK_IOSS(Ioss_IOFactory.h)
#include VTK_IOSS(Ioss_NodeBlock.h)
#include VTK_IOSS(Ioss_NodeSet.h)
#include VTK_IOSS(Ioss_Region.h)
#include VTK_IOSS(Ioss_SideBlock.h)
#include VTK_IOSS(Ioss_SideSet.h)
#include VTK_IOSS(Ioss_StructuredBlock.h)
// clang-format on

#include <map>
#include <numeric>
#include <set>

namespace
{
//=============================================================================
bool HandleGlobalIds(vtkPartitionedDataSetCollection* pdc, int association,
  std::set<unsigned int> indicesToIgnore, vtkMultiProcessController* controller)
{
  std::vector<std::vector<vtkDataSet*>> datasets(pdc->GetNumberOfPartitionedDataSets());
  for (unsigned int i = 0; i < pdc->GetNumberOfPartitionedDataSets(); ++i)
  {
    datasets[i] = vtkCompositeDataSet::GetDataSets<vtkDataSet>(pdc->GetPartitionedDataSet(i));
  }
  // check if global ids are present, otherwise create them.
  int hasGlobalIds = true;
  for (unsigned int i = 0; i < pdc->GetNumberOfPartitionedDataSets(); ++i)
  {
    if (indicesToIgnore.find(i) != indicesToIgnore.end())
    {
      continue;
    }
    for (auto& ds : datasets[i])
    {
      auto* pd = ds->GetAttributes(association);
      auto* gids = vtkIdTypeArray::SafeDownCast(pd->GetGlobalIds());
      if (!gids)
      {
        hasGlobalIds = false;
        break;
      }
    }
  }
  if (controller->GetNumberOfProcesses() > 1)
  {
    controller->AllReduce(&hasGlobalIds, &hasGlobalIds, 1, vtkCommunicator::MIN_OP);
  }

  if (!hasGlobalIds)
  {
    vtkIdType numElements = 0;
    for (unsigned int i = 0; i < pdc->GetNumberOfPartitionedDataSets(); ++i)
    {
      if (indicesToIgnore.find(i) != indicesToIgnore.end())
      {
        continue;
      }
      for (auto& ds : datasets[i])
      {
        numElements += ds->GetNumberOfElements(association);
      }
    }

    vtkIdType startId = 1; // start with 1 since Exodus ids start with 1.
    if (controller->GetNumberOfProcesses() > 1)
    {
      vtkNew<vtkIdTypeArray> sourceNumberOfElements;
      sourceNumberOfElements->InsertNextValue(numElements);
      vtkNew<vtkIdTypeArray> resultNumberOfElementsPerCore;
      controller->AllGatherV(sourceNumberOfElements, resultNumberOfElementsPerCore);

      startId = std::accumulate(resultNumberOfElementsPerCore->GetPointer(0),
        resultNumberOfElementsPerCore->GetPointer(controller->GetLocalProcessId()), startId);
    }
    for (unsigned int i = 0; i < pdc->GetNumberOfPartitionedDataSets(); ++i)
    {
      if (indicesToIgnore.find(i) != indicesToIgnore.end())
      {
        continue;
      }
      for (auto& ds : datasets[i])
      {
        const auto numberOfElements = ds->GetNumberOfElements(association);
        vtkNew<vtkIdTypeArray> globalIds;
        globalIds->SetName("ids");
        globalIds->SetNumberOfComponents(1);
        globalIds->SetNumberOfTuples(numberOfElements);
        vtkSMPTools::For(0, numberOfElements, [&](vtkIdType begin, vtkIdType end) {
          auto globalIdsPtr = globalIds->GetPointer(0);
          for (vtkIdType i = begin; i < end; ++i)
          {
            globalIdsPtr[i] = startId + i;
          }
        });
        ds->GetAttributes(association)->SetGlobalIds(globalIds);
        startId += numberOfElements;
      }
    }
  }
  // returns if globals were created or not.
  return !hasGlobalIds;
}

//=============================================================================
std::set<unsigned int> GetDatasetIndices(vtkDataAssembly* assembly, std::set<std::string> paths)
{
  if (assembly && assembly->GetRootNodeName())
  {
    std::vector<int> indices;
    for (const auto& path : paths)
    {
      const auto idx = assembly->GetFirstNodeByPath(path.c_str());
      if (idx != -1)
      {
        indices.push_back(idx);
      }
    }
    const auto vector = assembly->GetDataSetIndices(indices);
    return std::set<unsigned int>{ vector.begin(), vector.end() };
  }
  return {};
}

//=============================================================================
std::map<unsigned char, int64_t> GetElementCounts(
  vtkPartitionedDataSet* pd, vtkMultiProcessController* controller)
{
  std::set<unsigned char> cellTypes;
  auto datasets = vtkCompositeDataSet::GetDataSets<vtkDataSet>(pd);
  for (auto& ds : datasets)
  {
    switch (ds->GetDataObjectType())
    {
      case VTK_UNSTRUCTURED_GRID:
      {
        auto ug = vtkUnstructuredGrid::SafeDownCast(ds);
        auto distinctCellTypes = ug->GetDistinctCellTypesArray();
        auto range = vtk::DataArrayValueRange(distinctCellTypes);
        std::copy(range.begin(), range.end(), std::inserter(cellTypes, cellTypes.end()));
        break;
      }
      case VTK_POLY_DATA:
      case VTK_UNSTRUCTURED_GRID_BASE:
      {
        vtkNew<vtkCellTypes> cellTypesOfUnstructuredData;
        ds->GetCellTypes(cellTypesOfUnstructuredData);
        auto range = vtk::DataArrayValueRange(cellTypesOfUnstructuredData->GetCellTypesArray());
        std::copy(range.begin(), range.end(), std::inserter(cellTypes, cellTypes.end()));
        break;
      }
      case VTK_IMAGE_DATA:
      case VTK_STRUCTURED_POINTS:
      case VTK_UNIFORM_GRID:
      case VTK_RECTILINEAR_GRID:
      case VTK_STRUCTURED_GRID:
      case VTK_EXPLICIT_STRUCTURED_GRID:
      {
        if (ds->GetNumberOfCells() > 0)
        {
          cellTypes.insert(ds->GetCellType(0));
          // this is added in case there is an empty cell.
          if (ds->GetCellGhostArray())
          {
            cellTypes.insert(VTK_EMPTY_CELL);
          }
        }
        break;
      }
      default:
        vtkLogF(ERROR, "Unsupported data set type: %s", ds->GetClassName());
        break;
    }
  }

  // now reduce this across all ranks as well.
  if (controller->GetNumberOfProcesses() > 1)
  {
    vtkNew<vtkUnsignedCharArray> source;
    source->SetNumberOfTuples(cellTypes.size());
    std::copy(cellTypes.begin(), cellTypes.end(), source->GetPointer(0));

    vtkNew<vtkUnsignedCharArray> result;
    controller->AllGatherV(source, result);

    auto range = vtk::DataArrayValueRange(result);
    std::copy(range.begin(), range.end(), std::inserter(cellTypes, cellTypes.end()));
  }

  // compute element counts
  std::atomic<int64_t> elementCounts[VTK_NUMBER_OF_CELL_TYPES];
  std::fill_n(elementCounts, VTK_NUMBER_OF_CELL_TYPES, 0);

  for (auto& ds : datasets)
  {
    vtkSMPTools::For(0, ds->GetNumberOfCells(), [&](vtkIdType start, vtkIdType end) {
      for (vtkIdType cc = start; cc < end; ++cc)
      {
        // memory_order_relaxed is safe here, since we're not using the atomics for
        // synchronization.
        elementCounts[ds->GetCellType(cc)].fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // convert element counts to a map
  std::map<unsigned char, int64_t> elementCountsMap;
  for (unsigned char i = 0; i < VTK_NUMBER_OF_CELL_TYPES; ++i)
  {
    if (elementCounts[i] > 0)
    {
      elementCountsMap[i] = elementCounts[i];
    }
  }
  return elementCountsMap;
}

//=============================================================================
Ioss::Field::BasicType GetFieldType(vtkDataArray* array)
{
  if (array->GetDataType() == VTK_DOUBLE || array->GetDataType() == VTK_FLOAT)
  {
    return Ioss::Field::DOUBLE;
  }
  else if (array->GetDataTypeSize() <= 32)
  {
    return Ioss::Field::INT32;
  }
  else
  {
    return Ioss::Field::INT64;
  }
}

//=============================================================================
std::vector<std::tuple<std::string, Ioss::Field::BasicType, int>> GetFields(int association,
  bool chooseArraysToWrite, vtkDataArraySelection* arraySelection, vtkCompositeDataSet* cds,
  vtkMultiProcessController* controller)
{
  std::vector<std::tuple<std::string, Ioss::Field::BasicType, int>> fields;
  vtkDataSetAttributesFieldList fieldList;
  for (auto& ds : vtkCompositeDataSet::GetDataSets<vtkDataSet>(cds))
  {
    fieldList.IntersectFieldList(ds->GetAttributes(association));
  }

  vtkNew<vtkDataSetAttributes> tmpDA;
  tmpDA->CopyAllocate(fieldList, 1);
  tmpDA->SetNumberOfTuples(1);
  if (tmpDA->GetGlobalIds())
  {
    // we don't want to add global ids again.
    tmpDA->RemoveArray(tmpDA->GetGlobalIds()->GetName());
  }
  if (tmpDA->HasArray("element_side"))
  {
    // we don't want to add element_side again.
    tmpDA->RemoveArray("element_side");
  }
  if (tmpDA->HasArray("object_id"))
  {
    // skip "object_id". that's an array added by Ioss reader.
    tmpDA->RemoveArray("object_id");
  }
  if (tmpDA->HasArray("original_object_id"))
  {
    // skip "original_object_id". that's an array added by Ioss reader.
    tmpDA->RemoveArray("original_object_id");
  }
  if (controller->GetNumberOfProcesses() == 1)
  {
    for (int idx = 0, max = tmpDA->GetNumberOfArrays(); idx < max; ++idx)
    {
      auto array = tmpDA->GetArray(idx);
      if (array && (!chooseArraysToWrite || arraySelection->ArrayIsEnabled(array->GetName())))
      {
        const auto type = ::GetFieldType(array);
        fields.emplace_back(array->GetName(), type, array->GetNumberOfComponents());
      }
    }
  }
  else // controller->GetNumberOfProcesses() > 1
  {
    // gather the number of elements from all ranks.
    vtkNew<vtkIdTypeArray> sendNumberOfElements;
    sendNumberOfElements->InsertNextValue(cds->GetNumberOfElements(association));
    vtkNew<vtkIdTypeArray> recvNumberOfElements;
    controller->AllGather(sendNumberOfElements, recvNumberOfElements);
    // create an unstructured grid to pack the tmpDA as field data.
    auto send = vtkSmartPointer<vtkUnstructuredGrid>::New();
    send->GetFieldData()->ShallowCopy(tmpDA);
    // now gather all field data from all ranks.
    std::vector<vtkSmartPointer<vtkDataObject>> recv;
    controller->AllGather(send, recv);
    // now intersect all field data to get the common fields.
    vtkDataSetAttributesFieldList coresFieldList;
    for (size_t i = 0; i < recv.size(); ++i)
    {
      const auto ug = vtkUnstructuredGrid::SafeDownCast(recv[i]);
      const auto numberOfElements = recvNumberOfElements->GetValue(i);
      // skip empty datasets.
      if (ug && numberOfElements > 0)
      {
        const auto fd = ug->GetFieldData();
        // convert field data to dataset attributes
        vtkNew<vtkDataSetAttributes> localDa;
        for (int idx = 0, max = fd->GetNumberOfArrays(); idx < max; ++idx)
        {
          auto array = fd->GetArray(idx);
          if (array && (!chooseArraysToWrite || arraySelection->ArrayIsEnabled(array->GetName())))
          {
            localDa->AddArray(array);
          }
        }
        // intersect field data with current field list.
        coresFieldList.IntersectFieldList(localDa);
      }
    }
    // now we have the common fields. we need to create a new field data
    vtkNew<vtkDataSetAttributes> coresTempDA;
    coresTempDA->CopyAllocate(coresFieldList, 0);
    for (int idx = 0, max = coresTempDA->GetNumberOfArrays(); idx < max; ++idx)
    {
      if (auto* array = coresTempDA->GetArray(idx))
      {
        const auto type = ::GetFieldType(array);
        fields.emplace_back(array->GetName(), type, array->GetNumberOfComponents());
      }
    }
  }
  return fields;
}

//=============================================================================
template <typename T>
struct PutFieldWorker
{
  std::vector<std::vector<T>> Data;
  size_t Offset{ 0 };
  const std::vector<vtkIdType>* SourceIds = nullptr;
  PutFieldWorker(int numComponents, size_t targetSize)
    : Data(numComponents)
  {
    for (int cc = 0; cc < numComponents; ++cc)
    {
      this->Data[cc].resize(targetSize);
    }
  }

  void SetSourceIds(const std::vector<vtkIdType>* ids) { this->SourceIds = ids; }

  template <typename ArrayType>
  void operator()(ArrayType* array)
  {
    using SourceT = vtk::GetAPIType<ArrayType>;
    vtkSMPThreadLocal<std::vector<SourceT>> tlTuple;
    vtkSMPTools::For(0, this->SourceIds->size(), [&](vtkIdType start, vtkIdType end) {
      auto tuple = tlTuple.Local();
      tuple.resize(this->Data.size());
      for (vtkIdType cc = start; cc < end; ++cc)
      {
        array->GetTypedTuple((*this->SourceIds)[cc], tuple.data());
        for (size_t comp = 0; comp < this->Data.size(); ++comp)
        {
          this->Data[comp][this->Offset + cc] = static_cast<T>(tuple[comp]);
        }
      }
    });

    this->Offset += this->SourceIds->size();
  }

  void ImplicitPointsOperator(vtkDataSet* ds)
  {
    vtkSMPThreadLocal<std::vector<double>> tlTuple;
    vtkSMPTools::For(0, this->SourceIds->size(), [&](vtkIdType start, vtkIdType end) {
      auto tuple = tlTuple.Local();
      tuple.resize(this->Data.size());
      for (vtkIdType cc = start; cc < end; ++cc)
      {
        ds->GetPoint((*this->SourceIds)[cc], tuple.data());
        for (size_t comp = 0; comp < this->Data.size(); ++comp)
        {
          this->Data[comp][this->Offset + cc] = static_cast<T>(tuple[comp]);
        }
      }
    });
  }
};

template <typename T>
struct DisplacementWorker
{
  std::vector<std::vector<T>>& Data;
  size_t Offset{ 0 };
  double Magnitude;
  const std::vector<vtkIdType>* SourceIds = nullptr;
  DisplacementWorker(std::vector<std::vector<T>>& data, double magnitude)
    : Data(data)
    , Magnitude(magnitude)
  {
  }

  void SetSourceIds(const std::vector<vtkIdType>* ids) { this->SourceIds = ids; }

  template <typename ArrayType>
  void operator()(ArrayType* array)
  {
    using SourceT = vtk::GetAPIType<ArrayType>;
    vtkSMPTools::For(0, this->SourceIds->size(), [&](vtkIdType start, vtkIdType end) {
      SourceT* displ = new SourceT[this->Data.size()];
      for (vtkIdType cc = start; cc < end; ++cc)
      {
        array->GetTypedTuple((*this->SourceIds)[cc], displ);
        for (size_t comp = 0; comp < this->Data.size(); ++comp)
        {
          this->Data[comp][this->Offset + cc] -= (displ[comp] * this->Magnitude);
        }
      }
      delete[] displ;
    });

    this->Offset += this->SourceIds->size();
  }
};

//=============================================================================
struct vtkGroupingEntity
{
  vtkIOSSWriter* Writer = nullptr;
  vtkGroupingEntity(vtkIOSSWriter* writer)
    : Writer(writer)
  {
  }
  virtual ~vtkGroupingEntity() = default;
  virtual Ioss::EntityType GetIOSSEntityType() const
  {
    try
    {
      return vtkIOSSUtilities::GetIOSSEntityType(this->GetEntityType());
    }
    catch (std::runtime_error&)
    {
      return Ioss::EntityType::INVALID_TYPE;
    }
  }
  virtual vtkIOSSWriter::EntityType GetEntityType() const
  {
    return vtkIOSSWriter::EntityType::NUMBER_OF_ENTITY_TYPES;
  }
  virtual void DefineModel(Ioss::Region& region) const = 0;
  virtual void Model(Ioss::Region& region) const = 0;
  virtual void DefineTransient(Ioss::Region& region) const = 0;
  virtual void Transient(Ioss::Region& region) const = 0;
  virtual void AppendMD5(vtksysMD5* md5) const = 0;

protected:
  template <typename IossGroupingEntityT, typename DatasetT>
  void PutFields(IossGroupingEntityT* block,
    const std::vector<std::tuple<std::string, Ioss::Field::BasicType, int>>& fields,
    const std::vector<std::vector<vtkIdType>>& lIds, const std::vector<DatasetT*>& datasets,
    int association) const
  {
    for (const auto& field : fields)
    {
      switch (std::get<1>(field))
      {
        case Ioss::Field::DOUBLE:
          this->PutField<double>(
            block, std::get<0>(field), std::get<2>(field), lIds, datasets, association);
          break;

        case Ioss::Field::INT32:
          this->PutField<int32_t>(
            block, std::get<0>(field), std::get<2>(field), lIds, datasets, association);
          break;

        case Ioss::Field::INT64:
          this->PutField<int64_t>(
            block, std::get<0>(field), std::get<2>(field), lIds, datasets, association);
          break;

        default:
          vtkLogF(TRACE, "Unsupported field type. Skipping %s", std::get<0>(field).c_str());
          break;
      }
    }
  }

  template <typename T, typename IossGroupingEntityT, typename DatasetT>
  void PutField(IossGroupingEntityT* block, const std::string& name, int numComponents,
    const std::vector<std::vector<vtkIdType>>& lIds, const std::vector<DatasetT*>& datasets,
    int association) const
  {
    assert(datasets.size() == lIds.size());
    const size_t totalSize = std::accumulate(lIds.begin(), lIds.end(), static_cast<size_t>(0),
      [](size_t sum, const std::vector<vtkIdType>& ids) { return sum + ids.size(); });

    using Dispatcher = vtkArrayDispatch::DispatchByValueType<vtkArrayDispatch::AllTypes>;
    PutFieldWorker<T> worker(numComponents, totalSize);
    for (size_t dsIndex = 0; dsIndex < datasets.size(); ++dsIndex)
    {
      auto& ds = datasets[dsIndex];
      auto& lids = lIds[dsIndex];
      worker.SetSourceIds(&lids);
      if (auto array = ds->GetAttributes(association)->GetArray(name.c_str()))
      {
        if (!Dispatcher::Execute(array, worker))
        {
          vtkLogF(ERROR, "Failed to dispatch array %s", name.c_str());
        }
      }
    }

    for (int comp = 0; comp < numComponents; ++comp)
    {
      const auto fieldName = numComponents == 1 ? name : name + std::to_string(comp + 1);
      block->put_field_data(fieldName, worker.Data[comp]);
    }
  }

  void DefineFields(Ioss::GroupingEntity* block,
    const std::vector<std::tuple<std::string, Ioss::Field::BasicType, int>>& fields,
    Ioss::Field::RoleType role, int64_t elementCount) const
  {
    for (const auto& field : fields)
    {
      if (std::get<2>(field) == 1)
      {
        block->field_add(
          Ioss::Field(std::get<0>(field), std::get<1>(field), "scalar", role, elementCount));
      }
      else
      {
        for (int comp = 0; comp < std::get<2>(field); ++comp)
        {
          block->field_add(Ioss::Field(std::get<0>(field) + std::to_string(comp + 1),
            std::get<1>(field), "scalar", role, elementCount));
        }
      }
    }
  }
};

} // end namespace {}

VTK_ABI_NAMESPACE_BEGIN
/**
 * Builds an Ioss::NodeBlock. Since an exodus file has a single common node
 * block, we need to build one based on all points from all blocks.
 *
 * Another thing to handle is displacements. If input dataset is coming from
 * IOSS reader, the point coordinates may have been displaced using the
 * displacement vectors in the dataset.
 */
struct vtkNodeBlock : vtkGroupingEntity
{
  const std::vector<vtkDataSet*> DataSets;
  const std::string Name;

  // build a map of ds idx, gid, and lid and use that later.
  std::vector<int32_t> Ids;
  std::vector<std::vector<vtkIdType>> IdsRaw;

  std::vector<std::tuple<std::string, Ioss::Field::BasicType, int>> Fields;

  vtkNodeBlock(vtkPartitionedDataSetCollection* pdc, const std::string& name,
    vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkGroupingEntity(writer)
    , DataSets(vtkCompositeDataSet::GetDataSets<vtkDataSet>(pdc))
    , Name(name)
  {
    this->IdsRaw.reserve(this->DataSets.size());

    std::set<int32_t> id_set;
    for (auto& ds : this->DataSets)
    {
      auto* pd = ds->GetPointData();
      auto* gids = vtkIdTypeArray::SafeDownCast(pd->GetGlobalIds());
      if (!gids)
      {
        throw std::runtime_error("point global ids missing.");
      }

      const auto numPoints = ds->GetNumberOfPoints();
      assert(gids->GetNumberOfTuples() == numPoints);

      this->Ids.reserve(this->Ids.size() + numPoints);
      this->IdsRaw.emplace_back();
      this->IdsRaw.back().reserve(numPoints);
      const vtkIdType gidOffset = writer->GetOffsetGlobalIds() ? 1 : 0;
      for (vtkIdType cc = 0; cc < numPoints; ++cc)
      {
        const auto gid = gids->GetValue(cc);
        if (id_set.insert(gid).second)
        {
          this->Ids.push_back(gid + gidOffset);
          this->IdsRaw.back().push_back(cc);
        }
      }
    }

    assert(this->DataSets.size() == this->IdsRaw.size());
    this->Fields = ::GetFields(vtkDataObject::POINT, writer->GetChooseFieldsToWrite(),
      writer->GetNodeBlockFieldSelection(), pdc, controller);
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::NODEBLOCK;
  }

  void AppendMD5(vtksysMD5* md5) const override
  {
    vtksysMD5_Append(md5, reinterpret_cast<const unsigned char*>(this->Ids.data()),
      static_cast<int>(sizeof(int32_t) * this->Ids.size()));
  }

  void DefineModel(Ioss::Region& region) const override
  {
    auto* nodeBlock = new Ioss::NodeBlock(region.get_database(), this->Name, this->Ids.size(), 3);
    nodeBlock->property_add(Ioss::Property("id", 1)); // block id.
    region.add(nodeBlock);
  }

  void DefineTransient(Ioss::Region& region) const override
  {
    auto* nodeBlock = region.get_node_block(this->Name);
    this->DefineFields(nodeBlock, this->Fields, Ioss::Field::TRANSIENT, this->Ids.size());
  }

  void Model(Ioss::Region& region) const override
  {
    auto* nodeBlock = region.get_node_block(this->Name);
    nodeBlock->put_field_data("ids", this->Ids);

    // add mesh coordinates
    using Dispatcher = vtkArrayDispatch::DispatchByValueType<vtkArrayDispatch::Reals>;
    PutFieldWorker<double> worker(3, this->Ids.size());
    for (size_t dsIndex = 0; dsIndex < this->DataSets.size(); ++dsIndex)
    {
      auto& ds = this->DataSets[dsIndex];
      auto& lids = this->IdsRaw[dsIndex];
      worker.SetSourceIds(&lids);
      if (auto ps = vtkPointSet::SafeDownCast(ds))
      {
        if (ps->GetPoints())
        {
          if (!Dispatcher::Execute(ps->GetPoints()->GetData(), worker))
          {
            vtkLog(ERROR, "Failed to dispatch points.");
          }
        }
      }
      else
      {
        worker.ImplicitPointsOperator(ds);
      }
    }

    // if displacement array is present, offset the mesh coordinates by the
    // provided displacement.
    const auto displMagnitude =
      this->DataSets.empty() ? 0.0 : this->Writer->GetDisplacementMagnitude();
    const std::string displName =
      displMagnitude > 0 ? vtkIOSSUtilities::GetDisplacementFieldName(this->DataSets.front()) : "";
    if (!displName.empty() && displMagnitude > 0.0)
    {
      DisplacementWorker<double> dworker(worker.Data, displMagnitude);
      for (size_t dsIndex = 0; dsIndex < this->DataSets.size(); ++dsIndex)
      {
        auto& ds = this->DataSets[dsIndex];
        auto& lids = this->IdsRaw[dsIndex];
        dworker.SetSourceIds(&lids);
        if (auto dispArray = ds->GetPointData()->GetArray(displName.c_str()))
        {
          if (!Dispatcher::Execute(dispArray, dworker))
          {
            vtkLog(ERROR, "Failed to dispatch displacements.");
          }
        }
      }
    }

    nodeBlock->put_field_data("mesh_model_coordinates_x", worker.Data[0]);
    nodeBlock->put_field_data("mesh_model_coordinates_y", worker.Data[1]);
    nodeBlock->put_field_data("mesh_model_coordinates_z", worker.Data[2]);
  }

  void Transient(Ioss::Region& region) const override
  {
    auto* nodeBlock = region.get_node_block(this->Name);
    this->PutFields(nodeBlock, this->Fields, this->IdsRaw, this->DataSets, vtkDataObject::POINT);
  }
};

/**
 * Builds an Ioss::(*)Block from a vtkPartitionedDataSet. The differences
 * between the Ioss and VTK data model for the two are handled as follows:
 *
 * * We only support vtkPartitionedDataSet comprising of one or more vtkDataSets.
 *   All other dataset types are simply ignored.
 *
 * * A Block cannot have multiple "pieces" in the same file. So if a
 *   vtkPartitionedDataSet has multiple datasets, we need to "combine" them into
 *   one.
 *
 * * A Block cannot have elements of different types. However,
 *   vtkDataSet supports heterogeneous cells. So if all
 *   vtkDataSets in the vtkPartitionedDataSet have more than 1 cell type,
 *   we create multiple blocks. Each Block is uniquely named by
 *   using the given block name and the element type as a suffix.
 *
 *   In MPI world, the cell types are gathered across all ranks to ensure each
 *   ranks creates identical blocks / block names.
 *
 */
struct vtkEntityBlock : public vtkGroupingEntity
{
  const std::vector<vtkDataSet*> DataSets;
  std::string RootName;
  int BlockId;
  int StartSplitElementBlockId;

  std::map<unsigned char, int64_t> ElementCounts;
  std::vector<std::tuple<std::string, Ioss::Field::BasicType, int>> Fields;

  vtkEntityBlock(vtkPartitionedDataSet* pds, vtkIOSSWriter::EntityType entityType,
    const std::string& name, const int blockId, int startSplitElementBlockId,
    vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkGroupingEntity(writer)
    , DataSets(vtkCompositeDataSet::GetDataSets<vtkDataSet>(pds))
    , RootName(name)
    , BlockId(blockId)
    , StartSplitElementBlockId(startSplitElementBlockId)
  {
    for (auto& ds : this->DataSets)
    {
      auto* cd = ds->GetCellData();
      auto* gids = vtkIdTypeArray::SafeDownCast(cd->GetGlobalIds());
      if (!gids)
      {
        throw std::runtime_error("cell global ids missing!");
      }
    }

    this->ElementCounts = ::GetElementCounts(pds, controller);
    this->Fields = ::GetFields(vtkDataObject::CELL, writer->GetChooseFieldsToWrite(),
      writer->GetFieldSelection(entityType), pds, controller);
  }

  void AppendMD5(vtksysMD5* md5) const override
  {
    vtksysMD5_Append(md5, reinterpret_cast<const unsigned char*>(this->RootName.c_str()), -1);
    for (auto& pair : this->ElementCounts)
    {
      vtksysMD5_Append(md5, reinterpret_cast<const unsigned char*>(&pair.first),
        static_cast<int>(sizeof(pair.first)));
      vtksysMD5_Append(md5, reinterpret_cast<const unsigned char*>(&pair.second),
        static_cast<int>(sizeof(pair.second)));
    }
  }

  /**
   * Get the block name and id of a given element in a block.
   */
  std::pair<int, std::string> GetSubElementBlockInfo(
    unsigned char vtkCellType, std::string elementType) const
  {
    const bool preservedStructure = this->ElementCounts.size() == 1;
    if (preservedStructure)
    {
      return std::make_pair(this->BlockId, this->RootName);
    }
    else
    {
      const int splitElementBlockId =
        this->StartSplitElementBlockId + static_cast<int>(vtkCellType);
      const std::string blockName = this->RootName + "_" + elementType;
      return std::make_pair(splitElementBlockId, blockName);
    }
  }

  virtual Ioss::EntityBlock* CreateEntity(Ioss::DatabaseIO* db, const std::string& blockName,
    const std::string& elementType, int64_t elementCount) const = 0;

  virtual void AddEntity(Ioss::Region& region, Ioss::EntityBlock* entityBlock) const = 0;

  virtual Ioss::EntityBlock* GetEntity(
    Ioss::Region& region, const std::string& blockName) const = 0;

  void DefineModel(Ioss::Region& region) const override
  {
    for (const auto& element : this->ElementCounts)
    {
      const int64_t elementCount = element.second;
      const unsigned char vtk_cell_type = element.first;

      const auto* elementTopology = vtkIOSSUtilities::GetElementTopology(vtk_cell_type);
      const auto& elementType = elementTopology->name();
      const auto blockInfo = this->GetSubElementBlockInfo(vtk_cell_type, elementType);

      auto entityBlock =
        this->CreateEntity(region.get_database(), blockInfo.second, elementType, elementCount);
      entityBlock->property_add(Ioss::Property("id", blockInfo.first));
      if (this->Writer->GetPreserveOriginalIds())
      {
        entityBlock->property_add(
          Ioss::Property("original_id", this->BlockId, Ioss::Property::ATTRIBUTE));
      }
      this->AddEntity(region, entityBlock);
    }
  }

  void DefineTransient(Ioss::Region& region) const override
  {
    for (const auto& element : this->ElementCounts)
    {
      const int64_t elementCount = element.second;
      const unsigned char vtk_cell_type = element.first;

      const auto* elementTopology = vtkIOSSUtilities::GetElementTopology(vtk_cell_type);
      const auto& elementType = elementTopology->name();
      const auto blockName = this->GetSubElementBlockInfo(vtk_cell_type, elementType).second;

      auto* entityBlock = this->GetEntity(region, blockName);
      this->DefineFields(entityBlock, this->Fields, Ioss::Field::TRANSIENT, elementCount);
    }
  }

  void Model(Ioss::Region& region) const override
  {
    for (const auto& element : this->ElementCounts)
    {
      const int64_t elementCount = element.second;
      const unsigned char vtk_cell_type = element.first;

      const auto* elementTopology = vtkIOSSUtilities::GetElementTopology(vtk_cell_type);
      const auto& elementType = elementTopology->name();
      const int nodeCount = elementTopology->number_nodes();
      const auto blockName = this->GetSubElementBlockInfo(vtk_cell_type, elementType).second;

      auto* entityBlock = this->GetEntity(region, blockName);

      // populate ids.
      std::vector<int32_t> elementIds; // these are global ids.
      elementIds.reserve(elementCount);

      std::vector<int32_t> connectivity;
      connectivity.reserve(elementCount * nodeCount);

      const int32_t gidOffset = this->Writer->GetOffsetGlobalIds() ? 1 : 0;
      const bool removeGhosts = this->Writer->GetRemoveGhosts();
      for (auto& ds : this->DataSets)
      {
        vtkUnsignedCharArray* ghost = ds->GetCellGhostArray();
        auto* gids = vtkIdTypeArray::SafeDownCast(ds->GetCellData()->GetGlobalIds());
        auto* pointGIDs = vtkIdTypeArray::SafeDownCast(ds->GetPointData()->GetGlobalIds());

        vtkNew<vtkIdList> tempCellPointIds;
        for (vtkIdType cc = 0, max = ds->GetNumberOfCells(); cc < max; ++cc)
        {
          const bool process = !removeGhosts || !ghost || ghost->GetValue(cc) == 0;
          if (process && ds->GetCellType(cc) == vtk_cell_type)
          {
            elementIds.push_back(gidOffset + gids->GetValue(cc));

            vtkIdType numPts;
            vtkIdType const* cellPoints;
            ds->GetCellPoints(cc, numPts, cellPoints, tempCellPointIds);
            assert(numPts == nodeCount);

            // map cell's point to global ids for those points.
            std::transform(cellPoints, cellPoints + numPts, std::back_inserter(connectivity),
              [&](vtkIdType ptid) { return gidOffset + pointGIDs->GetValue(ptid); });
          }
        }
      }
      assert(elementIds.size() == static_cast<size_t>(elementCount));
      assert(connectivity.size() == static_cast<size_t>(elementCount * nodeCount));
      entityBlock->put_field_data("ids", elementIds);
      entityBlock->put_field_data("connectivity", connectivity);
    }
  }

  void Transient(Ioss::Region& region) const override
  {
    for (const auto& element : this->ElementCounts)
    {
      const unsigned char vtk_cell_type = element.first;

      const auto* elementTopology = vtkIOSSUtilities::GetElementTopology(vtk_cell_type);
      const auto& elementType = elementTopology->name();
      const auto blockName = this->GetSubElementBlockInfo(vtk_cell_type, elementType).second;

      auto* entityBlock = this->GetEntity(region, blockName);

      // populate ids.
      std::vector<std::vector<vtkIdType>> lIds; // these are local ids.
      const bool removeGhosts = this->Writer->GetRemoveGhosts();
      for (auto& ds : this->DataSets)
      {
        vtkUnsignedCharArray* ghost = ds->GetCellGhostArray();
        lIds.emplace_back();
        lIds.back().reserve(ds->GetNumberOfCells());
        for (vtkIdType cc = 0, max = ds->GetNumberOfCells(); cc < max; ++cc)
        {
          const bool process = !removeGhosts || !ghost || ghost->GetValue(cc) == 0;
          if (process && ds->GetCellType(cc) == vtk_cell_type)
          {
            lIds.back().push_back(cc);
          }
        }
      }

      // add fields.
      this->PutFields(entityBlock, this->Fields, lIds, this->DataSets, vtkDataObject::CELL);
    }
  }
};

//=============================================================================
struct vtkEdgeBlock : public vtkEntityBlock
{
  vtkEdgeBlock(vtkPartitionedDataSet* pds, const std::string& name, const int blockId,
    int startSplitElementBlockId, vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkEntityBlock(
        pds, this->GetEntityType(), name, blockId, startSplitElementBlockId, controller, writer)
  {
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::EDGEBLOCK;
  }

  Ioss::EntityBlock* CreateEntity(Ioss::DatabaseIO* db, const std::string& blockName,
    const std::string& elementType, const int64_t elementCount) const override
  {
    return new Ioss::EdgeBlock(db, blockName, elementType, elementCount);
  }

  void AddEntity(Ioss::Region& region, Ioss::EntityBlock* entityBlock) const override
  {
    region.add(dynamic_cast<Ioss::EdgeBlock*>(entityBlock));
  }

  Ioss::EntityBlock* GetEntity(Ioss::Region& region, const std::string& blockName) const override
  {
    return region.get_edge_block(blockName);
  }
};

//=============================================================================
struct vtkFaceBlock : public vtkEntityBlock
{
  vtkFaceBlock(vtkPartitionedDataSet* pds, const std::string& name, const int blockId,
    int startSplitElementBlockId, vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkEntityBlock(
        pds, this->GetEntityType(), name, blockId, startSplitElementBlockId, controller, writer)
  {
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::FACEBLOCK;
  }

  Ioss::EntityBlock* CreateEntity(Ioss::DatabaseIO* db, const std::string& blockName,
    const std::string& elementType, const int64_t elementCount) const override
  {
    return new Ioss::FaceBlock(db, blockName, elementType, elementCount);
  }

  void AddEntity(Ioss::Region& region, Ioss::EntityBlock* entityBlock) const override
  {
    region.add(dynamic_cast<Ioss::FaceBlock*>(entityBlock));
  }

  Ioss::EntityBlock* GetEntity(Ioss::Region& region, const std::string& blockName) const override
  {
    return region.get_face_block(blockName);
  }
};

//=============================================================================
struct vtkElementBlock : public vtkEntityBlock
{
  vtkElementBlock(vtkPartitionedDataSet* pds, const std::string& name, const int blockId,
    int startSplitElementBlockId, vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkEntityBlock(
        pds, this->GetEntityType(), name, blockId, startSplitElementBlockId, controller, writer)
  {
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::ELEMENTBLOCK;
  }

  Ioss::EntityBlock* CreateEntity(Ioss::DatabaseIO* db, const std::string& blockName,
    const std::string& elementType, const int64_t elementCount) const override
  {
    return new Ioss::ElementBlock(db, blockName, elementType, elementCount);
  }

  void AddEntity(Ioss::Region& region, Ioss::EntityBlock* entityBlock) const override
  {
    region.add(dynamic_cast<Ioss::ElementBlock*>(entityBlock));
  }

  Ioss::EntityBlock* GetEntity(Ioss::Region& region, const std::string& blockName) const override
  {
    return region.get_element_block(blockName);
  }
};

//=============================================================================
struct vtkNodeSet : public vtkGroupingEntity
{
  const std::vector<vtkDataSet*> DataSets;
  std::string Name;
  int BlockId;
  int64_t Count;

  std::vector<std::tuple<std::string, Ioss::Field::BasicType, int>> Fields;

  vtkNodeSet(vtkPartitionedDataSet* pds, const std::string& name, int blockId,
    vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkGroupingEntity(writer)
    , DataSets{ vtkCompositeDataSet::GetDataSets<vtkDataSet>(pds) }
    , Name(name)
    , BlockId(blockId)
    , Count(0)
  {
    for (auto& ds : this->DataSets)
    {
      auto* gids = vtkIdTypeArray::SafeDownCast(ds->GetPointData()->GetGlobalIds());
      if (!gids)
      {
        throw std::runtime_error("missing point global ids for nodesets.");
      }
      const auto numPoints = ds->GetNumberOfPoints();
      assert(gids->GetNumberOfTuples() == numPoints);
      this->Count += numPoints;
    }

    // in a nodeSet, number of points == number of cells, because cells are vertices
    this->Fields = ::GetFields(vtkDataObject::CELL, writer->GetChooseFieldsToWrite(),
      writer->GetNodeSetFieldSelection(), pds, controller);
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::NODESET;
  }

  void AppendMD5(vtksysMD5* md5) const override
  {
    vtksysMD5_Append(md5, reinterpret_cast<const unsigned char*>(this->Name.c_str()), -1);
    vtksysMD5_Append(md5, reinterpret_cast<const unsigned char*>(&this->Count),
      static_cast<int>(sizeof(this->Count)));
  }

  void DefineModel(Ioss::Region& region) const override
  {
    auto* nodeSet = new Ioss::NodeSet(region.get_database(), this->Name, this->Count);
    nodeSet->property_add(Ioss::Property("id", this->BlockId));
    region.add(nodeSet);
  }

  void DefineTransient(Ioss::Region& region) const override
  {
    auto nodeSet = region.get_nodeset(this->Name);
    this->DefineFields(nodeSet, this->Fields, Ioss::Field::TRANSIENT, this->Count);
  }

  void Model(Ioss::Region& region) const override
  {
    auto* nodeSet = region.get_nodeset(this->Name);
    // create global ids.
    std::vector<int32_t> ids;
    ids.reserve(static_cast<size_t>(this->Count));
    for (const auto& ds : this->DataSets)
    {
      auto gids = vtkIdTypeArray::SafeDownCast(ds->GetPointData()->GetGlobalIds());
      const vtkIdType gidOffset = this->Writer->GetOffsetGlobalIds() ? 1 : 0;
      for (vtkIdType ptId = 0, max = ds->GetNumberOfPoints(); ptId < max; ++ptId)
      {
        ids.push_back(gidOffset + gids->GetValue(ptId));
      }
    }
    nodeSet->put_field_data("ids", ids);
  }

  void Transient(Ioss::Region& region) const override
  {
    auto* nodeSet = region.get_nodeset(this->Name);
    // create local ids.
    std::vector<std::vector<vtkIdType>> idsRaw;
    idsRaw.reserve(this->DataSets.size());
    for (const auto& ds : this->DataSets)
    {
      idsRaw.emplace_back();
      idsRaw.back().reserve(static_cast<size_t>(ds->GetNumberOfPoints()));
      for (vtkIdType ptId = 0, max = ds->GetNumberOfPoints(); ptId < max; ++ptId)
      {
        idsRaw.back().push_back(ptId);
      }
    }
    this->PutFields(nodeSet, this->Fields, idsRaw, this->DataSets, vtkDataObject::CELL);
  }
};

//=============================================================================
struct vtkEntitySet : public vtkGroupingEntity
{
  const std::vector<vtkDataSet*> DataSets;
  std::string Name;
  int BlockId;
  int64_t Count;

  std::vector<std::tuple<std::string, Ioss::Field::BasicType, int>> Fields;

  vtkEntitySet(vtkPartitionedDataSet* pds, vtkIOSSWriter::EntityType entityType,
    const std::string& name, int blockId, vtkMultiProcessController* controller,
    vtkIOSSWriter* writer)
    : vtkGroupingEntity(writer)
    , DataSets(vtkCompositeDataSet::GetDataSets<vtkDataSet>(pds))
    , Name(name)
    , BlockId(blockId)
    , Count(0)
  {
    for (auto& ds : this->DataSets)
    {
      // no need to check for global ids
      if (vtkIntArray::SafeDownCast(ds->GetCellData()->GetArray("element_side")) == nullptr)
      {
        throw std::runtime_error("missing 'element_side' cell array.");
      }

      this->Count += ds->GetNumberOfCells();
    }
    this->Fields = ::GetFields(vtkDataObject::CELL, writer->GetChooseFieldsToWrite(),
      writer->GetFieldSelection(entityType), pds, controller);
  }

  void AppendMD5(vtksysMD5* md5) const override
  {
    vtksysMD5_Append(md5, reinterpret_cast<const unsigned char*>(this->Name.c_str()), -1);
    vtksysMD5_Append(md5, reinterpret_cast<const unsigned char*>(&this->Count),
      static_cast<int>(sizeof(this->Count)));
  }

  virtual Ioss::GroupingEntity* CreateEntity(
    Ioss::DatabaseIO* db, const std::string& blockName, int64_t elementCount) const = 0;

  virtual void AddEntity(Ioss::Region& region, Ioss::GroupingEntity* entitySet) const = 0;

  virtual Ioss::GroupingEntity* GetEntity(
    Ioss::Region& region, const std::string& blockName) const = 0;

  void DefineModel(Ioss::Region& region) const override
  {
    auto entitySet = this->CreateEntity(region.get_database(), this->Name, this->Count);
    entitySet->property_add(Ioss::Property("id", this->BlockId));
    this->AddEntity(region, entitySet);
  }

  void DefineTransient(Ioss::Region& region) const override
  {
    auto entity = this->GetEntity(region, this->Name);
    this->DefineFields(entity, this->Fields, Ioss::Field::TRANSIENT, this->Count);
  }

  void Model(Ioss::Region& region) const override
  {
    auto entity = this->GetEntity(region, this->Name);

    std::vector<int32_t> elementSide;
    elementSide.reserve(this->Count * 2);

    const bool removeGhosts = this->Writer->GetRemoveGhosts();
    for (auto& ds : this->DataSets)
    {
      auto elemSideArray = vtkIntArray::SafeDownCast(ds->GetCellData()->GetArray("element_side"));
      if (!elemSideArray)
      {
        throw std::runtime_error("missing 'element_side' cell array.");
      }
      vtkUnsignedCharArray* ghost = ds->GetCellGhostArray();
      const auto elementSideRange = vtk::DataArrayTupleRange(elemSideArray);
      for (vtkIdType cc = 0; cc < elementSideRange.size(); ++cc)
      {
        const bool process = !removeGhosts || !ghost || ghost->GetValue(cc) == 0;
        if (process)
        {
          for (const auto& comp : elementSideRange[cc])
          {
            elementSide.push_back(comp);
          }
        }
      }
    }

    assert(elementSide.size() == static_cast<size_t>(this->Count * 2));
    entity->put_field_data("element_side", elementSide);
  }

  void Transient(Ioss::Region& region) const override
  {
    auto entity = this->GetEntity(region, this->Name);

    // populate ids.
    std::vector<std::vector<vtkIdType>> lIds; // these are local ids.
    const bool removeGhosts = this->Writer->GetRemoveGhosts();
    for (auto& ds : this->DataSets)
    {
      vtkUnsignedCharArray* ghost = ds->GetCellGhostArray();
      lIds.emplace_back();
      lIds.back().reserve(static_cast<size_t>(ds->GetNumberOfCells()));
      for (vtkIdType cc = 0, max = ds->GetNumberOfCells(); cc < max; ++cc)
      {
        const bool process = !removeGhosts || !ghost || ghost->GetValue(cc) == 0;
        if (process)
        {
          lIds.back().push_back(cc);
        }
      }
    }

    // add fields.
    this->PutFields(entity, this->Fields, lIds, this->DataSets, vtkDataObject::CELL);
  }
};

//=============================================================================
struct vtkEdgeSet : public vtkEntitySet
{
  vtkEdgeSet(vtkPartitionedDataSet* pds, const std::string& name, int blockId,
    vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkEntitySet(pds, this->GetEntityType(), name, blockId, controller, writer)
  {
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::EDGESET;
  }

  Ioss::GroupingEntity* CreateEntity(
    Ioss::DatabaseIO* db, const std::string& blockName, const int64_t elementCount) const override
  {
    return new Ioss::EdgeSet(db, blockName, elementCount);
  }

  void AddEntity(Ioss::Region& region, Ioss::GroupingEntity* entitySet) const override
  {
    region.add(dynamic_cast<Ioss::EdgeSet*>(entitySet));
  }

  Ioss::GroupingEntity* GetEntity(Ioss::Region& region, const std::string& blockName) const override
  {
    return region.get_edgeset(blockName);
  }
};

//=============================================================================
struct vtkFaceSet : public vtkEntitySet
{
  vtkFaceSet(vtkPartitionedDataSet* pds, const std::string& name, int blockId,
    vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkEntitySet(pds, this->GetEntityType(), name, blockId, controller, writer)
  {
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::FACESET;
  }

  Ioss::GroupingEntity* CreateEntity(
    Ioss::DatabaseIO* db, const std::string& blockName, const int64_t elementCount) const override
  {
    return new Ioss::FaceSet(db, blockName, elementCount);
  }

  void AddEntity(Ioss::Region& region, Ioss::GroupingEntity* entitySet) const override
  {
    region.add(dynamic_cast<Ioss::FaceSet*>(entitySet));
  }

  Ioss::GroupingEntity* GetEntity(Ioss::Region& region, const std::string& blockName) const override
  {
    return region.get_faceset(blockName);
  }
};

//=============================================================================
struct vtkElementSet : public vtkEntitySet
{
  vtkElementSet(vtkPartitionedDataSet* pds, const std::string& name, int blockId,
    vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkEntitySet(pds, this->GetEntityType(), name, blockId, controller, writer)
  {
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::ELEMENTSET;
  }

  Ioss::GroupingEntity* CreateEntity(
    Ioss::DatabaseIO* db, const std::string& blockName, const int64_t elementCount) const override
  {
    return new Ioss::ElementSet(db, blockName, elementCount);
  }

  void AddEntity(Ioss::Region& region, Ioss::GroupingEntity* entitySet) const override
  {
    region.add(dynamic_cast<Ioss::ElementSet*>(entitySet));
  }

  Ioss::GroupingEntity* GetEntity(Ioss::Region& region, const std::string& blockName) const override
  {
    return region.get_elementset(blockName);
  }
};

//=============================================================================
struct vtkSideSet : public vtkEntitySet
{
  vtkSideSet(vtkPartitionedDataSet* pds, const std::string& name, int blockId,
    vtkMultiProcessController* controller, vtkIOSSWriter* writer)
    : vtkEntitySet(pds, this->GetEntityType(), name, blockId, controller, writer)
  {
  }

  vtkIOSSWriter::EntityType GetEntityType() const override
  {
    return vtkIOSSWriter::EntityType::SIDESET;
  }

  Ioss::GroupingEntity* CreateEntity(
    Ioss::DatabaseIO* db, const std::string& blockName, const int64_t elementCount) const override
  {
    // for mixed topology blocks, IOSS uses "unknown"
    const auto* mixed_topo = Ioss::ElementTopology::factory("unknown");
    const auto& elementType = mixed_topo->name();
    auto* sideBlock =
      new Ioss::SideBlock(db, "sideblock_0", elementType, elementType, elementCount);
    auto* sideSet = new Ioss::SideSet(db, blockName);
    sideSet->add(sideBlock);
    return sideSet;
  }

  void AddEntity(Ioss::Region& region, Ioss::GroupingEntity* entitySet) const override
  {
    region.add(dynamic_cast<Ioss::SideSet*>(entitySet));
  }

  Ioss::GroupingEntity* GetEntity(Ioss::Region& region, const std::string& blockName) const override
  {
    return region.get_sideset(blockName)->get_side_block("sideblock_0");
  }
};

//=============================================================================
class vtkIOSSModel::vtkInternals
{
public:
  vtkSmartPointer<vtkMultiProcessController> Controller;
  vtkSmartPointer<vtkPartitionedDataSetCollection> DataSet;
  std::multimap<Ioss::EntityType, std::shared_ptr<vtkGroupingEntity>> EntityGroups;
  bool GlobalIdsCreated;
};

//----------------------------------------------------------------------------
vtkIOSSModel::vtkIOSSModel(vtkPartitionedDataSetCollection* pdc, vtkIOSSWriter* writer)
  : Internals(new vtkIOSSModel::vtkInternals())
{
  auto& internals = (*this->Internals);
  auto& dataset = internals.DataSet;
  internals.Controller = writer->GetController()
    ? vtk::MakeSmartPointer(writer->GetController())
    : vtk::TakeSmartPointer(vtkMultiProcessController::SafeDownCast(vtkDummyController::New()));
  const auto& controller = internals.Controller;
  auto& entityGroups = internals.EntityGroups;

  // shallow copy the dataset. because we might need to add global ids to it.
  dataset = vtkSmartPointer<vtkPartitionedDataSetCollection>::New();
  dataset->CopyStructure(pdc);
  dataset->ShallowCopy(pdc);

  // detect which vtkPartitionedDataSets are element blocks, node sets, and side sets.
  const auto assemblyName = writer->GetAssemblyName();
  vtkSmartPointer<vtkDataAssembly> assembly;
  if (assemblyName && strcmp(assemblyName, "Assembly") == 0)
  {
    assembly = dataset->GetDataAssembly();
  }
  else // if (strcmp(assemblyName, "vtkDataAssemblyUtilities::HierarchyName()") == 0)
  {
    assembly = vtkSmartPointer<vtkDataAssembly>::New();
    if (!vtkDataAssemblyUtilities::GenerateHierarchy(dataset, assembly))
    {
      vtkErrorWithObjectMacro(writer, "Failed to generate hierarchy.");
      return;
    }
  }
  using EntityType = vtkIOSSWriter::EntityType;
  std::map<EntityType, std::set<unsigned int>> entityIndices;
  for (int i = EntityType::EDGEBLOCK; i < EntityType::NUMBER_OF_ENTITY_TYPES; ++i)
  {
    const auto entityType = static_cast<EntityType>(i);
    entityIndices[entityType] = ::GetDatasetIndices(assembly, writer->GetSelectors(entityType));
  }
  // write the above for loop with one line
  const bool indicesEmpty = std::all_of(entityIndices.begin(), entityIndices.end(),
    [](const std::pair<EntityType, std::set<unsigned int>>& indices) {
      return indices.second.empty();
    });
  if (indicesEmpty)
  {
    // if no indices are specified, then all blocks will be processed as element blocks
    // but, if the dataset was read from vtkIOSSReader, then we can deduce the type of the block
    const auto dataAssembly = dataset->GetDataAssembly();
    const bool isIOSS = (dataAssembly && dataAssembly->GetRootNodeName() &&
      strcmp(dataAssembly->GetRootNodeName(), "IOSS") == 0);
    if (isIOSS)
    {
      for (int i = EntityType::EDGEBLOCK; i < EntityType::NUMBER_OF_ENTITY_TYPES; ++i)
      {
        const auto entityType = static_cast<EntityType>(i);
        const auto iossEntitySelector =
          std::string("/IOSS/") + vtkIOSSReader::GetDataAssemblyNodeNameForEntityType(i);
        entityIndices[entityType] = ::GetDatasetIndices(dataAssembly, { iossEntitySelector });
      }
    }
    else
    {
      // all blocks are element blocks
      entityIndices[EntityType::ELEMENTBLOCK] = ::GetDatasetIndices(assembly, { "/" });
    }
  }

  // merge sets indices into a single set of indices.
  std::set<unsigned int> indicesToIgnore;
  for (int i = EntityType::SET_START; i < EntityType::SET_END; ++i)
  {
    const auto entityType = static_cast<EntityType>(i);
    indicesToIgnore.insert(entityIndices[entityType].begin(), entityIndices[entityType].end());
  }

  internals.GlobalIdsCreated = false;
  // create global point ids if needed
  internals.GlobalIdsCreated |= ::HandleGlobalIds(dataset, vtkDataObject::POINT, {}, controller);
  // create global cell ids if needed (node sets and side sets should not have global cell ids)
  internals.GlobalIdsCreated |=
    ::HandleGlobalIds(dataset, vtkDataObject::CELL, indicesToIgnore, controller);

  // extract the names and ids of the blocks.
  std::vector<std::string> blockNames(dataset->GetNumberOfPartitionedDataSets());
  std::vector<int> blockIds(dataset->GetNumberOfPartitionedDataSets());
  for (unsigned int pidx = 0; pidx < dataset->GetNumberOfPartitionedDataSets(); ++pidx)
  {
    blockIds[pidx] = pidx + 1;
    blockNames[pidx] = "block_" + std::to_string(blockIds[pidx]);
    if (auto info = dataset->GetMetaData(pidx))
    {
      if (info->Has(vtkCompositeDataSet::NAME()))
      {
        blockNames[pidx] = info->Get(vtkCompositeDataSet::NAME());
      }
      // this is true only if the dataset is coming from IOSS reader.
      if (info->Has(vtkIOSSReader::ENTITY_ID()))
      {
        blockIds[pidx] = info->Get(vtkIOSSReader::ENTITY_ID());
      }
    }
  }
  // this will be used as a start id for split blocks to ensure uniqueness.
  int startSplitEBlockId = *std::max_element(blockIds.begin(), blockIds.end()) + 1;
  // ensure that all processes have the same startSplitEBlockId.
  if (controller && controller->GetNumberOfProcesses() > 1)
  {
    int globalStartSplitBlockId;
    controller->AllReduce(
      &startSplitEBlockId, &globalStartSplitBlockId, 1, vtkCommunicator::MAX_OP);
    startSplitEBlockId = globalStartSplitBlockId;
  }

  // first things first, determine all information necessary about node block.
  // there's just 1 node block for exodus, build that.
  auto nodeBlock = std::make_shared<vtkNodeBlock>(dataset, "nodeblock_1", controller, writer);
  entityGroups.emplace(nodeBlock->GetIOSSEntityType(), nodeBlock);

  // process group entities.
  int blockCounter = 0;
  for (unsigned int pidx = 0; pidx < dataset->GetNumberOfPartitionedDataSets(); ++pidx)
  {
    const std::string& blockName = blockNames[pidx];
    const int& blockId = blockIds[pidx];
    const auto pds = dataset->GetPartitionedDataSet(pidx);

    // now create each type of GroupingEntity.

    // edge block
    const bool edgeBlockFound =
      entityIndices[EntityType::EDGEBLOCK].find(pidx) != entityIndices[EntityType::EDGEBLOCK].end();
    if (edgeBlockFound)
    {
      try
      {
        if (blockCounter++ != 0)
        {
          // add the number of cell types to the block id to ensure uniqueness.
          startSplitEBlockId += VTK_NUMBER_OF_CELL_TYPES;
        }
        auto edgeBlock = std::make_shared<vtkEdgeBlock>(
          pds, blockName, blockId, startSplitEBlockId, controller, writer);
        entityGroups.emplace(edgeBlock->GetIOSSEntityType(), edgeBlock);
        continue;
      }
      catch (std::runtime_error&)
      {
        break;
      }
    }

    // face block
    const bool faceBlockFound =
      entityIndices[EntityType::FACEBLOCK].find(pidx) != entityIndices[EntityType::FACEBLOCK].end();
    if (faceBlockFound)
    {
      try
      {
        if (blockCounter++ != 0)
        {
          // add the number of cell types to the block id to ensure uniqueness.
          startSplitEBlockId += VTK_NUMBER_OF_CELL_TYPES;
        }
        auto faceBlock = std::make_shared<vtkFaceBlock>(
          pds, blockName, blockId, startSplitEBlockId, controller, writer);
        entityGroups.emplace(faceBlock->GetIOSSEntityType(), faceBlock);
        continue;
      }
      catch (std::runtime_error&)
      {
        break;
      }
    }

    // element block
    const bool elementBlockFound = entityIndices[EntityType::ELEMENTBLOCK].find(pidx) !=
      entityIndices[EntityType::ELEMENTBLOCK].end();
    if (elementBlockFound)
    {
      try
      {
        if (blockCounter++ != 0)
        {
          // add the number of cell types to the block id to ensure uniqueness.
          startSplitEBlockId += VTK_NUMBER_OF_CELL_TYPES;
        }
        auto elementBlock = std::make_shared<vtkElementBlock>(
          pds, blockName, blockId, startSplitEBlockId, controller, writer);
        entityGroups.emplace(elementBlock->GetIOSSEntityType(), elementBlock);
        continue;
      }
      catch (std::runtime_error&)
      {
        break;
      }
    }

    // node set
    const bool nodeSetFound =
      entityIndices[EntityType::NODESET].find(pidx) != entityIndices[EntityType::NODESET].end();
    if (nodeSetFound)
    {
      try
      {
        auto nodeSet = std::make_shared<vtkNodeSet>(pds, blockName, blockId, controller, writer);
        entityGroups.emplace(nodeSet->GetIOSSEntityType(), nodeSet);
        continue;
      }
      catch (std::runtime_error&)
      {
        break;
      }
    }

    // edge set
    const bool edgeSetFound =
      entityIndices[EntityType::EDGESET].find(pidx) != entityIndices[EntityType::EDGESET].end();
    if (edgeSetFound)
    {
      try
      {
        auto edgeSet = std::make_shared<vtkEdgeSet>(pds, blockName, blockId, controller, writer);
        entityGroups.emplace(edgeSet->GetIOSSEntityType(), edgeSet);
        continue;
      }
      catch (std::runtime_error&)
      {
        break;
      }
    }

    // face set
    const bool faceSetFound =
      entityIndices[EntityType::FACESET].find(pidx) != entityIndices[EntityType::FACESET].end();
    if (faceSetFound)
    {
      try
      {
        auto faceSet = std::make_shared<vtkFaceSet>(pds, blockName, blockId, controller, writer);
        entityGroups.emplace(faceSet->GetIOSSEntityType(), faceSet);
        continue;
      }
      catch (std::runtime_error&)
      {
        break;
      }
    }

    // element set
    const bool elementSetFound = entityIndices[EntityType::ELEMENTSET].find(pidx) !=
      entityIndices[EntityType::ELEMENTSET].end();
    if (elementSetFound)
    {
      try
      {
        auto elementSet =
          std::make_shared<vtkElementSet>(pds, blockName, blockId, controller, writer);
        entityGroups.emplace(elementSet->GetIOSSEntityType(), elementSet);
        continue;
      }
      catch (std::runtime_error&)
      {
        break;
      }
    }

    // side set
    const bool sideSetFound =
      entityIndices[EntityType::SIDESET].find(pidx) != entityIndices[EntityType::SIDESET].end();
    if (sideSetFound)
    {
      try
      {
        auto sideSet = std::make_shared<vtkSideSet>(pds, blockName, blockId, controller, writer);
        entityGroups.emplace(sideSet->GetIOSSEntityType(), sideSet);
        continue;
      }
      catch (std::runtime_error&)
      {
        break;
      }
    }
  }
}

//----------------------------------------------------------------------------
vtkIOSSModel::~vtkIOSSModel() = default;

//----------------------------------------------------------------------------
void vtkIOSSModel::DefineModel(Ioss::Region& region) const
{
  auto& internals = (*this->Internals);
  region.begin_mode(Ioss::STATE_DEFINE_MODEL);
  for (auto& entity : internals.EntityGroups)
  {
    entity.second->DefineModel(region);
  }
  region.end_mode(Ioss::STATE_DEFINE_MODEL);
}

//----------------------------------------------------------------------------
void vtkIOSSModel::Model(Ioss::Region& region) const
{
  auto& internals = (*this->Internals);
  region.begin_mode(Ioss::STATE_MODEL);
  for (auto& entity : internals.EntityGroups)
  {
    entity.second->Model(region);
  }
  region.end_mode(Ioss::STATE_MODEL);
}

//----------------------------------------------------------------------------
void vtkIOSSModel::DefineTransient(Ioss::Region& region) const
{
  auto& internals = (*this->Internals);
  region.begin_mode(Ioss::STATE_DEFINE_TRANSIENT);
  for (auto& entity : internals.EntityGroups)
  {
    entity.second->DefineTransient(region);
  }
  region.end_mode(Ioss::STATE_DEFINE_TRANSIENT);
}

//----------------------------------------------------------------------------
void vtkIOSSModel::Transient(Ioss::Region& region, double time) const
{
  auto& internals = (*this->Internals);
  region.begin_mode(Ioss::STATE_TRANSIENT);
  const int step = region.add_state(time);
  region.begin_state(step);
  for (auto& entity : internals.EntityGroups)
  {
    entity.second->Transient(region);
  }
  region.end_state(step);
  region.end_mode(Ioss::STATE_TRANSIENT);
}

//----------------------------------------------------------------------------
std::string vtkIOSSModel::MD5() const
{
  unsigned char digest[16];
  char md5Hash[33];

  vtksysMD5* md5 = vtksysMD5_New();
  vtksysMD5_Initialize(md5);

  const auto& internals = (*this->Internals);
  size_t numberOfItems = internals.EntityGroups.size();
  vtksysMD5_Append(
    md5, reinterpret_cast<const unsigned char*>(&numberOfItems), static_cast<int>(sizeof(size_t)));

  for (const auto& entity : internals.EntityGroups)
  {
    entity.second->AppendMD5(md5);
  }

  vtksysMD5_Finalize(md5, digest);
  vtksysMD5_DigestToHex(digest, md5Hash);
  vtksysMD5_Delete(md5);
  md5Hash[32] = '\0';
  return std::string(md5Hash);
}

//----------------------------------------------------------------------------
bool vtkIOSSModel::GlobalIdsCreated() const
{
  return this->Internals->GlobalIdsCreated;
}
VTK_ABI_NAMESPACE_END
