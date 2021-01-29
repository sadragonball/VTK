/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkOpenFOAMReader.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
// Thanks to Terry Jordan (terry.jordan@sa.netl.doe.gov) of SAIC
// at the National Energy Technology Laboratory who originally developed this class.
//
// --------
// Takuya Oshima of Niigata University, Japan (oshima@eng.niigata-u.ac.jp)
// provided the major bulk of improvements (rewrite) that made the reader
// truly functional.
//
// Token-based FoamFile format lexer/parser,
// performance/stability/compatibility enhancements, gzipped file
// support, lagrangian field support, variable timestep support,
// builtin cell-to-point filter, pointField support, polyhedron
// decomposition support, multiregion support,
// parallelization support for
// decomposed cases in conjunction with vtkPOpenFOAMReader etc.
//
// --------
// Philippose Rajan (sarith@rocketmail.com)
// provided various adjustments
//
// * GUI Based selection of mesh regions and fields available in the case
// * Minor bug fixes / Strict memory allocation checks
// * Minor performance enhancements
//
// --------
// Mark Olesen (OpenCFD Ltd.) www.openfoam.com
// provided various bugfixes, improvements, cleanup
//
// ---------------------------------------------------------------------------
//
// Bugs or support questions should be addressed to the discourse forum
// https://discourse.paraview.org/ and/or KitWare
//
// ---------------------------------------------------------------------------

// Hide VTK_DEPRECATED_IN_9_0_0() warnings for this class.
#define VTK_DEPRECATION_LEVEL 0

// Hijack the CRC routine of zlib to omit CRC check for gzipped files
// (on OSes other than Windows where the mechanism doesn't work due
// to pre-bound DLL symbols) if set to 1, or not (set to 0). Affects
// performance by about 3% - 4%.
#define VTK_FOAMFILE_OMIT_CRCCHECK 0

// The input/output buffer sizes for zlib in bytes.
#define VTK_FOAMFILE_INBUFSIZE (16384)
#define VTK_FOAMFILE_OUTBUFSIZE (131072)
#define VTK_FOAMFILE_INCLUDE_STACK_SIZE (10)

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
// No strtoll on msvc:
#define strtoll _strtoi64
#endif

#if VTK_FOAMFILE_OMIT_CRCCHECK
#define ZLIB_INTERNAL
#endif

// for possible future extension of linehead-aware directives
#define VTK_FOAMFILE_RECOGNIZE_LINEHEAD 0

// Ignore things like 'U_0' restart files.
// This could also be made part of the GUI properties
#define VTK_FOAMFILE_IGNORE_FIELD_RESTART 0

//------------------------------------------------------------------------------
// Developer option to debug the reader states
#define VTK_FOAMFILE_DEBUG 0

// Similar to vtkErrorMacro etc.
#if VTK_FOAMFILE_DEBUG
#define vtkFoamDebug(x)                                                                            \
  do                                                                                               \
  {                                                                                                \
    std::cerr << "" x;                                                                             \
  } while (false)
#else
#define vtkFoamDebug(x)                                                                            \
  do                                                                                               \
  {                                                                                                \
  } while (false)
#endif // VTK_FOAMFILE_DEBUG

//------------------------------------------------------------------------------

#include "vtkOpenFOAMReader.h"

#include "vtk_zlib.h"
#include "vtksys/RegularExpression.hxx"
#include "vtksys/SystemTools.hxx"

#include "vtkAssume.h"
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkCharArray.h"
#include "vtkCollection.h"
#include "vtkDataArraySelection.h"
#include "vtkDirectory.h"
#include "vtkDoubleArray.h"
#include "vtkFloatArray.h"
#include "vtkHexahedron.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkIntArray.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkPolygon.h"
#include "vtkPolyhedron.h"
#include "vtkPyramid.h"
#include "vtkQuad.h"
#include "vtkSmartPointer.h"
#include "vtkSortDataArray.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStringArray.h"
#include "vtkTetra.h"
#include "vtkTriangle.h"
#include "vtkTypeInt32Array.h"
#include "vtkTypeInt64Array.h"
#include "vtkTypeInt8Array.h"
#include "vtkTypeTraits.h"
#include "vtkTypeUInt8Array.h"
#include "vtkUnstructuredGrid.h"
#include "vtkVertex.h"
#include "vtkWeakPointer.h"
#include "vtkWedge.h"

#if !(defined(_WIN32) && !defined(__CYGWIN__) || defined(__LIBCATAMOUNT__))
// for getpwnam() / getpwuid()
#include <pwd.h>
#include <sys/types.h>
// for getuid()
#include <unistd.h>
#endif
// for fabs()
#include <cmath>
// for isalnum() / isspace() / isdigit()
#include <cctype>

#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <typeinfo>
#include <unordered_set>
#include <utility>
#include <vector>

#if VTK_FOAMFILE_OMIT_CRCCHECK
uLong ZEXPORT crc32(uLong, const Bytef*, uInt)
{
  return 0;
}
#endif

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

vtkStandardNewMacro(vtkOpenFOAMReader);

// The name for finiteArea mesh
// pending: static constexpr const char* const NAME_AREAMESH = "areaMesh";

// The name for finiteVolume internal mesh (unzoned)
static constexpr const char* const NAME_INTERNALMESH = "internalMesh";

//------------------------------------------------------------------------------
// Local Functions

namespace
{

// True if data array uses 64-bit representation for its storage
bool Is64BitArray(const vtkDataArray* array)
{
  return (array && array->GetElementComponentSize() == 8);
}

// Given a data array and a flag indicating whether 64 bit labels are used,
// lookup and return a single element in the array. The data array must
// be either a vtkTypeInt32Array or vtkTypeInt64Array.
vtkTypeInt64 GetLabelValue(const vtkDataArray* array, vtkIdType idx, bool use64BitLabels)
{
  if (!use64BitLabels)
  {
    vtkTypeInt64 result =
      static_cast<vtkTypeInt64>(static_cast<const vtkTypeInt32Array*>(array)->GetValue(idx));
    assert(result >= -1); // some arrays store -1 == 'uninitialized'.
    return result;
  }
  else
  {
    vtkTypeInt64 result = static_cast<const vtkTypeInt64Array*>(array)->GetValue(idx);
    assert(result >= -1); // some arrays store -1 == 'uninitialized'.
    return result;
  }
}

// Setter analogous to the above getter.
void SetLabelValue(vtkDataArray* array, vtkIdType idx, vtkTypeInt64 value, bool use64BitLabels)
{
  if (!use64BitLabels)
  {
    assert(static_cast<vtkTypeInt32>(value) >= 0);
    static_cast<vtkTypeInt32Array*>(array)->SetValue(idx, static_cast<vtkTypeInt32>(value));
  }
  else
  {
    assert(value >= 0);
    static_cast<vtkTypeInt64Array*>(array)->SetValue(idx, value);
  }
}

// Another helper for appending an id to a list
void AppendLabelValue(vtkDataArray* array, vtkTypeInt64 val, bool use64BitLabels)
{
  if (!use64BitLabels)
  {
    assert(static_cast<vtkTypeInt32>(val) >= 0);
    static_cast<vtkTypeInt32Array*>(array)->InsertNextValue(static_cast<vtkTypeInt32>(val));
  }
  else
  {
    assert(val >= 0);
    static_cast<vtkTypeInt64Array*>(array)->InsertNextValue(val);
  }
}

} // End anonymous namespace

//------------------------------------------------------------------------------
// Forward Declarations

struct vtkFoamDict;
struct vtkFoamEntry;
struct vtkFoamEntryValue;
struct vtkFoamFile;
struct vtkFoamIOobject;
struct vtkFoamToken;

//------------------------------------------------------------------------------
// class vtkFoamError
// for exception-carrying object or general place to collect errors
struct vtkFoamError : public std::string
{
  vtkFoamError& operator<<(const std::string& str)
  {
    this->std::string::operator+=(str);
    return *this;
  }
  vtkFoamError& operator<<(const char* str)
  {
    this->std::string::operator+=(str);
    return *this;
  }
  template <class T>
  vtkFoamError& operator<<(const T& val)
  {
    std::ostringstream os;
    os << val;
    this->std::string::operator+=(os.str());
    return *this;
  }
};

//------------------------------------------------------------------------------
// Some storage containers

// Manage a list of pointers
template <typename T>
struct vtkFoamPtrList : public std::vector<T*>
{
private:
  typedef std::vector<T*> Superclass;

  // Plain 'delete' each entry
  void DeleteAll()
  {
    for (T* ptr : *this)
    {
      delete ptr;
    }
  }

public:
  // Inherit all constructors
  using std::vector<T*>::vector;

  // Default construct
  vtkFoamPtrList() = default;

  // No copy construct/assignment
  vtkFoamPtrList(const vtkFoamPtrList&) = delete;
  void operator=(const vtkFoamPtrList&) = delete;

  // Destructor - delete each entry
  ~vtkFoamPtrList() { DeleteAll(); }

  // Remove top element, deleting its pointer
  void remove_back()
  {
    if (!Superclass::empty())
    {
      delete Superclass::back();
      Superclass::pop_back();
    }
  }

  // Clear list, delete all elements
  void clear()
  {
    DeleteAll();
    Superclass::clear();
  }
};

// Manage a list of vtkDataObject pointers
template <typename ObjectT>
struct vtkFoamDataArrayVector : public std::vector<ObjectT*>
{
private:
  typedef std::vector<ObjectT*> Superclass;

  // Invoke vtkDataObject Delete() on each (non-null) entry
  void DeleteAll()
  {
    for (ObjectT* ptr : *this)
    {
      if (ptr)
      {
        ptr->Delete();
      }
    }
  }

public:
  // Destructor - invoke vtkDataObject Delete() on each entry
  ~vtkFoamDataArrayVector() { DeleteAll(); }

  // Remove top element, invoking vtkDataObject Delete() on it
  void remove_back()
  {
    if (!Superclass::empty())
    {
      ObjectT* ptr = Superclass::back();
      if (ptr)
      {
        ptr->Delete();
      }
      Superclass::pop_back();
    }
  }

  // Clear list, invoking vtkDataObject Delete() on each element
  void clear()
  {
    DeleteAll();
    Superclass::clear();
  }
};

// Forward Declarations
typedef vtkFoamDataArrayVector<vtkDataArray> vtkFoamLabelArrayVector;

//------------------------------------------------------------------------------
// A std::vector-like data structure where the data
// lies on the stack. If the requested size in the
// resize method is larger than N, the class allocates
// the array on the heap.
//
// Unlike std::vector, the array is not default initialized
// and behaves more like std::array in that manner.
template <typename T, size_t N = 2 * 64 / sizeof(T)>
struct vtkFoamStackVector
{
  typedef T value_type;

  /**
   * Default construct, zero-sized
   */
  vtkFoamStackVector() = default;

  /**
   * Construct with specified length
   */
  explicit vtkFoamStackVector(std::size_t len) { this->resize(len); }

  ~vtkFoamStackVector()
  {
    if (ptr != stck)
    {
      delete[] ptr;
    }
  }

  bool empty() const noexcept { return !size_; }
  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return capacity_; }

  T* data() noexcept { return ptr; }
  const T* data() const noexcept { return ptr; }

  T* begin() noexcept { return ptr; }
  T* end() noexcept { return (ptr + size_); }
  const T* begin() const noexcept { return ptr; }
  const T* end() const noexcept { return (ptr + size_); }

  void reserve(std::size_t len)
  {
    if (capacity_ < len)
    {
      capacity_ = len;
      if (ptr != stck)
      {
        delete[] ptr;
      }
      ptr = new T[capacity_];
    }
  }

  void resize(std::size_t len)
  {
    reserve(len);
    size_ = len;
  }

  T& operator[](std::size_t pos) { return ptr[pos]; }
  const T& operator[](std::size_t pos) const { return ptr[pos]; }

private:
  T stck[N];
  T* ptr = stck;
  std::size_t capacity_ = N;
  std::size_t size_ = 0;
};

//------------------------------------------------------------------------------
// struct vtkFoamLabelListList - details in the implementation class
struct vtkFoamLabelListList
{
  using CellType = vtkFoamStackVector<vtkTypeInt64>;

  virtual ~vtkFoamLabelListList() = default;

  virtual size_t GetLabelSize() const = 0; // in bytes
  bool IsLabel64() const { return this->GetLabelSize() == 8; }
  virtual vtkIdType GetNumberOfElements() const = 0;
  virtual vtkDataArray* GetOffsetsArray() = 0;
  virtual vtkDataArray* GetDataArray() = 0;

  virtual void ResizeExact(vtkIdType numElem, vtkIdType numValues) = 0;
  virtual void ResizeData(vtkIdType numValues) = 0;

  // Fill offsets with zero
  virtual void ResetOffsets() = 0;

  virtual vtkTypeInt64 GetBeginOffset(vtkIdType i) const = 0;
  virtual vtkTypeInt64 GetEndOffset(vtkIdType i) const = 0;
  virtual vtkIdType GetSize(vtkIdType i) const = 0;
  virtual void SetOffset(vtkIdType i, vtkIdType val) = 0;
  virtual void IncrementOffset(vtkIdType i) = 0;

  // Combine assignment of the new offset and accessing the data
  virtual void* WritePointer(vtkIdType cellId, vtkIdType dataOffset, vtkIdType elemLength) = 0;

  virtual vtkTypeInt64 GetValue(vtkIdType bodyIndex) const = 0;
  virtual void SetValue(vtkIdType bodyIndex, vtkTypeInt64 val) = 0;

  virtual vtkTypeInt64 GetValue(vtkIdType cellId, vtkIdType subIndex) const = 0;
  virtual void SetValue(vtkIdType cellId, vtkIdType subIndex, vtkTypeInt64 val) = 0;

  virtual void InsertValue(vtkIdType bodyIndex, vtkTypeInt64 val) = 0;
  virtual void GetCell(vtkIdType i, CellType& cell) const = 0;
};

//------------------------------------------------------------------------------
// struct vtkFoamLabelListListImpl (implementation for vtkFoamLabelListList)
// This is roughly comparable to an OpenFOAM CompactListList and largely
// mirrors what the new vtkCellArray (2020: VTK_CELL_ARRAY_V2) now does.
// It contains packed data and a table of offsets
//
template <typename ArrayT>
struct vtkFoamLabelListListImpl : public vtkFoamLabelListList
{
private:
  ArrayT* Offsets;
  ArrayT* Data;

public:
  using LabelArrayType = ArrayT;
  using LabelType = typename ArrayT::ValueType;

  // Default construct
  vtkFoamLabelListListImpl()
    : Offsets(LabelArrayType::New())
    , Data(LabelArrayType::New())
  {
  }

  // Construct a shallow copy from base class
  explicit vtkFoamLabelListListImpl(const vtkFoamLabelListList& rhs)
    : Offsets(nullptr)
    , Data(nullptr)
  {
    assert("Require same element representation." && this->IsLabel64() == rhs.IsLabel64());
    const auto& rhsCast = static_cast<const vtkFoamLabelListListImpl<LabelArrayType>&>(rhs);
    this->Offsets = rhsCast.Offsets;
    this->Data = rhsCast.Data;
    this->Offsets->Register(nullptr); // ref count the copy
    this->Data->Register(nullptr);
  }

  vtkFoamLabelListListImpl(const vtkFoamLabelListListImpl<ArrayT>& rhs)
    : Offsets(rhs.Offsets)
    , Data(rhs.Data)
  {
    this->Offsets->Register(nullptr); // ref count the copy
    this->Data->Register(nullptr);
  }

  void operator=(const vtkFoamLabelListListImpl<ArrayT>&) = delete;

  // Destructor
  ~vtkFoamLabelListListImpl() override
  {
    this->Offsets->Delete();
    this->Data->Delete();
  }

  size_t GetLabelSize() const override { return sizeof(LabelType); }
  vtkIdType GetNumberOfElements() const override { return this->Offsets->GetNumberOfTuples() - 1; }
  vtkDataArray* GetOffsetsArray() override { return this->Offsets; }
  vtkDataArray* GetDataArray() override { return this->Data; }

  void ResizeExact(vtkIdType numElem, vtkIdType numValues) override
  {
    this->Offsets->SetNumberOfValues(numElem + 1);
    this->Data->SetNumberOfValues(numValues);
    this->Offsets->SetValue(0, 0);
  }
  void ResizeData(vtkIdType numValues) override { this->Data->Resize(numValues); }
  void ResetOffsets() override { this->Offsets->FillValue(0); }

  vtkTypeInt64 GetBeginOffset(vtkIdType i) const override { return this->Offsets->GetValue(i); }
  vtkTypeInt64 GetEndOffset(vtkIdType i) const override { return this->Offsets->GetValue(i + 1); }
  vtkIdType GetSize(vtkIdType i) const override
  {
    return this->Offsets->GetValue(i + 1) - this->Offsets->GetValue(i);
  }
  void SetOffset(vtkIdType i, vtkIdType val) override
  {
    this->Offsets->SetValue(i, static_cast<LabelType>(val));
  }
  void IncrementOffset(vtkIdType i) override
  {
    this->Offsets->SetValue(i, this->Offsets->GetValue(i) + 1);
  }

  void* WritePointer(vtkIdType cellId, vtkIdType dataOffset, vtkIdType subLength) override
  {
    return this->Data->WritePointer(*(this->Offsets->GetPointer(cellId)) = dataOffset, subLength);
  }

  vtkTypeInt64 GetValue(vtkIdType bodyIndex) const override
  {
    return this->Data->GetValue(bodyIndex);
  }
  void SetValue(vtkIdType bodyIndex, vtkTypeInt64 value) override
  {
    this->Data->SetValue(bodyIndex, static_cast<LabelType>(value));
  }

  vtkTypeInt64 GetValue(vtkIdType cellId, vtkIdType subIndex) const override
  {
    return this->Data->GetValue(this->Offsets->GetValue(cellId) + subIndex);
  }
  void SetValue(vtkIdType cellId, vtkIdType subIndex, vtkTypeInt64 value) override
  {
    this->Data->SetValue(this->Offsets->GetValue(cellId) + subIndex, static_cast<LabelType>(value));
  }

  void InsertValue(vtkIdType bodyIndex, vtkTypeInt64 value) override
  {
    this->Data->InsertValue(bodyIndex, value);
  }

  void GetCell(vtkIdType i, CellType& cell) const override
  {
    auto idx = this->Offsets->GetValue(i);
    const auto last = this->Offsets->GetValue(i + 1);
    cell.resize(last - idx);

    auto outIter = cell.begin();
    while (idx != last)
    {
      *outIter = this->Data->GetValue(idx);
      ++outIter;
      ++idx;
    }
  }
};

// Forward Declarations
typedef vtkFoamLabelListListImpl<vtkTypeInt32Array> vtkFoamLabelListList32;
typedef vtkFoamLabelListListImpl<vtkTypeInt64Array> vtkFoamLabelListList64;

//------------------------------------------------------------------------------
// struct vtkFoamPatch
// A simple struct to hold OpenFOAM boundary patch information extracted
// from polyMesh/boundary. Similar to Foam::polyPatch
struct vtkFoamPatch
{
  // General patch types (fits as vtkTypeInt8)
  enum patchType
  {
    GEOMETRICAL = 0, // symmetryPlane, wedge, cyclic, empty, etc.
    PHYSICAL = 1,    // patch, wall
    PROCESSOR = 2    // processor
  };

  std::string name_;
  vtkIdType index_ = 0;
  vtkIdType start_ = 0;
  vtkIdType size_ = 0;
  vtkIdType offset_ = 0; // The start-face offset into all boundaries
  patchType type_ = patchType::GEOMETRICAL;
  bool owner_ = true; // Patch owner (processor patch)

  // The first patch face
  vtkIdType startFace() const noexcept { return (this->start_); }

  // One beyond the last patch face
  vtkIdType endFace() const noexcept { return (this->start_ + this->size_); }

  // The patch local face (as per OpenFOAM polyPatch)
  vtkIdType whichFace(vtkIdType meshFacei) const { return (meshFacei - this->start_); }
};

//------------------------------------------------------------------------------
// struct vtkFoamBoundaries
// A collection of boundary patches with additional grouping and selection information
struct vtkFoamBoundaries : public std::vector<vtkFoamPatch>
{
  // Collect and forwarding of errors (cannot use vtkErrorMacro here)
  vtkFoamError error_;

  // Patch groups, according to the inGroups keyword
  std::map<std::string, std::vector<vtkIdType>> groups;

  // Active patch groups
  std::unordered_set<std::string> groupActive;

  // Active patch indices, selected directly
  std::unordered_set<vtkIdType> patchActive;

  // Active patch indices, selected by group
  std::unordered_set<vtkIdType> patchActiveByGroup;

  // We maintain the corresponding face-instances time directory
  // to ensure that topology or other changes are noticed
  std::string timeName_;

  // Reset group and patch selections
  void clearSelections()
  {
    groupActive.clear();
    patchActive.clear();
    patchActiveByGroup.clear();
  }

  // Reset storage and errors, leaves timeName intact
  void clearAll()
  {
    this->clear();
    error_.clear();
    groups.clear();
    this->clearSelections();
  }

  const vtkFoamError& error() const noexcept { return error_; }
  vtkFoamError& error() noexcept { return error_; }

  // The start label of boundary faces in the polyMesh face list.
  // Same as mesh nInternalFaces() if boundaries exist
  vtkIdType startFace() const { return this->empty() ? 0 : this->front().startFace(); }

  // One beyond the last boundary face
  vtkIdType endFace() const { return this->empty() ? 0 : this->back().endFace(); }

  void enablePatch(vtkIdType patchIndex) { patchActive.emplace(patchIndex); }

  void enableGroup(const std::string& groupName)
  {
    auto citer = groups.find(groupName);
    if (citer != groups.end())
    {
      const std::vector<vtkIdType>& patchIndices = citer->second;
      for (const vtkIdType patchIndex : patchIndices)
      {
        patchActiveByGroup.emplace(patchIndex);
      }
    }
  }

  // True if given patch index is active
  bool isActive(vtkIdType patchIndex) const
  {
    return (patchActive.find(patchIndex) != patchActive.end()) ||
      (patchActiveByGroup.find(patchIndex) != patchActiveByGroup.end());
  }

  // Set contents from dictionary
  // Return false on errors
  bool update(const vtkFoamDict& dict);

  // The patch index for a given face label, -1 for internal face or out-of-bounds
  vtkIdType whichPatch(vtkIdType faceIndex) const;
};

//------------------------------------------------------------------------------
// Simple handling of common OpenFOAM data types
struct vtkFoamTypes
{
  // Primitive types, with nComponents encoded in lower 4 bits
  enum dataType
  {
    NO_TYPE = 0,
    LABEL_TYPE = 0x11,
    SCALAR_TYPE = 1,
    VECTOR_TYPE = 3,
    SYMM_TENSOR_TYPE = 6,
    TENSOR_TYPE = 9,
    SPH_TENSOR_TYPE = 0x21
  };

  // The number of data components
  static int GetNumberOfComponents(const dataType dtype) noexcept { return (dtype & 0xF); }

  static bool IsLabel(const dataType dtype) noexcept { return dtype == LABEL_TYPE; }
  static bool IsScalar(const dataType dtype) noexcept { return dtype == SCALAR_TYPE; }
  static bool IsNumeric(const dataType dtype) noexcept { return IsLabel(dtype) || IsScalar(dtype); }

  // Is a VectorSpace type?
  static bool IsVectorSpace(const dataType dtype) noexcept
  {
    return GetNumberOfComponents(dtype) > 1 || dtype == SPH_TENSOR_TYPE;
  }

  // Parse things like "scalarField" or "ScalarField" -> SCALAR_TYPE etc.
  // Ignore case on first letter, which makes it convenient for "volScalarField" too.
  static dataType ToEnum(const std::string& fieldType, size_t pos = 0);
};

//------------------------------------------------------------------------------
// class vtkOpenFOAMReaderPrivate
// the reader core of vtkOpenFOAMReader
class vtkOpenFOAMReaderPrivate : public vtkObject
{
public:
  static vtkOpenFOAMReaderPrivate* New();
  vtkTypeMacro(vtkOpenFOAMReaderPrivate, vtkObject);

  vtkGetMacro(TimeStep, int);
  vtkSetMacro(TimeStep, int);

  void SetTimeValue(double requestedTime);

  vtkStringArray* GetTimeNames() { return this->TimeNames; }
  vtkDoubleArray* GetTimeValues() { return this->TimeValues; }

  bool HasPolyMesh() const
  {
    return this->PolyMeshFacesDir && this->PolyMeshFacesDir->GetNumberOfValues();
  }

  const std::string& GetRegionName() const noexcept { return this->RegionName; }

  // Read mesh/fields and create dataset
  int RequestData(vtkMultiBlockDataSet* output);
  int MakeMetaDataAtTimeStep(vtkStringArray*, vtkStringArray*, vtkStringArray*, bool);

  // Gather time instances information and create mesh times
  bool MakeInformationVector(const std::string& casePath, const std::string& controlDictPath,
    const std::string& procName, vtkOpenFOAMReader* parent, bool requirePolyMesh = true);

  // Copy time instances information and create mesh times
  void SetupInformation(const std::string& casePath, const std::string& regionName,
    const std::string& procName, vtkOpenFOAMReaderPrivate* master, bool requirePolyMesh = true);

private:
  vtkOpenFOAMReader* Parent;

  // Case and region
  std::string CasePath;
  std::string RegionName;
  std::string ProcessorName;

  // time information
  vtkDoubleArray* TimeValues;
  vtkStringArray* TimeNames;
  int TimeStep;
  int TimeStepOld;

  int InternalMeshSelectionStatus;
  int InternalMeshSelectionStatusOld;

  // filenames / directories
  vtkStringArray* VolFieldFiles;
  vtkStringArray* DimFieldFiles;
  vtkStringArray* AreaFieldFiles;
  vtkStringArray* PointFieldFiles;
  vtkStringArray* LagrangianFieldFiles;
  vtkStringArray* PolyMeshPointsDir;
  vtkStringArray* PolyMeshFacesDir;

  // Mesh dimensions and construction information
  vtkIdType NumPoints;
  vtkIdType NumInternalFaces;
  vtkIdType NumFaces;
  vtkIdType NumCells;

  // The face owner, neighbour (labelList)
  vtkDataArray* FaceOwner;
  vtkDataArray* FaceNeigh;

  // For cell-to-point interpolation
  vtkPolyData* AllBoundaries;
  vtkDataArray* AllBoundariesPointMap;
  vtkDataArray* InternalPoints;

  // For caching mesh
  vtkUnstructuredGrid* InternalMesh;
  vtkMultiBlockDataSet* BoundaryMesh;
  vtkFoamLabelArrayVector* BoundaryPointMap;
  vtkFoamBoundaries BoundaryDict;

  // Zones
  vtkMultiBlockDataSet* CellZoneMesh;
  vtkMultiBlockDataSet* FaceZoneMesh;
  vtkMultiBlockDataSet* PointZoneMesh;

  // For polyhedral decomposition
  int NumTotalAdditionalCells;
  vtkIdTypeArray* AdditionalCellIds;
  vtkIntArray* NumAdditionalCells;
  vtkFoamLabelArrayVector* AdditionalCellPoints;

  // Constructor and destructor are kept private
  vtkOpenFOAMReaderPrivate();
  ~vtkOpenFOAMReaderPrivate() override;

  vtkOpenFOAMReaderPrivate(const vtkOpenFOAMReaderPrivate&) = delete;
  void operator=(const vtkOpenFOAMReaderPrivate&) = delete;

  // Clear mesh construction
  void ClearInternalMeshes();
  void ClearBoundaryMeshes();
  void ClearZoneMeshes();
  void ClearMeshes();

  std::string RegionPath() const
  {
    return (this->RegionName.empty() ? "" : "/") + this->RegionName;
  }
  std::string RegionPrefix() const
  {
    return this->RegionName + (this->RegionName.empty() ? "" : "/");
  }
  std::string TimePath(int timeIndex) const
  {
    return this->CasePath + this->TimeNames->GetValue(timeIndex);
  }
  std::string TimeRegionPath(int timeIndex) const
  {
    return this->TimePath(timeIndex) + this->RegionPath();
  }
  std::string CurrentTimePath() const { return this->TimePath(this->TimeStep); }
  std::string CurrentTimeRegionPath() const { return this->TimeRegionPath(this->TimeStep); }
  std::string CurrentTimeRegionMeshPath(vtkStringArray* dir) const
  {
    return this->CasePath + dir->GetValue(this->TimeStep) + this->RegionPath() + "/polyMesh/";
  }

  // Append time directories for mesh
  void AppendMeshDirToArray(vtkStringArray*, vtkIdType timeIndex, bool changed);

  // Search time directories for mesh
  void PopulatePolyMeshDirArrays();

  void AddFieldName(
    const std::string& fieldName, const std::string& fieldType, bool isLagrangian = false);
  // Search a time directory for field objects
  void GetFieldNames(const std::string&, bool isLagrangian = false);
  void SortFieldFiles(vtkStringArray* selections, vtkStringArray* files);
  void LocateLagrangianClouds(const std::string& timePath);

  // List time directories according to system/controlDict
  bool ListTimeDirectoriesByControlDict(const vtkFoamDict& dict);

  // List time directories by searching in a case directory
  bool ListTimeDirectoriesByInstances();

  // Read polyMesh/points (vectorField)
  vtkFloatArray* ReadPointsFile();

  // Read polyMesh/faces (faceCompactList or faceList)
  std::unique_ptr<vtkFoamLabelListList> ReadFacesFile(const std::string& meshDir);

  // Read polyMesh/{owner,neighbour}, check overall number of faces. Return meshCells
  std::unique_ptr<vtkFoamLabelListList> ReadOwnerNeighbourFiles(const std::string& meshDir);

  // Create meshCells from owner/neighbour information
  std::unique_ptr<vtkFoamLabelListList> CreateCellFaces(
    const vtkDataArray& faceOwner, const vtkDataArray& faceNeigh);

  bool CheckFaceList(const vtkFoamLabelListList& faces);

  // Create volume mesh
  void InsertCellsToGrid(vtkUnstructuredGrid*, const vtkFoamLabelListList& meshCells,
    const vtkFoamLabelListList& meshFaces, vtkFloatArray* pointArray,
    vtkIdTypeArray* additionalCellIds = nullptr, vtkDataArray* cellLabels = nullptr);

  vtkUnstructuredGrid* MakeInternalMesh(const vtkFoamLabelListList& meshCells,
    const vtkFoamLabelListList& meshFaces, vtkFloatArray* pointArray);

  void InsertFacesToGrid(vtkPolyData*, const vtkFoamLabelListList& meshFaces, vtkIdType startFace,
    vtkIdType endFace, vtkDataArray*, vtkIdList*, vtkDataArray*, bool);

  template <typename T1, typename T2>
  bool ExtendArray(T1*, vtkIdType);

  vtkMultiBlockDataSet* MakeBoundaryMesh(const vtkFoamLabelListList& meshFaces, vtkFloatArray*);

  void TruncateFaceOwner();

  // Move additional points for decomposed cells
  vtkPoints* MoveInternalMesh(vtkUnstructuredGrid*, vtkFloatArray*);
  void MoveBoundaryMesh(vtkMultiBlockDataSet*, vtkFloatArray*);

  // cell-to-point interpolator
  void InterpolateCellToPoint(
    vtkFloatArray*, vtkFloatArray*, vtkPointSet*, vtkDataArray*, vtkTypeInt64);

  // Convert OpenFOAM dimension array to string
  std::string ConstructDimensions(const vtkFoamDict& dict) const;

  // read and create cell/point fields
  bool ReadFieldFile(vtkFoamIOobject& io, vtkFoamDict& dict, const std::string& varName,
    const vtkDataArraySelection* selection);
  vtkFloatArray* FillField(vtkFoamEntry& entry, vtkIdType nElements, const vtkFoamIOobject& io,
    vtkFoamTypes::dataType fieldDataType);
  void GetVolFieldAtTimeStep(const std::string& varName, bool isInternalField = false);
  // void GetAreaFieldAtTimeStep(const std::string& varName);
  void GetPointFieldAtTimeStep(const std::string& varName);

  // Create lagrangian mesh/fields
  vtkMultiBlockDataSet* MakeLagrangianMesh();

  // Read specified file (typeName)
  std::unique_ptr<vtkFoamDict> GatherBlocks(const char* typeName, bool mandatory);

  // Create (cell|face|point) zones
  bool GetCellZoneMesh(vtkMultiBlockDataSet* zoneMesh, const vtkFoamLabelListList& meshCells,
    const vtkFoamLabelListList& meshFaces, vtkPoints*);
  bool GetFaceZoneMesh(
    vtkMultiBlockDataSet* zoneMesh, const vtkFoamLabelListList& meshFaces, vtkPoints*);
  bool GetPointZoneMesh(vtkMultiBlockDataSet* zoneMesh, vtkPoints*);
};

vtkStandardNewMacro(vtkOpenFOAMReaderPrivate);

//------------------------------------------------------------------------------
// Local Functions

namespace
{

// Set named block
void SetBlock(vtkMultiBlockDataSet* parent, unsigned int blockIndex, vtkDataObject* block,
  const std::string& name)
{
  parent->SetBlock(blockIndex, block);
  parent->GetMetaData(blockIndex)->Set(vtkCompositeDataSet::NAME(), name.c_str());
}

// Append named block
void AppendBlock(vtkMultiBlockDataSet* parent, vtkDataObject* block, const std::string& name)
{
  ::SetBlock(parent, parent->GetNumberOfBlocks(), block, name);
}

// Set array name and fieldData attributes
// The optional suffix is for dimensions etc
void AddArrayToFieldData(vtkDataSetAttributes* fieldData, vtkDataArray* array,
  const std::string& name, const std::string& suffix = "")
{
  if (suffix.empty())
  {
    array->SetName(name.c_str());
  }
  else
  {
    array->SetName((name + suffix).c_str());
  }

  if (array->GetNumberOfComponents() == 1 && name == "p")
  {
    fieldData->SetScalars(array);
  }
  else if (array->GetNumberOfComponents() == 3 && name == "U")
  {
    fieldData->SetVectors(array);
  }
  else
  {
    fieldData->AddArray(array);
  }
}

} // End anonymous namespace

//------------------------------------------------------------------------------
// Simple handling of common OpenFOAM data types

vtkFoamTypes::dataType vtkFoamTypes::ToEnum(const std::string& fieldType, size_t pos)
{
  vtkFoamTypes::dataType dtype(vtkFoamTypes::NO_TYPE);

  const char firstChar = fieldType[pos];
  ++pos; // First character is handled separately (for ignoring case)

  size_t len = fieldType.find("Field", pos);
  if (len != std::string::npos)
  {
    len -= pos;
  }

  switch (firstChar)
  {
    case 'L':
    case 'l':
    {
      if (fieldType.compare(pos, len, "abel") == 0)
      {
        // (Label | label)
        dtype = vtkFoamTypes::LABEL_TYPE;
      }
    }
    break;

    case 'S':
    case 's':
    {
      if (fieldType.compare(pos, len, "calar") == 0)
      {
        // (Scalar | scalar)
        dtype = vtkFoamTypes::SCALAR_TYPE;
      }
      else if (fieldType.compare(pos, len, "phericalTensor") == 0)
      {
        // (SphericalTensor | sphericalTensor)
        dtype = vtkFoamTypes::SPH_TENSOR_TYPE;
      }
      else if (fieldType.compare(pos, len, "ymmTensor") == 0)
      {
        // (SymmTensor | symmTensor)
        dtype = vtkFoamTypes::SYMM_TENSOR_TYPE;
      }
    }
    break;

    case 'T':
    case 't':
    {
      if (fieldType.compare(pos, len, "ensor") == 0)
      {
        // (Tensor | tensor)
        dtype = vtkFoamTypes::TENSOR_TYPE;
      }
    }
    break;

    case 'V':
    case 'v':
    {
      if (fieldType.compare(pos, len, "ector") == 0)
      {
        // (Vector | vector)
        dtype = vtkFoamTypes::VECTOR_TYPE;
      }
    }
    break;
  }

  return dtype;
}

//------------------------------------------------------------------------------
// class vtkFoamStreamOption
// Some elements from Foam::IOstreamOption and from Foam::IOstream
// - format (ASCII | BINARY)
// - label, scalar sizes
//
// Note: all enums pack into 32-bits, so we can use them in vtkFoamToken, vtkFoamFile etc.
// without adversely affecting the size of the structures
struct vtkFoamStreamOption
{
public:
  // The OpenFOAM input stream format is ASCII or BINARY
  enum fileFormat : unsigned char
  {
    ASCII = 0, // Default is ASCII unless otherwise specified
    BINARY
  };

  // Bitwidth of an OpenFOAM label (integer type)
  enum labelType : unsigned char
  {
    UNKNOWN_LABEL = 0, // For assertions
    INT32,
    INT64
  };

  // Bitwidth of an OpenFOAM scalar (floating-point type)
  enum scalarType : unsigned char
  {
    UNKNOWN_SCALAR = 0, // For assertions
    FLOAT32,
    FLOAT64
  };

private:
  fileFormat Format = fileFormat::ASCII;
  labelType LabelType = labelType::UNKNOWN_LABEL;
  scalarType ScalarType = scalarType::FLOAT64;

public:
  // Default construct
  vtkFoamStreamOption() = default;

  // Construct with specified handling for labels/floats
  vtkFoamStreamOption(const bool use64BitLabels, const bool use64BitFloats)
  {
    this->SetLabel64(use64BitLabels);
    this->SetFloat64(use64BitFloats);
  }

  fileFormat GetFormat() const { return this->Format; }
  bool IsAsciiFormat() const { return this->Format != fileFormat::BINARY; }
  void SetUseBinaryFormat(const bool on)
  {
    this->Format = (on ? fileFormat::BINARY : fileFormat::ASCII);
  }

  labelType GetLabelType() const { return this->LabelType; }
  bool IsLabel64() const { return this->LabelType == labelType::INT64; }
  bool HasLabelType() const noexcept { return this->LabelType != labelType::UNKNOWN_LABEL; }
  void SetLabelType(labelType t) { this->LabelType = t; }
  void SetLabel64(const bool on) { this->LabelType = (on ? labelType::INT64 : labelType::INT32); }

  scalarType GetScalarType() const { return this->ScalarType; }
  bool IsFloat64() const { return this->ScalarType == scalarType::FLOAT64; }

  void SetScalarType(scalarType t) { this->ScalarType = t; }
  void SetFloat64(const bool on)
  {
    this->ScalarType = (on ? scalarType::FLOAT64 : scalarType::FLOAT32);
  }

  const vtkFoamStreamOption& GetStreamOption() const
  {
    return static_cast<const vtkFoamStreamOption&>(*this);
  }
  void SetStreamOption(const vtkFoamStreamOption& opt)
  {
    static_cast<vtkFoamStreamOption&>(*this) = opt;
  }
};

//------------------------------------------------------------------------------
// class vtkFoamToken
// token class which also works as container for list types
// - a word token is treated as a string token for simplicity
// - handles only atomic types. Handling of list types are left to the
//   derived classes.
struct vtkFoamToken : public vtkFoamStreamOption
{
public:
  enum tokenType
  {
    // Undefined type
    UNDEFINED = 0,
    // atomic types
    PUNCTUATION,
    LABEL,
    SCALAR,
    STRING,
    IDENTIFIER,
    // List types (vtkObject-derived)
    STRINGLIST,
    LABELLIST,
    SCALARLIST,
    VECTORLIST,
    // List types (non-vtkObject)
    LABELLISTLIST,
    ENTRYVALUELIST,
    BOOLLIST,
    EMPTYLIST,
    DICTIONARY,
    // error state
    TOKEN_ERROR
  };

protected:
  tokenType Type = tokenType::UNDEFINED;
  union {
    char Char;
    vtkTypeInt64 Int;
    double Double;
    // Any/all pointer types
    void* AnyPointer;
    std::string* String;
    // List types (vtkObject-derived)
    vtkObjectBase* VtkObjectPtr;
    vtkStringArray* StringListPtr;
    vtkFloatArray* ScalarListPtr;
    vtkFloatArray* VectorListPtr;
    vtkDataArray* LabelListPtr;
    // List types (non-vtkObject)
    vtkFoamLabelListList* LabelListListPtr;
    vtkFoamPtrList<vtkFoamEntryValue>* EntryValuePtrs;
    vtkFoamDict* DictPtr;
  };

  void Clear()
  {
    if (this->Type == STRING || this->Type == IDENTIFIER)
    {
      delete this->String;
    }
  }

  void AssignData(const vtkFoamToken& tok)
  {
    switch (tok.Type)
    {
      case PUNCTUATION:
        this->Char = tok.Char;
        break;
      case LABEL:
        this->Int = tok.Int;
        break;
      case SCALAR:
        this->Double = tok.Double;
        break;
      case STRING:
      case IDENTIFIER:
        this->String = new std::string(*tok.String);
        break;
      case UNDEFINED:
      case STRINGLIST:
      case LABELLIST:
      case SCALARLIST:
      case VECTORLIST:
      case LABELLISTLIST:
      case ENTRYVALUELIST:
      case BOOLLIST:
      case EMPTYLIST:
      case DICTIONARY:
      case TOKEN_ERROR:
        break;
    }
  }

public:
  // Default construct
  vtkFoamToken() = default;

  vtkFoamToken(const vtkFoamToken& tok)
    : vtkFoamStreamOption(tok)
    , Type(tok.Type)
  {
    this->AssignData(tok);
  }
  ~vtkFoamToken() { this->Clear(); }

  tokenType GetType() const { return this->Type; }

  template <typename T>
  bool Is() const;
  template <typename T>
  T To() const;
#if defined(_MSC_VER)
  // workaround for Win32-64ids-nmake70
  template <>
  bool Is<vtkTypeInt32>() const;
  template <>
  bool Is<vtkTypeInt64>() const;
  template <>
  bool Is<float>() const;
  template <>
  bool Is<double>() const;
  template <>
  vtkTypeInt32 To<vtkTypeInt32>() const;
  template <>
  vtkTypeInt64 To<vtkTypeInt64>() const;
  template <>
  float To<float>() const;
  template <>
  double To<double>() const;
#endif

  // True if token represents punctuation
  bool IsPunctuation() const noexcept { return this->Type == PUNCTUATION; }

  // True if token represents a numerical value
  bool IsNumeric() const noexcept { return this->Type == LABEL || this->Type == SCALAR; }

  vtkTypeInt64 ToInt() const
  {
    assert("Label type not set!" && this->HasLabelType());
    return this->Int;
  }

  // Mostly the same as To<float>, with additional check
  float ToFloat() const
  {
    return this->Type == LABEL ? static_cast<float>(this->Int)
                               : this->Type == SCALAR ? static_cast<float>(this->Double) : 0.0F;
  }

  // Mostly the same as To<double>, with additional check
  double ToDouble() const
  {
    return this->Type == LABEL ? static_cast<double>(this->Int)
                               : this->Type == SCALAR ? this->Double : 0.0;
  }

  std::string ToString() const { return *this->String; }
  std::string ToIdentifier() const { return *this->String; }

  void SetBad()
  {
    this->Clear();
    this->Type = TOKEN_ERROR;
  }
  void SetIdentifier(const std::string& idString)
  {
    this->operator=(idString);
    this->Type = IDENTIFIER;
  }

  void operator=(const char value)
  {
    this->Clear();
    this->Type = PUNCTUATION;
    this->Char = value;
  }
  void operator=(const vtkTypeInt32 value)
  {
    this->Clear();

    assert("Label type not set!" && this->HasLabelType());
    if (this->IsLabel64())
    {
      vtkGenericWarningMacro("Setting a 64 bit label from a 32 bit integer.");
    }

    this->Type = LABEL;
    this->Int = static_cast<vtkTypeInt32>(value);
  }
  void operator=(const vtkTypeInt64 value)
  {
    this->Clear();

    assert("Label type not set!" && this->HasLabelType());
    if (!this->IsLabel64())
    {
      vtkGenericWarningMacro("Setting a 32 bit label from a 64 bit integer. "
                             "Precision loss may occur.");
    }

    this->Type = LABEL;
    this->Int = value;
  }
  void operator=(const double value)
  {
    this->Clear();
    this->Type = SCALAR;
    this->Double = value;
  }
  void operator=(const char* value)
  {
    this->Clear();
    this->Type = STRING;
    this->String = new std::string(value);
  }
  void operator=(const std::string& value)
  {
    this->Clear();
    this->Type = STRING;
    this->String = new std::string(value);
  }
  vtkFoamToken& operator=(const vtkFoamToken& tok)
  {
    this->Clear();
    this->SetStreamOption(tok);
    this->Type = tok.Type;
    this->AssignData(tok);
    return *this;
  }
  bool operator==(const char value) const
  {
    return this->Type == PUNCTUATION && this->Char == value;
  }
  bool operator==(const vtkTypeInt32 value) const
  {
    assert("Label type not set!" && this->HasLabelType());
    return this->Type == LABEL && this->Int == static_cast<vtkTypeInt64>(value);
  }
  bool operator==(const vtkTypeInt64 value) const
  {
    assert("Label type not set!" && this->HasLabelType());
    return this->Type == LABEL && this->Int == value;
  }
  bool operator==(const std::string& value) const
  {
    return this->Type == STRING && *this->String == value;
  }
  bool operator!=(const std::string& value) const
  {
    return this->Type != STRING || *this->String != value;
  }
  bool operator!=(const char value) const { return !this->operator==(value); }

  friend std::ostringstream& operator<<(std::ostringstream& str, const vtkFoamToken& tok)
  {
    switch (tok.GetType())
    {
      case TOKEN_ERROR:
        str << "badToken (an unexpected EOF?)";
        break;
      case PUNCTUATION:
        str << tok.Char;
        break;
      case LABEL:
        assert("Label type not set!" && tok.HasLabelType());
        if (tok.IsLabel64())
        {
          str << tok.Int;
        }
        else
        {
          str << static_cast<vtkTypeInt32>(tok.Int);
        }
        break;
      case SCALAR:
        str << tok.Double;
        break;
      case STRING:
      case IDENTIFIER:
        str << *(tok.String);
        break;
      case UNDEFINED:
      case STRINGLIST:
      case LABELLIST:
      case SCALARLIST:
      case VECTORLIST:
      case LABELLISTLIST:
      case ENTRYVALUELIST:
      case BOOLLIST:
      case EMPTYLIST:
      case DICTIONARY:
        break;
    }
    return str;
  }
};

template <>
inline bool vtkFoamToken::Is<vtkTypeInt8>() const
{
  // masquerade for bool
  return this->Type == LABEL;
}

template <>
inline bool vtkFoamToken::Is<vtkTypeInt32>() const
{
  assert("Label type not set!" && this->HasLabelType());
  return this->Type == LABEL && !(this->IsLabel64());
}

template <>
inline bool vtkFoamToken::Is<vtkTypeInt64>() const
{
  assert("Label type not set!" && this->HasLabelType());
  return this->Type == LABEL;
}

template <>
inline bool vtkFoamToken::Is<float>() const
{
  return this->Type == LABEL || this->Type == SCALAR;
}

template <>
inline bool vtkFoamToken::Is<double>() const
{
  return this->Type == SCALAR;
}

// ie, a bool value
template <>
inline vtkTypeInt8 vtkFoamToken::To<vtkTypeInt8>() const
{
  return static_cast<vtkTypeInt8>(this->Int);
}

template <>
inline vtkTypeInt32 vtkFoamToken::To<vtkTypeInt32>() const
{
  assert("Label type not set!" && this->HasLabelType());
  if (this->IsLabel64())
  {
    vtkGenericWarningMacro("Casting 64 bit label to int32. Precision loss "
                           "may occur.");
  }
  return static_cast<vtkTypeInt32>(this->Int);
}

template <>
inline vtkTypeInt64 vtkFoamToken::To<vtkTypeInt64>() const
{
  assert("Label type not set!" && this->HasLabelType());
  return this->Int;
}

template <>
inline float vtkFoamToken::To<float>() const
{
  return this->Type == LABEL ? static_cast<float>(this->Int) : static_cast<float>(this->Double);
}

template <>
inline double vtkFoamToken::To<double>() const
{
  return this->Type == LABEL ? static_cast<double>(this->Int) : this->Double;
}

//------------------------------------------------------------------------------
// class vtkFoamFileStack
// list of variables that have to be saved when a file is included.
struct vtkFoamFileStack
{
protected:
  vtkOpenFOAMReader* Reader;
  std::string FileName;
  FILE* File;
  bool IsCompressed;
  z_stream Z;
  int ZStatus;
  int LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
  bool WasNewline;
#endif

  // buffer pointers. using raw pointers for performance reason.
  unsigned char* Inbuf;
  unsigned char* Outbuf;
  unsigned char* BufPtr;
  unsigned char* BufEndPtr;

  vtkFoamFileStack(vtkOpenFOAMReader* reader)
    : Reader(reader)
    , File(nullptr)
    , IsCompressed(false)
    , ZStatus(Z_OK)
    , LineNumber(0)
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
    , WasNewline(true)
#endif
    , Inbuf(nullptr)
    , Outbuf(nullptr)
    , BufPtr(nullptr)
    , BufEndPtr(nullptr)
  {
    this->Z.zalloc = Z_NULL;
    this->Z.zfree = Z_NULL;
    this->Z.opaque = Z_NULL;
  }

  void Reset()
  {
    // this->FileName = "";
    this->File = nullptr;
    this->IsCompressed = false;
    // this->ZStatus = Z_OK;
    this->Z.zalloc = Z_NULL;
    this->Z.zfree = Z_NULL;
    this->Z.opaque = Z_NULL;
    // this->LineNumber = 0;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
    this->WasNewline = true;
#endif

    this->Inbuf = nullptr;
    this->Outbuf = nullptr;
    // this->BufPtr = nullptr;
    // this->BufEndPtr = nullptr;
  }

public:
  const std::string& GetFileName() const noexcept { return this->FileName; }
  int GetLineNumber() const noexcept { return this->LineNumber; }
  vtkOpenFOAMReader* GetReader() const noexcept { return this->Reader; }
};

//------------------------------------------------------------------------------
// class vtkFoamFile
// Read and tokenize the input. Retains format and label/scalar size informatio
struct vtkFoamFile
  : public vtkFoamStreamOption
  , public vtkFoamFileStack
{
private:
  typedef vtkFoamFileStack Superclass;

public:
  // The dictionary #inputMode values
  enum inputModes
  {
    INPUT_MODE_MERGE,
    INPUT_MODE_OVERWRITE,
    INPUT_MODE_PROTECT,
    INPUT_MODE_WARN,
    INPUT_MODE_ERROR
  };

  // Check for existence of specified file
  static bool IsFile(const std::string& file, bool checkGzip = true)
  {
    return (vtksys::SystemTools::FileExists(file, true) ||
      (checkGzip && vtksys::SystemTools::FileExists(file + ".gz", true)));
  }

  // Generic exception throwing with stack trace
  void ThrowStackTrace(const std::string& msg);

private:
  // The path to the case
  std::string CasePath;

  // The current input mode
  inputModes InputMode;

  // Handling include files
  vtkFoamFileStack* Stack[VTK_FOAMFILE_INCLUDE_STACK_SIZE];
  int StackI;

  bool InflateNext(unsigned char* buf, size_t requestSize, vtkTypeInt64* readSize = nullptr);
  int NextTokenHead();

  // Keep exception throwing / recursive codes out-of-line to make
  // putBack(), getc() and readExpecting() inline expandable
  void ThrowDuplicatedPutBackException();
  void ThrowUnexpectedEOFException();
  void ThrowUnexpectedNondigitException(int c);
  void ThrowUnexpectedTokenException(char, int c);
  int ReadNext();

  void PutBack(const int c)
  {
    if (--this->Superclass::BufPtr < this->Superclass::Outbuf)
    {
      this->ThrowDuplicatedPutBackException();
    }
    *this->Superclass::BufPtr = static_cast<unsigned char>(c);
  }

  // get a character
  int Getc()
  {
    return this->Superclass::BufPtr == this->Superclass::BufEndPtr ? this->ReadNext()
                                                                   : *this->Superclass::BufPtr++;
  }

  vtkFoamError StackString()
  {
    vtkFoamError err;
    if (this->StackI > 0)
    {
      err << "\n included";

      for (int stackI = this->StackI - 1; stackI >= 0; stackI--)
      {
        err << " from line " << this->Stack[stackI]->GetLineNumber() << " of "
            << this->Stack[stackI]->GetFileName() << "\n";
      }
      err << ": ";
    }
    return err;
  }

  bool CloseIncludedFile()
  {
    if (this->StackI == 0)
    {
      return false;
    }
    this->Clear();
    this->StackI--;
    // use the default bitwise assignment operator
    this->Superclass::operator=(*this->Stack[this->StackI]);
    delete this->Stack[this->StackI];
    return true;
  }

  void Clear()
  {
    if (this->Superclass::IsCompressed)
    {
      inflateEnd(&this->Superclass::Z);
    }

    delete[] this->Superclass::Inbuf;
    delete[] this->Superclass::Outbuf;
    this->Superclass::Inbuf = this->Superclass::Outbuf = nullptr;

    if (this->Superclass::File)
    {
      fclose(this->Superclass::File);
      this->Superclass::File = nullptr;
    }
    // don't reset the line number so that the last line number is
    // retained after close
    // lineNumber_ = 0;
  }

  //! Return file name (part beyond last /)
  std::string ExtractName(const std::string& path) const
  {
#if defined(_WIN32)
    const std::string pathFindSeparator = "/\\", pathSeparator = "\\";
#else
    const std::string pathFindSeparator = "/", pathSeparator = "/";
#endif
    auto pos = path.find_last_of(pathFindSeparator);
    if (pos == std::string::npos)
    {
      // no slash
      return path;
    }
    else if (pos + 1 == path.size())
    {
      // final trailing slash
      const auto endPos = pos;
      pos = path.find_last_of(pathFindSeparator, pos - 1);
      if (pos == std::string::npos)
      {
        // no further slash
        return path.substr(0, endPos);
      }
      else
      {
        return path.substr(pos + 1, endPos - pos - 1);
      }
    }
    else
    {
      return path.substr(pos + 1);
    }
  }

  //! Return directory path name (part before last /)
  std::string ExtractPath(const std::string& path) const
  {
#if defined(_WIN32)
    const std::string pathFindSeparator = "/\\", pathSeparator = "\\";
#else
    const std::string pathFindSeparator = "/", pathSeparator = "/";
#endif
    const auto pos = path.find_last_of(pathFindSeparator);
    return pos == std::string::npos ? std::string(".") + pathSeparator : path.substr(0, pos + 1);
  }

public:
  // No default construct, copy or assignment
  vtkFoamFile() = delete;
  vtkFoamFile(const vtkFoamFile&) = delete;
  void operator=(const vtkFoamFile&) = delete;

  vtkFoamFile(const std::string& casePath, vtkOpenFOAMReader* reader)
    : vtkFoamStreamOption(reader->GetUse64BitLabels(), reader->GetUse64BitFloats())
    , vtkFoamFileStack(reader)
    , CasePath(casePath)
    , InputMode(INPUT_MODE_MERGE)
    , StackI(0)
  {
  }
  ~vtkFoamFile() { this->Close(); }

  std::string GetCasePath() const noexcept { return this->CasePath; }
  std::string GetFilePath() const { return this->ExtractPath(this->FileName); }

  inputModes GetInputMode() const noexcept { return this->InputMode; }

  std::string ExpandPath(const std::string& pathIn, const std::string& defaultPath)
  {
    std::string expandedPath;
    bool isExpanded = false, wasPathSeparator = true;
    size_t charI = 0;
    const size_t nChars = pathIn.length();

    std::string::size_type delim = 0;

    if ('<' == pathIn[0] && (delim = pathIn.find(">/")) != std::string::npos)
    {
      // Expand a leading <tag>/
      // Convenient for frequently used directories - see OpenFOAM stringOps.C
      //
      // Handle
      //   <case>/       => FOAM_CASE directory
      //   <constant>/   => FOAM_CASE/constant directory
      //   <system>/     => FOAM_CASE/system directory
      //   <etc>/        => not handled

      const std::string tag(pathIn, 1, delim - 2);

      if (tag == "case")
      {
        expandedPath = this->CasePath + '/';
        isExpanded = true;
        wasPathSeparator = false;
      }
      else if (tag == "constant" || tag == "system")
      {
        expandedPath = this->CasePath + '/' + tag + '/';
        isExpanded = true;
        wasPathSeparator = false;
      }
      // <etc> in not handled

      if (isExpanded)
      {
        charI = delim + 2;
      }
    }

    while (charI < nChars)
    {
      const char c = pathIn[charI];
      switch (c)
      {
        case '$': // $-variable expansion
        {
          std::string variable;
          while (++charI < nChars && (isalnum(pathIn[charI]) || pathIn[charI] == '_'))
          {
            variable += pathIn[charI];
          }
          if (variable == "FOAM_CASE") // discard path until the variable
          {
            expandedPath = this->CasePath;
            wasPathSeparator = true;
            isExpanded = true;
          }
          else if (variable == "FOAM_CASENAME")
          {
            // FOAM_CASENAME is the final directory name from CasePath
            expandedPath += this->ExtractName(this->CasePath);
            wasPathSeparator = false;
            isExpanded = true;
          }
          else
          {
            std::string value;
            if (vtksys::SystemTools::GetEnv(variable, value))
            {
              expandedPath += value;
            }
            const auto len = expandedPath.length();
            if (len > 0)
            {
              const char c2 = expandedPath[len - 1];
              wasPathSeparator = (c2 == '/' || c2 == '\\');
            }
            else
            {
              wasPathSeparator = false;
            }
          }
        }
        break;
        case '~': // home directory expansion
          // not using vtksys::SystemTools::ConvertToUnixSlashes() for
          // a bit better handling of "~"
          if (wasPathSeparator)
          {
            std::string userName;
            while (++charI < nChars && (pathIn[charI] != '/' && pathIn[charI] != '\\') &&
              pathIn[charI] != '$')
            {
              userName += pathIn[charI];
            }

            std::string homeDir;
            if (userName.empty())
            {
              if (!vtksys::SystemTools::GetEnv("HOME", homeDir) || homeDir.empty())
              {
#if defined(_WIN32) && !defined(__CYGWIN__) || defined(__LIBCATAMOUNT__)
                // No fallback
                homeDir.clear();
#else
                const struct passwd* pwentry = getpwuid(getuid());
                if (pwentry == nullptr)
                {
                  this->ThrowStackTrace("Home directory path not found");
                }
                homeDir = pwentry->pw_dir;
#endif
              }
              expandedPath = homeDir;
            }
            else if (userName == "OpenFOAM")
            {
              // So far only "~/.OpenFOAM" expansion is supported

              if (!vtksys::SystemTools::GetEnv("HOME", homeDir) || homeDir.empty())
              {
#if defined(_WIN32) && !defined(__CYGWIN__) || defined(__LIBCATAMOUNT__)
                // No fallback
                homeDir.clear();
#else
                const struct passwd* pwentry = getpwuid(getuid());
                if (pwentry == nullptr)
                {
                  this->ThrowStackTrace("Home directory path not found");
                }
                homeDir = pwentry->pw_dir;
#endif
              }

              if (homeDir.empty())
              {
                expandedPath = homeDir;
              }
              else
              {
                expandedPath = homeDir + "/.OpenFOAM";
              }
            }
            else
            {
#if defined(_WIN32) && !defined(__CYGWIN__) || defined(__LIBCATAMOUNT__)
              if (!vtksys::SystemTools::GetEnv("HOME", homeDir))
              {
                // No fallback
                homeDir.clear();
              }
              expandedPath = this->ExtractPath(homeDir) + userName;
#else
              const struct passwd* pwentry = getpwnam(userName.c_str());
              if (pwentry == nullptr)
              {
                this->ThrowStackTrace("No home directory for user " + userName);
              }
              expandedPath = pwentry->pw_dir;
#endif
            }
            wasPathSeparator = false;
            isExpanded = true;
            break;
          }
          VTK_FALLTHROUGH;
        default:
          wasPathSeparator = (c == '/' || c == '\\');
          expandedPath += c;
          charI++;
      }
    }
    if (isExpanded || expandedPath[0] == '/' || expandedPath[0] == '\\')
    {
      return expandedPath;
    }
    else
    {
      return defaultPath + expandedPath;
    }
  }

  void IncludeFile(const std::string& includedFileName, const std::string& defaultPath)
  {
    if (this->StackI >= VTK_FOAMFILE_INCLUDE_STACK_SIZE)
    {
      throw this->StackString() << "Exceeded maximum #include recursions of "
                                << VTK_FOAMFILE_INCLUDE_STACK_SIZE;
    }
    // use the default bitwise copy constructor
    this->Stack[this->StackI++] = new vtkFoamFileStack(*this);
    this->Superclass::Reset();

    this->Open(this->ExpandPath(includedFileName, defaultPath));
  }

  // the tokenizer
  // returns true if success, false if encountered EOF
  bool Read(vtkFoamToken& token)
  {
    token.SetStreamOption(this->GetStreamOption());
    const bool use64BitLabels = this->IsLabel64();

    // expanded the outermost loop in nextTokenHead() for performance
    int c;
    while (isspace(c = this->Getc())) // isspace() accepts -1 as EOF
    {
      if (c == '\n')
      {
        ++this->Superclass::LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
        this->Superclass::WasNewline = true;
#endif
      }
    }
    if (c == '/')
    {
      this->PutBack(c);
      c = this->NextTokenHead();
    }
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
    if (c != '#')
    {
      this->Superclass::WasNewline = false;
    }
#endif

    constexpr int MAXLEN = 1024;
    char buf[MAXLEN + 1];
    int charI = 0;
    switch (c)
    {
      case '(':
      case ')':
        // high-priority punctuation token
        token = static_cast<char>(c);
        return true;
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
      case '0':
      case '-':
        // undetermined number token
        do
        {
          buf[charI++] = static_cast<unsigned char>(c);
        } while (isdigit(c = this->Getc()) && charI < MAXLEN);
        if (c != '.' && c != 'e' && c != 'E' && charI < MAXLEN && c != EOF)
        {
          // label token
          buf[charI] = '\0';
          if (use64BitLabels)
          {
            token = static_cast<vtkTypeInt64>(strtoll(buf, nullptr, 10));
          }
          else
          {
            token = static_cast<vtkTypeInt32>(strtol(buf, nullptr, 10));
          }
          this->PutBack(c);
          return true;
        }
        VTK_FALLTHROUGH;
      case '.':
        // scalar token
        if (c == '.' && charI < MAXLEN)
        {
          // read decimal fraction part
          buf[charI++] = static_cast<unsigned char>(c);
          while (isdigit(c = this->Getc()) && charI < MAXLEN)
          {
            buf[charI++] = static_cast<unsigned char>(c);
          }
        }
        if ((c == 'e' || c == 'E') && charI < MAXLEN)
        {
          // read exponent part
          buf[charI++] = static_cast<unsigned char>(c);
          if (((c = this->Getc()) == '+' || c == '-') && charI < MAXLEN)
          {
            buf[charI++] = static_cast<unsigned char>(c);
            c = this->Getc();
          }
          while (isdigit(c) && charI < MAXLEN)
          {
            buf[charI++] = static_cast<unsigned char>(c);
            c = this->Getc();
          }
        }
        if (charI == 1 && buf[0] == '-')
        {
          token = '-';
          this->PutBack(c);
          return true;
        }
        buf[charI] = '\0';
        token = strtod(buf, nullptr);
        this->PutBack(c);
        break;
      case ';':
      case '{':
      case '}':
      case '[':
      case ']':
      case ':':
      case ',':
      case '=':
      case '+':
      case '*':
      case '/':
        // low-priority punctuation token
        token = static_cast<char>(c);
        return true;
      case '"':
      {
        // string token
        bool wasEscape = false;
        while ((c = this->Getc()) != EOF && charI < MAXLEN)
        {
          if (c == '\\' && !wasEscape)
          {
            wasEscape = true;
            continue;
          }
          else if (c == '"' && !wasEscape)
          {
            break;
          }
          else if (c == '\n')
          {
            ++this->Superclass::LineNumber;
            if (!wasEscape)
            {
              this->ThrowStackTrace("Unescaped newline in string constant");
            }
          }
          buf[charI++] = static_cast<unsigned char>(c);
          wasEscape = false;
        }
        buf[charI] = '\0';
        token = buf;
      }
      break;
      case EOF:
        // end of file
        token.SetBad();
        return false;
      case '$':
      {
        vtkFoamToken identifierToken;
        if (!this->Read(identifierToken))
        {
          this->ThrowStackTrace("Unexpected EOF reading identifier");
        }
        if (identifierToken.GetType() != vtkFoamToken::STRING)
        {
          throw this->StackString() << "Expected a word, found " << identifierToken;
        }
        token.SetIdentifier(identifierToken.ToString());
        return true;
      }
      case '#':
      {
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
        // the OpenFOAM #-directives can indeed be placed in the
        // middle of a line
        if (!this->Superclass::WasNewline)
        {
          this->ThrowStackTrace("Encountered #-directive in the middle of a line");
        }
        this->Superclass::WasNewline = false;
#endif
        // read directive
        vtkFoamToken directiveToken;
        if (!this->Read(directiveToken))
        {
          this->ThrowStackTrace("Unexpected EOF reading directive");
        }
        if (directiveToken == "include")
        {
          vtkFoamToken fileNameToken;
          if (!this->Read(fileNameToken))
          {
            this->ThrowStackTrace("Unexpected EOF reading filename");
          }
          this->IncludeFile(fileNameToken.ToString(), this->ExtractPath(this->FileName));
        }
        else if (directiveToken == "sinclude" || directiveToken == "includeIfPresent")
        {
          vtkFoamToken fileNameToken;
          if (!this->Read(fileNameToken))
          {
            this->ThrowStackTrace("Unexpected EOF reading filename");
          }

          // special treatment since the file is allowed to be missing
          const std::string fullName =
            this->ExpandPath(fileNameToken.ToString(), this->ExtractPath(this->FileName));

          FILE* fh = vtksys::SystemTools::Fopen(fullName, "rb");
          if (fh)
          {
            fclose(fh);

            this->IncludeFile(fileNameToken.ToString(), this->ExtractPath(this->FileName));
          }
        }
        else if (directiveToken == "inputMode")
        {
          vtkFoamToken modeToken;
          if (!this->Read(modeToken))
          {
            this->ThrowStackTrace("Unexpected EOF reading inputMode specifier");
          }
          if (modeToken == "merge" || modeToken == "default")
          {
            this->InputMode = INPUT_MODE_MERGE;
          }
          else if (modeToken == "overwrite")
          {
            this->InputMode = INPUT_MODE_OVERWRITE;
          }
          else if (modeToken == "protect")
          {
            // not properly supported - treat like "merge" for now
            // this->InputMode = INPUT_MODE_PROTECT;
            this->InputMode = INPUT_MODE_MERGE;
          }
          else if (modeToken == "warn")
          {
            // not properly supported - treat like "error" for now
            // this->InputMode = INPUT_MODE_WARN;
            this->InputMode = INPUT_MODE_ERROR;
          }
          else if (modeToken == "error")
          {
            this->InputMode = INPUT_MODE_ERROR;
          }
          else
          {
            throw this->StackString() << "Expected one of inputMode specifiers "
                                         "(merge, overwrite, protect, warn, error, default), found "
                                      << modeToken;
          }
        }
        else if (directiveToken == '{')
        {
          // '#{' verbatim/code block. swallow everything until a closing '#}'
          // This hopefully matches the first one...
          while (true)
          {
            c = this->NextTokenHead();
            if (c == EOF)
            {
              this->ThrowStackTrace("Unexpected EOF while skipping over #{ directive");
            }
            else if (c == '#')
            {
              c = this->Getc();
              if (c == '/')
              {
                this->PutBack(c);
              }
              else if (c == '}')
              {
                break;
              }
            }
          }
        }
        else
        {
          throw this->StackString() << "Unsupported directive " << directiveToken;
        }
        return this->Read(token);
      }
      default:
        // parses as a word token, but gives the STRING type for simplicity
        int inBrace = 0;
        do
        {
          if (c == '(')
          {
            inBrace++;
          }
          else if (c == ')' && --inBrace == -1)
          {
            break;
          }
          buf[charI++] = static_cast<unsigned char>(c);
          // valid characters that constitutes a word
          // cf. src/OpenFOAM/primitives/strings/word/wordI.H
        } while ((c = this->Getc()) != EOF && !isspace(c) && c != '"' && c != '/' && c != ';' &&
          c != '{' && c != '}' && charI < MAXLEN);
        buf[charI] = '\0';
        token = buf;
        this->PutBack(c);
    }

    if (c == EOF)
    {
      this->ThrowUnexpectedEOFException();
    }
    if (charI == MAXLEN)
    {
      throw this->StackString() << "Exceeded maximum allowed length of " << MAXLEN;
    }
    return true;
  }

  void Open(const std::string& fileName)
  {
    // reset line number to indicate the beginning of the file when an
    // exception is thrown
    this->Superclass::LineNumber = 0;
    this->Superclass::FileName = fileName;

    if (this->Superclass::File)
    {
      this->ThrowStackTrace("File already opened within this object");
    }

    if ((this->Superclass::File = vtksys::SystemTools::Fopen(this->Superclass::FileName, "rb")) ==
      nullptr)
    {
      this->ThrowStackTrace("Cannot open file for reading");
    }

    unsigned char zMagic[2];
    if (fread(zMagic, 1, 2, this->Superclass::File) == 2 && zMagic[0] == 0x1f && zMagic[1] == 0x8b)
    {
      // gzip-compressed format
      this->Superclass::Z.avail_in = 0;
      this->Superclass::Z.next_in = Z_NULL;
      // + 32 to automatically recognize gzip format
      if (inflateInit2(&this->Superclass::Z, 15 + 32) == Z_OK)
      {
        this->Superclass::IsCompressed = true;
        this->Superclass::Inbuf = new unsigned char[VTK_FOAMFILE_INBUFSIZE];
      }
      else
      {
        fclose(this->Superclass::File);
        this->Superclass::File = nullptr;
        throw this->StackString() << "Can't init zstream "
                                  << (this->Superclass::Z.msg ? this->Superclass::Z.msg : "");
      }
    }
    else
    {
      // uncompressed format
      this->Superclass::IsCompressed = false;
    }
    rewind(this->Superclass::File);

    this->Superclass::ZStatus = Z_OK;
    this->Superclass::Outbuf = new unsigned char[VTK_FOAMFILE_OUTBUFSIZE + 1];
    this->Superclass::BufPtr = this->Superclass::Outbuf + 1;
    this->Superclass::BufEndPtr = this->Superclass::BufPtr;
    this->Superclass::LineNumber = 1;
  }

  void Close()
  {
    while (this->CloseIncludedFile())
      ;
    this->Clear();

    // Reinstate values from reader (eg, GUI)
    auto& streamOpt = static_cast<vtkFoamStreamOption&>(*this);
    streamOpt.SetLabel64(this->Reader->GetUse64BitLabels());
    streamOpt.SetFloat64(this->Reader->GetUse64BitFloats());
  }

  // fread or gzread with buffering handling
  vtkTypeInt64 Read(unsigned char* buf, const vtkTypeInt64 len)
  {
    const size_t buflen = (this->Superclass::BufEndPtr - this->Superclass::BufPtr);

    vtkTypeInt64 readlen;
    if (static_cast<size_t>(len) > buflen)
    {
      memcpy(buf, this->Superclass::BufPtr, buflen);
      this->InflateNext(buf + buflen, len - buflen, &readlen);
      if (readlen >= 0)
      {
        readlen += buflen;
      }
      else
      {
        if (buflen == 0) // return EOF
        {
          readlen = -1;
        }
        else
        {
          readlen = buflen;
        }
      }
      this->Superclass::BufPtr = this->Superclass::BufEndPtr;
    }
    else
    {
      memcpy(buf, this->Superclass::BufPtr, len);
      this->Superclass::BufPtr += len;
      readlen = len;
    }
    for (vtkTypeInt64 i = 0; i < readlen; ++i)
    {
      if (buf[i] == '\n')
      {
        ++this->Superclass::LineNumber;
      }
    }
    return readlen;
  }

  void ReadExpecting(const char expected)
  {
    // skip prepending invalid chars
    // expanded the outermost loop in nextTokenHead() for performance
    int c;
    while (isspace(c = this->Getc())) // isspace() accepts -1 as EOF
    {
      if (c == '\n')
      {
        ++this->Superclass::LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
        this->Superclass::WasNewline = true;
#endif
      }
    }
    if (c == '/')
    {
      this->PutBack(c);
      c = this->NextTokenHead();
    }
    if (c != expected)
    {
      this->ThrowUnexpectedTokenException(expected, c);
    }
  }

  void ReadExpecting(const char* str)
  {
    vtkFoamToken t;
    if (!this->Read(t) || t != str)
    {
      throw this->StackString() << "Expected string \"" << str << "\", found " << t;
    }
  }

  vtkTypeInt64 ReadIntValue();
  template <typename FloatType>
  FloatType ReadFloatValue();
};

int vtkFoamFile::ReadNext()
{
  if (!this->InflateNext(this->Superclass::Outbuf + 1, VTK_FOAMFILE_OUTBUFSIZE))
  {
    return this->CloseIncludedFile() ? this->Getc() : EOF;
  }
  return *this->Superclass::BufPtr++;
}

// specialized for reading an integer value.
// not using the standard strtol() for speed reason.
vtkTypeInt64 vtkFoamFile::ReadIntValue()
{
  // skip prepending invalid chars
  // expanded the outermost loop in nextTokenHead() for performance
  int c;
  while (isspace(c = this->Getc())) // isspace() accepts -1 as EOF
  {
    if (c == '\n')
    {
      ++this->Superclass::LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
      this->Superclass::WasNewline = true;
#endif
    }
  }
  if (c == '/')
  {
    this->PutBack(c);
    c = this->NextTokenHead();
  }

  // leading sign?
  const bool negNum = (c == '-');
  if (negNum || c == '+')
  {
    c = this->Getc();
    if (c == '\n')
    {
      ++this->Superclass::LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
      this->Superclass::WasNewline = true;
#endif
    }
  }

  if (!isdigit(c)) // isdigit() accepts -1 as EOF
  {
    if (c == EOF)
    {
      this->ThrowUnexpectedEOFException();
    }
    else
    {
      this->ThrowUnexpectedNondigitException(c);
    }
  }

  vtkTypeInt64 num = c - '0';
  while (isdigit(c = this->Getc()))
  {
    num = 10 * num + c - '0';
  }

  if (c == EOF)
  {
    this->ThrowUnexpectedEOFException();
  }
  this->PutBack(c);

  return negNum ? -num : num;
}

// extremely simplified high-performing string to floating point
// conversion code based on
// ParaView3/VTK/Utilities/vtksqlite/vtk_sqlite3.c
template <typename FloatType>
FloatType vtkFoamFile::ReadFloatValue()
{
  // skip prepending invalid chars
  // expanded the outermost loop in nextTokenHead() for performance
  int c;
  while (isspace(c = this->Getc())) // isspace() accepts -1 as EOF
  {
    if (c == '\n')
    {
      ++this->Superclass::LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
      this->Superclass::WasNewline = true;
#endif
    }
  }
  if (c == '/')
  {
    this->PutBack(c);
    c = this->NextTokenHead();
  }

  // leading sign?
  const bool negNum = (c == '-');
  if (negNum || c == '+')
  {
    c = this->Getc();
    if (c == '\n')
    {
      ++this->Superclass::LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
      this->Superclass::WasNewline = true;
#endif
    }
  }

  if (!isdigit(c) && c != '.') // Attention: isdigit() accepts EOF
  {
    this->ThrowUnexpectedNondigitException(c);
  }

  double num = 0;

  // read integer part (before '.')
  if (c != '.')
  {
    num = c - '0';
    while (isdigit(c = this->Getc()))
    {
      num = num * 10.0 + (c - '0');
    }
  }

  // read decimal part (after '.')
  if (c == '.')
  {
    double divisor = 1.0;

    while (isdigit(c = this->Getc()))
    {
      num = num * 10.0 + (c - '0');
      divisor *= 10.0;
    }
    num /= divisor;
  }

  // read exponent part
  if (c == 'E' || c == 'e')
  {
    int esign = 1;
    int eval = 0;
    double scale = 1.0;

    c = this->Getc();
    if (c == '-')
    {
      esign = -1;
      c = this->Getc();
    }
    else if (c == '+')
    {
      c = this->Getc();
    }

    while (isdigit(c))
    {
      eval = eval * 10 + (c - '0');
      c = this->Getc();
    }

    // fast exponent multiplication!
    while (eval >= 64)
    {
      scale *= 1.0e+64;
      eval -= 64;
    }
    while (eval >= 16)
    {
      scale *= 1.0e+16;
      eval -= 16;
    }
    while (eval >= 4)
    {
      scale *= 1.0e+4;
      eval -= 4;
    }
    while (eval >= 1)
    {
      scale *= 1.0e+1;
      eval -= 1;
    }

    if (esign < 0)
    {
      num /= scale;
    }
    else
    {
      num *= scale;
    }
  }

  if (c == EOF)
  {
    this->ThrowUnexpectedEOFException();
  }
  this->PutBack(c);

  return static_cast<FloatType>(negNum ? -num : num);
}

void vtkFoamFile::ThrowStackTrace(const std::string& msg)
{
  throw this->StackString() << msg;
}

// hacks to keep exception throwing code out-of-line to make
// putBack() and readExpecting() inline expandable
void vtkFoamFile::ThrowUnexpectedEOFException()
{
  this->ThrowStackTrace("Unexpected EOF");
}

void vtkFoamFile::ThrowUnexpectedNondigitException(int c)
{
  throw this->StackString() << "Expected a number, found a non-digit character "
                            << static_cast<char>(c);
}

void vtkFoamFile::ThrowUnexpectedTokenException(char expected, int c)
{
  vtkFoamError err;
  err << this->StackString() << "Expected punctuation token '" << expected << "', found ";
  if (c == EOF)
  {
    err << "EOF";
  }
  else
  {
    err << static_cast<char>(c);
  }
  throw err;
}

void vtkFoamFile::ThrowDuplicatedPutBackException()
{
  this->ThrowStackTrace("Attempted duplicated putBack()");
}

bool vtkFoamFile::InflateNext(unsigned char* buf, size_t requestSize, vtkTypeInt64* readSize)
{
  if (readSize)
  {
    *readSize = -1; // Set to an error state for early returns
  }
  size_t size;
  if (this->Superclass::IsCompressed)
  {
    if (this->Superclass::ZStatus != Z_OK)
    {
      return false;
    }
    this->Superclass::Z.next_out = buf;
    this->Superclass::Z.avail_out = static_cast<uInt>(requestSize);

    do
    {
      if (this->Superclass::Z.avail_in == 0)
      {
        this->Superclass::Z.next_in = this->Superclass::Inbuf;
        this->Superclass::Z.avail_in = static_cast<uInt>(
          fread(this->Superclass::Inbuf, 1, VTK_FOAMFILE_INBUFSIZE, this->Superclass::File));
        if (ferror(this->Superclass::File))
        {
          this->ThrowStackTrace("failed in fread()");
        }
      }
      this->Superclass::ZStatus = inflate(&this->Superclass::Z, Z_NO_FLUSH);
      if (this->Superclass::ZStatus == Z_STREAM_END
#if VTK_FOAMFILE_OMIT_CRCCHECK
        // the dummy CRC function causes data error when finalizing
        // so we have to proceed even when a data error is detected
        || this->Superclass::ZStatus == Z_DATA_ERROR
#endif
      )
      {
        break;
      }
      if (this->Superclass::ZStatus != Z_OK)
      {
        throw this->StackString() << "Inflation failed: "
                                  << (this->Superclass::Z.msg ? this->Superclass::Z.msg : "");
      }
    } while (this->Superclass::Z.avail_out > 0);

    size = requestSize - this->Superclass::Z.avail_out;
  }
  else
  {
    // not compressed
    size = fread(buf, 1, requestSize, this->Superclass::File);
  }

  if (size <= 0)
  {
    // retain the current location bufPtr_ to the end of the buffer so that
    // getc() returns EOF again when called next time
    return false;
  }
  // size > 0
  // reserve the first byte for getback char
  this->Superclass::BufPtr = this->Superclass::Outbuf + 1;
  this->Superclass::BufEndPtr = this->Superclass::BufPtr + size;
  if (readSize)
  {
    // Cast size_t to int64. Should be OK since requestSize came from OpenFOAM (signed integer)
    *readSize = static_cast<vtkTypeInt64>(size);
  }
  return true;
}

// get next semantically valid character
int vtkFoamFile::NextTokenHead()
{
  for (;;)
  {
    int c;
    while (isspace(c = this->Getc())) // isspace() accepts -1 as EOF
    {
      if (c == '\n')
      {
        ++this->Superclass::LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
        this->Superclass::WasNewline = true;
#endif
      }
    }
    if (c == '/')
    {
      if ((c = this->Getc()) == '/')
      {
        while ((c = this->Getc()) != EOF && c != '\n')
          ;
        if (c == EOF)
        {
          return c;
        }
        ++this->Superclass::LineNumber;
#if VTK_FOAMFILE_RECOGNIZE_LINEHEAD
        this->Superclass::WasNewline = true;
#endif
      }
      else if (c == '*')
      {
        for (;;)
        {
          while ((c = this->Getc()) != EOF && c != '*')
          {
            if (c == '\n')
            {
              ++this->Superclass::LineNumber;
            }
          }
          if (c == EOF)
          {
            return c;
          }
          else if ((c = this->Getc()) == '/')
          {
            break;
          }
          this->PutBack(c);
        }
      }
      else
      {
        this->PutBack(c); // may be an EOF
        return '/';
      }
    }
    else // may be an EOF
    {
      return c;
    }
  }
#if defined(__hpux)
  return EOF; // this line should not be executed; workaround for HP-UXia64-aCC
#endif
}

//------------------------------------------------------------------------------
// class vtkFoamIOobject
// Extends vtkFoamFile with OpenFOAM class/object information
struct vtkFoamIOobject : public vtkFoamFile
{
private:
  typedef vtkFoamFile Superclass;

  std::string objectName_;
  std::string headerClassName_;
  vtkFoamError error_;

  // Inform IO object that lagrangian/positions has extra data (OpenFOAM v1.4 - v2.4)
  const bool LagrangianPositionsExtraData_;

  // Reads OpenFOAM format/class/object information and handles "arch" information
  void ReadHeader();

  // Attempt to open file (or file.gz) and read header
  bool OpenFile(const std::string& file, bool checkGzip = false)
  {
    try
    {
      this->Superclass::Open(file);
      checkGzip = false;
    }
    catch (const vtkFoamError& err)
    {
      if (!checkGzip)
      {
        this->SetError(err);
        return false;
      }
    }

    if (checkGzip)
    {
      try
      {
        this->Superclass::Open(file + ".gz");
      }
      catch (const vtkFoamError& err)
      {
        this->SetError(err);
        return false;
      }
    }

    try
    {
      this->ReadHeader();
    }
    catch (const vtkFoamError& err)
    {
      this->Superclass::Close();
      this->SetError(err);
      return false;
    }
    return true;
  }

public:
  // No generated methods
  vtkFoamIOobject() = delete;
  vtkFoamIOobject(const vtkFoamIOobject&) = delete;
  void operator=(const vtkFoamIOobject&) = delete;

  // Construct for specified case -path
  vtkFoamIOobject(const std::string& casePath, vtkOpenFOAMReader* reader)
    : vtkFoamFile(casePath, reader)
    , LagrangianPositionsExtraData_(static_cast<bool>(!reader->GetPositionsIsIn13Format()))
  {
  }

  ~vtkFoamIOobject() { this->Close(); }

  // Attempt to open file (without gzip fallback) and read FoamFile header
  bool Open(const std::string& file) { return OpenFile(file); }

  // Attempt to open file (with gzip fallback) and read FoamFile header
  bool OpenOrGzip(const std::string& file) { return OpenFile(file, true); }

  void Close()
  {
    this->Superclass::Close();
    this->objectName_.clear();
    this->headerClassName_.clear();
    this->error_.clear();
  }

  const std::string& GetClassName() const noexcept { return this->headerClassName_; }
  const std::string& GetObjectName() const noexcept { return this->objectName_; }
  const vtkFoamError& GetError() const noexcept { return this->error_; }
  void SetError(const vtkFoamError& e) { this->error_ = e; }
  bool HasError() const noexcept { return !this->error_.empty(); }
  bool GetLagrangianPositionsExtraData() const { return this->LagrangianPositionsExtraData_; }
};

//------------------------------------------------------------------------------
// workarounding class for older compilers (gcc-3.3.x and possibly older)
template <typename T>
struct vtkFoamReadValue
{
public:
  static T ReadValue(vtkFoamIOobject& io);
};

template <>
inline vtkTypeInt8 vtkFoamReadValue<vtkTypeInt8>::ReadValue(vtkFoamIOobject& io)
{
  return static_cast<vtkTypeInt8>(io.ReadIntValue());
}

template <>
inline vtkTypeInt32 vtkFoamReadValue<vtkTypeInt32>::ReadValue(vtkFoamIOobject& io)
{
  return static_cast<vtkTypeInt32>(io.ReadIntValue());
}

template <>
inline vtkTypeInt64 vtkFoamReadValue<vtkTypeInt64>::ReadValue(vtkFoamIOobject& io)
{
  return io.ReadIntValue();
}

template <>
inline float vtkFoamReadValue<float>::ReadValue(vtkFoamIOobject& io)
{
  return io.ReadFloatValue<float>();
}

template <>
inline double vtkFoamReadValue<double>::ReadValue(vtkFoamIOobject& io)
{
  return io.ReadFloatValue<double>();
}

//------------------------------------------------------------------------------
// class vtkFoamEntryValue
// a class that represents a value of a dictionary entry that corresponds to
// its keyword. note that an entry can have more than one value.
struct vtkFoamEntryValue : public vtkFoamToken
{
private:
  typedef vtkFoamToken Superclass;

  bool IsUniformEntry;
  bool Managed;
  const vtkFoamEntry* UpperEntryPtr;

  vtkFoamEntryValue() = delete;
  vtkObjectBase* ToVTKObject() { return this->Superclass::VtkObjectPtr; }
  void Clear();
  void ReadList(vtkFoamIOobject& io);

public:
  // reads primitive int/float lists
  template <typename listT, typename primitiveT>
  class listTraits
  {
    listT* Ptr;

  public:
    listTraits()
      : Ptr(listT::New())
    {
    }
    listT* GetPtr() { return this->Ptr; }

    void ReadUniformValues(vtkFoamIOobject& io, const vtkIdType size)
    {
      primitiveT value = vtkFoamReadValue<primitiveT>::ReadValue(io);
      this->Ptr->FillValue(value);
    }

    void ReadAsciiList(vtkFoamIOobject& io, const vtkIdType size)
    {
      for (vtkIdType i = 0; i < size; i++)
      {
        this->Ptr->SetValue(i, vtkFoamReadValue<primitiveT>::ReadValue(io));
      }
    }

    void ReadBinaryList(vtkFoamIOobject& io, const size_t size)
    {
      const size_t nbytes = (size * sizeof(primitiveT));

      typedef typename listT::ValueType ListValueType;
      if (typeid(ListValueType) == typeid(primitiveT))
      {
        io.Read(reinterpret_cast<unsigned char*>(this->Ptr->GetPointer(0)), nbytes);
      }
      else
      {
        vtkDataArray* fileData =
          vtkDataArray::CreateDataArray(vtkTypeTraits<primitiveT>::VTKTypeID());
        fileData->SetNumberOfComponents(this->Ptr->GetNumberOfComponents());
        fileData->SetNumberOfTuples(this->Ptr->GetNumberOfTuples());
        io.Read(reinterpret_cast<unsigned char*>(fileData->GetVoidPointer(0)), nbytes);
        this->Ptr->DeepCopy(fileData);
        fileData->Delete();
      }
    }

    void ReadValue(vtkFoamIOobject&, vtkFoamToken& currToken)
    {
      if (!currToken.Is<primitiveT>())
      {
        throw vtkFoamError() << "Expected an integer or a (, found " << currToken;
      }
      this->Ptr->InsertNextValue(currToken.To<primitiveT>());
    }
  };

  // reads rank 1 lists of types vector, sphericalTensor, symmTensor
  // and tensor. if isPositions is true it reads Cloud type of data as
  // particle positions. cf. (the positions format)
  // src/lagrangian/basic/particle/particleIO.C - writePosition()
  template <typename listT, typename primitiveT, int nComponents, bool isPositions = false>
  class vectorListTraits
  {
    listT* Ptr;

  public:
    vectorListTraits()
      : Ptr(listT::New())
    {
      this->Ptr->SetNumberOfComponents(nComponents);
    }
    listT* GetPtr() { return this->Ptr; }

    void ReadUniformValues(vtkFoamIOobject& io, const vtkIdType size)
    {
      io.ReadExpecting('(');
      primitiveT vectorValue[nComponents];
      for (int j = 0; j < nComponents; j++)
      {
        vectorValue[j] = vtkFoamReadValue<primitiveT>::ReadValue(io);
      }
      for (vtkIdType i = 0; i < size; i++)
      {
        this->Ptr->SetTuple(i, vectorValue);
      }
      io.ReadExpecting(')');
      if (isPositions)
      {
        // skip label celli
        vtkFoamReadValue<int>::ReadValue(io);
      }
    }

    void ReadAsciiList(vtkFoamIOobject& io, const vtkIdType size)
    {
      typedef typename listT::ValueType ListValueType;
      for (vtkIdType i = 0; i < size; i++)
      {
        io.ReadExpecting('(');
        ListValueType* vectorTupleI = this->Ptr->GetPointer(nComponents * i);
        for (int j = 0; j < nComponents; j++)
        {
          vectorTupleI[j] = static_cast<ListValueType>(vtkFoamReadValue<primitiveT>::ReadValue(io));
        }
        io.ReadExpecting(')');
        if (isPositions)
        {
          // skip label celli
          vtkFoamReadValue<vtkTypeInt64>::ReadValue(io);
        }
      }
    }

    void ReadBinaryList(vtkFoamIOobject& io, const size_t size)
    {
      // Should be OK, since size came from OpenFOAM also used (signed integer)
      const vtkTypeInt64 listLen = static_cast<vtkTypeInt64>(size);

      if (isPositions) // lagrangian/positions (class Cloud)
      {
        // xyz (3*scalar) + celli (label)
        // in OpenFOAM 1.4 -> 2.4 also had facei (label) and stepFraction (scalar)

        const unsigned labelSize = (io.IsLabel64() ? 8 : 4);
        const unsigned tupleLength = (sizeof(primitiveT) * nComponents + labelSize +
          (io.GetLagrangianPositionsExtraData() ? (labelSize + sizeof(primitiveT)) : 0));

        // MSVC doesn't support variable-sized stack arrays (JAN-2017)
        // memory management via std::vector
        std::vector<unsigned char> bufferContainer;
        bufferContainer.resize(tupleLength);
        primitiveT* buffer = reinterpret_cast<primitiveT*>(bufferContainer.data());

        for (vtkTypeInt64 i = 0; i < listLen; ++i)
        {
          io.ReadExpecting('(');
          io.Read(reinterpret_cast<unsigned char*>(buffer), tupleLength);
          io.ReadExpecting(')');
          this->Ptr->SetTuple(i, buffer);
        }
      }
      else
      {
        typedef typename listT::ValueType ListValueType;

        // Compiler hint for better unrolling:
        VTK_ASSUME(this->Ptr->GetNumberOfComponents() == nComponents);

        const unsigned tupleLength = (sizeof(primitiveT) * nComponents);
        primitiveT buffer[nComponents];

        for (vtkTypeInt64 i = 0; i < listLen; ++i)
        {
          const int readLength = io.Read(reinterpret_cast<unsigned char*>(buffer), tupleLength);
          if (readLength != tupleLength)
          {
            throw vtkFoamError() << "Failed to read tuple " << i << '/' << listLen << ": Expected "
                                 << tupleLength << " bytes, got " << readLength << " bytes.";
          }
          for (int c = 0; c < nComponents; ++c)
          {
            this->Ptr->SetTypedComponent(i, c, static_cast<ListValueType>(buffer[c]));
          }
        }
      }
    }

    void ReadValue(vtkFoamIOobject& io, vtkFoamToken& currToken)
    {
      if (currToken != '(')
      {
        throw vtkFoamError() << "Expected '(', found " << currToken;
      }
      primitiveT v[nComponents];
      for (int j = 0; j < nComponents; j++)
      {
        v[j] = vtkFoamReadValue<primitiveT>::ReadValue(io);
      }
      this->Ptr->InsertNextTuple(v);
      io.ReadExpecting(')');
    }
  };

  vtkFoamEntryValue(const vtkFoamEntry* upperEntryPtr)
    : IsUniformEntry(false)
    , Managed(true)
    , UpperEntryPtr(upperEntryPtr)
  {
  }
  vtkFoamEntryValue(vtkFoamEntryValue& val, const vtkFoamEntry*);
  ~vtkFoamEntryValue() { this->Clear(); }

  void SetEmptyList()
  {
    this->Clear();
    this->IsUniformEntry = false;
    this->Superclass::Type = EMPTYLIST;
  }
  bool IsUniform() const noexcept { return this->IsUniformEntry; }
  int Read(vtkFoamIOobject& io);
  void ReadDictionary(vtkFoamIOobject& io, const vtkFoamToken& firstKeyword);

  const vtkDataArray& LabelList() const { return *this->Superclass::LabelListPtr; }
  vtkDataArray& LabelList() { return *this->Superclass::LabelListPtr; }

  const vtkFoamLabelListList& LabelListList() const { return *this->Superclass::LabelListListPtr; }

  const vtkFloatArray& ScalarList() const { return *this->Superclass::ScalarListPtr; }
  vtkFloatArray& ScalarList() { return *this->Superclass::ScalarListPtr; }

  const vtkFloatArray& VectorList() const { return *this->Superclass::VectorListPtr; }
  vtkFloatArray& VectorList() { return *this->Superclass::VectorListPtr; }

  const vtkStringArray& StringList() const { return *this->Superclass::StringListPtr; }
  vtkStringArray& StringList() { return *this->Superclass::StringListPtr; }

  const vtkFoamDict& Dictionary() const { return *this->Superclass::DictPtr; }
  vtkFoamDict& Dictionary() { return *this->Superclass::DictPtr; }

  // Release the managed pointer
  void* Ptr()
  {
    this->Managed = false;               // Not managed = do not delete pointer in destructor
    return this->Superclass::AnyPointer; // List pointers are in a single union
  }

  std::string ToString() const
  {
    return this->Superclass::Type == STRING ? this->Superclass::ToString() : std::string();
  }
  float ToFloat() const
  {
    return this->Superclass::IsNumeric() ? this->Superclass::To<float>() : 0.0F;
  }
  double ToDouble() const
  {
    return this->Superclass::IsNumeric() ? this->Superclass::To<double>() : 0.0;
  }
  // TODO is it ok to always use a 64bit int here?
  vtkTypeInt64 ToInt() const
  {
    return this->Superclass::Type == LABEL ? this->Superclass::To<vtkTypeInt64>() : 0;
  }

  // the following two are for an exceptional expression of
  // `LABEL{LABELorSCALAR}' without type prefix (e. g. `2{-0}' in
  // mixedRhoE B.C. in rhopSonicFoam/shockTube)
  void MakeLabelList(const vtkTypeInt64 labelValue, const vtkIdType size)
  {
    assert("Label type not set!" && this->HasLabelType());
    this->Superclass::Type = LABELLIST;
    if (this->IsLabel64())
    {
      vtkTypeInt64Array* array = vtkTypeInt64Array::New();
      array->SetNumberOfValues(size);
      array->FillValue(labelValue);
      this->Superclass::LabelListPtr = array;
    }
    else
    {
      vtkTypeInt32Array* array = vtkTypeInt32Array::New();
      array->SetNumberOfValues(size);
      array->FillValue(static_cast<vtkTypeInt32>(labelValue));
      this->Superclass::LabelListPtr = array;
    }
  }

  void MakeScalarList(const float scalarValue, const vtkIdType size)
  {
    this->Superclass::Type = SCALARLIST;
    this->Superclass::ScalarListPtr = vtkFloatArray::New();
    this->Superclass::ScalarListPtr->SetNumberOfValues(size);
    this->Superclass::ScalarListPtr->FillValue(scalarValue);
  }

  // Read dimensions set (always ASCII). The leading '[' has already been removed before calling.
  // - can be integer or floating point
  // - user-generated files may have only the first five dimensions.
  // Note
  // - may even have "human-readable" values such as [kg m^-1 s^-2] but they are very rare
  //   and we silently skip these
  void ReadDimensionSet(vtkFoamIOobject& io)
  {
    const int nDimensions = 7; // There are 7 base dimensions
    this->MakeScalarList(0.0, nDimensions);
    vtkFloatArray& dims = *(this->Superclass::ScalarListPtr);

    // Read using tokenizer to handle scalar/label, variable lengths, and ignore human-readable
    vtkFoamToken tok;
    char expectEnding = ']';
    bool goodInput = true;

    for (int ndims = 0; ndims < nDimensions && goodInput && expectEnding; ++ndims)
    {
      if (!io.Read(tok))
      {
        goodInput = false;
      }
      else if (tok.IsNumeric())
      {
        dims.SetValue(ndims, tok.ToFloat());
      }
      else if (tok.IsPunctuation())
      {
        if (tok == expectEnding)
        {
          expectEnding = '\0'; // Already got the closing ']'
        }
        else
        {
          goodInput = false;
        }
      }
      else
      {
        // Some unknown token type (eg, encountered human-readable units)
        // - skip until ']'
        while ((goodInput = io.Read(tok)) == true)
        {
          if (tok.IsPunctuation() && (tok == expectEnding))
          {
            expectEnding = '\0'; // Already got the closing ']'
            break;
          }
        }
        break;
      }
    }

    if (!goodInput)
    {
      io.ThrowStackTrace("Unexpected input while parsing dimensions array");
    }
    else if (expectEnding)
    {
      io.ReadExpecting(expectEnding);
    }
  }

  template <vtkFoamToken::tokenType listType, typename traitsT>
  void ReadNonuniformList(vtkFoamIOobject& io);

  // reads a list of labelLists. requires size prefix of the listList
  // to be present. size of each sublist must also be present in the
  // stream if the format is binary.
  void ReadLabelListList(vtkFoamIOobject& io)
  {
    // NOTE:
    // when OpenFOAM writes a "faceCompactList" it automatically switches to ASCII
    // if it detects that the offsets will overflow 32bits.
    //
    // We risk the same overflow potential when constructing a compact labelListList.
    // Thus assume the worst and use 64bit sizing when reading ASCII.

    const bool use64BitLabels = (io.IsLabel64() || io.IsAsciiFormat());

    vtkFoamToken currToken;
    currToken.SetStreamOption(io);
    if (!io.Read(currToken))
    {
      throw vtkFoamError() << "Unexpected EOF";
    }
    if (currToken.GetType() == vtkFoamToken::LABEL)
    {
      const vtkTypeInt64 listLen = currToken.To<vtkTypeInt64>();
      if (listLen < 0)
      {
        throw vtkFoamError() << "Illegal negative list length: " << listLen;
      }
      if (use64BitLabels)
      {
        this->LabelListListPtr = new vtkFoamLabelListList64;
      }
      else
      {
        this->LabelListListPtr = new vtkFoamLabelListList32;
      }
      // Initial guess for list length
      this->LabelListListPtr->ResizeExact(listLen, 4 * listLen);

      this->Superclass::Type = LABELLISTLIST;
      io.ReadExpecting('(');
      vtkIdType nTotalElems = 0;
      for (vtkTypeInt64 idx = 0; idx < listLen; ++idx)
      {
        if (!io.Read(currToken))
        {
          throw vtkFoamError() << "Unexpected EOF";
        }
        if (currToken.GetType() == vtkFoamToken::LABEL)
        {
          const vtkTypeInt64 sublistLen = currToken.To<vtkTypeInt64>();
          if (sublistLen < 0)
          {
            throw vtkFoamError() << "Illegal negative list length: " << sublistLen;
          }

          // LabelListListPtr->SetOffset(idx, nTotalElems);
          void* sublist = this->LabelListListPtr->WritePointer(idx, nTotalElems, sublistLen);

          if (io.IsAsciiFormat())
          {
            io.ReadExpecting('(');
            for (vtkTypeInt64 subIdx = 0; subIdx < sublistLen; ++subIdx)
            {
              vtkTypeInt64 value(vtkFoamReadValue<vtkTypeInt64>::ReadValue(io));
              this->LabelListListPtr->SetValue(idx, subIdx, value);
            }
            io.ReadExpecting(')');
          }
          else if (sublistLen > 0)
          {
            // Non-empty (binary) list - only read parentheses only when size > 0
            const size_t nbytes =
              static_cast<size_t>(sublistLen * this->LabelListListPtr->GetLabelSize());

            io.ReadExpecting('(');
            io.Read(reinterpret_cast<unsigned char*>(sublist), nbytes);
            io.ReadExpecting(')');
          }
          nTotalElems += sublistLen;
        }
        else if (currToken == '(')
        {
          this->Superclass::LabelListListPtr->SetOffset(idx, nTotalElems);
          while (io.Read(currToken) && currToken != ')')
          {
            if (currToken.GetType() != vtkFoamToken::LABEL)
            {
              throw vtkFoamError() << "Expected an integer, found " << currToken;
            }
            this->Superclass::LabelListListPtr->InsertValue(nTotalElems++, currToken.To<int>());
            ++nTotalElems;
          }
        }
        else
        {
          throw vtkFoamError() << "Expected integer or '(', found " << currToken;
        }
      }

      // Set the final offset
      this->Superclass::LabelListListPtr->SetOffset(listLen, nTotalElems);

      // Shrink to the actually used size
      this->Superclass::LabelListListPtr->ResizeData(nTotalElems);
      io.ReadExpecting(')');
    }
    else
    {
      throw vtkFoamError() << "Expected integer, found " << currToken;
    }
  }

  // Read compact labelListList which has offsets and data
  void ReadCompactLabelListList(vtkFoamIOobject& io)
  {
    if (io.IsAsciiFormat())
    {
      this->ReadLabelListList(io);
      return;
    }

    this->SetStreamOption(io);
    const bool use64BitLabels = io.IsLabel64();

    if (use64BitLabels)
    {
      this->LabelListListPtr = new vtkFoamLabelListList64;
    }
    else
    {
      this->LabelListListPtr = new vtkFoamLabelListList32;
    }
    this->Superclass::Type = LABELLISTLIST;
    for (int arrayI = 0; arrayI < 2; arrayI++)
    {
      vtkFoamToken currToken;
      currToken.SetStreamOption(io);
      if (!io.Read(currToken))
      {
        throw vtkFoamError() << "Unexpected EOF";
      }
      if (currToken.GetType() == vtkFoamToken::LABEL)
      {
        vtkTypeInt64 listLen = currToken.To<vtkTypeInt64>();
        if (listLen < 0)
        {
          throw vtkFoamError() << "Illegal negative list length: " << listLen;
        }

        vtkDataArray* array = (arrayI == 0 ? this->Superclass::LabelListListPtr->GetOffsetsArray()
                                           : this->Superclass::LabelListListPtr->GetDataArray());
        array->SetNumberOfValues(static_cast<vtkIdType>(listLen));

        if (listLen > 0)
        {
          // Non-empty (binary) list - only read parentheses only when size > 0

          io.ReadExpecting('('); // Begin list
          io.Read(reinterpret_cast<unsigned char*>(array->GetVoidPointer(0)),
            static_cast<vtkTypeInt64>(listLen * array->GetDataTypeSize()));
          io.ReadExpecting(')'); // End list
        }
      }
      else
      {
        throw vtkFoamError() << "Expected integer, found " << currToken;
      }
    }
  }

  bool ReadField(vtkFoamIOobject& io)
  {
    this->SetStreamOption(io);

    try
    {
      // lagrangian labels (cf. gnemdFoam/nanoNozzle)
      if (io.GetClassName() == "labelField")
      {
        if (io.IsLabel64())
        {
          this->ReadNonuniformList<LABELLIST, listTraits<vtkTypeInt64Array, vtkTypeInt64>>(io);
        }
        else
        {
          this->ReadNonuniformList<LABELLIST, listTraits<vtkTypeInt32Array, vtkTypeInt32>>(io);
        }
      }
      // lagrangian scalars

      else if (io.GetClassName() == "scalarField")
      {
        if (io.IsFloat64())
        {
          this->ReadNonuniformList<SCALARLIST, listTraits<vtkFloatArray, double>>(io);
        }
        else
        {
          this->ReadNonuniformList<SCALARLIST, listTraits<vtkFloatArray, float>>(io);
        }
      }
      else if (io.GetClassName() == "sphericalTensorField")
      {
        if (io.IsFloat64())
        {
          this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, double, 1, false>>(
            io);
        }
        else
        {
          this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, float, 1, false>>(
            io);
        }
      }
      // polyMesh/points, lagrangian vectors

      else if (io.GetClassName() == "vectorField")
      {
        if (io.IsFloat64())
        {
          this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, double, 3, false>>(
            io);
        }
        else
        {
          this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, float, 3, false>>(
            io);
        }
      }
      else if (io.GetClassName() == "symmTensorField")
      {
        if (io.IsFloat64())
        {
          this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, double, 6, false>>(
            io);
        }
        else
        {
          this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, float, 6, false>>(
            io);
        }
      }
      else if (io.GetClassName() == "tensorField")
      {
        if (io.IsFloat64())
        {
          this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, double, 9, false>>(
            io);
        }
        else
        {
          this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, float, 9, false>>(
            io);
        }
      }
      else
      {
        throw vtkFoamError() << "Unsupported field type " << io.GetClassName();
      }
    }
    catch (const vtkFoamError& err)
    {
      io.SetError(err);
      return false;
    }
    return true;
  }
};

// generic reader for nonuniform lists. requires size prefix of the
// list to be present in the stream if the format is binary.
template <vtkFoamToken::tokenType listType, typename traitsT>
void vtkFoamEntryValue::ReadNonuniformList(vtkFoamIOobject& io)
{
  this->SetStreamOption(io);

  vtkFoamToken currToken;
  currToken.SetStreamOption(io);
  if (!io.Read(currToken))
  {
    throw vtkFoamError() << "Unexpected EOF";
  }
  traitsT list;
  this->Superclass::Type = listType;
  this->Superclass::VtkObjectPtr = list.GetPtr();
  if (currToken.Is<vtkTypeInt64>())
  {
    const vtkTypeInt64 size = currToken.To<vtkTypeInt64>();
    if (size < 0)
    {
      throw vtkFoamError() << "List size must not be negative: size = " << size;
    }
    list.GetPtr()->SetNumberOfTuples(size);
    if (io.IsAsciiFormat())
    {
      if (!io.Read(currToken))
      {
        throw vtkFoamError() << "Unexpected EOF";
      }
      // some objects have lists with only one element enclosed by {}
      // e. g. simpleFoam/pitzDaily3Blocks/constant/polyMesh/faceZones
      if (currToken == '{')
      {
        list.ReadUniformValues(io, size);
        io.ReadExpecting('}');
        return;
      }
      else if (currToken != '(')
      {
        throw vtkFoamError() << "Expected '(', found " << currToken;
      }
      list.ReadAsciiList(io, size);
      io.ReadExpecting(')');
    }
    else
    {
      if (size > 0)
      {
        // Non-empty (binary) list - only read parentheses only when size > 0
        io.ReadExpecting('(');
        list.ReadBinaryList(io, size);
        io.ReadExpecting(')');
      }
    }
  }
  else if (currToken == '(')
  {
    while (io.Read(currToken) && currToken != ')')
    {
      list.ReadValue(io, currToken);
    }
    list.GetPtr()->Squeeze();
  }
  else
  {
    throw vtkFoamError() << "Expected integer or '(', found " << currToken;
  }
}

//------------------------------------------------------------------------------
// class vtkFoamEntry
// a class that represents an entry of a dictionary. note that an
// entry can have more than one value.
struct vtkFoamEntry : public vtkFoamPtrList<vtkFoamEntryValue>
{
private:
  typedef vtkFoamPtrList<vtkFoamEntryValue> Superclass;
  std::string Keyword;
  vtkFoamDict* UpperDictPtr;

  vtkFoamEntry() = delete;

public:
  vtkFoamEntry(vtkFoamDict* upperDictPtr)
    : UpperDictPtr(upperDictPtr)
  {
  }
  vtkFoamEntry(const vtkFoamEntry& entry, vtkFoamDict* upperDictPtr)
    : Superclass(entry.size())
    , Keyword(entry.GetKeyword())
    , UpperDictPtr(upperDictPtr)
  {
    for (size_t i = 0; i < entry.size(); ++i)
    {
      (*this)[i] = new vtkFoamEntryValue(*entry[i], this);
    }
  }

  ~vtkFoamEntry() = default;

  void Clear() { this->Superclass::clear(); }

  const std::string& GetKeyword() const { return this->Keyword; }
  void SetKeyword(const std::string& keyword) { this->Keyword = keyword; }

  const vtkFoamEntryValue& FirstValue() const { return *this->Superclass::operator[](0); }
  vtkFoamEntryValue& FirstValue() { return *this->Superclass::operator[](0); }

  const vtkDataArray& LabelList() const { return this->FirstValue().LabelList(); }
  vtkDataArray& LabelList() { return this->FirstValue().LabelList(); }

  const vtkFoamLabelListList& LabelListList() const { return this->FirstValue().LabelListList(); }

  const vtkFloatArray& ScalarList() const { return this->FirstValue().ScalarList(); }
  vtkFloatArray& ScalarList() { return this->FirstValue().ScalarList(); }

  const vtkFloatArray& VectorList() const { return this->FirstValue().VectorList(); }
  vtkFloatArray& VectorList() { return this->FirstValue().VectorList(); }

  const vtkFoamDict& Dictionary() const { return this->FirstValue().Dictionary(); }
  vtkFoamDict& Dictionary() { return this->FirstValue().Dictionary(); }

  void* Ptr() { return this->FirstValue().Ptr(); }
  const vtkFoamDict* GetUpperDictPtr() const { return this->UpperDictPtr; }

  std::string ToString() const
  {
    return this->empty() ? std::string() : this->FirstValue().ToString();
  }
  float ToFloat() const { return this->empty() ? 0.0F : this->FirstValue().ToFloat(); }
  double ToDouble() const { return this->empty() ? 0.0 : this->FirstValue().ToDouble(); }
  vtkTypeInt64 ToInt() const { return this->empty() ? 0 : this->FirstValue().ToInt(); }

  void ReadDictionary(vtkFoamIOobject& io)
  {
    this->Superclass::push_back(new vtkFoamEntryValue(this));
    this->Superclass::back()->ReadDictionary(io, vtkFoamToken());
  }

  // read values of an entry
  void Read(vtkFoamIOobject& io);
};

//------------------------------------------------------------------------------
// class vtkFoamDict
// a class that holds a FoamFile data structure
struct vtkFoamDict : public std::vector<vtkFoamEntry*>
{
private:
  typedef std::vector<vtkFoamEntry*> Superclass;

  vtkFoamToken Token;
  const vtkFoamDict* UpperDictPtr;

  vtkFoamDict(const vtkFoamDict&) = delete;

public:
  explicit vtkFoamDict(const vtkFoamDict* upperDictPtr = nullptr)
    : UpperDictPtr(upperDictPtr)
  {
  }
  vtkFoamDict(const vtkFoamDict& dict, const vtkFoamDict* upperDictPtr)
    : Superclass(dict.size())
    , UpperDictPtr(upperDictPtr)
  {
    if (dict.GetType() == vtkFoamToken::DICTIONARY)
    {
      for (size_t i = 0; i < dict.size(); ++i)
      {
        (*this)[i] = new vtkFoamEntry(*dict[i], this);
      }
    }
    else
    {
      Superclass::assign(dict.size(), nullptr);
    }
  }

  ~vtkFoamDict()
  {
    if (this->Token.GetType() == vtkFoamToken::UNDEFINED)
    {
      for (auto* ptr : *this)
      {
        delete ptr;
      }
    }
  }

  // Remove top element, deleting its pointer
  void remove_back()
  {
    if (!Superclass::empty())
    {
      delete Superclass::back();
      Superclass::pop_back();
    }
  }

  void SetStreamOption(const vtkFoamStreamOption& opt) { this->Token.SetStreamOption(opt); }
  bool IsLabel64() const { return this->Token.IsLabel64(); } // convenience

  vtkFoamToken::tokenType GetType() const
  {
    return (this->Token.GetType() == vtkFoamToken::UNDEFINED ? vtkFoamToken::DICTIONARY
                                                             : this->Token.GetType());
  }

  const vtkFoamToken& GetToken() const { return this->Token; }
  const vtkFoamDict* GetUpperDictPtr() const { return this->UpperDictPtr; }

  // Return list of keywords - table of contents
  std::vector<std::string> Toc() const
  {
    std::vector<std::string> list;
    list.reserve(this->size());
    for (const vtkFoamEntry* eptr : *this)
    {
      const std::string& key = eptr->GetKeyword();
      if (!key.empty()) // should not really happen anyhow
      {
        list.push_back(key);
      }
    }
    return list;
  }

  vtkFoamEntry* Lookup(const std::string& keyword, bool regex = false) const
  {
    if (this->Token.GetType() == vtkFoamToken::UNDEFINED)
    {
      int lastMatch = -1;
      for (size_t i = 0; i < this->Superclass::size(); i++)
      {
        const std::string& key = this->operator[](i)->GetKeyword();
        vtksys::RegularExpression rex;
        if (key == keyword) // found
        {
          return this->operator[](i);
        }
        else if (regex && rex.compile(key) && rex.find(keyword) && rex.start(0) == 0 &&
          rex.end(0) == keyword.size())
        {
          // regular expression matches full keyword
          lastMatch = static_cast<int>(i);
        }
      }
      if (lastMatch >= 0)
      {
        return this->operator[](lastMatch);
      }
    }

    // Not found
    return nullptr;
  }

  // reads a FoamFile or a subdictionary. if the stream to be read is
  // a subdictionary the preceding '{' is assumed to have already been
  // thrown away.
  bool Read(vtkFoamIOobject& io, const bool isSubDictionary = false,
    const vtkFoamToken& firstToken = vtkFoamToken())
  {
    vtkFoamToken currToken;
    currToken.SetStreamOption(io);
    try
    {
      if (firstToken.GetType() == vtkFoamToken::UNDEFINED)
      {
        // read the first token
        if (!io.Read(currToken))
        {
          throw vtkFoamError() << "Unexpected EOF";
        }

        if (isSubDictionary)
        {
          // the following if clause is for an exceptional expression
          // of `LABEL{LABELorSCALAR}' without type prefix
          // (e. g. `2{-0}' in mixedRhoE B.C. in
          // rhopSonicFoam/shockTube)
          if (currToken.GetType() == vtkFoamToken::LABEL ||
            currToken.GetType() == vtkFoamToken::SCALAR)
          {
            this->Token = currToken;
            io.ReadExpecting('}');
            return true;
          }
          // return as empty dictionary

          else if (currToken == '}')
          {
            return true;
          }
        }
        else
        {
          // list of dictionaries is read as a usual dictionary
          // polyMesh/boundary, point/face/cell-Zones
          if (currToken.GetType() == vtkFoamToken::LABEL)
          {
            io.ReadExpecting('(');
            if (currToken.To<vtkTypeInt64>() > 0)
            {
              if (!io.Read(currToken))
              {
                throw vtkFoamError() << "Unexpected EOF";
              }
              // continue to read as a usual dictionary
            }
            else // return as empty dictionary

            {
              io.ReadExpecting(')');
              return true;
            }
          }
          // some boundary files does not have the number of boundary
          // patches (e.g. settlingFoam/tank3D). in this case we need to
          // explicitly read the file as a dictionary.

          else if (currToken == '(' && io.GetClassName() == "polyBoundaryMesh") // polyMesh/boundary

          {
            if (!io.Read(currToken)) // read the first keyword

            {
              throw vtkFoamError() << "Unexpected EOF";
            }
            if (currToken == ')') // return as empty dictionary

            {
              return true;
            }
          }
        }
      }
      // if firstToken is given as string read the following stream as
      // subdictionary

      else if (firstToken.GetType() == vtkFoamToken::STRING)
      {
        this->Superclass::push_back(new vtkFoamEntry(this));
        this->Superclass::back()->SetKeyword(firstToken.ToString());
        this->Superclass::back()->ReadDictionary(io);
        if (!io.Read(currToken) || currToken == '}' || currToken == ')')
        {
          return true;
        }
      }
      else // quite likely an identifier

      {
        currToken = firstToken;
      }

      if (currToken == ';' || currToken.GetType() == vtkFoamToken::STRING ||
        currToken.GetType() == vtkFoamToken::IDENTIFIER)
      {
        // general dictionary
        do
        {
          if (currToken.GetType() == vtkFoamToken::STRING)
          {
            vtkFoamEntry* previousEntry = this->Lookup(currToken.ToString());
            if (previousEntry != nullptr)
            {
              if (io.GetInputMode() == vtkFoamFile::INPUT_MODE_MERGE)
              {
                if (previousEntry->FirstValue().GetType() == vtkFoamToken::DICTIONARY)
                {
                  io.ReadExpecting('{');
                  previousEntry->FirstValue().Dictionary().Read(io, true);
                }
                else
                {
                  previousEntry->Clear();
                  previousEntry->Read(io);
                }
              }
              else if (io.GetInputMode() == vtkFoamFile::INPUT_MODE_OVERWRITE)
              {
                previousEntry->Clear();
                previousEntry->Read(io);
              }
              else // INPUT_MODE_ERROR
              {
                throw vtkFoamError()
                  << "Found duplicated entries with keyword " << currToken.ToString();
              }
            }
            else
            {
              this->Superclass::push_back(new vtkFoamEntry(this));
              this->Superclass::back()->SetKeyword(currToken.ToString());
              this->Superclass::back()->Read(io);
            }

            if (currToken == "FoamFile")
            {
              // Drop the FoamFile header subdictionary entry
              this->remove_back();
            }
          }
          else if (currToken.GetType() == vtkFoamToken::IDENTIFIER)
          {
            // substitute identifier
            const std::string identifier(currToken.ToIdentifier());

            for (const vtkFoamDict* uDictPtr = this;;)
            {
              const vtkFoamEntry* identifiedEntry = uDictPtr->Lookup(identifier);

              if (identifiedEntry != nullptr)
              {
                if (identifiedEntry->FirstValue().GetType() != vtkFoamToken::DICTIONARY)
                {
                  throw vtkFoamError()
                    << "Expected dictionary for substituting entry " << identifier;
                }
                const vtkFoamDict& identifiedDict = identifiedEntry->FirstValue().Dictionary();
                for (size_t entryI = 0; entryI < identifiedDict.size(); entryI++)
                {
                  // I think #inputMode handling should be done here
                  // as well, but the genuine FoamFile parser for OF
                  // 1.5 does not seem to be doing it.
                  this->Superclass::push_back(new vtkFoamEntry(*identifiedDict[entryI], this));
                }
                break;
              }
              else
              {
                uDictPtr = uDictPtr->GetUpperDictPtr();
                if (uDictPtr == nullptr)
                {
                  throw vtkFoamError() << "Substituting entry " << identifier << " not found";
                }
              }
            }
          }
          // skip empty entry only with ';'
        } while (io.Read(currToken) &&
          (currToken.GetType() == vtkFoamToken::STRING ||
            currToken.GetType() == vtkFoamToken::IDENTIFIER || currToken == ';'));

        if (currToken.GetType() == vtkFoamToken::TOKEN_ERROR || currToken == '}' ||
          currToken == ')')
        {
          return true;
        }
        throw vtkFoamError() << "Expected keyword, closing brace, ';' or EOF, found " << currToken;
      }
      throw vtkFoamError() << "Expected keyword or identifier, found " << currToken;
    }
    catch (const vtkFoamError& err)
    {
      if (isSubDictionary)
      {
        throw;
      }
      else
      {
        io.SetError(err);
        return false;
      }
    }
  }
};

void vtkFoamIOobject::ReadHeader()
{
  this->Superclass::ReadExpecting("FoamFile");
  this->Superclass::ReadExpecting('{');

  vtkFoamDict headerDict;
  headerDict.SetStreamOption(this->GetStreamOption());
  headerDict.Read(*this, true, vtkFoamToken()); // Throw exception in case of error

  const vtkFoamEntry* eptr;

  if ((eptr = headerDict.Lookup("format")) == nullptr)
  {
    throw vtkFoamError() << "No 'format' (ascii|binary) in FoamFile header";
  }
  this->SetUseBinaryFormat("binary" == eptr->ToString()); // case sensitive

  // Newer files have 'arch' entry with "label=(32|64) scalar=(32|64)"
  // If this entry does not exist, or is incomplete, use the fallback values
  // that come from the reader (defined in constructor and Close)
  if ((eptr = headerDict.Lookup("arch")) != nullptr)
  {
    const std::string archValue(eptr->ToString());

    // Match "label=(32|64)"
    {
      auto pos = archValue.find("label=");
      if (pos != std::string::npos)
      {
        pos += 6; // Skip past "label="
        if (archValue.compare(pos, 2, "32") == 0)
        {
          this->SetLabel64(false);
        }
        else if (archValue.compare(pos, 2, "64") == 0)
        {
          this->SetLabel64(true);
        }
      }
    }

    // Match "scalar=(32|64)"
    {
      auto pos = archValue.find("scalar=");
      if (pos != std::string::npos)
      {
        pos += 7; // Skip past "scalar="
        if (archValue.compare(pos, 2, "32") == 0)
        {
          this->SetFloat64(false);
        }
        else if (archValue.compare(pos, 2, "64") == 0)
        {
          this->SetFloat64(true);
        }
      }
    }
  }

  if ((eptr = headerDict.Lookup("class")) == nullptr)
  {
    throw vtkFoamError() << "No 'class' in FoamFile header";
  }
  this->headerClassName_ = eptr->ToString();

  if ((eptr = headerDict.Lookup("object")) == nullptr)
  {
    throw vtkFoamError() << "No 'object' in FoamFile header";
  }
  this->objectName_ = eptr->ToString();
}

vtkFoamEntryValue::vtkFoamEntryValue(vtkFoamEntryValue& value, const vtkFoamEntry* upperEntryPtr)
  : vtkFoamToken(value)
  , IsUniformEntry(value.IsUniform())
  , Managed(true)
  , UpperEntryPtr(upperEntryPtr)
{
  switch (this->Superclass::Type)
  {
    case VECTORLIST:
    {
      vtkFloatArray* fa = vtkFloatArray::SafeDownCast(value.ToVTKObject());
      if (fa->GetNumberOfComponents() == 6)
      {
        // create deepcopies for vtkObjects to avoid duplicated mainpulation
        vtkFloatArray* newfa = vtkFloatArray::New();
        newfa->DeepCopy(fa);
        this->Superclass::VtkObjectPtr = newfa;
      }
      else
      {
        this->Superclass::VtkObjectPtr = value.ToVTKObject();
        this->Superclass::VtkObjectPtr->Register(nullptr);
      }
    }
    break;
    case LABELLIST:
    case SCALARLIST:
    case STRINGLIST:
      this->Superclass::VtkObjectPtr = value.ToVTKObject();
      this->Superclass::VtkObjectPtr->Register(nullptr);
      break;
    case LABELLISTLIST:
      if (value.LabelListListPtr->IsLabel64())
      {
        this->LabelListListPtr = new vtkFoamLabelListList64(*value.LabelListListPtr);
      }
      else
      {
        this->LabelListListPtr = new vtkFoamLabelListList32(*value.LabelListListPtr);
      }
      break;
    case ENTRYVALUELIST:
    {
      const size_t nValues = value.EntryValuePtrs->size();
      this->EntryValuePtrs = new vtkFoamPtrList<vtkFoamEntryValue>(nValues);
      for (size_t valueI = 0; valueI < nValues; valueI++)
      {
        this->EntryValuePtrs->operator[](valueI) =
          new vtkFoamEntryValue(*value.EntryValuePtrs->operator[](valueI), upperEntryPtr);
      }
    }
    break;
    case DICTIONARY:
      // UpperEntryPtr is null when called from vtkFoamDict constructor
      if (this->UpperEntryPtr != nullptr)
      {
        this->DictPtr = new vtkFoamDict(*value.DictPtr, this->UpperEntryPtr->GetUpperDictPtr());
        this->DictPtr->SetStreamOption(value.GetStreamOption());
      }
      else
      {
        this->DictPtr = nullptr;
      }
      break;
    case BOOLLIST:
      break;
    case EMPTYLIST:
      break;
    case UNDEFINED:
    case PUNCTUATION:
    case LABEL:
    case SCALAR:
    case STRING:
    case IDENTIFIER:
    case TOKEN_ERROR:
    default:
      break;
  }
}

void vtkFoamEntryValue::Clear()
{
  if (this->Managed)
  {
    switch (this->Superclass::Type)
    {
      case LABELLIST:
      case SCALARLIST:
      case VECTORLIST:
      case STRINGLIST:
        this->VtkObjectPtr->Delete();
        break;
      case LABELLISTLIST:
        delete this->LabelListListPtr;
        break;
      case ENTRYVALUELIST:
        delete this->EntryValuePtrs;
        break;
      case DICTIONARY:
        delete this->DictPtr;
        break;
      case UNDEFINED:
      case PUNCTUATION:
      case LABEL:
      case SCALAR:
      case STRING:
      case IDENTIFIER:
      case TOKEN_ERROR:
      case BOOLLIST:
      case EMPTYLIST:
      default:
        break;
    }
  }
}

// general-purpose list reader - guess the type of the list and read
// it. only supports ascii format and assumes the preceding '(' has
// already been thrown away.  the reader supports nested list with
// variable lengths (e. g. `((token token) (token token token)).'
// also supports compound of tokens and lists (e. g. `((token token)
// token)') only if a list comes as the first value.
void vtkFoamEntryValue::ReadList(vtkFoamIOobject& io)
{
  this->SetStreamOption(io);

  vtkFoamToken currToken;
  currToken.SetStreamOption(io);
  io.Read(currToken);

  // initial guess of the list type
  if (currToken.GetType() == this->Superclass::LABEL)
  {
    // if the first token is of type LABEL it might be either an element of
    // a labelList or the size of a sublist so proceed to the next token
    vtkFoamToken nextToken;
    nextToken.SetStreamOption(io);
    if (!io.Read(nextToken))
    {
      throw vtkFoamError() << "Unexpected EOF";
    }
    if (nextToken.GetType() == this->Superclass::LABEL)
    {
      if (this->IsLabel64())
      {
        vtkTypeInt64Array* array = vtkTypeInt64Array::New();
        array->InsertNextValue(currToken.To<vtkTypeInt64>());
        array->InsertNextValue(nextToken.To<vtkTypeInt64>());
        this->Superclass::LabelListPtr = array;
      }
      else
      {
        vtkTypeInt32Array* array = vtkTypeInt32Array::New();
        array->InsertNextValue(currToken.To<vtkTypeInt32>());
        array->InsertNextValue(nextToken.To<vtkTypeInt32>());
        this->Superclass::LabelListPtr = array;
      }
      this->Superclass::Type = LABELLIST;
    }
    else if (nextToken.GetType() == this->Superclass::SCALAR)
    {
      this->Superclass::ScalarListPtr = vtkFloatArray::New();
      this->Superclass::ScalarListPtr->InsertNextValue(currToken.To<float>());
      this->Superclass::ScalarListPtr->InsertNextValue(nextToken.To<float>());
      this->Superclass::Type = SCALARLIST;
    }
    else if (nextToken == '(') // list of list: read recursively
    {
      this->Superclass::EntryValuePtrs = new vtkFoamPtrList<vtkFoamEntryValue>;
      this->Superclass::EntryValuePtrs->push_back(new vtkFoamEntryValue(this->UpperEntryPtr));
      this->Superclass::EntryValuePtrs->back()->SetStreamOption(*this);
      this->Superclass::EntryValuePtrs->back()->ReadList(io);
      this->Superclass::Type = ENTRYVALUELIST;
    }
    else if (nextToken == ')') // list with only one label element
    {
      if (this->IsLabel64())
      {
        vtkTypeInt64Array* array = vtkTypeInt64Array::New();
        array->SetNumberOfValues(1);
        array->SetValue(0, currToken.To<vtkTypeInt64>());
        this->Superclass::LabelListPtr = array;
      }
      else
      {
        vtkTypeInt32Array* array = vtkTypeInt32Array::New();
        array->SetNumberOfValues(1);
        array->SetValue(0, currToken.To<vtkTypeInt32>());
        this->Superclass::LabelListPtr = array;
      }
      this->Superclass::Type = LABELLIST;
      return;
    }
    else
    {
      throw vtkFoamError() << "Expected number, '(' or ')', found " << nextToken;
    }
  }
  else if (currToken.GetType() == this->Superclass::SCALAR)
  {
    this->Superclass::ScalarListPtr = vtkFloatArray::New();
    this->Superclass::ScalarListPtr->InsertNextValue(currToken.To<float>());
    this->Superclass::Type = SCALARLIST;
  }
  // if the first word is a string we have to read another token to determine
  // if the first word is a keyword for the following dictionary
  else if (currToken.GetType() == this->Superclass::STRING)
  {
    vtkFoamToken nextToken;
    nextToken.SetStreamOption(io);
    if (!io.Read(nextToken))
    {
      throw vtkFoamError() << "Unexpected EOF";
    }
    if (nextToken.GetType() == this->Superclass::STRING) // list of strings
    {
      this->Superclass::StringListPtr = vtkStringArray::New();
      this->Superclass::StringListPtr->InsertNextValue(currToken.ToString());
      this->Superclass::StringListPtr->InsertNextValue(nextToken.ToString());
      this->Superclass::Type = STRINGLIST;
    }
    // dictionary with the already read stringToken as the first keyword
    else if (nextToken == '{')
    {
      if (currToken.ToString().empty())
      {
        throw "Empty string is invalid as a keyword for dictionary entry";
      }
      this->ReadDictionary(io, currToken);
      // the dictionary read as list has the entry terminator ';' so
      // we have to skip it
      return;
    }
    else if (nextToken == ')') // list with only one string element
    {
      this->Superclass::StringListPtr = vtkStringArray::New();
      this->Superclass::StringListPtr->SetNumberOfValues(1);
      this->Superclass::StringListPtr->SetValue(0, currToken.ToString());
      this->Superclass::Type = STRINGLIST;
      return;
    }
    else
    {
      throw vtkFoamError() << "Expected string, '{' or ')', found " << nextToken;
    }
  }
  // list of lists or dictionaries: read recursively
  else if (currToken == '(' || currToken == '{')
  {
    this->Superclass::EntryValuePtrs = new vtkFoamPtrList<vtkFoamEntryValue>;
    this->Superclass::EntryValuePtrs->push_back(new vtkFoamEntryValue(this->UpperEntryPtr));
    this->Superclass::EntryValuePtrs->back()->SetStreamOption(io);
    if (currToken == '(')
    {
      this->Superclass::EntryValuePtrs->back()->ReadList(io);
    }
    else // currToken == '{'
    {
      this->Superclass::EntryValuePtrs->back()->ReadDictionary(io, vtkFoamToken());
    }
    // read all the following values as arbitrary entryValues
    // the alphaContactAngle b.c. in multiphaseInterFoam/damBreak4phase
    // reaquires this treatment (reading by readList() is not enough)
    do
    {
      this->Superclass::EntryValuePtrs->push_back(new vtkFoamEntryValue(this->UpperEntryPtr));
      this->Superclass::EntryValuePtrs->back()->SetStreamOption(io);
      this->Superclass::EntryValuePtrs->back()->Read(io);
    } while (*this->Superclass::EntryValuePtrs->back() != ')' &&
      *this->Superclass::EntryValuePtrs->back() != '}' &&
      *this->Superclass::EntryValuePtrs->back() != ';');

    if (*this->Superclass::EntryValuePtrs->back() != ')')
    {
      throw vtkFoamError() << "Expected ')' before " << *this->Superclass::EntryValuePtrs->back();
    }

    // Drop ')'
    this->Superclass::EntryValuePtrs->remove_back();
    this->Superclass::Type = ENTRYVALUELIST;
    return;
  }
  else if (currToken == ')') // empty list
  {
    this->Superclass::Type = EMPTYLIST;
    return;
  }
  // FIXME: may (or may not) need identifier handling

  while (io.Read(currToken) && currToken != ')')
  {
    if (this->Superclass::Type == LABELLIST)
    {
      if (currToken.GetType() == this->Superclass::SCALAR)
      {
        // Encountered a scalar while reading a labelList - switch representation
        // Need intermediate pointer since LabelListPtr and ScalarListPtr are in a union

        vtkDataArray* labels = this->LabelListPtr;
        const vtkIdType currLen = labels->GetNumberOfTuples();
        const bool use64BitLabels = ::Is64BitArray(labels); // <- Same as io.IsLabel64()

        // Copy, with append
        auto* scalars = vtkFloatArray::New();
        scalars->SetNumberOfValues(currLen + 1);
        for (vtkIdType i = 0; i < currLen; ++i)
        {
          scalars->SetValue(i, static_cast<float>(GetLabelValue(labels, i, use64BitLabels)));
        }
        scalars->SetValue(currLen, currToken.To<float>()); // Append value

        // Replace
        labels->Delete();
        this->Superclass::ScalarListPtr = scalars;
        this->Superclass::Type = SCALARLIST;
      }
      else if (currToken.GetType() == this->Superclass::LABEL)
      {
        assert("Label type not set!" && currToken.HasLabelType());
        if (currToken.IsLabel64())
        {
          assert(vtkTypeInt64Array::FastDownCast(this->LabelListPtr) != nullptr);
          static_cast<vtkTypeInt64Array*>(this->LabelListPtr)
            ->InsertNextValue(currToken.To<vtkTypeInt64>());
        }
        else
        {
          assert(vtkTypeInt32Array::FastDownCast(this->LabelListPtr) != nullptr);
          static_cast<vtkTypeInt32Array*>(this->LabelListPtr)
            ->InsertNextValue(currToken.To<vtkTypeInt32>());
        }
      }
      else
      {
        throw vtkFoamError() << "Expected a number, found " << currToken;
      }
    }
    else if (this->Superclass::Type == this->Superclass::SCALARLIST)
    {
      if (currToken.IsNumeric())
      {
        this->Superclass::ScalarListPtr->InsertNextValue(currToken.To<float>());
      }
      else if (currToken == '(')
      {
        vtkDebugWithObjectMacro(nullptr,
          "Found a list containing scalar data followed "
          "by a nested list, but this reader only "
          "supports nested lists that precede all "
          "scalars. Discarding nested list data.");
        vtkFoamEntryValue tmp(this->UpperEntryPtr);
        tmp.SetStreamOption(io);
        tmp.ReadList(io);
      }
      else
      {
        throw vtkFoamError() << "Expected a number, found " << currToken;
      }
    }
    else if (this->Superclass::Type == this->Superclass::STRINGLIST)
    {
      if (currToken.GetType() == this->Superclass::STRING)
      {
        this->Superclass::StringListPtr->InsertNextValue(currToken.ToString());
      }
      else
      {
        throw vtkFoamError() << "Expected a string, found " << currToken;
      }
    }
    else if (this->Superclass::Type == this->Superclass::ENTRYVALUELIST)
    {
      if (currToken.GetType() == this->Superclass::LABEL)
      {
        // skip the number of elements to make things simple
        if (!io.Read(currToken))
        {
          throw vtkFoamError() << "Unexpected EOF";
        }
      }
      if (currToken != '(')
      {
        throw vtkFoamError() << "Expected '(', found " << currToken;
      }
      this->Superclass::EntryValuePtrs->push_back(new vtkFoamEntryValue(this->UpperEntryPtr));
      this->Superclass::EntryValuePtrs->back()->ReadList(io);
    }
    else
    {
      throw vtkFoamError() << "Unexpected token " << currToken;
    }
  }

  if (this->Superclass::Type == this->Superclass::LABELLIST)
  {
    this->Superclass::LabelListPtr->Squeeze();
  }
  else if (this->Superclass::Type == this->Superclass::SCALARLIST)
  {
    this->Superclass::ScalarListPtr->Squeeze();
  }
  else if (this->Superclass::Type == this->Superclass::STRINGLIST)
  {
    this->Superclass::StringListPtr->Squeeze();
  }
}

// a list of dictionaries is actually read as a dictionary
void vtkFoamEntryValue::ReadDictionary(vtkFoamIOobject& io, const vtkFoamToken& firstKeyword)
{
  this->Superclass::DictPtr = new vtkFoamDict(this->UpperEntryPtr->GetUpperDictPtr());
  this->DictPtr->SetStreamOption(io);
  this->Superclass::Type = this->Superclass::DICTIONARY;
  this->Superclass::DictPtr->Read(io, true, firstKeyword);
}

// guess the type of the given entry value and read it
// return value: 0 if encountered end of entry (';') during parsing
// composite entry value, 1 otherwise
int vtkFoamEntryValue::Read(vtkFoamIOobject& io)
{
  this->SetStreamOption(io);

  vtkFoamToken currToken;
  currToken.SetStreamOption(io);
  if (!io.Read(currToken))
  {
    throw vtkFoamError() << "Unexpected EOF";
  }

  if (currToken == '{')
  {
    this->ReadDictionary(io, vtkFoamToken());
    return 1;
  }
  // for reading sublist from vtkFoamEntryValue::readList() or there
  // are cases where lists without the (non)uniform keyword appear
  // (e.g. coodles/pitsDaily/0/U)
  else if (currToken == '(')
  {
    this->ReadList(io);
    return 1;
  }
  else if (currToken == '[')
  {
    this->ReadDimensionSet(io);
    return 1;
  }
  else if (currToken == "uniform")
  {
    if (!io.Read(currToken))
    {
      throw vtkFoamError() << "Expected a uniform value or a list, found unexpected EOF";
    }
    if (currToken == '(')
    {
      this->ReadList(io);
    }
    else if (currToken == ';')
    {
      this->Superclass::operator=("uniform");
      return 0;
    }
    else if (currToken.GetType() == this->Superclass::LABEL ||
      currToken.GetType() == this->Superclass::SCALAR ||
      currToken.GetType() == this->Superclass::STRING)
    {
      this->Superclass::operator=(currToken);
    }
    else // unexpected punctuation token
    {
      throw vtkFoamError() << "Expected number, string or (, found " << currToken;
    }
    this->IsUniformEntry = true;
  }
  else if (currToken == "nonuniform")
  {
    if (!io.Read(currToken))
    {
      throw vtkFoamError() << "Expected list type specifier, found EOF";
    }
    this->IsUniformEntry = false;
    if (currToken == "List<scalar>")
    {
      if (io.IsFloat64())
      {
        this->ReadNonuniformList<SCALARLIST, listTraits<vtkFloatArray, double>>(io);
      }
      else
      {
        this->ReadNonuniformList<SCALARLIST, listTraits<vtkFloatArray, float>>(io);
      }
    }
    else if (currToken == "List<sphericalTensor>")
    {
      if (io.IsFloat64())
      {
        this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, double, 1, false>>(io);
      }
      else
      {
        this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, float, 1, false>>(io);
      }
    }
    else if (currToken == "List<vector>")
    {
      if (io.IsFloat64())
      {
        this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, double, 3, false>>(io);
      }
      else
      {
        this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, float, 3, false>>(io);
      }
    }
    else if (currToken == "List<symmTensor>")
    {
      if (io.IsFloat64())
      {
        this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, double, 6, false>>(io);
      }
      else
      {
        this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, float, 6, false>>(io);
      }
    }
    else if (currToken == "List<tensor>")
    {
      if (io.IsFloat64())
      {
        this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, double, 9, false>>(io);
      }
      else
      {
        this->ReadNonuniformList<VECTORLIST, vectorListTraits<vtkFloatArray, float, 9, false>>(io);
      }
    }
    // List<bool> may or may not be read as List<label>,
    // this needs checking but not many bool fields in use
    else if (currToken == "List<label>" || currToken == "List<bool>")
    {
      if (io.IsLabel64())
      {
        this->ReadNonuniformList<LABELLIST, listTraits<vtkTypeInt64Array, vtkTypeInt64>>(io);
      }
      else
      {
        this->ReadNonuniformList<LABELLIST, listTraits<vtkTypeInt32Array, vtkTypeInt32>>(io);
      }
    }
    // an empty list doesn't have a list type specifier
    else if (currToken.GetType() == this->Superclass::LABEL && currToken.To<vtkTypeInt64>() == 0)
    {
      this->Superclass::Type = this->Superclass::EMPTYLIST;
      if (io.IsAsciiFormat())
      {
        io.ReadExpecting('(');
        io.ReadExpecting(')');
      }
    }
    else if (currToken == ';')
    {
      this->Superclass::operator=("nonuniform");
      return 0;
    }
    else
    {
      throw vtkFoamError() << "Unsupported nonuniform list type " << currToken;
    }
  }
  // turbulentTemperatureCoupledBaffleMixed boundary condition
  // uses lists without a uniform/nonuniform keyword
  else if (currToken == "List<scalar>")
  {
    this->IsUniformEntry = false;
    if (io.IsFloat64())
    {
      this->ReadNonuniformList<SCALARLIST, listTraits<vtkFloatArray, double>>(io);
    }
    else
    {
      this->ReadNonuniformList<SCALARLIST, listTraits<vtkFloatArray, float>>(io);
    }
  }
  // zones have list without a uniform/nonuniform keyword
  else if (currToken == "List<label>")
  {
    this->IsUniformEntry = false;
    if (io.IsLabel64())
    {
      this->ReadNonuniformList<LABELLIST, listTraits<vtkTypeInt64Array, vtkTypeInt64>>(io);
    }
    else
    {
      this->ReadNonuniformList<LABELLIST, listTraits<vtkTypeInt32Array, vtkTypeInt32>>(io);
    }
  }
  else if (currToken == "List<bool>")
  {
    // List<bool> is read as a list of bytes (binary) or ints (ascii)
    // - primary location is the flipMap entry in faceZones
    this->IsUniformEntry = false;
    this->ReadNonuniformList<BOOLLIST, listTraits<vtkTypeInt8Array, vtkTypeInt8>>(io);
  }
  else if (currToken.GetType() == this->Superclass::PUNCTUATION ||
    currToken.GetType() == this->Superclass::LABEL ||
    currToken.GetType() == this->Superclass::SCALAR ||
    currToken.GetType() == this->Superclass::STRING ||
    currToken.GetType() == this->Superclass::IDENTIFIER)
  {
    this->Superclass::operator=(currToken);
  }
  return 1;
}

// read values of an entry
void vtkFoamEntry::Read(vtkFoamIOobject& io)
{
  for (;;)
  {
    this->Superclass::push_back(new vtkFoamEntryValue(this));
    this->Superclass::back()->SetStreamOption(io);
    if (!this->Superclass::back()->Read(io))
    {
      break;
    }

    if (this->Superclass::size() >= 2)
    {
      vtkFoamEntryValue& secondLastValue =
        *this->Superclass::operator[](this->Superclass::size() - 2);
      if (secondLastValue.GetType() == vtkFoamToken::LABEL)
      {
        vtkFoamEntryValue& lastValue = *this->Superclass::back();

        // a zero-sized nonuniform list without prefixing "nonuniform"
        // keyword nor list type specifier (i. e. `0()';
        // e. g. simpleEngine/0/polyMesh/pointZones) requires special
        // care (one with nonuniform prefix is treated within
        // vtkFoamEntryValue::read()). still this causes erroneous
        // behavior for `0 nonuniform 0()' but this should be extremely
        // rare
        if (lastValue.GetType() == vtkFoamToken::EMPTYLIST && secondLastValue == 0)
        {
          // Remove last value, and mark new last value as EMPTYLIST
          this->remove_back();
          this->Superclass::back()->SetEmptyList();
        }
        // for an exceptional expression of `LABEL{LABELorSCALAR}' without
        // type prefix (e. g. `2{-0}' in mixedRhoE B.C. in
        // rhopSonicFoam/shockTube)
        else if (lastValue.GetType() == vtkFoamToken::DICTIONARY)
        {
          if (lastValue.Dictionary().GetType() == vtkFoamToken::LABEL)
          {
            const vtkTypeInt64 asize = secondLastValue.To<vtkTypeInt64>();
            const vtkTypeInt64 value = lastValue.Dictionary().GetToken().ToInt();
            // Remove the last two values
            this->remove_back();
            this->remove_back();
            // Make new labelList
            this->Superclass::push_back(new vtkFoamEntryValue(this));
            this->Superclass::back()->SetStreamOption(io);
            this->Superclass::back()->MakeLabelList(value, asize);
          }
          else if (lastValue.Dictionary().GetType() == vtkFoamToken::SCALAR)
          {
            const vtkTypeInt64 asize = secondLastValue.To<vtkTypeInt64>();
            const float value = lastValue.Dictionary().GetToken().ToFloat();
            // Remove the last two values
            this->remove_back();
            this->remove_back();
            // Make new scalarList
            this->Superclass::push_back(new vtkFoamEntryValue(this));
            this->Superclass::back()->SetStreamOption(io);
            this->Superclass::back()->MakeScalarList(value, asize);
          }
        }
      }
    }

    if (this->Superclass::back()->GetType() == vtkFoamToken::IDENTIFIER)
    {
      // Substitute identifier
      const std::string identifier(this->Superclass::back()->ToIdentifier());
      this->remove_back();

      for (const vtkFoamDict* uDictPtr = this->UpperDictPtr;;)
      {
        const vtkFoamEntry* identifiedEntry = uDictPtr->Lookup(identifier);

        if (identifiedEntry != nullptr)
        {
          for (size_t valueI = 0; valueI < identifiedEntry->size(); valueI++)
          {
            this->Superclass::push_back(
              new vtkFoamEntryValue(*identifiedEntry->operator[](valueI), this));
            this->back()->SetStreamOption(io);
          }
          break;
        }
        else
        {
          uDictPtr = uDictPtr->GetUpperDictPtr();
          if (uDictPtr == nullptr)
          {
            throw vtkFoamError() << "substituting entry " << identifier << " not found";
          }
        }
      }
    }
    else if (*this->Superclass::back() == ';')
    {
      this->remove_back();
      break;
    }
    else if (this->Superclass::back()->GetType() == vtkFoamToken::DICTIONARY)
    {
      // subdictionary is not suffixed by an entry terminator ';'
      break;
    }
    else if (*this->Superclass::back() == '}' || *this->Superclass::back() == ')')
    {
      throw vtkFoamError() << "Unmatched " << *this->Superclass::back();
    }
  }
}

//------------------------------------------------------------------------------
// vtkOpenFOAMReaderPrivate constructor and destructor
vtkOpenFOAMReaderPrivate::vtkOpenFOAMReaderPrivate()
{
  // DATA TIMES
  this->TimeStep = 0;
  this->TimeStepOld = -1;
  this->TimeValues = vtkDoubleArray::New();
  this->TimeNames = vtkStringArray::New();

  // Selection
  this->InternalMeshSelectionStatus = 0;
  this->InternalMeshSelectionStatusOld = 0;

  // Mesh dimensions
  this->NumPoints = 0;
  this->NumInternalFaces = 0;
  this->NumFaces = 0;
  this->NumCells = 0;

  this->VolFieldFiles = vtkStringArray::New();
  this->DimFieldFiles = vtkStringArray::New();
  this->AreaFieldFiles = vtkStringArray::New();
  this->PointFieldFiles = vtkStringArray::New();
  this->LagrangianFieldFiles = vtkStringArray::New();
  this->PolyMeshPointsDir = vtkStringArray::New();
  this->PolyMeshFacesDir = vtkStringArray::New();

  // For creating cell-to-point translated data
  this->BoundaryPointMap = nullptr;
  this->AllBoundaries = nullptr;
  this->AllBoundariesPointMap = nullptr;
  this->InternalPoints = nullptr;

  // For caching mesh
  this->InternalMesh = nullptr;
  this->BoundaryMesh = nullptr;
  this->BoundaryPointMap = nullptr;
  this->FaceOwner = nullptr;
  this->FaceNeigh = nullptr;
  this->PointZoneMesh = nullptr;
  this->FaceZoneMesh = nullptr;
  this->CellZoneMesh = nullptr;

  // For polyhedral decomposition
  this->NumTotalAdditionalCells = 0;
  this->AdditionalCellIds = nullptr;
  this->NumAdditionalCells = nullptr;
  this->AdditionalCellPoints = nullptr;

  this->Parent = nullptr;
}

vtkOpenFOAMReaderPrivate::~vtkOpenFOAMReaderPrivate()
{
  this->TimeValues->Delete();
  this->TimeNames->Delete();

  this->PolyMeshPointsDir->Delete();
  this->PolyMeshFacesDir->Delete();
  this->VolFieldFiles->Delete();
  this->DimFieldFiles->Delete();
  this->AreaFieldFiles->Delete();
  this->PointFieldFiles->Delete();
  this->LagrangianFieldFiles->Delete();

  this->ClearMeshes();
}

void vtkOpenFOAMReaderPrivate::ClearInternalMeshes()
{
  if (this->FaceOwner != nullptr)
  {
    this->FaceOwner->Delete();
    this->FaceOwner = nullptr;
  }
  if (this->FaceNeigh != nullptr)
  {
    this->FaceNeigh->Delete();
    this->FaceNeigh = nullptr;
  }
  if (this->InternalMesh != nullptr)
  {
    this->InternalMesh->Delete();
    this->InternalMesh = nullptr;
  }

  // For polyhedral decomposition
  this->NumTotalAdditionalCells = 0;
  if (this->AdditionalCellIds != nullptr)
  {
    this->AdditionalCellIds->Delete();
    this->AdditionalCellIds = nullptr;
  }
  if (this->NumAdditionalCells != nullptr)
  {
    this->NumAdditionalCells->Delete();
    this->NumAdditionalCells = nullptr;
  }

  delete this->AdditionalCellPoints;
  this->AdditionalCellPoints = nullptr;
}

void vtkOpenFOAMReaderPrivate::ClearZoneMeshes()
{
  if (this->PointZoneMesh != nullptr)
  {
    this->PointZoneMesh->Delete();
    this->PointZoneMesh = nullptr;
  }
  if (this->FaceZoneMesh != nullptr)
  {
    this->FaceZoneMesh->Delete();
    this->FaceZoneMesh = nullptr;
  }
  if (this->CellZoneMesh != nullptr)
  {
    this->CellZoneMesh->Delete();
    this->CellZoneMesh = nullptr;
  }
}

void vtkOpenFOAMReaderPrivate::ClearBoundaryMeshes()
{
  if (this->BoundaryMesh != nullptr)
  {
    this->BoundaryMesh->Delete();
    this->BoundaryMesh = nullptr;
  }

  delete this->BoundaryPointMap;
  this->BoundaryPointMap = nullptr;

  if (this->InternalPoints != nullptr)
  {
    this->InternalPoints->Delete();
    this->InternalPoints = nullptr;
  }
  if (this->AllBoundaries != nullptr)
  {
    this->AllBoundaries->Delete();
    this->AllBoundaries = nullptr;
  }
  if (this->AllBoundariesPointMap != nullptr)
  {
    this->AllBoundariesPointMap->Delete();
    this->AllBoundariesPointMap = nullptr;
  }
}

void vtkOpenFOAMReaderPrivate::ClearMeshes()
{
  this->ClearInternalMeshes();
  this->ClearBoundaryMeshes();
  this->ClearZoneMeshes();
}

void vtkOpenFOAMReaderPrivate::SetTimeValue(double requestedTime)
{
  const vtkIdType nTimeValues = this->TimeValues->GetNumberOfTuples();

  if (nTimeValues > 0)
  {
    int minTimeI = 0;
    double minTimeDiff = fabs(this->TimeValues->GetValue(0) - requestedTime);
    for (int timeI = 1; timeI < nTimeValues; timeI++)
    {
      const double timeDiff(fabs(this->TimeValues->GetValue(timeI) - requestedTime));
      if (timeDiff < minTimeDiff)
      {
        minTimeI = timeI;
        minTimeDiff = timeDiff;
      }
    }
    this->SetTimeStep(minTimeI); // set Modified() if TimeStep changed
  }
}

//------------------------------------------------------------------------------
// Copy time instances information and create mesh times
void vtkOpenFOAMReaderPrivate::SetupInformation(const std::string& casePath,
  const std::string& regionName, const std::string& procName, vtkOpenFOAMReaderPrivate* master,
  bool requirePolyMesh)
{
  vtkFoamDebug(<< "SetupInformation (" << this->RegionName << "/" << procName
               << ") polyMesh:" << requirePolyMesh << "\n");

  // Copy parent, path and timestep information from master
  this->CasePath = casePath;
  this->RegionName = regionName;
  this->ProcessorName = procName;
  this->Parent = master->Parent;
  this->TimeValues->Delete();
  this->TimeValues = master->TimeValues;
  this->TimeValues->Register(nullptr);
  this->TimeNames->Delete();
  this->TimeNames = master->TimeNames;
  this->TimeNames->Register(nullptr);

  // Normally will have already checked for a region polyMesh/ before calling this method,
  // but provision for a missing mesh anyhow
  if (requirePolyMesh)
  {
    this->PopulatePolyMeshDirArrays();
  }
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReaderPrivate::AddFieldName(
  const std::string& fieldName, const std::string& fieldType, bool isLagrangian)
{
  if (fieldName.empty() || fieldType.empty())
  {
    return;
  }

  size_t len = fieldType.find("Field");
  bool isInternalField = false;

  if (len == std::string::npos)
  {
    return;
  }
  else if ((len + 5) == fieldType.size())
  {
    // OK: ends_with("Field")
  }
  else if (fieldType.compare(len, std::string::npos, "Field::Internal") == 0)
  {
    // OK: ends_with("Field::Internal")
    // but only valid for volScalarField::Internal, etc

    isInternalField = true;
    if (isLagrangian || (fieldType.compare(0, 3, "vol") != 0))
    {
      return;
    }
  }
  else
  {
    // Some other (unknown) type - ignore
    return;
  }

  if (isLagrangian)
  {
    // Lagrangian (point) fields: labelField, scalarField, vectorField, ...

    const auto fieldDataType(vtkFoamTypes::ToEnum(fieldType));
    if (fieldDataType != vtkFoamTypes::NO_TYPE)
    {
      // NB: Cloud has labelField too
      this->LagrangianFieldFiles->InsertNextValue(fieldName);
    }
    return;
  }

  size_t prefix = 0;
  vtkStringArray* target = nullptr;
  if (fieldType.compare(0, 3, "vol") == 0) // starts_with("vol")
  {
    // Volume fields: volScalarField, volVectorField, ...
    // or Dimensioned (internal) fields: volScalarField::Internal, ...
    prefix = 3;
    len -= prefix;

    target = this->VolFieldFiles;
    if (isInternalField)
    {
      target = this->DimFieldFiles;
    }
  }
  else if (fieldType.compare(0, 4, "area") == 0) // starts_with("area")
  {
    // Mesh area fields: areaScalarField, areaVectorField, ...
    prefix = 4;
    len -= prefix;

    target = this->AreaFieldFiles;
  }
  else if (fieldType.compare(0, 5, "point") == 0) // starts_with("point")
  {
    // Mesh point fields: pointScalarField, pointVectorField, ...
    prefix = 5;
    len -= prefix;

    target = this->PointFieldFiles;
  }

  if (target != nullptr)
  {
    const auto fieldDataType(vtkFoamTypes::ToEnum(fieldType.substr(prefix, len)));
    if (vtkFoamTypes::IsScalar(fieldDataType) || vtkFoamTypes::IsVectorSpace(fieldDataType))
    {
      target->InsertNextValue(fieldName);
    }
  }
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReaderPrivate::GetFieldNames(const std::string& tempPath, bool isLagrangian)
{
  // Open the directory and get num of files
  vtkNew<vtkDirectory> directory;
  if (!directory->Open(tempPath.c_str()))
  {
    // No data
    return;
  }

  // loop over all files and locate valid fields
  const vtkIdType nFieldFiles = directory->GetNumberOfFiles();
  for (vtkIdType fileI = 0; fileI < nFieldFiles; ++fileI)
  {
    const std::string fieldFile(directory->GetFile(fileI));
    const auto len = fieldFile.length();

    if (!len || (fieldFile[len - 1] == '~') || directory->FileIsDirectory(fieldFile.c_str()))
    {
      continue;
    }
#if VTK_FOAMFILE_IGNORE_FIELD_RESTART
    else if (len > 2 && (fieldFile[len - 2] == '_') && (fieldFile[len - 1] == '0'))
    {
      // Exclude "*_0" restart files
      continue;
    }
#endif
    else
    {
      // Exclude various backup extensions - cf. Foam::fileName::isBackup()

      auto sep = fieldFile.rfind('.');
      if (sep != std::string::npos)
      {
        ++sep;

        if (!fieldFile.compare(sep, std::string::npos, "bak") ||
          !fieldFile.compare(sep, std::string::npos, "BAK") ||
          !fieldFile.compare(sep, std::string::npos, "old") ||
          !fieldFile.compare(sep, std::string::npos, "save"))
        {
          continue;
        }
      }
    }

    vtkFoamIOobject io(this->CasePath, this->Parent);
    if (io.Open(tempPath + "/" + fieldFile)) // file exists and readable
    {
      this->AddFieldName(fieldFile, io.GetClassName(), isLagrangian);
      io.Close();
    }
  }
  // delay Squeeze of inserted objects until SortFieldFiles()
}

//------------------------------------------------------------------------------
// locate laglangian clouds
void vtkOpenFOAMReaderPrivate::LocateLagrangianClouds(const std::string& timePath)
{
  vtkNew<vtkDirectory> directory;

  if (directory->Open((timePath + this->RegionPath() + "/lagrangian").c_str()))
  {
    // search for sub-clouds (OF 1.5 format)
    const vtkIdType nFiles = directory->GetNumberOfFiles();
    bool isSubCloud = false;
    for (vtkIdType fileI = 0; fileI < nFiles; fileI++)
    {
      const std::string fileNameI(directory->GetFile(fileI));
      if (fileNameI != "." && fileNameI != ".." && directory->FileIsDirectory(fileNameI.c_str()))
      {
        vtkFoamIOobject io(this->CasePath, this->Parent);
        const std::string subCloudName(this->RegionPrefix() + "lagrangian/" + fileNameI);
        const std::string subCloudFullPath(timePath + "/" + subCloudName);
        // lagrangian positions. there are many concrete class names
        // e. g. Cloud<parcel>, basicKinematicCloud etc.
        if (io.OpenOrGzip(subCloudFullPath + "/positions") && io.GetObjectName() == "positions" &&
          io.GetClassName().find("Cloud") != std::string::npos)
        {
          isSubCloud = true;
          // a lagrangianPath has to be in a bit different format from
          // subCloudName to make the "lagrangian" reserved path
          // component and a mesh region with the same name
          // distinguishable later
          const std::string subCloudPath(this->RegionName + "/lagrangian/" + fileNameI);
          if (this->Parent->LagrangianPaths->LookupValue(subCloudPath) == -1)
          {
            this->Parent->LagrangianPaths->InsertNextValue(subCloudPath);
          }
          this->GetFieldNames(subCloudFullPath, true);
          this->Parent->PatchDataArraySelection->AddArray(subCloudName.c_str());
        }
      }
    }
    // if there's no sub-cloud then OF < 1.5 format
    if (!isSubCloud)
    {
      vtkFoamIOobject io(this->CasePath, this->Parent);
      const std::string cloudName(this->RegionPrefix() + "lagrangian");
      const std::string cloudFullPath(timePath + "/" + cloudName);
      if (io.OpenOrGzip(cloudFullPath + "/positions") && io.GetObjectName() == "positions" &&
        io.GetClassName().find("Cloud") != std::string::npos)
      {
        const std::string cloudPath(this->RegionName + "/lagrangian");
        if (this->Parent->LagrangianPaths->LookupValue(cloudPath) == -1)
        {
          this->Parent->LagrangianPaths->InsertNextValue(cloudPath);
        }
        this->GetFieldNames(cloudFullPath, true);
        this->Parent->PatchDataArraySelection->AddArray(cloudName.c_str());
      }
    }
    this->Parent->LagrangianPaths->Squeeze();
  }
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReaderPrivate::SortFieldFiles(vtkStringArray* selections, vtkStringArray* files)
{
  // The object (field) name in the FoamFile header should always correspond
  // to the filename (without any trailing .gz etc)

  auto names = vtkSmartPointer<vtkStringArray>::New();
  for (vtkIdType i = 0; i < files->GetNumberOfValues(); ++i)
  {
    std::string name(files->GetValue(i));
    const auto ending = name.rfind(".gz");
    if (ending != std::string::npos)
    {
      name.erase(ending);
    }
    names->InsertNextValue(name);
  }

  names->Squeeze();
  files->Squeeze();
  vtkSortDataArray::Sort(names, files);
  for (vtkIdType i = 0; i < names->GetNumberOfValues(); ++i)
  {
    selections->InsertNextValue(names->GetValue(i));
  }
}

//------------------------------------------------------------------------------
// Set contents from dictionary information read from the polyMesh/boundary file
bool vtkFoamBoundaries::update(const vtkFoamDict& dict)
{
  auto& patches = *this;
  patches.clearAll();
  patches.resize(dict.size());
  auto& inGroups = patches.groups;

  const auto nBoundaries = static_cast<vtkIdType>(patches.size());

  vtkIdType endFace = -1; // for sanity check
  vtkTypeInt64 nBoundaryFaces = 0;

  for (vtkIdType patchi = 0; patchi < nBoundaries; ++patchi)
  {
    // The name/dictionary, from "polyMesh/boundary" entry
    const vtkFoamEntry* eptr = dict[patchi];
    const std::string& patchName = eptr->GetKeyword();
    const vtkFoamDict& patchDict = eptr->Dictionary();

    // The patch entry to populate
    vtkFoamPatch& patch = patches[patchi];
    patch.index_ = patchi;
    patch.offset_ = nBoundaryFaces;
    patch.type_ = vtkFoamPatch::GEOMETRICAL;
    patch.owner_ = true; // Patch owner (processor patch)

    patch.name_ = patchName;
    if ((eptr = patchDict.Lookup("type")) == nullptr)
    {
      this->error() // Report errors
        << "No 'type' entry found for patch: " << patch.name_;
      return false;
    }
    const std::string patchTypeName(eptr->ToString());

    if ((eptr = patchDict.Lookup("startFace")) == nullptr)
    {
      this->error() // Report errors
        << "No 'startFace' entry found for patch: " << patch.name_;
      return false;
    }
    patch.start_ = eptr->ToInt();

    if ((eptr = patchDict.Lookup("nFaces")) == nullptr)
    {
      this->error() // Report errors
        << "No 'nFaces' entry found for patch: " << patch.name_;
      return false;
    }
    patch.size_ = eptr->ToInt();

    // Size, consistency sanity checks
    if (patch.start_ < 0 || patch.size_ < 0)
    {
      this->error() // Report errors
        << "The startFace:" << patch.start_ << " or nFaces:" << patch.size_
        << " are negative for patch " << patch.name_;
      return false;
    }
    if (endFace >= 0 && endFace != patch.start_)
    {
      this->error() // Report errors
        << "The end face number " << (endFace - 1) << " of patch " << patches[patchi - 1].name_
        << " is inconsistent with start face number " << patch.start_ << " of patch "
        << patch.name_;
      return false;
    }
    endFace = patch.endFace(); // <- The startFace for the next patch index

    // If the basic type of the patch is one of the following the
    // point-filtered values at patches are overridden by patch values
    if (patchTypeName == "patch" || patchTypeName == "wall" || patchTypeName == "mappedWall")
    {
      patch.type_ = vtkFoamPatch::PHYSICAL;
      nBoundaryFaces += patch.size_;
    }
    else if (patchTypeName == "processor" || patchTypeName == "processorCyclic")
    {
      patch.type_ = vtkFoamPatch::PROCESSOR;
      nBoundaryFaces += patch.size_;

      // Note owner/neighbour relationship for processor patch
      const auto* ownptr = patchDict.Lookup("myProcNo");
      const auto* neiptr = patchDict.Lookup("neighbProcNo");

      if (ownptr != nullptr && neiptr != nullptr &&
        (ownptr->FirstValue().GetType() == vtkFoamToken::LABEL) &&
        (neiptr->FirstValue().GetType() == vtkFoamToken::LABEL))
      {
        const vtkTypeInt64 own = ownptr->FirstValue().ToInt();
        const vtkTypeInt64 nei = neiptr->FirstValue().ToInt();
        patch.owner_ = (own < nei);
      }
    }

    // Handle inGroups which could have this type of content:
    //   - inGroups (name1 .. nameN);
    //   - inGroups 2(name1 name2);
    // but never for processor boundaries (potential clutter or false positives)
    if ((eptr = patchDict.Lookup("inGroups")) != nullptr && patch.type_ != vtkFoamPatch::PROCESSOR)
    {
      for (const vtkFoamEntryValue* subentry : *eptr)
      {
        if (subentry && subentry->GetType() == vtkFoamToken::STRINGLIST)
        {
          // Yes this is really needed, VTK constness is a bit odd
          vtkStringArray& groupNames = const_cast<vtkStringArray&>(subentry->StringList());
          const vtkIdType nGroups = groupNames.GetNumberOfValues();

          for (vtkIdType groupi = 0; groupi < nGroups; ++groupi)
          {
            const std::string& groupName = groupNames.GetValue(groupi);
            inGroups[groupName].push_back(patchi);
          }
        }
      }
    }
  }

  // Could also use HasError() for an additional sanity check
  return true;
}

//------------------------------------------------------------------------------
// Binary search for patch index for a given face label
// Return -1 for internal face or out-of-bounds
vtkIdType vtkFoamBoundaries::whichPatch(vtkIdType faceIndex) const
{
  if (this->empty() ||                         // Safety/short-circuit
    (faceIndex < this->front().startFace()) || // Internal mesh face
    (faceIndex >= (this->back().endFace())))   // Out-of-bounds
  {
    return -1;
  }

  // Binary search like std::lower_bound, but slightly modified
  auto first = this->begin();
  const auto last = this->end();
  auto count = this->size();

  while (count > 0)
  {
    auto iter = first;
    auto step = count / 2;
    iter += step;

    if (iter->start_ <= faceIndex) // NB: must include start in the comparison
    {
      first = ++iter;
      count -= step + 1;
    }
    else
    {
      count = step;
    }
  }
  return (first != last) ? first->index_ : -1;
}

//------------------------------------------------------------------------------
// Create field data lists and cell/point array selection lists
int vtkOpenFOAMReaderPrivate::MakeMetaDataAtTimeStep(vtkStringArray* cellSelectionNames,
  vtkStringArray* pointSelectionNames, vtkStringArray* lagrangianSelectionNames,
  bool listNextTimeStep)
{
  vtkFoamDebug(<< "MakeMetaDataAtTimeStep (" << this->RegionName << "/" << this->ProcessorName
               << ")\n");

  if (!this->HasPolyMesh())
  {
    // Ignore a region without a mesh, but will normally be precluded earlier
    vtkWarningMacro("Called MakeMetaDataAtTimeStep without a mesh.");
    return 1;
  }

  // Change in topology or selection, may need to update boundaries
  {
    auto& patches = this->BoundaryDict;

    // Change in topology, need to create/recreate from file
    const bool topoChanged = (patches.timeName_.empty() ||
      (this->PolyMeshFacesDir->GetValue(this->TimeStep) != patches.timeName_));

    // User selection changed
    const bool selectChanged = topoChanged ||
      (this->Parent->PatchDataArraySelection->GetMTime() != this->Parent->PatchSelectionMTimeOld);

    bool addInternalSelection = false;

    // Read contents of polyMesh/boundary to update patch definitions
    if (topoChanged)
    {
      patches.clearAll();
      patches.timeName_ = this->PolyMeshFacesDir->GetValue(this->TimeStep);

      const bool isSubRegion = !this->RegionName.empty();
      auto boundaryEntriesPtr(this->GatherBlocks("boundary", isSubRegion));

      if (boundaryEntriesPtr)
      {
        if (!patches.update(*boundaryEntriesPtr))
        {
          vtkErrorMacro(<< patches.error());
          return 0;
        }

        // On topology change, add the internal mesh by default
        addInternalSelection = true;
      }
      else if (isSubRegion)
      {
        // Could be missing polyMesh/boundary for sub-region
        return 0;
      }
    }

    // The internal mesh - set/check status
    if (selectChanged)
    {
      const std::string displayName(this->RegionPrefix() + NAME_INTERNALMESH);

      if (addInternalSelection)
      {
        this->Parent->PatchDataArraySelection->AddArray(displayName.c_str());
      }
      this->InternalMeshSelectionStatus =
        (this->Parent->PatchDataArraySelection->ArrayExists(displayName.c_str()) &&
          this->Parent->GetPatchArrayStatus(displayName.c_str()));
    }

    // The boundary mesh - change in user selection or topology
    if (selectChanged)
    {
      // Can perhaps do more with preserving old selections
      // and check which boundaries actually changed
      //
      // decltype(patches.groupActive) groupActiveOld;
      // decltype(patches.patchActive) patchActiveOld;
      // decltype(patches.patchActiveByGroup) patchActiveByGroupOld;
      //
      // std::swap(groupActiveOld, patches.groupActive);
      // std::swap(patchActiveOld, patches.patchActive);
      // std::swap(patchActiveByGroupOld, patches.patchActiveByGroup);

      // For now, simply start afresh
      patches.clearSelections();

      // Patch groups (sorted)
      const auto& inGroups = patches.groups;
      for (auto citer = inGroups.begin(), endIter = inGroups.end(); citer != endIter; ++citer)
      {
        const std::string& groupName = citer->first;
        const std::string displayName(this->RegionPrefix() + "group/" + groupName);
        if (this->Parent->PatchDataArraySelection->ArrayExists(displayName.c_str()))
        {
          if (this->Parent->GetPatchArrayStatus(displayName.c_str()))
          {
            // Selected by group
            patches.enableGroup(groupName);
          }
        }
        else
        {
          // Add to list with selection status == off.
          this->Parent->PatchDataArraySelection->DisableArray(displayName.c_str());
        }
      }

      // Individual patches
      for (vtkFoamPatch& patch : patches)
      {
        const std::string& patchName = patch.name_;

        // always hide processor patches for decomposed cases to keep
        // vtkAppendCompositeDataLeaves happy
        if (patch.type_ == vtkFoamPatch::PROCESSOR && !this->ProcessorName.empty())
        {
          continue;
        }

        const std::string displayName(this->RegionPrefix() + "patch/" + patchName);
        if (this->Parent->PatchDataArraySelection->ArrayExists(displayName.c_str()))
        {
          if (this->Parent->GetPatchArrayStatus(displayName.c_str()))
          {
            // Selected by patch
            patches.enablePatch(patch.index_);
          }
        }
        else
        {
          // Add to list with selection status == off.
          // The patch is added to list even if its size is zero
          this->Parent->PatchDataArraySelection->DisableArray(displayName.c_str());
        }
      }
    }
  }

  // Add scalars and vectors to metadata
  std::string timePath(this->CurrentTimePath());
  // do not do "RemoveAllArrays()" to accumulate array selections
  // this->CellDataArraySelection->RemoveAllArrays();
  this->VolFieldFiles->Initialize();
  this->DimFieldFiles->Initialize();
  this->AreaFieldFiles->Initialize();
  this->PointFieldFiles->Initialize();
  this->GetFieldNames(timePath + this->RegionPath());

  this->LagrangianFieldFiles->Initialize();
  if (listNextTimeStep)
  {
    this->Parent->LagrangianPaths->Initialize();
  }
  this->LocateLagrangianClouds(timePath);

  // if the requested timestep is 0 then we also look at the next
  // timestep to add extra objects that don't exist at timestep 0 into
  // selection lists. Note the ObjectNames array will be recreated in
  // RequestData() so we don't have to worry about duplicated fields.
  if (listNextTimeStep && this->TimeValues->GetNumberOfTuples() >= 2 && this->TimeStep == 0)
  {
    const std::string timePath2(this->TimePath(1));
    this->GetFieldNames(timePath2 + this->RegionPath());
    // if lagrangian clouds were not found at timestep 0
    if (this->Parent->LagrangianPaths->GetNumberOfTuples() == 0)
    {
      this->LocateLagrangianClouds(timePath2);
    }
  }

  // sort array names. volFields first, followed by internal fields
  this->SortFieldFiles(cellSelectionNames, this->VolFieldFiles);
  this->SortFieldFiles(cellSelectionNames, this->DimFieldFiles);
  // pending: this->SortFieldFiles(cellSelectionNames, this->AreaFieldFiles);
  this->SortFieldFiles(pointSelectionNames, this->PointFieldFiles);
  this->SortFieldFiles(lagrangianSelectionNames, this->LagrangianFieldFiles);

  return 1;
}

//------------------------------------------------------------------------------
// List time directories according to system/controlDict
bool vtkOpenFOAMReaderPrivate::ListTimeDirectoriesByControlDict(const vtkFoamDict& dict)
{
  // Note: use double (not float) to handle time values precisely

  const vtkFoamEntry* eptr;

  if ((eptr = dict.Lookup("startTime")) == nullptr)
  {
    vtkErrorMacro(<< "No 'startTime' in controlDict");
    return false;
  }
  const double startTime = eptr->ToDouble();

  if ((eptr = dict.Lookup("endTime")) == nullptr)
  {
    vtkErrorMacro(<< "No 'endTime' in controlDict");
    return false;
  }
  const double endTime = eptr->ToDouble();

  if ((eptr = dict.Lookup("deltaT")) == nullptr)
  {
    vtkErrorMacro(<< "No 'deltaT' in controlDict");
    return false;
  }
  const double deltaT = eptr->ToDouble();

  if ((eptr = dict.Lookup("writeInterval")) == nullptr)
  {
    vtkErrorMacro(<< "No 'writeInterval' in controlDict");
    return false;
  }
  const double writeInterval = eptr->ToDouble();

  if ((eptr = dict.Lookup("timeFormat")) == nullptr)
  {
    vtkErrorMacro(<< "No 'timeFormat' in controlDict");
    return false;
  }
  const std::string timeFormat(eptr->ToString());

  // Default timePrecision is 6
  const vtkTypeInt64 timePrecision =
    ((eptr = dict.Lookup("timePrecision")) != nullptr ? eptr->ToInt() : 6);

  // Calculate time step increment based on type of run
  if ((eptr = dict.Lookup("writeControl")) == nullptr)
  {
    vtkErrorMacro(<< "No 'writeControl' in controlDict");
    return false;
  }
  const std::string writeControl(eptr->ToString());

  double timeStepIncrement = 1;
  if (writeControl == "timeStep")
  {
    timeStepIncrement = writeInterval * deltaT;
  }
  else if (writeControl == "runTime" || writeControl == "adjustableRunTime")
  {
    timeStepIncrement = writeInterval;
  }
  else
  {
    vtkErrorMacro(<< "Cannot determine time-step for writeControl: " << writeControl);
    return false;
  }

  // How many timesteps there should be
  const double tempResult = (endTime - startTime) / timeStepIncrement;

  // +0.5 to round up
  const int tempNumTimeSteps = static_cast<int>(tempResult + 0.5) + 1;

  // Make sure time step dir exists
  vtkNew<vtkDirectory> testdir;
  this->TimeValues->Initialize();
  this->TimeNames->Initialize();

  // Determine time name based on Foam::Time::timeName()
  // cf. src/OpenFOAM/db/Time/Time.C
  std::ostringstream parser;
#ifdef _MSC_VER
  bool correctExponent = true;
#endif
  if (timeFormat == "general")
  {
    parser.setf(std::ios_base::fmtflags(0), std::ios_base::floatfield);
  }
  else if (timeFormat == "fixed")
  {
    parser.setf(std::ios_base::fmtflags(std::ios_base::fixed), std::ios_base::floatfield);
#ifdef _MSC_VER
    correctExponent = false;
#endif
  }
  else if (timeFormat == "scientific")
  {
    parser.setf(std::ios_base::fmtflags(std::ios_base::scientific), std::ios_base::floatfield);
  }
  else
  {
    vtkWarningMacro("Warning: unsupported time format. Assuming general.");
    parser.setf(std::ios_base::fmtflags(0), std::ios_base::floatfield);
  }
  parser.precision(timePrecision);

  for (int i = 0; i < tempNumTimeSteps; i++)
  {
    parser.str("");
    const double tempStep = i * timeStepIncrement + startTime;
    parser << tempStep; // stringstream doesn't require ends
#ifdef _MSC_VER
    // workaround for format difference in MSVC++:
    // remove an extra 0 from exponent
    if (correctExponent)
    {
      std::string tempStr(parser.str());
      const auto pos = tempStr.find('e');
      if (pos != std::string::npos && tempStr.length() >= pos + 3 && tempStr[pos + 2] == '0')
      {
        tempStr.erase(pos + 2, 1);
        parser.str(tempStr);
      }
    }
#endif
    // Add the time steps that actually exist to steps
    // allows the run to be stopped short of controlDict spec
    // allows for removal of timesteps
    if (testdir->Open((this->CasePath + parser.str()).c_str()))
    {
      this->TimeValues->InsertNextValue(tempStep);
      this->TimeNames->InsertNextValue(parser.str());
    }
    // necessary for reading the case/0 directory whatever the timeFormat is
    // based on Foam::Time::operator++() cf. src/OpenFOAM/db/Time/Time.C
    else if ((fabs(tempStep) < 1.0e-14L) // 10*SMALL
      && testdir->Open((this->CasePath + std::string("0")).c_str()))
    {
      this->TimeValues->InsertNextValue(tempStep);
      this->TimeNames->InsertNextValue(std::string("0"));
    }
  }

  this->TimeValues->Squeeze();
  this->TimeNames->Squeeze();

  if (this->TimeValues->GetNumberOfTuples() == 0)
  {
    // set the number of timesteps to 1 if the constant subdirectory exists
    if (testdir->Open((this->CasePath + "constant").c_str()))
    {
      parser.str("");
      parser << startTime;
      this->TimeValues->InsertNextValue(startTime);
      this->TimeValues->Squeeze();
      this->TimeNames->InsertNextValue(parser.str());
      this->TimeNames->Squeeze();
    }
  }
  return true;
}

//------------------------------------------------------------------------------
// List time directories by searching all valid time instances in a
// case directory
bool vtkOpenFOAMReaderPrivate::ListTimeDirectoriesByInstances()
{
  // Open the case directory
  vtkNew<vtkDirectory> testdir;
  if (!testdir->Open(this->CasePath.c_str()))
  {
    vtkErrorMacro(<< "Can't open directory " << this->CasePath);
    return false;
  }

  const bool ignore0Dir = this->Parent->GetSkipZeroTime();

  // search all the directories in the case directory and detect
  // directories with names convertible to numbers
  this->TimeValues->Initialize();
  this->TimeNames->Initialize();
  const vtkIdType nFiles = testdir->GetNumberOfFiles();
  for (vtkIdType filei = 0; filei < nFiles; ++filei)
  {
    const std::string timeName = testdir->GetFile(filei);
    bool isTimeDir = testdir->FileIsDirectory(timeName.c_str());

    if (!isTimeDir || timeName == "." || timeName == ".." || (ignore0Dir && timeName == "0"))
    {
      // Skip files, optionally ignore 0/ directory
      continue;
    }

    // Check if the name is convertible to a number
    for (size_t j = 0; j < timeName.length() && isTimeDir; ++j)
    {
      const char c = timeName[j];
      isTimeDir = (isdigit(c) || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E');
    }
    if (!isTimeDir)
    {
      continue;
    }

    // convert to a number
    char* endptr;
    double timeValue = strtod(timeName.c_str(), &endptr);
    // check if the value really was converted to a number
    if (timeValue == 0.0 && endptr == timeName.c_str())
    {
      continue;
    }

    // add to the instance list
    this->TimeValues->InsertNextValue(timeValue);
    this->TimeNames->InsertNextValue(timeName);
  }

  this->TimeValues->Squeeze();
  this->TimeNames->Squeeze();

  if (this->TimeValues->GetNumberOfTuples() > 1)
  {
    // sort the detected time directories
    vtkSortDataArray::Sort(this->TimeValues, this->TimeNames);

    // if there are duplicated timeValues found, remove duplicates
    // (e.g. "0" and "0.000")
    for (vtkIdType timeI = 1; timeI < this->TimeValues->GetNumberOfTuples(); timeI++)
    {
      // compare by exact match
      if (this->TimeValues->GetValue(timeI - 1) == this->TimeValues->GetValue(timeI))
      {
        vtkWarningMacro(<< "Different time directories with the same time value "
                        << this->TimeNames->GetValue(timeI - 1) << " and "
                        << this->TimeNames->GetValue(timeI) << " found. "
                        << this->TimeNames->GetValue(timeI) << " will be ignored.");
        this->TimeValues->RemoveTuple(timeI);
        // vtkStringArray does not have RemoveTuple()
        for (vtkIdType timeJ = timeI + 1; timeJ < this->TimeNames->GetNumberOfTuples(); timeJ++)
        {
          this->TimeNames->SetValue(timeJ - 1, this->TimeNames->GetValue(timeJ));
        }
        this->TimeNames->Resize(this->TimeNames->GetNumberOfTuples() - 1);
      }
    }
  }

  if (this->TimeValues->GetNumberOfTuples() == 0)
  {
    // set the number of timesteps to 1 if the constant subdirectory exists
    if (testdir->Open((this->CasePath + "constant").c_str()))
    {
      this->TimeValues->InsertNextValue(0.0);
      this->TimeValues->Squeeze();
      this->TimeNames->InsertNextValue("constant");
      this->TimeNames->Squeeze();
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// Gather the necessary information to create a path to the data
bool vtkOpenFOAMReaderPrivate::MakeInformationVector(const std::string& casePath,
  const std::string& controlDictPath, const std::string& procName, vtkOpenFOAMReader* parent,
  bool requirePolyMesh)
{
  vtkFoamDebug(<< "MakeInformationVector (" << this->RegionName << "/" << procName
               << ") polyMesh:" << requirePolyMesh << " - list times\n");

  this->CasePath = casePath;
  this->ProcessorName = procName;
  this->Parent = parent;

  bool ret = false; // Tentative return value

  // List timesteps by directory or predict from controlDict values

  int listByControlDict = this->Parent->GetListTimeStepsByControlDict();
  if (listByControlDict)
  {
    vtkFoamIOobject io(this->CasePath, this->Parent);

    // Open and check if controlDict is readable
    if (!io.Open(controlDictPath))
    {
      vtkErrorMacro(<< "Error opening " << io.GetFileName() << ": " << io.GetError());
      return false;
    }

    vtkFoamDict dict;
    if (!dict.Read(io))
    {
      vtkErrorMacro(<< "Error reading line " << io.GetLineNumber() << " of " << io.GetFileName()
                    << ": " << io.GetError());
      return false;
    }
    if (dict.GetType() != vtkFoamToken::DICTIONARY)
    {
      vtkErrorMacro(<< "The file " << io.GetFileName() << " is not a dictionary");
      return false;
    }

    const vtkFoamEntry* eptr;
    if ((eptr = dict.Lookup("writeControl")) == nullptr)
    {
      vtkErrorMacro(<< "No 'writeControl' in " << io.GetFileName());
      return false;
    }
    const std::string writeControl(eptr->ToString());

    // list time directories according to controlDict.
    // When (adjustTimeStep, writeControl) == (on, adjustableRunTime) or (off, timeStep)
    // list by time instances in the case directory otherwise
    // (different behaviour from paraFoam)

    bool adjustTimeStep = false;
    if ((eptr = dict.Lookup("adjustTimeStep")) != nullptr)
    {
      const std::string sw(eptr->ToString());

      // Switch values for 'true' (cf. src/OpenFOAM/db/Switch/Switch.C)
      adjustTimeStep = (sw == "on" || sw == "yes" || sw == "y" || sw == "true" || sw == "t");
    }

    if (adjustTimeStep ? (writeControl == "adjustableRunTime") : (writeControl == "timeStep"))
    {
      ret = this->ListTimeDirectoriesByControlDict(dict);
    }
    else
    {
      // Cannot list by controlDict, fall through to below
      listByControlDict = false;
    }
  }

  if (!listByControlDict)
  {
    ret = this->ListTimeDirectoriesByInstances();
  }

  if (!ret)
  {
    return ret;
  }

  // does not seem to be required even if number of timesteps reduced
  // upon refresh since ParaView rewinds TimeStep to 0, but for precaution
  if (this->TimeValues->GetNumberOfTuples() > 0)
  {
    if (this->TimeStep >= this->TimeValues->GetNumberOfTuples())
    {
      this->SetTimeStep(static_cast<int>(this->TimeValues->GetNumberOfTuples() - 1));
    }
  }
  else
  {
    this->SetTimeStep(0);
  }

  // Normally expect a (default region) polyMesh/, but not for multi-region cases
  if (requirePolyMesh)
  {
    this->PopulatePolyMeshDirArrays();
  }
  return ret;
}

//------------------------------------------------------------------------------
// Append time value for a mesh points/faces change
void vtkOpenFOAMReaderPrivate::AppendMeshDirToArray(
  vtkStringArray* instances, vtkIdType timeIndex, bool changed)
{
  if (changed)
  {
    // Changed, set to current time instance name
    instances->SetValue(timeIndex, this->TimeNames->GetValue(timeIndex));
  }
  else if (timeIndex > 0)
  {
    // No change, set to previous time instance name
    instances->SetValue(timeIndex, instances->GetValue(timeIndex - 1));
  }
  else
  {
    // No change for the first instance, set to "constant" time instance
    instances->SetValue(timeIndex, "constant");
  }
}

//------------------------------------------------------------------------------
// create a Lookup Table containing the location of the points
// and faces files for each time steps mesh
void vtkOpenFOAMReaderPrivate::PopulatePolyMeshDirArrays()
{
  // initialize size to number of timesteps
  const vtkIdType nTimes = this->TimeValues->GetNumberOfTuples();
  this->PolyMeshPointsDir->SetNumberOfValues(nTimes);
  this->PolyMeshFacesDir->SetNumberOfValues(nTimes);

  for (vtkIdType timei = 0; timei < nTimes; ++timei)
  {
    // The mesh directory for this timestep
    const std::string meshDir(this->TimeRegionPath(timei) + "/polyMesh/");

    const bool hasMeshDir = vtksys::SystemTools::FileIsDirectory(meshDir);
    const bool topoChanged = hasMeshDir && vtkFoamFile::IsFile(meshDir + "faces", true);
    const bool pointsMoved = hasMeshDir && vtkFoamFile::IsFile(meshDir + "points", true);

    AppendMeshDirToArray(this->PolyMeshFacesDir, timei, topoChanged);
    AppendMeshDirToArray(this->PolyMeshPointsDir, timei, pointsMoved);
  }
}

//------------------------------------------------------------------------------
// Read the points file into a vtkFloatArray
//
// - sets NumPoints

vtkFloatArray* vtkOpenFOAMReaderPrivate::ReadPointsFile()
{
  // Assume failure
  this->NumPoints = 0;

  vtkFoamIOobject io(this->CasePath, this->Parent);

  // Read polyMesh/points
  {
    const std::string pointPath =
      this->CurrentTimeRegionMeshPath(this->PolyMeshPointsDir) + "points";

    if (!io.OpenOrGzip(pointPath))
    {
      vtkErrorMacro(<< "Error opening " << io.GetFileName() << ": " << io.GetError());
      return nullptr;
    }
  }

  vtkFloatArray* pointArray = nullptr;

  try
  {
    vtkFoamEntryValue dict(nullptr);

    if (io.IsFloat64())
    {
      dict.ReadNonuniformList<vtkFoamToken::VECTORLIST,
        vtkFoamEntryValue::vectorListTraits<vtkFloatArray, double, 3, false>>(io);
    }
    else
    {
      dict.ReadNonuniformList<vtkFoamToken::VECTORLIST,
        vtkFoamEntryValue::vectorListTraits<vtkFloatArray, float, 3, false>>(io);
    }

    pointArray = static_cast<vtkFloatArray*>(dict.Ptr());
  }
  catch (const vtkFoamError& err)
  {
    vtkErrorMacro("Mesh points data are neither 32 nor 64 bit, or some other "
                  "parse error occurred while reading points. Failed at line "
      << io.GetLineNumber() << " of " << io.GetFileName() << ": " << err);
    return nullptr;
  }

  assert(pointArray);

  // Set the number of points
  this->NumPoints = pointArray->GetNumberOfTuples();

  return pointArray;
}

//------------------------------------------------------------------------------
// Read the faces into a vtkFoamLabelListList
//
// - sets NumFaces, clears NumInternalFaces
//
// Return meshFaces

std::unique_ptr<vtkFoamLabelListList> vtkOpenFOAMReaderPrivate::ReadFacesFile(
  const std::string& meshDir)
{
  // Assume failure
  this->NumFaces = 0;
  this->NumInternalFaces = 0;

  vtkFoamIOobject io(this->CasePath, this->Parent);

  // Read polyMesh/faces
  {
    const std::string facePath(meshDir + "faces");
    if (!io.OpenOrGzip(facePath))
    {
      vtkErrorMacro(<< "Error opening " << io.GetFileName() << ": " << io.GetError()
                    << ". If you are trying to read a parallel decomposed case, "
                       "set Case Type to Decomposed Case.");
      return nullptr;
    }
  }

  vtkFoamEntryValue dict(nullptr);
  dict.SetStreamOption(io);
  try
  {
    if (io.GetClassName() == "faceCompactList")
    {
      dict.ReadCompactLabelListList(io);
    }
    else
    {
      dict.ReadLabelListList(io);
    }
  }
  catch (const vtkFoamError& err)
  {
    vtkErrorMacro(<< "Error reading line " << io.GetLineNumber() << " of " << io.GetFileName()
                  << ": " << err);
    return nullptr;
  }

  std::unique_ptr<vtkFoamLabelListList> meshFaces(static_cast<vtkFoamLabelListList*>(dict.Ptr()));

  if (meshFaces)
  {
    this->NumFaces = meshFaces->GetNumberOfElements();
  }

  return meshFaces;
}

//------------------------------------------------------------------------------
// Read owner, neighbour files and create meshCells
//
// - sets NumFaces and NumInternalFaces, and NumCells
//
// Return meshCells

std::unique_ptr<vtkFoamLabelListList> vtkOpenFOAMReaderPrivate::ReadOwnerNeighbourFiles(
  const std::string& meshDir)
{
  // Assume failure
  this->NumCells = 0;

  vtkFoamIOobject io(this->CasePath, this->Parent);

  // Read polyMesh/owner
  {
    const std::string ownPath(meshDir + "owner");
    if (!io.OpenOrGzip(ownPath))
    {
      vtkErrorMacro(<< "Error opening " << io.GetFileName() << ": " << io.GetError());
      return nullptr;
    }
  }
  const bool use64BitLabels = io.IsLabel64();

  {
    vtkFoamEntryValue ownerDict(nullptr);
    ownerDict.SetStreamOption(io);
    try
    {
      if (use64BitLabels)
      {
        ownerDict.ReadNonuniformList<vtkFoamEntryValue::LABELLIST,
          vtkFoamEntryValue::listTraits<vtkTypeInt64Array, vtkTypeInt64>>(io);
      }
      else
      {
        ownerDict.ReadNonuniformList<vtkFoamEntryValue::LABELLIST,
          vtkFoamEntryValue::listTraits<vtkTypeInt32Array, vtkTypeInt32>>(io);
      }
    }
    catch (const vtkFoamError& err)
    {
      vtkErrorMacro(<< "Error reading line " << io.GetLineNumber() << " of " << io.GetFileName()
                    << ": " << err);
      return nullptr;
    }
    io.Close();

    this->FaceOwner = static_cast<vtkDataArray*>(ownerDict.Ptr());
  }

  // Read polyMesh/neighbour
  {
    const std::string neiPath(meshDir + "neighbour");
    if (!io.OpenOrGzip(neiPath))
    {
      vtkErrorMacro(<< "Error opening " << io.GetFileName() << ": " << io.GetError());
      return nullptr;
    }
  }

  if (use64BitLabels != io.IsLabel64())
  {
    vtkErrorMacro(<< "owner/neighbour with different label-size: should not happen"
                  << io.GetCasePath());
    return nullptr;
  }

  {
    vtkFoamEntryValue neighDict(nullptr);
    neighDict.SetStreamOption(io);
    try
    {
      if (use64BitLabels)
      {
        neighDict.ReadNonuniformList<vtkFoamEntryValue::LABELLIST,
          vtkFoamEntryValue::listTraits<vtkTypeInt64Array, vtkTypeInt64>>(io);
      }
      else
      {
        neighDict.ReadNonuniformList<vtkFoamEntryValue::LABELLIST,
          vtkFoamEntryValue::listTraits<vtkTypeInt32Array, vtkTypeInt32>>(io);
      }
    }
    catch (const vtkFoamError& err)
    {
      vtkErrorMacro(<< "Error reading line " << io.GetLineNumber() << " of " << io.GetFileName()
                    << ": " << err);
      return nullptr;
    }
    io.Close();

    this->FaceNeigh = static_cast<vtkDataArray*>(neighDict.Ptr());
  }

  // Basic checks
  const vtkDataArray& faceOwner = *this->FaceOwner;
  const vtkDataArray& faceNeigh = *this->FaceNeigh;

  const vtkIdType nFaces = faceOwner.GetNumberOfTuples();
  const vtkIdType nInternalFaces = faceNeigh.GetNumberOfTuples();

  if (nFaces < nInternalFaces)
  {
    vtkErrorMacro(<< "Number of owner faces " << nFaces
                  << " not equal or greater than number of neighbour faces " << nInternalFaces);
    return nullptr;
  }

  // Set or check number of mesh faces
  if (this->NumFaces == 0)
  {
    this->NumFaces = nFaces;
  }
  else if (this->NumFaces != nFaces)
  {
    vtkErrorMacro(<< "Expected " << this->NumFaces << " faces, but owner had " << nFaces
                  << " faces");
    return nullptr;
  }
  this->NumInternalFaces = nInternalFaces;

  // The cell-faces
  return this->CreateCellFaces(faceOwner, faceNeigh);
}

//------------------------------------------------------------------------------
// Create meshCells from owner/neighbour information
//
// - sets NumFaces and NumInternalFaces (again), and NumCells
//
// Return meshCells
std::unique_ptr<vtkFoamLabelListList> vtkOpenFOAMReaderPrivate::CreateCellFaces(
  const vtkDataArray& faceOwner, const vtkDataArray& faceNeigh)
{
  // Reset
  this->NumCells = 0;

  const bool use64BitLabels = ::Is64BitArray(&faceOwner);

  const vtkIdType nFaces = faceOwner.GetNumberOfTuples();
  const vtkIdType nInternalFaces = faceNeigh.GetNumberOfTuples();

  // Extra safety
  this->NumFaces = nFaces;
  this->NumInternalFaces = nInternalFaces;

  // Determine the number of cells and number of cell faces (total)
  vtkTypeInt64 nCells = -1;
  vtkTypeInt64 nTotalCellFaces = 0;

  for (vtkIdType facei = 0; facei < nInternalFaces; ++facei)
  {
    const vtkTypeInt64 own = GetLabelValue(&faceOwner, facei, use64BitLabels);
    if (own >= 0)
    {
      ++nTotalCellFaces;
      if (nCells < own)
      {
        nCells = own; // <- max(nCells, own)
      }
    }

    const vtkTypeInt64 nei = GetLabelValue(&faceNeigh, facei, use64BitLabels);
    if (nei >= 0)
    {
      ++nTotalCellFaces;
      if (nCells < nei)
      {
        nCells = nei; // <- max(nCells, nei)
      }
    }
  }

  for (vtkIdType facei = nInternalFaces; facei < nFaces; ++facei)
  {
    const vtkTypeInt64 own = GetLabelValue(&faceOwner, facei, use64BitLabels);
    if (own >= 0)
    {
      ++nTotalCellFaces;
      if (nCells < own)
      {
        nCells = own; // <- max(nCells, own)
      }
    }
  }
  ++nCells;

  if (nCells == 0)
  {
    vtkWarningMacro(<< "The mesh contains no cells");
  }

  // Set the number of cells
  this->NumCells = static_cast<vtkIdType>(nCells);

  // Create meshCells. Avoid 32bit overflow for nTotalCellFaces
  std::unique_ptr<vtkFoamLabelListList> meshCells;
  if (use64BitLabels || (VTK_TYPE_INT32_MAX < nTotalCellFaces))
  {
    meshCells.reset(new vtkFoamLabelListList64);
  }
  else
  {
    meshCells.reset(new vtkFoamLabelListList32);
  }
  auto& cells = *meshCells;

  cells.ResizeExact(nCells, nTotalCellFaces);
  cells.ResetOffsets(); // Fill offsets with zero

  // Count number of faces for each cell
  // Establish the per-cell face count
  {
    // Accumulate offsets into slot *above*
    constexpr vtkIdType cellIndexOffset = 1;

    for (vtkIdType facei = 0; facei < nInternalFaces; ++facei)
    {
      const vtkTypeInt64 own = GetLabelValue(&faceOwner, facei, use64BitLabels);
      if (own >= 0)
      {
        cells.IncrementOffset(cellIndexOffset + own);
      }

      const vtkTypeInt64 nei = GetLabelValue(&faceNeigh, facei, use64BitLabels);
      if (nei >= 0)
      {
        cells.IncrementOffset(cellIndexOffset + nei);
      }
    }

    for (vtkIdType facei = nInternalFaces; facei < nFaces; ++facei)
    {
      const vtkTypeInt64 own = GetLabelValue(&faceOwner, facei, use64BitLabels);
      if (own >= 0)
      {
        cells.IncrementOffset(cellIndexOffset + own);
      }
    }

    // Reduce per-cell face count -> start offsets
    vtkTypeInt64 currOffset = 0;
    for (vtkIdType celli = 1; celli <= nCells; ++celli)
    {
      currOffset += cells.GetBeginOffset(celli);
      cells.SetOffset(celli, currOffset);
    }
  }

  // Deep copy of offsets into a temporary array
  std::unique_ptr<vtkFoamLabelListList> tmpAddr;
  if (cells.IsLabel64())
  {
    tmpAddr.reset(new vtkFoamLabelListList64);
  }
  else
  {
    tmpAddr.reset(new vtkFoamLabelListList32);
  }
  tmpAddr->ResizeExact(nCells, 1);
  tmpAddr->GetOffsetsArray()->DeepCopy(cells.GetOffsetsArray());

  // Add face numbers to cell-faces list, using tmpAddr offsets to manage the locations
  for (vtkIdType facei = 0; facei < nInternalFaces; ++facei)
  {
    const vtkTypeInt64 own = GetLabelValue(&faceOwner, facei, use64BitLabels);
    if (own >= 0)
    {
      const vtkTypeInt64 next = tmpAddr->GetBeginOffset(own);
      tmpAddr->IncrementOffset(own);
      cells.SetValue(next, facei);
    }

    const vtkTypeInt64 nei = GetLabelValue(&faceNeigh, facei, use64BitLabels);
    if (nei >= 0)
    {
      const vtkTypeInt64 next = tmpAddr->GetBeginOffset(nei);
      tmpAddr->IncrementOffset(nei);
      cells.SetValue(next, facei);
    }
  }

  for (vtkIdType facei = nInternalFaces; facei < nFaces; ++facei)
  {
    const vtkTypeInt64 own = GetLabelValue(&faceOwner, facei, use64BitLabels);
    if (own >= 0)
    {
      const vtkTypeInt64 next = tmpAddr->GetBeginOffset(own);
      tmpAddr->IncrementOffset(own);
      cells.SetValue(next, facei);
    }
  }

  return meshCells;
}

//------------------------------------------------------------------------------
bool vtkOpenFOAMReaderPrivate::CheckFaceList(const vtkFoamLabelListList& faces)
{
  const vtkIdType nFaces = faces.GetNumberOfElements();
  const vtkIdType nPoints = this->NumPoints;

  vtkFoamLabelListList::CellType face;
  for (vtkIdType facei = 0; facei < nFaces; ++facei)
  {
    faces.GetCell(facei, face);

    if (face.size() < 3)
    {
      vtkErrorMacro(<< "Face " << facei << " is bad. Has " << face.size()
                    << " points but requires 3 or more");
      return false;
    }

    for (const vtkTypeInt64 pointi : face)
    {
      if (pointi < 0 || pointi >= nPoints)
      {
        vtkErrorMacro(<< "Face " << facei << " is bad. Point " << pointi
                      << " out of range: " << nPoints << " points");
        return false;
      }
    }
  }
  return true;
}

//------------------------------------------------------------------------------
// determine cell shape and insert the cell into the mesh
// hexahedron, prism, pyramid, tetrahedron and decompose polyhedron
void vtkOpenFOAMReaderPrivate::InsertCellsToGrid(vtkUnstructuredGrid* internalMesh,
  const vtkFoamLabelListList& meshCells, const vtkFoamLabelListList& meshFaces,
  vtkFloatArray* pointArray, vtkIdTypeArray* additionalCells, vtkDataArray* cellList)
{
  const bool cellList64Bit = ::Is64BitArray(cellList);
  const bool faceOwner64Bit = ::Is64BitArray(this->FaceOwner);

  // Scratch array for inserting primitive cell points
  constexpr vtkIdType maxNPoints = 256; // max num of points per cell
  auto cellPoints = vtkSmartPointer<vtkIdList>::New();
  cellPoints->SetNumberOfIds(maxNPoints);

  // Scratch array for inserting polyhedral faces and sizes
  constexpr vtkIdType maxNPolyPoints = 1024; // max num of nPoints per face + points per cell
  auto polyPoints = vtkSmartPointer<vtkIdList>::New();
  polyPoints->SetNumberOfIds(maxNPolyPoints);

  // Scratch array for analyzing cell types (cell shape)
  vtkFoamLabelListList::CellType cellFaces;

  const vtkIdType nCells = (cellList == nullptr ? this->NumCells : cellList->GetNumberOfTuples());

  // Local variable for polyhedral decomposition
  vtkIdType nAdditionalPoints = 0;

  vtkSmartPointer<vtkIdTypeArray> arrayId;
  if (cellList)
  {
    if (additionalCells) // sanity check
    {
      vtkErrorMacro(<< "Decompose polyhedral is not supported on mesh subset");
      return;
    }

    // create array holding cell id only on zone mesh
    arrayId = vtkSmartPointer<vtkIdTypeArray>::New();
    arrayId->SetName("CellId");
    arrayId->SetNumberOfTuples(nCells);
    internalMesh->GetCellData()->AddArray(arrayId);
  }

  for (vtkIdType cellI = 0; cellI < nCells; cellI++)
  {
    vtkIdType cellId = cellI;
    if (cellList != nullptr)
    {
      cellId = GetLabelValue(cellList, cellI, cellList64Bit);
      if (cellId >= this->NumCells)
      {
        vtkWarningMacro(<< "cellLabels id " << cellId << " exceeds the number of cells " << nCells
                        << ". Inserting an empty cell.");
        internalMesh->InsertNextCell(VTK_EMPTY_CELL, 0, cellPoints->GetPointer(0));
        continue;
      }
      arrayId->SetValue(cellI, cellId);
    }

    meshCells.GetCell(cellId, cellFaces);

    // determine type of the cell
    // cf. src/OpenFOAM/meshes/meshShapes/cellMatcher/{hex|prism|pyr|tet}-
    // Matcher.C
    int cellType = VTK_POLYHEDRON; // Fallback value
    if (cellFaces.size() == 6)
    {
      bool allQuads = false;
      for (size_t facei = 0; facei < cellFaces.size(); ++facei)
      {
        allQuads = (meshFaces.GetSize(cellFaces[facei]) == 4);
        if (!allQuads)
        {
          break;
        }
      }
      if (allQuads)
      {
        cellType = VTK_HEXAHEDRON;
      }
    }
    else if (cellFaces.size() == 5)
    {
      int nTris = 0, nQuads = 0;
      for (size_t facei = 0; facei < cellFaces.size(); ++facei)
      {
        const vtkIdType nPoints = meshFaces.GetSize(cellFaces[facei]);
        if (nPoints == 3)
        {
          ++nTris;
        }
        else if (nPoints == 4)
        {
          ++nQuads;
        }
        else
        {
          break;
        }
      }
      if (nTris == 2 && nQuads == 3)
      {
        cellType = VTK_WEDGE;
      }
      else if (nTris == 4 && nQuads == 1)
      {
        cellType = VTK_PYRAMID;
      }
    }
    else if (cellFaces.size() == 4)
    {
      bool allTris = false;
      for (size_t facei = 0; facei < cellFaces.size(); ++facei)
      {
        allTris = (meshFaces.GetSize(cellFaces[facei]) == 3);
        if (!allTris)
        {
          break;
        }
      }
      if (allTris)
      {
        cellType = VTK_TETRA;
      }
    }

    // Not a known (standard) primitive mesh-shape
    if (cellType == VTK_POLYHEDRON)
    {
      bool allEmpty = true;
      for (size_t facei = 0; facei < cellFaces.size(); ++facei)
      {
        allEmpty = (meshFaces.GetSize(cellFaces[facei]) == 0);
        if (!allEmpty)
        {
          break;
        }
      }
      if (allEmpty)
      {
        cellType = VTK_EMPTY_CELL;
      }
    }

    // Cell shape constructor based on the one implementd by Terry
    // Jordan, with lots of improvements. Not as elegant as the one in
    // OpenFOAM but it's simple and works reasonably fast.

    // OFhex | vtkHexahedron
    if (cellType == VTK_HEXAHEDRON)
    {
      // get first face in correct order
      vtkTypeInt64 cellBaseFaceId = cellFaces[0];
      vtkFoamLabelListList::CellType face0Points;
      meshFaces.GetCell(cellBaseFaceId, face0Points);

      if (GetLabelValue(this->FaceOwner, cellBaseFaceId, faceOwner64Bit) == cellId)
      {
        // if it is an owner face flip the points
        for (int j = 0; j < 4; j++)
        {
          cellPoints->SetId(j, face0Points[3 - j]);
        }
      }
      else
      {
        // add base face to cell points
        for (int j = 0; j < 4; j++)
        {
          cellPoints->SetId(j, face0Points[j]);
        }
      }
      vtkIdType baseFacePoint0 = cellPoints->GetId(0);
      vtkIdType baseFacePoint2 = cellPoints->GetId(2);
      vtkTypeInt64 cellOppositeFaceI = -1, pivotPoint = -1;
      vtkTypeInt64 dupPoint = -1;
      vtkFoamLabelListList::CellType faceIPoints;
      for (int faceI = 1; faceI < 5; faceI++) // skip face 0 and 5
      {
        vtkTypeInt64 cellFaceI = cellFaces[faceI];
        meshFaces.GetCell(cellFaceI, faceIPoints);
        int foundDup = -1, pointI = 0;
        for (; pointI < 4; pointI++) // each point
        {
          vtkTypeInt64 faceIPointI = faceIPoints[pointI];
          // matching two points in base face is enough to find a
          // duplicated point since neighboring faces share two
          // neighboring points (i. e. an edge)
          if (baseFacePoint0 == faceIPointI)
          {
            foundDup = 0;
            break;
          }
          else if (baseFacePoint2 == faceIPointI)
          {
            foundDup = 2;
            break;
          }
        }
        if (foundDup >= 0)
        {
          // find the pivot point if still haven't
          if (pivotPoint == -1)
          {
            dupPoint = foundDup;

            vtkTypeInt64 faceINextPoint = faceIPoints[(pointI + 1) % 4];

            // if the next point of the faceI-th face matches the
            // previous point of the base face use the previous point
            // of the faceI-th face as the pivot point; or use the
            // next point otherwise
            if (faceINextPoint ==
              (GetLabelValue(this->FaceOwner, cellFaceI, faceOwner64Bit) == cellId
                  ? cellPoints->GetId(1 + foundDup)
                  : cellPoints->GetId(3 - foundDup)))
            {
              pivotPoint = faceIPoints[(3 + pointI) % 4];
            }
            else
            {
              pivotPoint = faceINextPoint;
            }

            if (cellOppositeFaceI >= 0)
            {
              break;
            }
          }
        }
        else
        {
          // if no duplicated point found, faceI is the opposite face
          cellOppositeFaceI = cellFaceI;

          if (pivotPoint >= 0)
          {
            break;
          }
        }
      }

      // if the opposite face is not found until face 4, face 5 is
      // always the opposite face
      if (cellOppositeFaceI == -1)
      {
        cellOppositeFaceI = cellFaces[5];
      }

      // find the pivot point in opposite face
      vtkFoamLabelListList::CellType oppositeFacePoints;
      meshFaces.GetCell(cellOppositeFaceI, oppositeFacePoints);
      int pivotPointI = 0;
      for (; pivotPointI < 4; pivotPointI++)
      {
        if (oppositeFacePoints[pivotPointI] == pivotPoint)
        {
          break;
        }
      }

      // shift the pivot point if the point corresponds to point 2
      // of the base face
      if (dupPoint == 2)
      {
        pivotPointI = (pivotPointI + 2) % 4;
      }
      // copy the face-point list of the opposite face to cell-point list
      int basePointI = 4;
      if (GetLabelValue(this->FaceOwner, cellOppositeFaceI, faceOwner64Bit) == cellId)
      {
        for (int pointI = pivotPointI; pointI < 4; pointI++)
        {
          cellPoints->SetId(basePointI++, oppositeFacePoints[pointI]);
        }
        for (int pointI = 0; pointI < pivotPointI; pointI++)
        {
          cellPoints->SetId(basePointI++, oppositeFacePoints[pointI]);
        }
      }
      else
      {
        for (int pointI = pivotPointI; pointI >= 0; pointI--)
        {
          cellPoints->SetId(basePointI++, oppositeFacePoints[pointI]);
        }
        for (int pointI = 3; pointI > pivotPointI; pointI--)
        {
          cellPoints->SetId(basePointI++, oppositeFacePoints[pointI]);
        }
      }

      // create the hex cell and insert it into the mesh
      internalMesh->InsertNextCell(cellType, 8, cellPoints->GetPointer(0));
    }

    // the cell construction is about the same as that of a hex, but
    // the point ordering have to be reversed!!
    else if (cellType == VTK_WEDGE)
    {
      // find the base face number
      int baseFaceId = 0;
      for (int j = 0; j < 5; j++)
      {
        if (meshFaces.GetSize(cellFaces[j]) == 3)
        {
          baseFaceId = j;
          break;
        }
      }

      // get first face in correct order
      vtkTypeInt64 cellBaseFaceId = cellFaces[baseFaceId];
      vtkFoamLabelListList::CellType face0Points;
      meshFaces.GetCell(cellBaseFaceId, face0Points);

      if (GetLabelValue(this->FaceOwner, cellBaseFaceId, faceOwner64Bit) == cellId)
      {
        for (int j = 0; j < 3; j++)
        {
          cellPoints->SetId(j, face0Points[j]);
        }
      }
      else
      {
        // if it is a neighbor face flip the points
        for (int j = 0; j < 3; j++)
        {
          // add base face to cell points
          cellPoints->SetId(j, face0Points[2 - j]);
        }
      }
      vtkIdType baseFacePoint0 = cellPoints->GetId(0);
      vtkIdType baseFacePoint2 = cellPoints->GetId(2);
      vtkTypeInt64 cellOppositeFaceI = -1, pivotPoint = -1;
      bool dupPoint2 = false;
      vtkFoamLabelListList::CellType faceIPoints;
      for (int faceI = 0; faceI < 5; faceI++)
      {
        if (faceI == baseFaceId)
        {
          continue;
        }
        vtkTypeInt64 cellFaceI = cellFaces[faceI];
        if (meshFaces.GetSize(cellFaceI) == 3)
        {
          cellOppositeFaceI = cellFaceI;
        }
        // find the pivot point if still haven't
        else if (pivotPoint == -1)
        {
          meshFaces.GetCell(cellFaceI, faceIPoints);
          bool found0Dup = false;
          int pointI = 0;
          for (; pointI < 4; pointI++) // each point
          {
            vtkTypeInt64 faceIPointI = faceIPoints[pointI];
            // matching two points in base face is enough to find a
            // duplicated point since neighboring faces share two
            // neighboring points (i. e. an edge)
            if (baseFacePoint0 == faceIPointI)
            {
              found0Dup = true;
              break;
            }
            else if (baseFacePoint2 == faceIPointI)
            {
              break;
            }
          }
          // the matching point must always be found so omit the check
          vtkIdType baseFacePrevPoint, baseFaceNextPoint;
          if (found0Dup)
          {
            baseFacePrevPoint = cellPoints->GetId(2);
            baseFaceNextPoint = cellPoints->GetId(1);
          }
          else
          {
            baseFacePrevPoint = cellPoints->GetId(1);
            baseFaceNextPoint = cellPoints->GetId(0);
            dupPoint2 = true;
          }

          vtkTypeInt64 faceINextPoint = faceIPoints[(pointI + 1) % 4];
          vtkTypeInt64 faceIPrevPoint = faceIPoints[(3 + pointI) % 4];

          // if the next point of the faceI-th face matches the
          // previous point of the base face use the previous point of
          // the faceI-th face as the pivot point; or use the next
          // point otherwise
          vtkTypeInt64 faceOwnerVal = GetLabelValue(this->FaceOwner, cellFaceI, faceOwner64Bit);
          if (faceINextPoint == (faceOwnerVal == cellId ? baseFacePrevPoint : baseFaceNextPoint))
          {
            pivotPoint = faceIPrevPoint;
          }
          else
          {
            pivotPoint = faceINextPoint;
          }
        }

        // break when both of opposite face and pivot point are found
        if (cellOppositeFaceI >= 0 && pivotPoint >= 0)
        {
          break;
        }
      }

      // find the pivot point in opposite face
      vtkFoamLabelListList::CellType oppositeFacePoints;
      meshFaces.GetCell(cellOppositeFaceI, oppositeFacePoints);
      int pivotPointI = 0;
      for (; pivotPointI < 3; pivotPointI++)
      {
        if (oppositeFacePoints[pivotPointI] == pivotPoint)
        {
          break;
        }
      }
      if (pivotPointI != 3)
      {
        // We have found a pivot. We can process cell as a wedge
        vtkTypeInt64 faceOwnerVal =
          GetLabelValue(this->FaceOwner, static_cast<vtkIdType>(cellOppositeFaceI), faceOwner64Bit);
        if (faceOwnerVal == cellId)
        {
          if (dupPoint2)
          {
            pivotPointI = (pivotPointI + 2) % 3;
          }
          int basePointI = 3;
          for (int pointI = pivotPointI; pointI >= 0; pointI--)
          {
            cellPoints->SetId(basePointI++, oppositeFacePoints[pointI]);
          }
          for (int pointI = 2; pointI > pivotPointI; pointI--)
          {
            cellPoints->SetId(basePointI++, oppositeFacePoints[pointI]);
          }
        }
        else
        {
          // shift the pivot point if the point corresponds to point 2
          // of the base face
          if (dupPoint2)
          {
            pivotPointI = (1 + pivotPointI) % 3;
          }
          // copy the face-point list of the opposite face to cell-point list
          int basePointI = 3;
          for (int pointI = pivotPointI; pointI < 3; pointI++)
          {
            cellPoints->SetId(basePointI++, oppositeFacePoints[pointI]);
          }
          for (int pointI = 0; pointI < pivotPointI; pointI++)
          {
            cellPoints->SetId(basePointI++, oppositeFacePoints[pointI]);
          }
        }

        // create the wedge cell and insert it into the mesh
        internalMesh->InsertNextCell(cellType, 6, cellPoints->GetPointer(0));
      }
      else
      {
        // We did not find a pivot: this cell was suspected to be a wedge but it
        // is not. Let's process it like a polyhedron instead.
        cellType = VTK_POLYHEDRON;
      }
    }

    // OFpyramid | vtkPyramid || OFtet | vtkTetrahedron
    else if (cellType == VTK_PYRAMID || cellType == VTK_TETRA)
    {
      const vtkIdType nPoints = (cellType == VTK_PYRAMID ? 5 : 4);
      size_t baseFaceId = 0;
      if (cellType == VTK_PYRAMID)
      {
        // Find the pyramid base
        for (size_t j = 0; j < cellFaces.size(); j++)
        {
          if (meshFaces.GetSize(cellFaces[j]) == 4)
          {
            baseFaceId = j;
            break;
          }
        }
      }

      const vtkTypeInt64 cellBaseFaceId = cellFaces[baseFaceId];
      vtkFoamLabelListList::CellType baseFacePoints;
      meshFaces.GetCell(cellBaseFaceId, baseFacePoints);

      // Take any adjacent (non-base) face
      const size_t adjacentFaceId = (baseFaceId ? 0 : 1);
      const vtkTypeInt64 cellAdjacentFaceId = cellFaces[adjacentFaceId];

      vtkFoamLabelListList::CellType adjacentFacePoints;
      meshFaces.GetCell(cellAdjacentFaceId, adjacentFacePoints);

      // Find the apex point (non-common to the base)
      // initialize with anything
      // - if the search really fails, we have much bigger problems anyhow
      vtkIdType apexPointI = adjacentFacePoints[0];
      for (size_t ptI = 0; ptI < adjacentFacePoints.size(); ++ptI)
      {
        apexPointI = adjacentFacePoints[ptI];
        bool foundDup = false;
        for (size_t baseI = 0; baseI < baseFacePoints.size(); ++baseI)
        {
          foundDup = (apexPointI == baseFacePoints[baseI]);
          if (foundDup)
          {
            break;
          }
        }
        if (!foundDup)
        {
          break;
        }
      }

      // Add base-face points (in order) to cell points
      if (GetLabelValue(this->FaceOwner, cellBaseFaceId, faceOwner64Bit) == cellId)
      {
        // if it is an owner face, flip the points (to point inwards)
        for (vtkIdType j = 0; j < static_cast<vtkIdType>(baseFacePoints.size()); ++j)
        {
          cellPoints->SetId(j, baseFacePoints[baseFacePoints.size() - 1 - j]);
        }
      }
      else
      {
        for (vtkIdType j = 0; j < static_cast<vtkIdType>(baseFacePoints.size()); ++j)
        {
          cellPoints->SetId(j, baseFacePoints[j]);
        }
      }

      // ... and add the apex-point
      cellPoints->SetId(nPoints - 1, apexPointI);

      // create the tetra or pyramid cell and insert it into the mesh
      internalMesh->InsertNextCell(cellType, nPoints, cellPoints->GetPointer(0));
    }

    // erroneous cells
    else if (cellType == VTK_EMPTY_CELL)
    {
      vtkWarningMacro("Warning: No points in cellId " << cellId);
      internalMesh->InsertNextCell(VTK_EMPTY_CELL, 0, cellPoints->GetPointer(0));
    }

    // OFpolyhedron || vtkPolyhedron
    if (cellType == VTK_POLYHEDRON)
    {
      if (additionalCells != nullptr) // decompose into tets and pyramids
      {
        // calculate cell centroid and insert it to point list
        vtkDataArray* polyCellPoints;
        if (cellList64Bit)
        {
          polyCellPoints = vtkTypeInt64Array::New();
        }
        else
        {
          polyCellPoints = vtkTypeInt32Array::New();
        }
        this->AdditionalCellPoints->push_back(polyCellPoints);
        float centroid[3];
        centroid[0] = centroid[1] = centroid[2] = 0.0F;
        for (size_t j = 0; j < cellFaces.size(); j++)
        {
          // remove duplicate points from faces
          vtkTypeInt64 cellFacesJ = cellFaces[j];
          vtkFoamLabelListList::CellType faceJPoints;
          meshFaces.GetCell(cellFacesJ, faceJPoints);
          for (size_t k = 0; k < faceJPoints.size(); k++)
          {
            vtkTypeInt64 faceJPointK = faceJPoints[k];
            bool foundDup = false;
            for (vtkIdType l = 0; l < polyCellPoints->GetDataSize(); l++)
            {
              vtkTypeInt64 polyCellPoint = GetLabelValue(polyCellPoints, l, cellList64Bit);
              if (polyCellPoint == faceJPointK)
              {
                foundDup = true;
                break; // look no more
              }
            }
            if (!foundDup)
            {
              AppendLabelValue(polyCellPoints, faceJPointK, cellList64Bit);
              float* pointK = pointArray->GetPointer(3 * faceJPointK);
              centroid[0] += pointK[0];
              centroid[1] += pointK[1];
              centroid[2] += pointK[2];
            }
          }
        }
        polyCellPoints->Squeeze();
        const float weight = 1.0F / static_cast<float>(polyCellPoints->GetDataSize());
        centroid[0] *= weight;
        centroid[1] *= weight;
        centroid[2] *= weight;
        pointArray->InsertNextTuple(centroid);

        // polyhedron decomposition.
        // a tweaked algorithm based on applications/utilities/postProcessing/
        // graphics/PVFoamReader/vtkFoam/vtkFoamAddInternalMesh.C
        bool insertDecomposedCell = true;
        int nAdditionalCells = 0;
        for (size_t j = 0; j < cellFaces.size(); j++)
        {
          vtkTypeInt64 cellFacesJ = cellFaces[j];
          vtkFoamLabelListList::CellType faceJPoints;
          meshFaces.GetCell(cellFacesJ, faceJPoints);
          vtkTypeInt64 faceOwnerValue = GetLabelValue(this->FaceOwner, cellFacesJ, faceOwner64Bit);
          int flipNeighbor = (faceOwnerValue == cellId ? -1 : 1);
          size_t nTris = faceJPoints.size() % 2;

          size_t vertI = 2;

          // shift the start and end of the vertex loop if the
          // triangle of a decomposed face is going to be flat. Far
          // from perfect but better than nothing to avoid flat cells
          // which stops time integration of Stream Tracer especially
          // for split-hex unstructured meshes created by
          // e. g. autoRefineMesh
          if (faceJPoints.size() >= 5 && nTris)
          {
            float *point0, *point1, *point2;
            point0 = pointArray->GetPointer(3 * faceJPoints[faceJPoints.size() - 1]);
            point1 = pointArray->GetPointer(3 * faceJPoints[0]);
            point2 = pointArray->GetPointer(3 * faceJPoints[faceJPoints.size() - 2]);
            float vsizeSqr1 = 0.0F, vsizeSqr2 = 0.0F, dotProduct = 0.0F;
            for (int i = 0; i < 3; i++)
            {
              const float v1 = point1[i] - point0[i], v2 = point2[i] - point0[i];
              vsizeSqr1 += v1 * v1;
              vsizeSqr2 += v2 * v2;
              dotProduct += v1 * v2;
            }
            // compare in squared representation to avoid using sqrt()
            if (dotProduct * (float)fabs(dotProduct) / (vsizeSqr1 * vsizeSqr2) < -1.0F + 1.0e-3F)
            {
              vertI = 1;
            }
          }

          cellPoints->SetId(0,
            faceJPoints[(vertI == 2) ? static_cast<vtkIdType>(0)
                                     : static_cast<vtkIdType>(faceJPoints.size() - 1)]);
          cellPoints->SetId(4, static_cast<vtkIdType>(this->NumPoints + nAdditionalPoints));

          // decompose a face into quads in order (flipping the
          // decomposed face if owner)
          size_t nQuadVerts = faceJPoints.size() - 1 - nTris;
          for (; vertI < nQuadVerts; vertI += 2)
          {
            cellPoints->SetId(1, faceJPoints[vertI - flipNeighbor]);
            cellPoints->SetId(2, faceJPoints[vertI]);
            cellPoints->SetId(3, faceJPoints[vertI + flipNeighbor]);

            // if the decomposed cell is the first one insert it to
            // the original position; or append to the decomposed cell
            // list otherwise
            if (insertDecomposedCell)
            {
              internalMesh->InsertNextCell(VTK_PYRAMID, 5, cellPoints->GetPointer(0));
              insertDecomposedCell = false;
            }
            else
            {
              nAdditionalCells++;
              additionalCells->InsertNextTypedTuple(cellPoints->GetPointer(0));
            }
          }

          // if the number of vertices is odd there's a triangle
          if (nTris)
          {
            if (flipNeighbor == -1)
            {
              cellPoints->SetId(1, faceJPoints[vertI]);
              cellPoints->SetId(2, faceJPoints[vertI - 1]);
            }
            else
            {
              cellPoints->SetId(1, faceJPoints[vertI - 1]);
              cellPoints->SetId(2, faceJPoints[vertI]);
            }
            cellPoints->SetId(3, static_cast<vtkIdType>(this->NumPoints + nAdditionalPoints));

            if (insertDecomposedCell)
            {
              internalMesh->InsertNextCell(VTK_TETRA, 4, cellPoints->GetPointer(0));
              insertDecomposedCell = false;
            }
            else
            {
              // set the 5th vertex number to -1 to distinguish a tetra cell
              cellPoints->SetId(4, -1);
              nAdditionalCells++;
              additionalCells->InsertNextTypedTuple(cellPoints->GetPointer(0));
            }
          }
        }

        nAdditionalPoints++;
        this->AdditionalCellIds->InsertNextValue(cellId);
        this->NumAdditionalCells->InsertNextValue(nAdditionalCells);
        this->NumTotalAdditionalCells += nAdditionalCells;
      }
      else // don't decompose; use VTK_POLYHEDRON
      {
        // get first face
        vtkTypeInt64 cellFaces0 = cellFaces[0];
        vtkFoamLabelListList::CellType baseFacePoints;
        meshFaces.GetCell(cellFaces0, baseFacePoints);
        size_t nPoints = baseFacePoints.size();
        size_t nPolyPoints = baseFacePoints.size() + 1;
        if (nPoints > static_cast<size_t>(maxNPoints) ||
          nPolyPoints > static_cast<size_t>(maxNPolyPoints))
        {
          vtkErrorMacro(<< "Too large polyhedron at cellId = " << cellId);
          return;
        }
        polyPoints->SetId(0, static_cast<vtkIdType>(baseFacePoints.size()));
        vtkTypeInt64 faceOwnerValue = GetLabelValue(this->FaceOwner, cellFaces0, faceOwner64Bit);
        if (faceOwnerValue == cellId)
        {
          // add first face to cell points
          for (size_t j = 0; j < baseFacePoints.size(); j++)
          {
            vtkTypeInt64 pointJ = baseFacePoints[j];
            cellPoints->SetId(static_cast<vtkIdType>(j), static_cast<vtkIdType>(pointJ));
            polyPoints->SetId(static_cast<vtkIdType>(j + 1), static_cast<vtkIdType>(pointJ));
          }
        }
        else
        {
          // if it is a _neighbor_ face flip the points
          for (size_t j = 0; j < baseFacePoints.size(); j++)
          {
            vtkTypeInt64 pointJ = baseFacePoints[baseFacePoints.size() - 1 - j];
            cellPoints->SetId(static_cast<vtkIdType>(j), static_cast<vtkIdType>(pointJ));
            polyPoints->SetId(static_cast<vtkIdType>(j + 1), static_cast<vtkIdType>(pointJ));
          }
        }

        // loop through faces and create a list of all points
        // j = 1 skip baseFace
        for (size_t j = 1; j < cellFaces.size(); j++)
        {
          // remove duplicate points from faces
          vtkTypeInt64 cellFacesJ = cellFaces[j];
          vtkFoamLabelListList::CellType faceJPoints;
          meshFaces.GetCell(cellFacesJ, faceJPoints);
          if (nPolyPoints >= static_cast<size_t>(maxNPolyPoints))
          {
            vtkErrorMacro(<< "Too large polyhedron at cellId = " << cellId);
            return;
          }
          polyPoints->SetId(
            static_cast<vtkIdType>(nPolyPoints++), static_cast<vtkIdType>(faceJPoints.size()));
          int pointI, delta; // must be signed
          faceOwnerValue = GetLabelValue(this->FaceOwner, cellFacesJ, faceOwner64Bit);
          if (faceOwnerValue == cellId)
          {
            pointI = 0;
            delta = 1;
          }
          else
          {
            // if it is a _neighbor_ face flip the points
            pointI = static_cast<int>(faceJPoints.size()) - 1;
            delta = -1;
          }
          for (size_t k = 0; k < faceJPoints.size(); k++, pointI += delta)
          {
            vtkTypeInt64 faceJPointK = faceJPoints[pointI];
            bool foundDup = false;
            for (vtkIdType l = 0; l < static_cast<vtkIdType>(nPoints); l++)
            {
              if (cellPoints->GetId(l) == faceJPointK)
              {
                foundDup = true;
                break; // look no more
              }
            }
            if (!foundDup)
            {
              if (nPoints >= static_cast<size_t>(maxNPoints))
              {
                vtkErrorMacro(<< "Too large polyhedron at cellId = " << cellId);
                return;
              }
              cellPoints->SetId(
                static_cast<vtkIdType>(nPoints++), static_cast<vtkIdType>(faceJPointK));
            }
            if (nPolyPoints >= static_cast<size_t>(maxNPolyPoints))
            {
              vtkErrorMacro(<< "Too large polyhedron at cellId = " << cellId);
              return;
            }
            polyPoints->SetId(
              static_cast<vtkIdType>(nPolyPoints++), static_cast<vtkIdType>(faceJPointK));
          }
        }

        // create the poly cell and insert it into the mesh
        internalMesh->InsertNextCell(VTK_POLYHEDRON, static_cast<vtkIdType>(nPoints),
          cellPoints->GetPointer(0), static_cast<vtkIdType>(cellFaces.size()),
          polyPoints->GetPointer(0));
      }
    }
  }
}

//------------------------------------------------------------------------------
// derive cell types and create the internal mesh
vtkUnstructuredGrid* vtkOpenFOAMReaderPrivate::MakeInternalMesh(
  const vtkFoamLabelListList& meshCells, const vtkFoamLabelListList& meshFaces,
  vtkFloatArray* pointArray)
{
  // Create Mesh
  vtkUnstructuredGrid* internalMesh = vtkUnstructuredGrid::New();
  internalMesh->Allocate(this->NumCells);

  if (this->Parent->GetDecomposePolyhedra())
  {
    // For polyhedral decomposition
    this->NumTotalAdditionalCells = 0;
    this->AdditionalCellIds = vtkIdTypeArray::New();
    this->NumAdditionalCells = vtkIntArray::New();
    this->AdditionalCellPoints = new vtkFoamLabelArrayVector;

    auto additionalCells = vtkSmartPointer<vtkIdTypeArray>::New();
    additionalCells->SetNumberOfComponents(5); // Accommodate tetra or pyramid

    this->InsertCellsToGrid(internalMesh, meshCells, meshFaces, pointArray, additionalCells);

    // for polyhedral decomposition
    pointArray->Squeeze();
    this->AdditionalCellIds->Squeeze();
    this->NumAdditionalCells->Squeeze();
    additionalCells->Squeeze();

    // insert decomposed cells into mesh
    const int nComponents = additionalCells->GetNumberOfComponents();
    const vtkIdType nAdditionalCells = additionalCells->GetNumberOfTuples();
    for (vtkIdType i = 0; i < nAdditionalCells; i++)
    {
      if (additionalCells->GetComponent(i, 4) == -1)
      {
        internalMesh->InsertNextCell(VTK_TETRA, 4, additionalCells->GetPointer(i * nComponents));
      }
      else
      {
        internalMesh->InsertNextCell(VTK_PYRAMID, 5, additionalCells->GetPointer(i * nComponents));
      }
    }
    internalMesh->Squeeze();
  }
  else
  {
    this->InsertCellsToGrid(internalMesh, meshCells, meshFaces, pointArray);
  }

  // Set points for internalMesh
  vtkPoints* points = vtkPoints::New();
  points->SetData(pointArray);
  internalMesh->SetPoints(points);
  points->Delete();

  return internalMesh;
}

//------------------------------------------------------------------------------
// insert faces to grid
void vtkOpenFOAMReaderPrivate::InsertFacesToGrid(vtkPolyData* boundaryMesh,
  const vtkFoamLabelListList& meshFaces, vtkIdType startFace, vtkIdType endFace,
  vtkDataArray* boundaryPointMap, vtkIdList* facePointsVtkId, vtkDataArray* labels,
  bool isLookupValue)
{
  vtkPolyData& bm = *boundaryMesh;

  const bool faceLabels64Bit = ::Is64BitArray(labels);

  for (vtkIdType j = startFace; j < endFace; j++)
  {
    vtkIdType faceId = j;
    if (labels != nullptr)
    {
      faceId = GetLabelValue(labels, j, faceLabels64Bit);
      if (faceId >= this->FaceOwner->GetNumberOfTuples())
      {
        vtkWarningMacro(<< "faceLabels id " << faceId << " exceeds the number of faces "
                        << this->FaceOwner->GetNumberOfTuples());
        bm.InsertNextCell(VTK_EMPTY_CELL, 0, facePointsVtkId->GetPointer(0));
        continue;
      }
    }

    const vtkIdType nFacePoints = meshFaces.GetSize(faceId);

    if (isLookupValue)
    {
      for (vtkIdType fp = 0; fp < nFacePoints; ++fp)
      {
        const auto pointId = static_cast<vtkIdType>(meshFaces.GetValue(faceId, fp));
        facePointsVtkId->SetId(fp, boundaryPointMap->LookupValue(pointId));
      }
    }
    else
    {
      if (boundaryPointMap)
      {
        const bool bpMap64Bit = ::Is64BitArray(boundaryPointMap); // null-safe
        for (vtkIdType fp = 0; fp < nFacePoints; ++fp)
        {
          const auto pointId = static_cast<vtkIdType>(meshFaces.GetValue(faceId, fp));
          facePointsVtkId->SetId(fp, GetLabelValue(boundaryPointMap, pointId, bpMap64Bit));
        }
      }
      else
      {
        for (vtkIdType fp = 0; fp < nFacePoints; ++fp)
        {
          const auto pointId = static_cast<vtkIdType>(meshFaces.GetValue(faceId, fp));
          facePointsVtkId->SetId(fp, pointId);
        }
      }
    }

    if (nFacePoints == 3)
    {
      // Triangle
      bm.InsertNextCell(VTK_TRIANGLE, 3, facePointsVtkId->GetPointer(0));
    }
    else if (nFacePoints == 4)
    {
      // Quad
      bm.InsertNextCell(VTK_QUAD, 4, facePointsVtkId->GetPointer(0));
    }
    else
    {
      // Polygon
      bm.InsertNextCell(VTK_POLYGON, static_cast<int>(nFacePoints), facePointsVtkId->GetPointer(0));
    }
  }
}

//------------------------------------------------------------------------------
// Returns requested boundary meshes
vtkMultiBlockDataSet* vtkOpenFOAMReaderPrivate::MakeBoundaryMesh(
  const vtkFoamLabelListList& meshFaces, vtkFloatArray* pointArray)
{
  const auto& patches = this->BoundaryDict;
  const vtkIdType nBoundaries = static_cast<vtkIdType>(patches.size());

  // Final consistency check for boundaries
  if (patches.endFace() > meshFaces.GetNumberOfElements())
  {
    vtkErrorMacro(<< "The boundary describes " << patches.startFace() << " to "
                  << (patches.endFace() - 1) << " faces, but mesh only has "
                  << meshFaces.GetNumberOfElements() << " faces");
    return nullptr;
  }

  vtkMultiBlockDataSet* boundaryMesh = vtkMultiBlockDataSet::New();

  if (this->Parent->GetCreateCellToPoint())
  {
    this->AllBoundaries = vtkPolyData::New();
    this->AllBoundaries->AllocateEstimate(
      // ==> nBoundaryFaces
      meshFaces.GetNumberOfElements() - patches.startFace(), 1);
  }
  this->BoundaryPointMap = new vtkFoamLabelArrayVector;

  vtkIdTypeArray* nBoundaryPointsList = vtkIdTypeArray::New();
  nBoundaryPointsList->SetNumberOfValues(nBoundaries);

  // count the max number of points per face and the number of points
  // (with duplicates) in mesh
  vtkIdType maxNFacePoints = 0;
  for (vtkIdType patchi = 0; patchi < nBoundaries; ++patchi)
  {
    const vtkFoamPatch& patch = patches[patchi];
    const vtkIdType startFace = patch.startFace();
    const vtkIdType endFace = patch.endFace();

    vtkIdType nPoints = 0;
    for (vtkIdType facei = startFace; facei < endFace; ++facei)
    {
      vtkIdType nFacePoints = meshFaces.GetSize(facei);
      nPoints += nFacePoints;
      if (maxNFacePoints < nFacePoints)
      {
        maxNFacePoints = nFacePoints;
      }
    }
    nBoundaryPointsList->SetValue(patchi, nPoints);
  }

  // Allocate array for converting int vector to vtkIdType List:
  // workaround for 64bit machines
  vtkIdList* facePointsVtkId = vtkIdList::New();
  facePointsVtkId->SetNumberOfIds(maxNFacePoints);

  // Use same integer width as per faces
  const bool meshPoints64Bit = meshFaces.IsLabel64();

  // create initial internal point list: set all points to -1
  if (this->Parent->GetCreateCellToPoint())
  {
    if (meshPoints64Bit)
    {
      this->InternalPoints = vtkTypeInt64Array::New();
    }
    else
    {
      this->InternalPoints = vtkTypeInt32Array::New();
    }
    this->InternalPoints->SetNumberOfValues(this->NumPoints);
    this->InternalPoints->FillComponent(0, -1);

    // Mark boundary points as 0
    for (const vtkFoamPatch& patch : patches)
    {
      if (patch.type_ == vtkFoamPatch::PHYSICAL || patch.type_ == vtkFoamPatch::PROCESSOR)
      {
        const vtkIdType startFace = patch.startFace();
        const vtkIdType endFace = patch.endFace();

        for (vtkIdType facei = startFace; facei < endFace; ++facei)
        {
          const vtkIdType nFacePoints = meshFaces.GetSize(facei);
          for (vtkIdType pointi = 0; pointi < nFacePoints; ++pointi)
          {
            SetLabelValue(
              this->InternalPoints, meshFaces.GetValue(facei, pointi), 0, meshPoints64Bit);
          }
        }
      }
    }
  }

  vtkTypeInt64 nAllBoundaryPoints = 0;
  std::vector<std::vector<vtkIdType>> procCellList;
  vtkTypeInt8Array* pointTypes = nullptr;

  if (this->Parent->GetCreateCellToPoint())
  {
    // Create global to AllBounaries point map
    for (vtkIdType pointi = 0; pointi < this->NumPoints; ++pointi)
    {
      if (GetLabelValue(this->InternalPoints, pointi, meshPoints64Bit) == 0)
      {
        SetLabelValue(this->InternalPoints, pointi, nAllBoundaryPoints, meshPoints64Bit);
        nAllBoundaryPoints++;
      }
    }

    if (!this->ProcessorName.empty())
    {
      // Initialize physical-processor boundary shared point list
      procCellList.resize(static_cast<size_t>(nAllBoundaryPoints));
      pointTypes = vtkTypeInt8Array::New();
      pointTypes->SetNumberOfTuples(nAllBoundaryPoints);
      pointTypes->FillValue(0);
    }
  }

  for (vtkIdType patchi = 0; patchi < nBoundaries; ++patchi)
  {
    const vtkFoamPatch& patch = patches[patchi];
    const vtkIdType startFace = patch.startFace();
    const vtkIdType endFace = patch.endFace();
    const vtkIdType nFaces = patch.size_;

    if (this->Parent->GetCreateCellToPoint() &&
      (patch.type_ == vtkFoamPatch::PHYSICAL || patch.type_ == vtkFoamPatch::PROCESSOR))
    {
      // Add faces to AllBoundaries
      this->InsertFacesToGrid(this->AllBoundaries, meshFaces, startFace, endFace,
        this->InternalPoints, facePointsVtkId, nullptr, false);

      if (!this->ProcessorName.empty())
      {
        // Mark belonging boundary types and, if PROCESSOR, cell numbers
        const vtkIdType absStartFace = patch.offset_;
        const vtkIdType absEndFace = absStartFace + nFaces;
        for (vtkIdType facei = absStartFace; facei < absEndFace; ++facei)
        {
          vtkIdType nPoints;
          const vtkIdType* points;
          this->AllBoundaries->GetCellPoints(facei, nPoints, points);
          if (patch.type_ == vtkFoamPatch::PHYSICAL)
          {
            for (vtkIdType pointi = 0; pointi < nPoints; ++pointi)
            {
              *pointTypes->GetPointer(points[pointi]) |= vtkFoamPatch::PHYSICAL;
            }
          }
          else
          {
            // PROCESSOR
            for (vtkIdType pointi = 0; pointi < nPoints; ++pointi)
            {
              const vtkIdType procPoint = points[pointi];
              *pointTypes->GetPointer(procPoint) |= vtkFoamPatch::PROCESSOR;
              procCellList[procPoint].push_back(facei);
            }
          }
        }
      }
    }

    // Skip below if not active
    if (!patches.isActive(patch.index_))
    {
      continue;
    }

    // Create the boundary patch mesh
    vtkPolyData* bm = vtkPolyData::New();
    ::AppendBlock(boundaryMesh, bm, patch.name_);

    bm->AllocateEstimate(nFaces, 1);
    const vtkIdType nBoundaryPoints = nBoundaryPointsList->GetValue(patchi);

    // create global to boundary-local point map and boundary points

    vtkDataArray* boundaryPointList;
    if (meshPoints64Bit)
    {
      boundaryPointList = vtkTypeInt64Array::New();
    }
    else
    {
      boundaryPointList = vtkTypeInt32Array::New();
    }
    boundaryPointList->SetNumberOfValues(nBoundaryPoints);
    vtkIdType pointi = 0;
    for (vtkIdType facei = startFace; facei < endFace; ++facei)
    {
      const vtkIdType nFacePoints = meshFaces.GetSize(facei);
      for (int fp = 0; fp < nFacePoints; ++fp)
      {
        SetLabelValue(boundaryPointList, pointi, meshFaces.GetValue(facei, fp), meshPoints64Bit);
        ++pointi;
      }
    }
    vtkSortDataArray::Sort(boundaryPointList);

    vtkDataArray* bpMap;
    if (meshPoints64Bit)
    {
      bpMap = vtkTypeInt64Array::New();
    }
    else
    {
      bpMap = vtkTypeInt32Array::New();
    }
    this->BoundaryPointMap->push_back(bpMap);
    vtkFloatArray* boundaryPointArray = vtkFloatArray::New();
    boundaryPointArray->SetNumberOfComponents(3);
    vtkIdType oldPointJ = -1;
    for (int j = 0; j < nBoundaryPoints; j++)
    {
      vtkTypeInt64 pointJ = GetLabelValue(boundaryPointList, j, meshPoints64Bit);
      if (pointJ != oldPointJ)
      {
        oldPointJ = pointJ;
        boundaryPointArray->InsertNextTuple(pointArray->GetPointer(3 * pointJ));
        AppendLabelValue(bpMap, pointJ, meshPoints64Bit);
      }
    }
    boundaryPointArray->Squeeze();
    bpMap->Squeeze();
    boundaryPointList->Delete();
    vtkPoints* boundaryPoints = vtkPoints::New();
    boundaryPoints->SetData(boundaryPointArray);
    boundaryPointArray->Delete();

    // Set points for boundary
    bm->SetPoints(boundaryPoints);
    boundaryPoints->Delete();

    // insert faces to boundary mesh
    this->InsertFacesToGrid(
      bm, meshFaces, startFace, endFace, bpMap, facePointsVtkId, nullptr, true);
    bm->Delete();
    bpMap->ClearLookup();
  }

  nBoundaryPointsList->Delete();
  facePointsVtkId->Delete();

  if (this->Parent->GetCreateCellToPoint())
  {
    this->AllBoundaries->Squeeze();
    if (meshPoints64Bit)
    {
      this->AllBoundariesPointMap = vtkTypeInt64Array::New();
    }
    else
    {
      this->AllBoundariesPointMap = vtkTypeInt32Array::New();
    }
    vtkDataArray& abpMap = *this->AllBoundariesPointMap;
    abpMap.SetNumberOfValues(nAllBoundaryPoints);

    // create lists of internal points and AllBoundaries points
    vtkIdType nInternalPoints = 0;
    for (vtkIdType pointI = 0, allBoundaryPointI = 0; pointI < this->NumPoints; pointI++)
    {
      vtkIdType globalPointId = GetLabelValue(this->InternalPoints, pointI, meshPoints64Bit);
      if (globalPointId == -1)
      {
        SetLabelValue(this->InternalPoints, nInternalPoints, pointI, meshPoints64Bit);
        nInternalPoints++;
      }
      else
      {
        SetLabelValue(&abpMap, allBoundaryPointI, pointI, meshPoints64Bit);
        allBoundaryPointI++;
      }
    }
    // shrink to the number of internal points
    if (nInternalPoints > 0)
    {
      this->InternalPoints->Resize(nInternalPoints);
    }
    else
    {
      this->InternalPoints->Delete();
      this->InternalPoints = nullptr;
    }

    // set dummy vtkPoints to tell the grid the number of points
    // (otherwise GetPointCells will crash)
    vtkPoints* allBoundaryPoints = vtkPoints::New();
    allBoundaryPoints->SetNumberOfPoints(abpMap.GetNumberOfTuples());
    this->AllBoundaries->SetPoints(allBoundaryPoints);
    allBoundaryPoints->Delete();

    if (!this->ProcessorName.empty())
    {
      // remove links to processor boundary faces from point-to-cell
      // links of physical-processor shared points to avoid cracky seams
      // on fixedValue-type boundaries which are noticeable when all the
      // decomposed meshes are appended
      this->AllBoundaries->BuildLinks();
      for (int pointI = 0; pointI < nAllBoundaryPoints; pointI++)
      {
        if (pointTypes->GetValue(pointI) == (vtkFoamPatch::PHYSICAL | vtkFoamPatch::PROCESSOR))
        {
          const std::vector<vtkIdType>& procCells = procCellList[pointI];
          for (size_t cellI = 0; cellI < procCellList[pointI].size(); cellI++)
          {
            this->AllBoundaries->RemoveReferenceToCell(pointI, procCells[cellI]);
          }
          // omit reclaiming memory as the possibly recovered size should
          // not typically be so large
        }
      }
      pointTypes->Delete();
    }
  }

  return boundaryMesh;
}

//------------------------------------------------------------------------------
// truncate face owner to have only boundary face info
void vtkOpenFOAMReaderPrivate::TruncateFaceOwner()
{
  const vtkIdType boundaryStartFace =
    (!this->BoundaryDict.empty() ? this->BoundaryDict.startFace()
                                 : this->FaceOwner->GetNumberOfTuples());

  // All boundary faces
  const vtkIdType nBoundaryFaces = this->FaceOwner->GetNumberOfTuples() - boundaryStartFace;
  memmove(this->FaceOwner->GetVoidPointer(0), this->FaceOwner->GetVoidPointer(boundaryStartFace),
    static_cast<size_t>(this->FaceOwner->GetDataTypeSize() * nBoundaryFaces));
  this->FaceOwner->Resize(nBoundaryFaces);

  // Has side effect on neighbour too
  if (this->FaceNeigh != nullptr)
  {
    this->FaceNeigh->Delete();
    this->FaceNeigh = nullptr;
  }
}

//------------------------------------------------------------------------------
// this is necessary due to the strange vtkDataArrayTemplate::Resize()
// implementation when the array size is to be extended
template <typename T1, typename T2>
bool vtkOpenFOAMReaderPrivate::ExtendArray(T1* array, vtkIdType nTuples)
{
  vtkIdType newSize = nTuples * array->GetNumberOfComponents();
  void* ptr = malloc(static_cast<size_t>(newSize * array->GetDataTypeSize()));
  if (ptr == nullptr)
  {
    return false;
  }
  memmove(ptr, array->GetVoidPointer(0), array->GetDataSize() * array->GetDataTypeSize());
  array->SetArray(static_cast<T2*>(ptr), newSize, 0);
  return true;
}

//------------------------------------------------------------------------------
// move polyhedral cell centroids
vtkPoints* vtkOpenFOAMReaderPrivate::MoveInternalMesh(
  vtkUnstructuredGrid* internalMesh, vtkFloatArray* pointArray)
{
  const auto nOldPoints = internalMesh->GetPoints()->GetNumberOfPoints();

  if (this->Parent->GetDecomposePolyhedra())
  {
    const vtkIdType nAdditionalPoints = static_cast<vtkIdType>(this->AdditionalCellPoints->size());
    this->ExtendArray<vtkFloatArray, float>(pointArray, this->NumPoints + nAdditionalPoints);

    const bool cellPoints64Bit =
      (nAdditionalPoints > 0 && ::Is64BitArray(this->AdditionalCellPoints->front()));

    for (int i = 0; i < nAdditionalPoints; i++)
    {
      vtkDataArray* polyCellPoints = this->AdditionalCellPoints->operator[](i);
      float centroid[3];
      centroid[0] = centroid[1] = centroid[2] = 0.0F;
      vtkIdType nCellPoints = polyCellPoints->GetDataSize();
      for (vtkIdType j = 0; j < nCellPoints; j++)
      {
        float* pointK =
          pointArray->GetPointer(3 * GetLabelValue(polyCellPoints, j, cellPoints64Bit));
        centroid[0] += pointK[0];
        centroid[1] += pointK[1];
        centroid[2] += pointK[2];
      }
      const float weight = (nCellPoints ? 1.0F / static_cast<float>(nCellPoints) : 0.0F);
      centroid[0] *= weight;
      centroid[1] *= weight;
      centroid[2] *= weight;
      pointArray->InsertTuple(this->NumPoints + i, centroid);
    }
  }

  if (nOldPoints != pointArray->GetNumberOfTuples())
  {
    vtkErrorMacro(<< "Mismatch in number of old points (" << nOldPoints << ") and new points ("
                  << pointArray->GetNumberOfTuples() << ')');
    return nullptr;
  }

  // instantiate the points class
  vtkPoints* points = vtkPoints::New();
  points->SetData(pointArray);
  internalMesh->SetPoints(points);
  return points;
}

//------------------------------------------------------------------------------
// move boundary points
void vtkOpenFOAMReaderPrivate::MoveBoundaryMesh(
  vtkMultiBlockDataSet* boundaryMesh, vtkFloatArray* pointArray)
{
  const auto& patches = this->BoundaryDict;

  unsigned int activeBoundaryIndex = 0;
  for (const vtkFoamPatch& patch : patches)
  {
    if (patches.isActive(patch.index_))
    {
      vtkDataArray* bpMap = this->BoundaryPointMap->operator[](activeBoundaryIndex);
      const vtkIdType nBoundaryPoints = bpMap->GetNumberOfTuples();
      const bool meshPoints64Bit = ::Is64BitArray(bpMap);

      vtkFloatArray* boundaryPointArray = vtkFloatArray::New();
      boundaryPointArray->SetNumberOfComponents(3);
      boundaryPointArray->SetNumberOfTuples(nBoundaryPoints);
      for (vtkIdType pointi = 0; pointi < nBoundaryPoints; ++pointi)
      {
        boundaryPointArray->SetTuple(
          pointi, GetLabelValue(bpMap, pointi, meshPoints64Bit), pointArray);
      }
      vtkPoints* boundaryPoints = vtkPoints::New();
      boundaryPoints->SetData(boundaryPointArray);
      boundaryPointArray->Delete();

      vtkPolyData* bm = vtkPolyData::SafeDownCast(boundaryMesh->GetBlock(activeBoundaryIndex));
      bm->SetPoints(boundaryPoints);
      boundaryPoints->Delete();

      ++activeBoundaryIndex;
    }
  }
}

//------------------------------------------------------------------------------
// as of now the function does not do interpolation, but do just averaging.
void vtkOpenFOAMReaderPrivate::InterpolateCellToPoint(vtkFloatArray* pData, vtkFloatArray* iData,
  vtkPointSet* mesh, vtkDataArray* pointList, vtkTypeInt64 nPoints)
{
  if (nPoints == 0)
  {
    return;
  }

  const bool meshPoints64Bit = ::Is64BitArray(pointList);

  // a dummy call to let GetPointCells() build the cell links if still not built
  // (not using BuildLinks() since it always rebuild links)
  vtkIdList* pointCells = vtkIdList::New();
  mesh->GetPointCells(0, pointCells);
  pointCells->Delete();

  // Set up to grab point cells
  vtkUnstructuredGrid* ug = vtkUnstructuredGrid::SafeDownCast(mesh);
  vtkPolyData* pd = vtkPolyData::SafeDownCast(mesh);
  vtkIdType nCells;
  vtkIdType* cells;

  const int nComponents = iData->GetNumberOfComponents();

  if (nComponents == 1)
  {
    // a special case with the innermost componentI loop unrolled
    float* tuples = iData->GetPointer(0);
    for (vtkTypeInt64 pointI = 0; pointI < nPoints; pointI++)
    {
      vtkTypeInt64 pI = pointList ? GetLabelValue(pointList, pointI, meshPoints64Bit) : pointI;
      if (ug)
      {
        ug->GetPointCells(pI, nCells, cells);
      }
      else
      {
        pd->GetPointCells(pI, nCells, cells);
      }

      // use double intermediate variable for precision
      double interpolatedValue = 0.0;
      for (int cellI = 0; cellI < nCells; cellI++)
      {
        interpolatedValue += tuples[cells[cellI]];
      }
      interpolatedValue = (nCells ? interpolatedValue / static_cast<double>(nCells) : 0.0);
      pData->SetValue(pI, static_cast<float>(interpolatedValue));
    }
  }
  else if (nComponents == 3)
  {
    // a special case with the innermost componentI loop unrolled
    float* pDataPtr = pData->GetPointer(0);
    for (vtkTypeInt64 pointI = 0; pointI < nPoints; pointI++)
    {
      vtkTypeInt64 pI = pointList ? GetLabelValue(pointList, pointI, meshPoints64Bit) : pointI;
      if (ug)
      {
        ug->GetPointCells(pI, nCells, cells);
      }
      else
      {
        pd->GetPointCells(pI, nCells, cells);
      }

      // use double intermediate variables for precision
      const double weight = (nCells ? 1.0 / static_cast<double>(nCells) : 0.0);
      double summedValue0 = 0.0, summedValue1 = 0.0, summedValue2 = 0.0;

      // hand unrolling
      for (int cellI = 0; cellI < nCells; cellI++)
      {
        const float* tuple = iData->GetPointer(3 * cells[cellI]);
        summedValue0 += tuple[0];
        summedValue1 += tuple[1];
        summedValue2 += tuple[2];
      }

      float* interpolatedValue = &pDataPtr[3 * pI];
      interpolatedValue[0] = static_cast<float>(weight * summedValue0);
      interpolatedValue[1] = static_cast<float>(weight * summedValue1);
      interpolatedValue[2] = static_cast<float>(weight * summedValue2);
    }
  }
  else
  {
    float* pDataPtr = pData->GetPointer(0);
    for (vtkTypeInt64 pointI = 0; pointI < nPoints; pointI++)
    {
      vtkTypeInt64 pI = pointList ? GetLabelValue(pointList, pointI, meshPoints64Bit) : pointI;
      if (ug)
      {
        ug->GetPointCells(pI, nCells, cells);
      }
      else
      {
        pd->GetPointCells(pI, nCells, cells);
      }

      // use double intermediate variables for precision
      const double weight = (nCells ? 1.0 / static_cast<double>(nCells) : 0.0);
      float* interpolatedValue = &pDataPtr[nComponents * pI];
      // a bit strange loop order but this works fastest
      for (int componentI = 0; componentI < nComponents; componentI++)
      {
        const float* tuple = iData->GetPointer(componentI);
        double summedValue = 0.0;
        for (int cellI = 0; cellI < nCells; cellI++)
        {
          summedValue += tuple[nComponents * cells[cellI]];
        }
        interpolatedValue[componentI] = static_cast<float>(weight * summedValue);
      }
    }
  }
}

//------------------------------------------------------------------------------
bool vtkOpenFOAMReaderPrivate::ReadFieldFile(vtkFoamIOobject& io, vtkFoamDict& dict,
  const std::string& varName, const vtkDataArraySelection* selection)
{
  const std::string varPath(this->CurrentTimeRegionPath() + "/" + varName);

  // Open the file
  if (!io.Open(varPath))
  {
    vtkErrorMacro(<< "Error opening " << io.GetFileName() << ": " << io.GetError());
    return false;
  }

  // if the variable is disabled on selection panel then skip it
  if (selection->ArrayExists(io.GetObjectName().c_str()) &&
    !selection->ArrayIsEnabled(io.GetObjectName().c_str()))
  {
    return false;
  }

  // Read the field file into dictionary
  if (!dict.Read(io))
  {
    vtkErrorMacro(<< "Error reading line " << io.GetLineNumber() << " of " << io.GetFileName()
                  << ": " << io.GetError());
    return false;
  }

  if (dict.GetType() != vtkFoamToken::DICTIONARY)
  {
    vtkErrorMacro(<< "File " << io.GetFileName() << "is not valid as a field file");
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
vtkFloatArray* vtkOpenFOAMReaderPrivate::FillField(vtkFoamEntry& entry, vtkIdType nElements,
  const vtkFoamIOobject& io, vtkFoamTypes::dataType fieldDataType)
{
  vtkFloatArray* data;
  const std::string& className = io.GetClassName();

  if (entry.FirstValue().IsUniform())
  {
    if (entry.FirstValue().GetType() == vtkFoamToken::SCALAR ||
      entry.FirstValue().GetType() == vtkFoamToken::LABEL)
    {
      const float num = entry.ToFloat();
      data = vtkFloatArray::New();
      data->SetNumberOfValues(nElements);
      data->FillValue(num);
    }
    else
    {
      float tupleBuffer[9], *tuple;
      int nComponents;
      // have to determine the type of vector
      if (entry.FirstValue().GetType() == vtkFoamToken::LABELLIST)
      {
        vtkDataArray& ll = entry.LabelList();
        nComponents = static_cast<int>(ll.GetNumberOfTuples());
        for (int componentI = 0; componentI < nComponents; componentI++)
        {
          tupleBuffer[componentI] = static_cast<float>(ll.GetTuple1(componentI));
        }
        tuple = tupleBuffer;
      }
      else if (entry.FirstValue().GetType() == vtkFoamToken::SCALARLIST)
      {
        vtkFloatArray& sl = entry.ScalarList();
        nComponents = static_cast<int>(sl.GetSize());
        tuple = sl.GetPointer(0);
      }
      else
      {
        vtkErrorMacro(<< "Wrong list type for uniform field");
        return nullptr;
      }

      if ((vtkFoamTypes::GetNumberOfComponents(fieldDataType) == nComponents) &&
        vtkFoamTypes::IsVectorSpace(fieldDataType))
      {
        data = vtkFloatArray::New();
        data->SetNumberOfComponents(nComponents);
        data->SetNumberOfTuples(nElements);
        if (nComponents == 6)
        {
          // Remap symmTensor tuple
          // OpenFOAM = (XX, XY, XZ, YY, YZ, ZZ)
          // VTK uses = (XX, YY, ZZ, XY, YZ, XZ)

          std::swap(tuple[1], tuple[3]); // swap XY <-> YY
          std::swap(tuple[2], tuple[5]); // swap XZ <-> ZZ
        }
        for (vtkIdType i = 0; i < nElements; i++)
        {
          data->SetTuple(i, tuple);
        }
      }
      else
      {
        vtkErrorMacro(<< "Number of components and field class doesn't match "
                      << "for " << io.GetFileName() << ". class = " << className
                      << ", nComponents = " << nComponents);
        return nullptr;
      }
    }
  }
  else // nonuniform
  {
    if ((entry.FirstValue().GetType() == vtkFoamToken::SCALARLIST &&
          vtkFoamTypes::IsScalar(fieldDataType)) ||
      (entry.FirstValue().GetType() == vtkFoamToken::VECTORLIST &&
        vtkFoamTypes::IsVectorSpace(fieldDataType)))
    {
      const vtkIdType nTuples = entry.ScalarList().GetNumberOfTuples();
      if (nTuples != nElements)
      {
        vtkErrorMacro(<< "Number of cells/points in mesh and field don't match: "
                      << "mesh = " << nElements << ", field = " << nTuples);
        return nullptr;
      }
      data = static_cast<vtkFloatArray*>(entry.Ptr());
      const int nComponents = data->GetNumberOfComponents();
      if (nComponents == 6)
      {
        for (vtkIdType tuplei = 0; tuplei < nTuples; ++tuplei)
        {
          float* tuple = data->GetPointer(nComponents * tuplei);

          // Remap symmTensor tuple
          // OpenFOAM = (XX, XY, XZ, YY, YZ, ZZ)
          // VTK uses = (XX, YY, ZZ, XY, YZ, XZ)

          std::swap(tuple[1], tuple[3]); // swap XY <-> YY
          std::swap(tuple[2], tuple[5]); // swap XZ <-> ZZ
        }
      }
    }
    else if (entry.FirstValue().GetType() == vtkFoamToken::EMPTYLIST && nElements <= 0)
    {
      data = vtkFloatArray::New();

      // Set appropriate number of components for empty list as well
      const int nComp = vtkFoamTypes::GetNumberOfComponents(fieldDataType);
      if (nComp > 0)
      {
        data->SetNumberOfComponents(nComp);
      }
    }
    else
    {
      vtkErrorMacro(<< io.GetFileName() << " is not a valid " << io.GetClassName());
      return nullptr;
    }
  }
  return data;
}

//------------------------------------------------------------------------------
// Convert OpenFOAM dimension array to string representation
std::string vtkOpenFOAMReaderPrivate::ConstructDimensions(const vtkFoamDict& dict) const
{
  const int nDimensions = 7; // There are 7 base dimensions
  static const char* units[7] = { "kg", "m", "s", "K", "mol", "A", "cd" };

  if (!this->Parent->GetAddDimensionsToArrayNames())
  {
    return std::string();
  }

  const vtkFoamEntry* dimEntry = dict.Lookup("dimensions");
  if ((dimEntry == nullptr) || (dimEntry->FirstValue().GetType() != vtkFoamToken::SCALARLIST))
  {
    return std::string();
  }

  const vtkFloatArray& values = dimEntry->ScalarList();
  const vtkIdType nValues = values.GetNumberOfTuples();

  // Expect seven dimensions, but may have only the first five.
  // OpenFOAM accepts both and so do we.
  if (nValues != 5 && nValues != nDimensions)
  {
    return std::string();
  }

  // Make a copy
  float dims[7] = { 0 };

  for (vtkIdType i = 0; i < nValues; ++i)
  {
    dims[i] = values.GetValue(i);
  }

  const auto equal = // Compare floats with rounding
    [](const float a, const float b) { return (std::abs(a - b) < 1e-3); };

  const auto integral = // Test if integral/non-integral
    [](const float val) { return (std::abs(val - std::round(val)) < 1e-4); };

  // Stringify. Use stringstream to build the string
  std::ostringstream dimensions, denominator;
  dimensions << " [";
  int nPositive = 0;
  int nNegative = 0;

  // Some standard units
  if (equal(dims[0], 1) && equal(dims[1], -1) && equal(dims[2], -2))
  {
    dimensions << "Pa";
    nPositive = 1;
    dims[0] = dims[1] = dims[2] = 0;
  }
  else if (equal(dims[0], 1) && equal(dims[1], 1) && equal(dims[2], -2))
  {
    dimensions << "N";
    nPositive = 1;
    dims[0] = dims[1] = dims[2] = 0;
  }
  else if (equal(dims[0], 1) && equal(dims[1], 2) && equal(dims[2], -3))
  {
    dimensions << "W";
    nPositive = 1;
    dims[0] = dims[1] = dims[2] = 0;
  }
  // Note: cannot know if 'J' or 'N m' is the better representation, so skip that one

  for (int dimi = 0; dimi < nDimensions; ++dimi)
  {
    float expon = dims[dimi];

    if (expon > 0)
    {
      if (nPositive++)
      {
        dimensions << ' ';
      }
      dimensions << units[dimi];
      if (equal(expon, 1))
      {
        continue;
      }
      if (!integral(expon))
      {
        dimensions << '^';
      }
      dimensions << expon;
    }
    else if (expon < 0)
    {
      expon = -expon;
      if (nNegative++)
      {
        denominator << ' ';
      }
      denominator << units[dimi];
      if (equal(expon, 1))
      {
        continue;
      }
      if (!integral(expon))
      {
        denominator << '^';
      }
      denominator << expon;
    }
  }

  // Finalize, adding denominator as required
  if (nNegative)
  {
    if (nPositive == 0)
    {
      // No numerator
      dimensions << '1';
    }
    dimensions << '/';

    if (nNegative > 1)
    {
      dimensions << '(' << denominator.str() << ')';
    }
    else
    {
      dimensions << denominator.str();
    }
  }
  else if (nPositive == 0)
  {
    // No dimensions
    dimensions << '-';
  }

  dimensions << ']';
  return dimensions.str();
}

//------------------------------------------------------------------------------
// Read volume or internal field at a timestep
void vtkOpenFOAMReaderPrivate::GetVolFieldAtTimeStep(
  const std::string& varName, bool isInternalField)
{
  vtkUnstructuredGrid* internalMesh = this->InternalMesh;
  vtkMultiBlockDataSet* boundaryMesh = this->BoundaryMesh;

  vtkFoamIOobject io(this->CasePath, this->Parent);
  vtkFoamDict dict;
  if (!this->ReadFieldFile(io, dict, varName, this->Parent->CellDataArraySelection))
  {
    return;
  }

  // For internal field (eg, volScalarField::Internal)
  const bool hasColons = (io.GetClassName().find("::Internal") != std::string::npos);

  if ((io.GetClassName().compare(0, 3, "vol") != 0) ||
    (hasColons ? !isInternalField : isInternalField))
  {
    vtkErrorMacro(<< io.GetFileName() << " is not a volume/internal field");
    return;
  }

  // Eg, from "volScalarField" or "volScalarField::Internal" -> SCALAR_TYPE
  const auto fieldDataType(vtkFoamTypes::ToEnum(io.GetClassName(), 3));

  // The internalField
  vtkFoamEntry* iEntry = nullptr;
  {
    const std::string entryName = (isInternalField ? "value" : "internalField");

    iEntry = dict.Lookup(entryName);
    if (iEntry == nullptr)
    {
      vtkErrorMacro(<< entryName << " not found in " << io.GetFileName());
      return;
    }

    if (iEntry->FirstValue().GetType() == vtkFoamToken::EMPTYLIST)
    {
      if (this->NumCells > 0)
      {
        vtkErrorMacro(<< entryName << " of " << io.GetFileName() << " is empty");
      }
      return;
    }
  }

  vtkFloatArray* iData = this->FillField(*iEntry, this->NumCells, io, fieldDataType);
  if (iData == nullptr)
  {
    return;
  }

  const std::string dimString(this->ConstructDimensions(dict));

  vtkFloatArray *acData = nullptr, *ctpData = nullptr;

  if (this->Parent->GetCreateCellToPoint())
  {
    acData = vtkFloatArray::New();
    acData->SetNumberOfComponents(iData->GetNumberOfComponents());
    acData->SetNumberOfTuples(this->AllBoundaries->GetNumberOfCells());
  }

  if (iData->GetSize() > 0)
  {
    // Add field only if internal Mesh exists (skip if not selected).
    // Note we still need to read internalField even if internal mesh is
    // not selected, since boundaries without value entries may refer to
    // the internalField.
    if (internalMesh != nullptr)
    {
      if (this->Parent->GetDecomposePolyhedra())
      {
        // add values for decomposed cells
        this->ExtendArray<vtkFloatArray, float>(
          iData, this->NumCells + this->NumTotalAdditionalCells);
        vtkIdType nTuples = this->AdditionalCellIds->GetNumberOfTuples();
        vtkIdType additionalCellI = this->NumCells;
        for (vtkIdType tupleI = 0; tupleI < nTuples; tupleI++)
        {
          const int nCells = this->NumAdditionalCells->GetValue(tupleI);
          const vtkIdType cellId = this->AdditionalCellIds->GetValue(tupleI);
          for (int cellI = 0; cellI < nCells; cellI++)
          {
            iData->InsertTuple(additionalCellI++, cellId, iData);
          }
        }
      }

      // Set data to internal mesh
      ::AddArrayToFieldData(internalMesh->GetCellData(), iData, io.GetObjectName(), dimString);

      if (this->Parent->GetCreateCellToPoint())
      {
        // Create cell-to-point interpolated data
        ctpData = vtkFloatArray::New();
        ctpData->SetNumberOfComponents(iData->GetNumberOfComponents());
        ctpData->SetNumberOfTuples(internalMesh->GetPoints()->GetNumberOfPoints());
        if (this->InternalPoints != nullptr)
        {
          this->InterpolateCellToPoint(ctpData, iData, internalMesh, this->InternalPoints,
            this->InternalPoints->GetNumberOfTuples());
        }

        if (this->Parent->GetDecomposePolyhedra())
        {
          // assign cell values to additional points
          vtkIdType nPoints = this->AdditionalCellIds->GetNumberOfTuples();
          for (vtkIdType pointI = 0; pointI < nPoints; pointI++)
          {
            ctpData->SetTuple(
              this->NumPoints + pointI, this->AdditionalCellIds->GetValue(pointI), iData);
          }
        }
      }
    }
  }
  else
  {
    // determine as there's no cells
    iData->Delete();
    if (acData != nullptr)
    {
      acData->Delete();
    }
    return;
  }

  // Set boundary values
  const vtkFoamEntry* bfieldEntries = nullptr;
  if (!isInternalField)
  {
    bfieldEntries = dict.Lookup("boundaryField");
    if (bfieldEntries == nullptr)
    {
      vtkWarningMacro(<< "boundaryField not found in object " << varName
                      << " at time = " << this->TimeNames->GetValue(this->TimeStep));
      iData->Delete();
      if (acData != nullptr)
      {
        acData->Delete();
      }
      if (ctpData != nullptr)
      {
        ctpData->Delete();
      }
      return;
    }
  }

  const auto& patches = this->BoundaryDict;

  const bool faceOwner64Bit = ::Is64BitArray(this->FaceOwner);

  unsigned int activeBoundaryIndex = 0;
  for (const vtkFoamPatch& patch : patches)
  {
    const vtkIdType nFaces = patch.size_;

    vtkFloatArray* vData = nullptr;
    std::string bcType;

    if (bfieldEntries != nullptr)
    {
      bool badEntry = false;

      const vtkFoamEntry* bfieldEntry = bfieldEntries->Dictionary().Lookup(patch.name_, true);
      if (bfieldEntry == nullptr)
      {
        badEntry = true;
        vtkWarningMacro(<< "boundaryField " << patch.name_ << " not found in object " << varName
                        << " at time = " << this->TimeNames->GetValue(this->TimeStep));
      }
      else if (bfieldEntry->FirstValue().GetType() != vtkFoamToken::DICTIONARY)
      {
        badEntry = true;
        vtkWarningMacro(<< "boundaryField " << patch.name_ << " not a subdictionary in object "
                        << varName << " at time = " << this->TimeNames->GetValue(this->TimeStep));
      }
      else
      {
        const vtkFoamDict& patchDict = bfieldEntry->Dictionary();

        // Look for "value" entry
        vtkFoamEntry* vEntry = patchDict.Lookup("value");
        if (vEntry == nullptr)
        {
          // For alternative fallback
          const vtkFoamEntry* eptr = patchDict.Lookup("type");
          if (eptr != nullptr)
          {
            bcType = eptr->ToString();
          }
        }
        else
        {
          vData = this->FillField(*vEntry, nFaces, io, fieldDataType);
          if (vData == nullptr)
          {
            badEntry = true;
          }
        }
      }

      // If anything unexpected happened, get out
      if (badEntry)
      {
        iData->Delete();
        if (acData != nullptr)
        {
          acData->Delete();
        }
        if (ctpData != nullptr)
        {
          ctpData->Delete();
        }
        return;
      }
    }

    // Relative start into the FaceOwner list, which may have been truncated (boundaries only)
    // or have its original length

    const vtkIdType boundaryStartFace = patch.start_ -
      (this->FaceOwner->GetNumberOfTuples() < this->NumFaces ? patches.startFace() : 0);

    if (vData == nullptr)
    {
      // No "value" entry
      // Default to patch-internal values as boundary values
      vData = vtkFloatArray::New();
      vData->SetNumberOfComponents(iData->GetNumberOfComponents());
      vData->SetNumberOfTuples(nFaces);

      // Ad hoc handling of some known bcs without a "value"
      if (bcType == "noSlip")
      {
        vData->FillValue(0);
      }
      else
      {
        for (vtkIdType facei = 0; facei < nFaces; ++facei)
        {
          vtkTypeInt64 cellId =
            GetLabelValue(this->FaceOwner, boundaryStartFace + facei, faceOwner64Bit);
          vData->SetTuple(facei, cellId, iData);
        }
      }
    }

    if (this->Parent->GetCreateCellToPoint())
    {
      const vtkIdType startFace = patch.offset_;

      // if reading a processor sub-case of a decomposed case as is,
      // use the patch values of the processor patch as is
      if (patch.type_ == vtkFoamPatch::PHYSICAL ||
        (this->ProcessorName.empty() && patch.type_ == vtkFoamPatch::PROCESSOR))
      {
        // set the same value to AllBoundaries
        for (vtkIdType facei = 0; facei < nFaces; ++facei)
        {
          acData->SetTuple(facei + startFace, facei, vData);
        }
      }
      // implies && !this->ProcessorName.empty()
      else if (patch.type_ == vtkFoamPatch::PROCESSOR)
      {
        // average patch internal value and patch value assuming the
        // patch value to be the patchInternalField of the neighbor
        // decomposed mesh. Using double precision to avoid degrade in
        // accuracy.
        const int nComponents = vData->GetNumberOfComponents();
        for (vtkIdType faceI = 0; faceI < nFaces; faceI++)
        {
          const float* vTuple = vData->GetPointer(nComponents * faceI);
          const float* iTuple = iData->GetPointer(nComponents *
            GetLabelValue(this->FaceOwner, boundaryStartFace + faceI, faceOwner64Bit));
          float* acTuple = acData->GetPointer(nComponents * (startFace + faceI));
          for (int componentI = 0; componentI < nComponents; componentI++)
          {
            acTuple[componentI] = static_cast<float>(
              (static_cast<double>(vTuple[componentI]) + static_cast<double>(iTuple[componentI])) *
              0.5);
          }
        }
      }
    }

    if (patches.isActive(patch.index_))
    {
      vtkPolyData* bm = vtkPolyData::SafeDownCast(boundaryMesh->GetBlock(activeBoundaryIndex));
      ::AddArrayToFieldData(bm->GetCellData(), vData, io.GetObjectName(), dimString);

      if (this->Parent->GetCreateCellToPoint())
      {
        // construct cell-to-point interpolated boundary values. This
        // is done independently from allBoundary interpolation so
        // that the interpolated values are not affected by
        // neighboring patches especially at patch edges and for
        // baffle patches
        vtkFloatArray* pData = vtkFloatArray::New();
        pData->SetNumberOfComponents(vData->GetNumberOfComponents());
        vtkIdType nPoints = bm->GetPoints()->GetNumberOfPoints();
        pData->SetNumberOfTuples(nPoints);
        this->InterpolateCellToPoint(pData, vData, bm, nullptr, nPoints);
        ::AddArrayToFieldData(bm->GetPointData(), pData, io.GetObjectName(), dimString);
        pData->Delete();
      }

      ++activeBoundaryIndex;
    }
    vData->Delete();
  }
  iData->Delete();

  if (this->Parent->GetCreateCellToPoint())
  {
    // Create cell-to-point interpolated data for all boundaries and
    // override internal values
    vtkFloatArray* bpData = vtkFloatArray::New();
    bpData->SetNumberOfComponents(acData->GetNumberOfComponents());
    vtkIdType nPoints = this->AllBoundariesPointMap->GetNumberOfTuples();
    bpData->SetNumberOfTuples(nPoints);
    this->InterpolateCellToPoint(bpData, acData, this->AllBoundaries, nullptr, nPoints);
    acData->Delete();

    if (ctpData != nullptr)
    {
      const bool meshPoints64Bit = ::Is64BitArray(this->AllBoundariesPointMap);

      // set cell-to-pint data for internal mesh
      for (vtkIdType pointI = 0; pointI < nPoints; pointI++)
      {
        ctpData->SetTuple(
          GetLabelValue(this->AllBoundariesPointMap, pointI, meshPoints64Bit), pointI, bpData);
      }
      ::AddArrayToFieldData(internalMesh->GetPointData(), ctpData, io.GetObjectName(), dimString);
      ctpData->Delete();
    }

    bpData->Delete();
  }
}

//------------------------------------------------------------------------------
// Read point field at a timestep
void vtkOpenFOAMReaderPrivate::GetPointFieldAtTimeStep(const std::string& varName)
{
  vtkUnstructuredGrid* internalMesh = this->InternalMesh;
  vtkMultiBlockDataSet* boundaryMesh = this->BoundaryMesh;

  vtkFoamIOobject io(this->CasePath, this->Parent);
  vtkFoamDict dict;
  if (!this->ReadFieldFile(io, dict, varName, this->Parent->PointDataArraySelection))
  {
    return;
  }

  if (io.GetClassName().compare(0, 5, "point") != 0)
  {
    vtkErrorMacro(<< io.GetFileName() << " is not a pointField");
    return;
  }

  // Eg, from "pointScalarField" -> SCALAR_TYPE
  const auto fieldDataType(vtkFoamTypes::ToEnum(io.GetClassName(), 5));

  vtkFoamEntry* iEntry = dict.Lookup("internalField");
  if (iEntry == nullptr)
  {
    vtkErrorMacro(<< "internalField not found in " << io.GetFileName());
    return;
  }

  if (iEntry->FirstValue().GetType() == vtkFoamToken::EMPTYLIST)
  {
    // if there's no cell there shouldn't be any boundary faces either
    if (this->NumPoints > 0)
    {
      vtkErrorMacro(<< "internalField of " << io.GetFileName() << " is empty");
    }
    return;
  }

  vtkFloatArray* iData = this->FillField(*iEntry, this->NumPoints, io, fieldDataType);
  if (iData == nullptr)
  {
    return;
  }

  const std::string dimString(this->ConstructDimensions(dict));

  // AdditionalCellPoints is nullptr if creation of InternalMesh had been skipped
  if (this->AdditionalCellPoints != nullptr)
  {
    // point-to-cell interpolation to additional cell centroidal points
    // for decomposed cells
    const int nAdditionalPoints = static_cast<int>(this->AdditionalCellPoints->size());
    const int nComponents = iData->GetNumberOfComponents();
    this->ExtendArray<vtkFloatArray, float>(iData, this->NumPoints + nAdditionalPoints);

    const bool cellPoints64Bit =
      (nAdditionalPoints > 0 && ::Is64BitArray(this->AdditionalCellPoints->front()));

    for (int i = 0; i < nAdditionalPoints; i++)
    {
      vtkDataArray* acp = this->AdditionalCellPoints->operator[](i);
      vtkIdType nPoints = acp->GetDataSize();
      double interpolatedValue[9];
      for (int k = 0; k < nComponents; k++)
      {
        interpolatedValue[k] = 0.0;
      }
      for (vtkIdType j = 0; j < nPoints; j++)
      {
        const float* tuple =
          iData->GetPointer(nComponents * GetLabelValue(acp, j, cellPoints64Bit));
        for (int k = 0; k < nComponents; k++)
        {
          interpolatedValue[k] += tuple[k];
        }
      }
      const double weight = 1.0 / static_cast<double>(nPoints);
      for (int k = 0; k < nComponents; k++)
      {
        interpolatedValue[k] *= weight;
      }
      // will automatically be converted to float
      iData->InsertTuple(this->NumPoints + i, interpolatedValue);
    }
  }

  if (iData->GetSize() > 0)
  {
    // Add field only if internal Mesh exists (skip if not selected).
    // Note we still need to read internalField even if internal mesh is
    // not selected, since boundaries without value entries may refer to
    // the internalField.
    if (internalMesh != nullptr)
    {
      // Set data to internal mesh
      ::AddArrayToFieldData(internalMesh->GetPointData(), iData, io.GetObjectName(), dimString);
    }
  }
  else
  {
    // determine as there's no points
    iData->Delete();
    return;
  }

  // Boundary
  // Use patch-internal values as boundary values
  const auto& patches = this->BoundaryDict;

  unsigned int activeBoundaryIndex = 0;

  for (const vtkFoamPatch& patch : patches)
  {
    if (patches.isActive(patch.index_))
    {
      vtkFloatArray* vData = vtkFloatArray::New();
      vtkDataArray* bpMap = this->BoundaryPointMap->operator[](activeBoundaryIndex);
      const vtkIdType nPoints = bpMap->GetNumberOfTuples();
      const bool meshPoints64Bit = ::Is64BitArray(bpMap);

      vData->SetNumberOfComponents(iData->GetNumberOfComponents());
      vData->SetNumberOfTuples(nPoints);
      for (vtkIdType j = 0; j < nPoints; j++)
      {
        vData->SetTuple(j, GetLabelValue(bpMap, j, meshPoints64Bit), iData);
      }

      vtkPolyData* bm = vtkPolyData::SafeDownCast(boundaryMesh->GetBlock(activeBoundaryIndex));
      ::AddArrayToFieldData(bm->GetPointData(), vData, io.GetObjectName(), dimString);
      vData->Delete();

      ++activeBoundaryIndex;
    }
  }
  iData->Delete();
}

//------------------------------------------------------------------------------
vtkMultiBlockDataSet* vtkOpenFOAMReaderPrivate::MakeLagrangianMesh()
{
  vtkMultiBlockDataSet* lagrangianMesh = vtkMultiBlockDataSet::New();

  for (int cloudI = 0; cloudI < this->Parent->LagrangianPaths->GetNumberOfTuples(); cloudI++)
  {
    const std::string& pathI = this->Parent->LagrangianPaths->GetValue(cloudI);

    // still can't distinguish on patch selection panel, but can
    // distinguish the "lagrangian" reserved path component and a mesh
    // region with the same name
    std::string subCloudName;
    if (pathI[0] == '/')
    {
      subCloudName = pathI.substr(1, std::string::npos);
    }
    else
    {
      subCloudName = pathI;
    }
    if (this->RegionName != pathI.substr(0, pathI.find('/')) ||
      !this->Parent->GetPatchArrayStatus(subCloudName.c_str()))
    {
      continue;
    }

    const std::string cloudPath(this->CurrentTimePath() + "/" + subCloudName + "/");
    const std::string positionsPath(cloudPath + "positions");

    // create an empty mesh to keep node/leaf structure of the
    // multi-block consistent even if mesh doesn't exist
    vtkPolyData* meshI = vtkPolyData::New();

    // Extract the cloud name
    ::AppendBlock(lagrangianMesh, meshI, pathI.substr(pathI.rfind('/') + 1));

    vtkFoamIOobject io(this->CasePath, this->Parent);
    if (!io.OpenOrGzip(positionsPath))
    {
      meshI->Delete();
      continue;
    }

    vtkFoamEntryValue dict(nullptr);
    try
    {
      if (io.IsFloat64())
      {
        dict.ReadNonuniformList<vtkFoamToken::VECTORLIST,
          vtkFoamEntryValue::vectorListTraits<vtkFloatArray, double, 3, true>>(io);
      }
      else
      {
        dict.ReadNonuniformList<vtkFoamToken::VECTORLIST,
          vtkFoamEntryValue::vectorListTraits<vtkFloatArray, float, 3, true>>(io);
      }
    }
    catch (const vtkFoamError& err)
    {
      vtkErrorMacro(<< "Error reading line " << io.GetLineNumber() << " of " << io.GetFileName()
                    << ": " << err);
      meshI->Delete();
      continue;
    }
    io.Close();

    vtkFloatArray* pointArray = static_cast<vtkFloatArray*>(dict.Ptr());
    const vtkIdType nParticles = pointArray->GetNumberOfTuples();

    // instantiate the points class
    vtkPoints* points = vtkPoints::New();
    points->SetData(pointArray);
    pointArray->Delete();

    // create lagrangian mesh
    meshI->AllocateEstimate(nParticles, 1);
    for (vtkIdType i = 0; i < nParticles; i++)
    {
      meshI->InsertNextCell(VTK_VERTEX, 1, &i);
    }
    meshI->SetPoints(points);
    points->Delete();

    // read lagrangian fields
    for (int fieldI = 0; fieldI < this->LagrangianFieldFiles->GetNumberOfValues(); fieldI++)
    {
      const std::string varPath(cloudPath + this->LagrangianFieldFiles->GetValue(fieldI));

      vtkFoamIOobject io2(this->CasePath, this->Parent);
      if (!io2.Open(varPath))
      {
        // if the field file doesn't exist we simply return without
        // issuing an error as a simple way of supporting multi-region
        // lagrangians
        continue;
      }

      // if the variable is disabled on selection panel then skip it
      const std::string selectionName(io2.GetObjectName());
      if (this->Parent->LagrangianDataArraySelection->ArrayExists(selectionName.c_str()) &&
        !this->Parent->GetLagrangianArrayStatus(selectionName.c_str()))
      {
        continue;
      }

      // read the field file into dictionary
      vtkFoamEntryValue dict2(nullptr);
      if (!dict2.ReadField(io2))
      {
        vtkErrorMacro(<< "Error reading line " << io2.GetLineNumber() << " of " << io2.GetFileName()
                      << ": " << io2.GetError());
        continue;
      }

      // set lagrangian values
      if (dict2.GetType() != vtkFoamToken::SCALARLIST &&
        dict2.GetType() != vtkFoamToken::VECTORLIST && dict2.GetType() != vtkFoamToken::LABELLIST)
      {
        vtkErrorMacro(<< io2.GetFileName() << ": Unsupported lagrangian field type "
                      << io2.GetClassName());
        continue;
      }

      vtkDataArray* lData = static_cast<vtkDataArray*>(dict2.Ptr());

      // GetNumberOfTuples() works for both scalar and vector
      const vtkIdType nParticles2 = lData->GetNumberOfTuples();
      if (nParticles2 != meshI->GetNumberOfCells())
      {
        vtkErrorMacro(<< io2.GetFileName()
                      << ": Sizes of lagrangian mesh and field don't match: mesh = "
                      << meshI->GetNumberOfCells() << ", field = " << nParticles2);
        lData->Delete();
        continue;
      }

      ::AddArrayToFieldData(meshI->GetCellData(), lData, selectionName);
      if (this->Parent->GetCreateCellToPoint())
      {
        // questionable if this is worth bothering with:
        ::AddArrayToFieldData(meshI->GetPointData(), lData, selectionName);
      }
      lData->Delete();
    }
    meshI->Delete();
  }
  return lagrangianMesh;
}

//------------------------------------------------------------------------------
// Returns a dictionary of block names for a specified domain
std::unique_ptr<vtkFoamDict> vtkOpenFOAMReaderPrivate::GatherBlocks(
  const char* typeName, bool mandatory)
{
  std::string blockPath(this->CurrentTimeRegionMeshPath(this->PolyMeshFacesDir) + typeName);

  vtkFoamIOobject io(this->CasePath, this->Parent);
  if (!io.OpenOrGzip(blockPath))
  {
    if (mandatory)
    {
      vtkErrorMacro(<< "Error opening " << io.GetFileName() << ": " << io.GetError());
    }
    return nullptr;
  }

  std::unique_ptr<vtkFoamDict> dictPtr(new vtkFoamDict);
  vtkFoamDict& dict = *dictPtr;
  if (!dict.Read(io))
  {
    vtkErrorMacro(<< "Error reading line " << io.GetLineNumber() << " of " << io.GetFileName()
                  << ": " << io.GetError());
    return nullptr;
  }
  if (dict.GetType() != vtkFoamToken::DICTIONARY)
  {
    vtkErrorMacro(<< "The file type of " << io.GetFileName() << " is not a dictionary");
    return nullptr;
  }
  return dictPtr;
}

//------------------------------------------------------------------------------
// Populate cell zone(s) mesh

bool vtkOpenFOAMReaderPrivate::GetCellZoneMesh(vtkMultiBlockDataSet* zoneMesh,
  const vtkFoamLabelListList& meshCells, const vtkFoamLabelListList& meshFaces, vtkPoints* points)
{
  constexpr const char* const zonePrefix = "cellZone";
  constexpr const char* const zoneTypeName = "cellZones";
  constexpr const char* const labelsName = "cellLabels";

  auto zonesDictPtr(this->GatherBlocks(zoneTypeName, false));
  if (zonesDictPtr == nullptr)
  {
    // Not an error if mesh zones are missing
    return true;
  }

  const vtkFoamDict& zones = *zonesDictPtr;
  // const bool use64BitLabels = zones.IsLabel64();

  // Limits
  const unsigned nZones = static_cast<unsigned>(zones.size());
  const vtkIdType maxLabels = this->NumCells;

  for (unsigned zonei = 0; zonei < nZones; ++zonei)
  {
    const std::string& zoneName = zones[zonei]->GetKeyword();
    const vtkFoamDict& dict = zones[zonei]->Dictionary();

    // Look up cellLabels
    vtkFoamEntry* eptr = dict.Lookup(labelsName);
    if (eptr == nullptr)
    {
      vtkErrorMacro(<< labelsName << " not found in " << zonePrefix);
      return false;
    }
    vtkFoamEntryValue& labelsEntry = eptr->FirstValue();

    // Some OpenFOAM versions write an empty list as zero label only (in binary)
    if ((labelsEntry.GetType() == vtkFoamToken::EMPTYLIST) ||
      (labelsEntry.GetType() == vtkFoamToken::LABEL && labelsEntry.ToInt() == 0))
    {
      // Allocate empty mesh if the list is empty
      vtkUnstructuredGrid* zm = vtkUnstructuredGrid::New();
      ::SetBlock(zoneMesh, zonei, zm, zoneName);
      zm->Delete();
      continue;
    }
    else if (labelsEntry.GetType() != vtkFoamToken::LABELLIST)
    {
      vtkErrorMacro(<< labelsName << " is not a labelList");
      return false;
    }

    vtkDataArray& labels = labelsEntry.LabelList();
    const vtkIdType nLabels = labels.GetNumberOfTuples();
    if (nLabels > maxLabels)
    {
      // Extremely improbable entry
      vtkErrorMacro(<< "The length " << zonePrefix << '/' << zoneName << '=' << nLabels
                    << " exceeds number of cells " << maxLabels);
      return false;
    }

    // Allocate new grid: we do not use resize() beforehand since it
    // could lead to undefined pointers if we return by error
    vtkUnstructuredGrid* zm = vtkUnstructuredGrid::New();
    zm->Allocate(nLabels);

    // Insert cells
    this->InsertCellsToGrid(zm, meshCells, meshFaces, nullptr, nullptr, &labels);

    // Set points for zone
    zm->SetPoints(points);

    ::SetBlock(zoneMesh, zonei, zm, zoneName);
    zm->Delete();
  }

  return true;
}

//------------------------------------------------------------------------------
// Populate face zone(s) mesh

bool vtkOpenFOAMReaderPrivate::GetFaceZoneMesh(
  vtkMultiBlockDataSet* zoneMesh, const vtkFoamLabelListList& meshFaces, vtkPoints* points)
{
  constexpr const char* const zonePrefix = "faceZone";
  constexpr const char* const zoneTypeName = "faceZones";
  constexpr const char* const labelsName = "faceLabels";

  auto zonesDictPtr(this->GatherBlocks(zoneTypeName, false));
  if (zonesDictPtr == nullptr)
  {
    // Not an error if mesh zones are missing
    return true;
  }

  const vtkFoamDict& zones = *zonesDictPtr;
  const bool use64BitLabels = zones.IsLabel64();

  // Limits
  const unsigned nZones = static_cast<unsigned>(zones.size());
  const vtkIdType maxLabels = this->FaceOwner->GetNumberOfTuples(); // NumFaces

  for (unsigned zonei = 0; zonei < nZones; ++zonei)
  {
    const std::string& zoneName = zones[zonei]->GetKeyword();
    const vtkFoamDict& dict = zones[zonei]->Dictionary();

    // Look up faceLabels
    vtkFoamEntry* eptr = dict.Lookup(labelsName);
    if (eptr == nullptr)
    {
      vtkErrorMacro(<< labelsName << " not found in " << zonePrefix);
      return false;
    }
    vtkFoamEntryValue& labelsEntry = eptr->FirstValue();

    // Some OpenFOAM versions write an empty list as zero label only (in binary)
    if ((labelsEntry.GetType() == vtkFoamToken::EMPTYLIST) ||
      (labelsEntry.GetType() == vtkFoamToken::LABEL && labelsEntry.ToInt() == 0))
    {
      // Allocate empty mesh if the list is empty
      vtkPolyData* zm = vtkPolyData::New();
      ::SetBlock(zoneMesh, zonei, zm, zoneName);
      zm->Delete();
      continue;
    }
    else if (labelsEntry.GetType() != vtkFoamToken::LABELLIST)
    {
      vtkErrorMacro(<< labelsName << " is not a labelList");
      return false;
    }

    vtkDataArray& labels = labelsEntry.LabelList();
    const vtkIdType nLabels = labels.GetNumberOfTuples();
    if (nLabels > maxLabels)
    {
      // Extremely improbable entry
      vtkErrorMacro(<< "The length " << zonePrefix << '/' << zoneName << '=' << nLabels
                    << " exceeds number of faces " << maxLabels);
      return false;
    }

    // Allocate new grid: we do not use resize() beforehand since it
    // could lead to undefined pointer if we return by error
    vtkPolyData* zm = vtkPolyData::New();
    zm->AllocateEstimate(nLabels, 1);

    // allocate array for converting int vector to vtkIdType vector:
    // workaround for 64bit machines
    vtkIdType maxNFacePoints = 0;
    for (vtkIdType labeli = 0; labeli < nLabels; ++labeli)
    {
      vtkIdType nFacePoints = meshFaces.GetSize(GetLabelValue(&labels, labeli, use64BitLabels));
      if (maxNFacePoints < nFacePoints)
      {
        maxNFacePoints = nFacePoints;
      }
    }
    vtkIdList* facePointsVtkId = vtkIdList::New();
    facePointsVtkId->SetNumberOfIds(maxNFacePoints);

    // Insert faces
    this->InsertFacesToGrid(zm, meshFaces, 0, nLabels, nullptr, facePointsVtkId, &labels, false);
    facePointsVtkId->Delete();

    // Set points for zone
    zm->SetPoints(points);

    ::SetBlock(zoneMesh, zonei, zm, zoneName);
    zm->Delete();
  }

  return true;
}

//------------------------------------------------------------------------------
// Populate point zone(s) mesh

bool vtkOpenFOAMReaderPrivate::GetPointZoneMesh(vtkMultiBlockDataSet* zoneMesh, vtkPoints* points)
{
  constexpr const char* const zonePrefix = "pointZone";
  constexpr const char* const zoneTypeName = "pointZones";
  constexpr const char* const labelsName = "pointLabels";

  auto zonesDictPtr(this->GatherBlocks(zoneTypeName, false));
  if (!zonesDictPtr)
  {
    // Not an error if mesh zones are missing
    return true;
  }

  const vtkFoamDict& zones = *zonesDictPtr;
  const bool use64BitLabels = zones.IsLabel64();

  // Limits
  const unsigned nZones = static_cast<unsigned>(zones.size());
  const vtkIdType maxLabels = this->NumPoints;

  for (unsigned zonei = 0; zonei < nZones; ++zonei)
  {
    const std::string& zoneName = zones[zonei]->GetKeyword();
    const vtkFoamDict& dict = zones[zonei]->Dictionary();

    // Look up pointLabels
    vtkFoamEntry* eptr = dict.Lookup(labelsName);
    if (eptr == nullptr)
    {
      vtkErrorMacro(<< labelsName << " not found in " << zonePrefix);
      return false;
    }
    vtkFoamEntryValue& labelsEntry = eptr->FirstValue();

    // Some OpenFOAM versions write an empty list as zero label only (in binary)
    if ((labelsEntry.GetType() == vtkFoamToken::EMPTYLIST) ||
      (labelsEntry.GetType() == vtkFoamToken::LABEL && labelsEntry.ToInt() == 0))
    {
      // Allocate empty mesh if the list is empty
      vtkPolyData* zm = vtkPolyData::New();
      ::SetBlock(zoneMesh, zonei, zm, zoneName);
      zm->Delete();
      continue;
    }
    else if (labelsEntry.GetType() != vtkFoamToken::LABELLIST)
    {
      vtkErrorMacro(<< labelsName << " is not a labelList");
      return false;
    }

    vtkDataArray& labels = labelsEntry.LabelList();
    const vtkIdType nLabels = labels.GetNumberOfTuples();
    if (nLabels > maxLabels)
    {
      // Extremely improbable entry
      vtkErrorMacro(<< "The length " << zonePrefix << '/' << zoneName << '=' << nLabels
                    << " exceeds number of points " << maxLabels);
      return false;
    }

    // Allocate new grid: we do not use resize() beforehand since it
    // could lead to undefined pointer if we return by error
    vtkPolyData* zm = vtkPolyData::New();
    zm->AllocateEstimate(nLabels, 1);

    // Insert points
    for (vtkIdType labeli = 0; labeli < nLabels; ++labeli)
    {
      vtkIdType pointLabel =
        static_cast<vtkIdType>(GetLabelValue(&labels, labeli, use64BitLabels)); // must be vtkIdType

      if (pointLabel < maxLabels)
      {
        zm->InsertNextCell(VTK_VERTEX, 1, &pointLabel);
      }
      else
      {
        zm->InsertNextCell(VTK_EMPTY_CELL, 0, &pointLabel);
        vtkWarningMacro(<< labelsName << " id " << pointLabel << " exceeds number of points "
                        << maxLabels);
      }
    }

    // Set points for zone
    zm->SetPoints(points);

    ::SetBlock(zoneMesh, zonei, zm, zoneName);
    zm->Delete();
  }

  return true;
}

//------------------------------------------------------------------------------
// return 0 if there's any error, 1 if success
int vtkOpenFOAMReaderPrivate::RequestData(vtkMultiBlockDataSet* output)
{
  if (!this->HasPolyMesh())
  {
    // Ignore a region without a mesh, but will normally be precluded earlier
    vtkWarningMacro("Called RequestData without a mesh.");
    return 1;
  }

  //----------------------------------------
  // Determine changes in state

  // Basics
  const bool changedStorageType =
    (this->Parent->Use64BitLabels != this->Parent->Use64BitLabelsOld) ||
    (this->Parent->Use64BitFloats != this->Parent->Use64BitFloatsOld);

  // Mesh changes
  const bool topoChanged = (this->TimeStepOld == -1) || (this->FaceOwner == nullptr) ||
    (this->PolyMeshFacesDir->GetValue(this->TimeStep) !=
      this->PolyMeshFacesDir->GetValue(this->TimeStepOld));

  const bool pointsMoved = (this->TimeStepOld == -1) ||
    (this->PolyMeshPointsDir->GetValue(this->TimeStep) !=
      this->PolyMeshPointsDir->GetValue(this->TimeStepOld));

  // Internal mesh
  bool recreateInternalMesh = (changedStorageType) || (topoChanged) || (!this->Parent->CacheMesh) ||
    (this->Parent->SkipZeroTime != this->Parent->SkipZeroTimeOld) ||
    (this->Parent->ListTimeStepsByControlDict != this->Parent->ListTimeStepsByControlDictOld);

  // Internal mesh - selection changes
  recreateInternalMesh |=
    (this->InternalMeshSelectionStatus != this->InternalMeshSelectionStatusOld);

  if (this->InternalMeshSelectionStatus)
  {
    // Cell representation changed that affects the internalMesh
    recreateInternalMesh |=
      (this->Parent->DecomposePolyhedra != this->Parent->DecomposePolyhedraOld);
  }

  // NOTE: this is still not quite right for zones, but until we get better separation
  // - can remove zones without triggering reread
  recreateInternalMesh |=
    (this->Parent->ReadZones && (this->Parent->ReadZones != this->Parent->ReadZonesOld));

  // Boundary mesh
  bool recreateBoundaryMesh = (changedStorageType) ||
    (this->Parent->PatchDataArraySelection->GetMTime() != this->Parent->PatchSelectionMTimeOld) ||
    (this->Parent->CreateCellToPoint != this->Parent->CreateCellToPointOld);

  // Fields
  bool updateVariables = (changedStorageType) || (this->TimeStep != this->TimeStepOld) ||
    (this->Parent->CellDataArraySelection->GetMTime() != this->Parent->CellSelectionMTimeOld) ||
    (this->Parent->PointDataArraySelection->GetMTime() != this->Parent->PointSelectionMTimeOld) ||
    (this->Parent->LagrangianDataArraySelection->GetMTime() !=
      this->Parent->LagrangianSelectionMTimeOld) ||
    (this->Parent->PositionsIsIn13Format != this->Parent->PositionsIsIn13FormatOld) ||
    (this->Parent->AddDimensionsToArrayNames != this->Parent->AddDimensionsToArrayNamesOld);

  // Apply these changes too
  recreateBoundaryMesh |= recreateInternalMesh;
  updateVariables |= recreateBoundaryMesh;

  const bool moveInternalPoints = !recreateInternalMesh && pointsMoved;
  const bool moveBoundaryPoints = !recreateBoundaryMesh && pointsMoved;

  // RegionName check is added since subregions have region name prefixes
  const bool createEulerians =
    this->Parent->PatchDataArraySelection->ArrayExists(NAME_INTERNALMESH) ||
    !this->RegionName.empty();

  vtkFoamDebug(<< "RequestData (" << this->RegionName << "/" << this->ProcessorName << ")\n"
               << " internal=" << recreateInternalMesh      //
               << " boundary=" << recreateBoundaryMesh      //
               << " zones=" << this->Parent->GetReadZones() //
               << " topoChanged=" << topoChanged            //
               << " pointsMoved=" << pointsMoved            //
               << " variables=" << updateVariables          //
               << " eulerians=" << createEulerians          //
               << "\n");

  //----------------------------------------

  // Determine if we need to reconstruct meshes
  if (recreateInternalMesh)
  {
    this->ClearInternalMeshes();
    this->ClearZoneMeshes();
  }
  if (recreateBoundaryMesh)
  {
    this->ClearBoundaryMeshes();
  }
  // Discard unwanted remnant zones
  if (!this->Parent->GetReadZones())
  {
    this->ClearZoneMeshes();
  }

  // Mesh primitives
  std::unique_ptr<vtkFoamLabelListList> meshCells;
  std::unique_ptr<vtkFoamLabelListList> meshFaces;

  std::string meshDir;
  if (createEulerians && (recreateInternalMesh || recreateBoundaryMesh))
  {
    // Path to polyMesh/ files
    meshDir = this->CurrentTimeRegionMeshPath(this->PolyMeshFacesDir);

    // Read polyMesh/faces, create the list of faces, set the number of faces
    meshFaces = this->ReadFacesFile(meshDir);
    if (!meshFaces)
    {
      return 0;
    }
    this->Parent->UpdateProgress(0.2);
  }

  if (createEulerians && recreateInternalMesh)
  {
    // Read polyMesh/{owner,neighbour}, create FaceOwner/FaceNeigh and meshCells vectors
    meshCells = this->ReadOwnerNeighbourFiles(meshDir);
    if (this->FaceNeigh)
    {
      // Transitional code - do not yet use neighbour information
      this->FaceNeigh->Delete();
      this->FaceNeigh = nullptr;
    }
    if (!meshCells)
    {
      return 0;
    }
    this->Parent->UpdateProgress(0.3);
  }

  vtkFloatArray* pointArray = nullptr;
  if (createEulerians &&
    (recreateInternalMesh ||
      (recreateBoundaryMesh && !recreateInternalMesh && this->InternalMesh == nullptr) ||
      moveInternalPoints || moveBoundaryPoints))
  {
    // Read polyMesh/points, set the number of faces
    pointArray = this->ReadPointsFile();
    if ((pointArray == nullptr && recreateInternalMesh) ||
      (meshFaces && !this->CheckFaceList(*meshFaces)))
    {
      return 0;
    }
    this->Parent->UpdateProgress(0.4);
  }

  // make internal mesh
  // Create Internal Mesh only if required for display
  if (createEulerians && recreateInternalMesh)
  {
    const std::string displayName(this->RegionPrefix() + NAME_INTERNALMESH);
    if (this->Parent->GetPatchArrayStatus(displayName.c_str()))
    {
      this->InternalMesh = this->MakeInternalMesh(*meshCells, *meshFaces, pointArray);
    }
  }

  // Read and construct zones
  if (createEulerians && recreateInternalMesh && this->Parent->GetReadZones())
  {
    vtkPoints* points = nullptr;
    if (this->InternalMesh != nullptr)
    {
      points = this->InternalMesh->GetPoints();
    }
    else
    {
      points = vtkPoints::New();
      points->SetData(pointArray);
    }

    this->PointZoneMesh = vtkMultiBlockDataSet::New();
    if (!this->GetPointZoneMesh(this->PointZoneMesh, points))
    {
      this->ClearZoneMeshes();
      if (this->InternalMesh == nullptr)
      {
        points->Delete();
      }
      pointArray->Delete();
      return 0;
    }
    if (this->PointZoneMesh->GetNumberOfBlocks() == 0)
    {
      this->PointZoneMesh->Delete();
      this->PointZoneMesh = nullptr;
    }

    this->FaceZoneMesh = vtkMultiBlockDataSet::New();
    if (!this->GetFaceZoneMesh(this->FaceZoneMesh, *meshFaces, points))
    {
      this->ClearZoneMeshes();
      if (this->InternalMesh == nullptr)
      {
        points->Delete();
      }
      pointArray->Delete();
      return 0;
    }
    if (this->FaceZoneMesh->GetNumberOfBlocks() == 0)
    {
      this->FaceZoneMesh->Delete();
      this->FaceZoneMesh = nullptr;
    }

    this->CellZoneMesh = vtkMultiBlockDataSet::New();
    if (!this->GetCellZoneMesh(this->CellZoneMesh, *meshCells, *meshFaces, points))
    {
      this->ClearZoneMeshes();
      if (this->InternalMesh == nullptr)
      {
        points->Delete();
      }
      pointArray->Delete();
      return 0;
    }
    if (this->CellZoneMesh->GetNumberOfBlocks() == 0)
    {
      this->CellZoneMesh->Delete();
      this->CellZoneMesh = nullptr;
    }
    if (this->InternalMesh == nullptr)
    {
      points->Delete();
    }
  }

  // Don't need meshCells beyond here
  meshCells.reset(nullptr);

  // Only need boundary face owners beyond here
  if (createEulerians && recreateInternalMesh)
  {
    this->TruncateFaceOwner();
  }

  if (createEulerians && recreateBoundaryMesh)
  {
    vtkFloatArray* boundaryPointArray;
    if (pointArray != nullptr)
    {
      boundaryPointArray = pointArray;
    }
    else
    {
      boundaryPointArray = static_cast<vtkFloatArray*>(this->InternalMesh->GetPoints()->GetData());
    }
    // Create boundary mesh
    this->BoundaryMesh = this->MakeBoundaryMesh(*meshFaces, boundaryPointArray);
    if (this->BoundaryMesh == nullptr)
    {
      if (pointArray != nullptr)
      {
        pointArray->Delete();
      }
      return 0;
    }
  }

  // Don't need meshFaces beyond here
  meshFaces.reset(nullptr);

  // if only point coordinates change refresh point vector
  if (createEulerians && moveInternalPoints)
  {
    // refresh the points in each mesh
    vtkPoints* points;
    // Check if Internal Mesh exists first....
    if (this->InternalMesh != nullptr)
    {
      points = this->MoveInternalMesh(this->InternalMesh, pointArray);
      if (points == nullptr)
      {
        pointArray->Delete();
        return 0;
      }
    }
    else
    {
      points = vtkPoints::New();
      points->SetData(pointArray);
    }

    if (this->PointZoneMesh != nullptr)
    {
      for (unsigned int i = 0; i < this->PointZoneMesh->GetNumberOfBlocks(); i++)
      {
        vtkPolyData::SafeDownCast(this->PointZoneMesh->GetBlock(i))->SetPoints(points);
      }
    }
    if (this->FaceZoneMesh != nullptr)
    {
      for (unsigned int i = 0; i < this->FaceZoneMesh->GetNumberOfBlocks(); i++)
      {
        vtkPolyData::SafeDownCast(this->FaceZoneMesh->GetBlock(i))->SetPoints(points);
      }
    }
    if (this->CellZoneMesh != nullptr)
    {
      for (unsigned int i = 0; i < this->CellZoneMesh->GetNumberOfBlocks(); i++)
      {
        vtkUnstructuredGrid::SafeDownCast(this->CellZoneMesh->GetBlock(i))->SetPoints(points);
      }
    }
    points->Delete();
  }

  if (createEulerians && moveBoundaryPoints)
  {
    // Check if Boundary Mesh exists first....
    if (this->BoundaryMesh != nullptr)
    {
      this->MoveBoundaryMesh(this->BoundaryMesh, pointArray);
    }
  }

  if (pointArray != nullptr)
  {
    pointArray->Delete();
  }
  this->Parent->UpdateProgress(0.5);

  vtkMultiBlockDataSet* lagrangianMesh = nullptr;
  if (updateVariables)
  {
    if (createEulerians)
    {
      // Clean up arrays of the previous timestep

      // Internal
      if (!recreateInternalMesh && this->InternalMesh != nullptr)
      {
        this->InternalMesh->GetCellData()->Initialize();
        this->InternalMesh->GetPointData()->Initialize();
      }

      // Boundary
      if (!recreateBoundaryMesh && this->BoundaryMesh != nullptr)
      {
        for (unsigned int i = 0; i < this->BoundaryMesh->GetNumberOfBlocks(); i++)
        {
          auto* bm = vtkPolyData::SafeDownCast(this->BoundaryMesh->GetBlock(i));
          bm->GetCellData()->Initialize();
          bm->GetPointData()->Initialize();
        }
      }

      // read field data variables into Internal/Boundary meshes
      vtkIdType nFieldsRead = 0;
      const vtkIdType nFieldsToRead = (this->VolFieldFiles->GetNumberOfValues() +
        this->DimFieldFiles->GetNumberOfValues() + this->PointFieldFiles->GetNumberOfValues());

      for (vtkIdType i = 0; i < this->VolFieldFiles->GetNumberOfValues(); ++i)
      {
        this->GetVolFieldAtTimeStep(this->VolFieldFiles->GetValue(i));
        this->Parent->UpdateProgress(0.5 + (0.5 * ++nFieldsRead) / nFieldsToRead);
      }
      for (vtkIdType i = 0; i < this->DimFieldFiles->GetNumberOfValues(); ++i)
      {
        this->GetVolFieldAtTimeStep(this->DimFieldFiles->GetValue(i), true); // Internal field
        this->Parent->UpdateProgress(0.5 + (0.5 * ++nFieldsRead) / nFieldsToRead);
      }
      for (vtkIdType i = 0; i < this->PointFieldFiles->GetNumberOfValues(); ++i)
      {
        this->GetPointFieldAtTimeStep(this->PointFieldFiles->GetValue(i));
        this->Parent->UpdateProgress(0.5 + (0.5 * ++nFieldsRead) / nFieldsToRead);
      }
    }
    // read lagrangian mesh and fields
    lagrangianMesh = this->MakeLagrangianMesh();
  }

  if (this->InternalMesh && this->Parent->CopyDataToCellZones && this->CellZoneMesh)
  {
    for (unsigned int i = 0; i < this->CellZoneMesh->GetNumberOfBlocks(); i++)
    {
      vtkUnstructuredGrid* ug = vtkUnstructuredGrid::SafeDownCast(this->CellZoneMesh->GetBlock(i));
      vtkIdTypeArray* idArray = vtkIdTypeArray::SafeDownCast(ug->GetCellData()->GetArray("CellId"));

      // allocate arrays, cellId array will be removed
      ug->GetCellData()->CopyAllocate(this->InternalMesh->GetCellData(), ug->GetNumberOfCells());

      // copy tuples
      for (vtkIdType j = 0; j < ug->GetNumberOfCells(); j++)
      {
        ug->GetCellData()->CopyData(this->InternalMesh->GetCellData(), idArray->GetValue(j), j);
      }

      // we need to add the id array because it has been previously removed
      ug->GetCellData()->AddArray(idArray);

      // copy points data
      ug->GetPointData()->ShallowCopy(this->InternalMesh->GetPointData());
    }
  }

  // Add Internal mesh to final output only if selected for display
  if (this->InternalMesh != nullptr)
  {
    ::SetBlock(output, 0, this->InternalMesh, NAME_INTERNALMESH);
  }

  // Set boundary meshes/data as output
  if (this->BoundaryMesh != nullptr && this->BoundaryMesh->GetNumberOfBlocks())
  {
    ::AppendBlock(output, this->BoundaryMesh, "boundary");
  }

  // Set lagrangian mesh as output
  if (lagrangianMesh != nullptr)
  {
    if (lagrangianMesh->GetNumberOfBlocks())
    {
      ::AppendBlock(output, lagrangianMesh, "lagrangian");
    }
    lagrangianMesh->Delete();
  }

  if (this->Parent->GetReadZones())
  {
    // Set zone meshes as output
    vtkMultiBlockDataSet* zones = nullptr;

    if (this->CellZoneMesh != nullptr)
    {
      if (zones == nullptr)
      {
        zones = vtkMultiBlockDataSet::New();
      }
      ::AppendBlock(zones, this->CellZoneMesh, "cellZones");
    }

    if (this->FaceZoneMesh != nullptr)
    {
      if (zones == nullptr)
      {
        zones = vtkMultiBlockDataSet::New();
      }
      ::AppendBlock(zones, this->FaceZoneMesh, "faceZones");
    }

    if (this->PointZoneMesh != nullptr)
    {
      if (zones == nullptr)
      {
        zones = vtkMultiBlockDataSet::New();
      }
      ::AppendBlock(zones, this->PointZoneMesh, "pointZones");
    }

    if (zones != nullptr)
    {
      ::AppendBlock(output, zones, "zones");
    }
  }

  if (this->Parent->GetCacheMesh())
  {
    this->TimeStepOld = this->TimeStep;
  }
  else
  {
    this->ClearMeshes();
    this->TimeStepOld = -1;
  }
  this->InternalMeshSelectionStatusOld = this->InternalMeshSelectionStatus;

  this->Parent->UpdateProgress(1.0);
  return 1;
}

//------------------------------------------------------------------------------
// constructor
vtkOpenFOAMReader::vtkOpenFOAMReader()
{
  this->SetNumberOfInputPorts(0);

  this->Parent = this;
  // must be false to avoid reloading by vtkAppendCompositeDataLeaves::Update()
  this->Refresh = false;

  // initialize file name
  this->FileName = nullptr;
  this->FileNameOld = new vtkStdString;

  // Case path
  this->CasePath = vtkCharArray::New();

  // Child readers
  this->Readers = vtkCollection::New();

  // VTK CLASSES
  this->PatchDataArraySelection = vtkDataArraySelection::New();
  this->CellDataArraySelection = vtkDataArraySelection::New();
  this->PointDataArraySelection = vtkDataArraySelection::New();
  this->LagrangianDataArraySelection = vtkDataArraySelection::New();

  this->PatchSelectionMTimeOld = 0;
  this->CellSelectionMTimeOld = 0;
  this->PointSelectionMTimeOld = 0;
  this->LagrangianSelectionMTimeOld = 0;

  // For creating cell-to-point translated data
  this->CreateCellToPoint = 1;
  this->CreateCellToPointOld = 1;

  // For caching mesh
  this->CacheMesh = 1;

  // For decomposing polyhedra
  this->DecomposePolyhedra = 0;
  this->DecomposePolyhedraOld = 0;

  // for lagrangian/positions format without the additional data that existed
  // in OpenFOAM 1.4-2.4
  this->PositionsIsIn13Format = 1;
  this->PositionsIsIn13FormatOld = 1;

  // for reading zones
  this->ReadZones = 0; // turned off by default
  this->ReadZonesOld = 0;

  // Ignore 0/ time directory, which is normally missing Lagrangian fields
  this->SkipZeroTime = false;
  this->SkipZeroTimeOld = false;

  // determine if time directories are to be listed according to controlDict
  this->ListTimeStepsByControlDict = 0;
  this->ListTimeStepsByControlDictOld = 0;

  // add dimensions to array names
  this->AddDimensionsToArrayNames = 0;
  this->AddDimensionsToArrayNamesOld = 0;

  // Lagrangian paths
  this->LagrangianPaths = vtkStringArray::New();

  this->CurrentReaderIndex = 0;
  this->NumberOfReaders = 0;
  this->Use64BitLabels = false;
  this->Use64BitFloats = true;
  this->Use64BitLabelsOld = false;
  this->Use64BitFloatsOld = true;
  this->CopyDataToCellZones = false;
}

//------------------------------------------------------------------------------
// destructor
vtkOpenFOAMReader::~vtkOpenFOAMReader()
{
  this->LagrangianPaths->Delete();

  this->PatchDataArraySelection->Delete();
  this->CellDataArraySelection->Delete();
  this->PointDataArraySelection->Delete();
  this->LagrangianDataArraySelection->Delete();

  this->Readers->Delete();
  this->CasePath->Delete();

  this->SetFileName(nullptr);
  delete this->FileNameOld;
}

//------------------------------------------------------------------------------
// CanReadFile
int vtkOpenFOAMReader::CanReadFile(const char* vtkNotUsed(fileName))
{
  return 1; // so far CanReadFile does nothing.
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReader::SetUse64BitLabels(bool val)
{
  if (this->Use64BitLabels != val)
  {
    this->Use64BitLabels = val;
    this->Refresh = true; // Need to reread everything
    this->Modified();
  }
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReader::SetUse64BitFloats(bool val)
{
  if (this->Use64BitFloats != val)
  {
    this->Use64BitFloats = val;
    this->Refresh = true; // Need to reread everything
    this->Modified();
  }
}

//------------------------------------------------------------------------------
// PrintSelf
void vtkOpenFOAMReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "File Name: " << (this->FileName ? this->FileName : "(none)") << endl;
  os << indent << "Refresh: " << this->Refresh << endl;
  os << indent << "CreateCellToPoint: " << this->CreateCellToPoint << endl;
  os << indent << "CacheMesh: " << this->CacheMesh << endl;
  os << indent << "DecomposePolyhedra: " << this->DecomposePolyhedra << endl;
  os << indent << "PositionsIsIn13Format: " << this->PositionsIsIn13Format << endl;
  os << indent << "ReadZones: " << this->ReadZones << endl;
  os << indent << "SkipZeroTime: " << this->SkipZeroTime << endl;
  os << indent << "ListTimeStepsByControlDict: " << this->ListTimeStepsByControlDict << endl;
  os << indent << "AddDimensionsToArrayNames: " << this->AddDimensionsToArrayNames << endl;

  this->Readers->InitTraversal();
  vtkObject* reader;
  while ((reader = this->Readers->GetNextItemAsObject()) != nullptr)
  {
    os << indent << "Reader instance " << static_cast<void*>(reader) << ": \n";
    reader->PrintSelf(os, indent.GetNextIndent());
  }
}

//------------------------------------------------------------------------------
// selection list handlers

int vtkOpenFOAMReader::GetNumberOfSelectionArrays(vtkDataArraySelection* s)
{
  return s->GetNumberOfArrays();
}

int vtkOpenFOAMReader::GetSelectionArrayStatus(vtkDataArraySelection* s, const char* name)
{
  return s->ArrayIsEnabled(name);
}

void vtkOpenFOAMReader::SetSelectionArrayStatus(
  vtkDataArraySelection* s, const char* name, int status)
{
  vtkMTimeType mTime = s->GetMTime();
  if (status)
  {
    s->EnableArray(name);
  }
  else
  {
    s->DisableArray(name);
  }
  if (mTime != s->GetMTime()) // indicate that the pipeline needs to be updated
  {
    this->Modified();
  }
}

const char* vtkOpenFOAMReader::GetSelectionArrayName(vtkDataArraySelection* s, int index)
{
  return s->GetArrayName(index);
}

void vtkOpenFOAMReader::DisableAllSelectionArrays(vtkDataArraySelection* s)
{
  vtkMTimeType mTime = s->GetMTime();
  s->DisableAllArrays();
  if (mTime != s->GetMTime())
  {
    this->Modified();
  }
}

void vtkOpenFOAMReader::EnableAllSelectionArrays(vtkDataArraySelection* s)
{
  vtkMTimeType mTime = s->GetMTime();
  s->EnableAllArrays();
  if (mTime != s->GetMTime())
  {
    this->Modified();
  }
}

//------------------------------------------------------------------------------
// RequestInformation
int vtkOpenFOAMReader::RequestInformation(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** vtkNotUsed(inputVector), vtkInformationVector* outputVector)
{
  if (!this->FileName || !*(this->FileName))
  {
    vtkErrorMacro("FileName has to be specified!");
    return 0;
  }

  if (this->Parent == this &&
    ((*this->FileNameOld != this->FileName) || this->Refresh ||
      (this->ListTimeStepsByControlDict != this->ListTimeStepsByControlDictOld) ||
      (this->SkipZeroTime != this->SkipZeroTimeOld)))
  {
    // Retain selection status when just refreshing a case
    if (!this->FileNameOld->empty() && *this->FileNameOld != this->FileName)
    {
      // Clear selections
      this->CellDataArraySelection->RemoveAllArrays();
      this->PointDataArraySelection->RemoveAllArrays();
      this->LagrangianDataArraySelection->RemoveAllArrays();
      this->PatchDataArraySelection->RemoveAllArrays();
    }

    // Reset NumberOfReaders here so that the variable will not be
    // reset unwantedly when MakeInformationVector() is called from
    // vtkPOpenFOAMReader
    this->NumberOfReaders = 0;

    if (!this->MakeInformationVector(outputVector, vtkStdString("")) ||
      !this->MakeMetaDataAtTimeStep(true))
    {
      return 0;
    }
    this->Refresh = false;
  }
  return 1;
}

//------------------------------------------------------------------------------
// RequestData
int vtkOpenFOAMReader::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** vtkNotUsed(inputVector), vtkInformationVector* outputVector)
{
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  vtkMultiBlockDataSet* output =
    vtkMultiBlockDataSet::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  // Times
  {
    int nTimes = 0;
    double requestedTimeValue(0);
    if (outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP()))
    {
      nTimes = outInfo->Length(vtkStreamingDemandDrivenPipeline::TIME_STEPS());

      requestedTimeValue = (1 == nTimes
          // Only one time-step available, UPDATE_TIME_STEP is unreliable
          ? outInfo->Get(vtkStreamingDemandDrivenPipeline::TIME_STEPS(), 0)
          : outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP()));
    }

    if (nTimes > 0)
    {
      outInfo->Set(vtkDataObject::DATA_TIME_STEP(), requestedTimeValue);
      this->SetTimeValue(requestedTimeValue);
    }
  }

  if (this->Parent == this)
  {
    output->GetFieldData()->AddArray(this->CasePath);
    if (!this->MakeMetaDataAtTimeStep(false))
    {
      return 0;
    }
    this->CurrentReaderIndex = 0;
  }

  // Create dataset
  int ret = 1;
  vtkOpenFOAMReaderPrivate* reader;

  // Avoid wrapping single region as a multiblock dataset
  if (this->Readers->GetNumberOfItems() == 1 &&
    (reader = vtkOpenFOAMReaderPrivate::SafeDownCast(this->Readers->GetItemAsObject(0)))
      ->GetRegionName()
      .empty())
  {
    ret = reader->RequestData(output);
    this->Parent->CurrentReaderIndex++;
  }
  else
  {
    this->Readers->InitTraversal();
    while ((reader = vtkOpenFOAMReaderPrivate::SafeDownCast(
              this->Readers->GetNextItemAsObject())) != nullptr)
    {
      auto subOutput = vtkSmartPointer<vtkMultiBlockDataSet>::New();
      if (reader->RequestData(subOutput))
      {
        std::string regionName(reader->GetRegionName());
        if (regionName.empty())
        {
          regionName = "defaultRegion";
        }
        if (reader->HasPolyMesh()) // sanity check
        {
          ::AppendBlock(output, subOutput, regionName);
        }
      }
      else
      {
        ret = 0;
      }
      this->Parent->CurrentReaderIndex++;
    }
  }

  if (this->Parent == this) // update only if this is the top-level reader
  {
    this->UpdateStatus();
  }

  return ret;
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReader::SetTimeInformation(
  vtkInformationVector* outputVector, vtkDoubleArray* timeValues)
{
  double timeRange[2];
  if (timeValues->GetNumberOfTuples() > 0)
  {
    outputVector->GetInformationObject(0)->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
      timeValues->GetPointer(0), static_cast<int>(timeValues->GetNumberOfTuples()));

    timeRange[0] = timeValues->GetValue(0);
    timeRange[1] = timeValues->GetValue(timeValues->GetNumberOfTuples() - 1);
  }
  else
  {
    timeRange[0] = timeRange[1] = 0.0;
    outputVector->GetInformationObject(0)->Set(
      vtkStreamingDemandDrivenPipeline::TIME_STEPS(), timeRange, 0);
  }
  outputVector->GetInformationObject(0)->Set(
    vtkStreamingDemandDrivenPipeline::TIME_RANGE(), timeRange, 2);
}

//------------------------------------------------------------------------------
int vtkOpenFOAMReader::MakeInformationVector(
  vtkInformationVector* outputVector, const vtkStdString& procName)
{
  this->FileNameOld->assign(this->FileName);

  // Clear prior case information
  this->Readers->RemoveAllItems();

  // Recreate case information
  vtkStdString casePath, controlDictPath;
  this->CreateCasePath(casePath, controlDictPath);

  if (!procName.empty())
  {
    casePath += procName + "/";
  }

  // Check for mesh directory and subregions.
  // Should check contents of constant/regionProperties, but that file may be missing so instead
  // check the existence of files in constant/polyMesh and constant/{region}/polyMesh.
  // A multi-region case will often not have the default region

  bool hasDefaultRegion = false;
  std::vector<std::string> regionNames;

  {
    const std::string constantPath(casePath + "constant/");
    vtkNew<vtkDirectory> dir;
    if (!dir->Open(constantPath.c_str()))
    {
      vtkErrorMacro(<< "Cannot open directory: " << constantPath);
      return 0;
    }

    hasDefaultRegion = vtkFoamFile::IsFile(constantPath + "polyMesh/faces", true);

    for (vtkIdType entryi = 0; entryi < dir->GetNumberOfFiles(); ++entryi)
    {
      std::string subDir(dir->GetFile(entryi));
      if (subDir != "." && subDir != ".." && dir->FileIsDirectory(subDir.c_str()) &&
        vtkFoamFile::IsFile((constantPath + subDir + "/polyMesh/faces"), true))
      {
        regionNames.push_back(std::move(subDir));
      }
    }

    if (!hasDefaultRegion && regionNames.empty())
    {
      vtkErrorMacro(<< this->FileName << " contains no meshes.");
      return 0;
    }

    // Consistently ordered
    std::sort(regionNames.begin(), regionNames.end());
  }

  auto masterReader = vtkSmartPointer<vtkOpenFOAMReaderPrivate>::New();
  if (!masterReader->MakeInformationVector(
        casePath, controlDictPath, procName, this->Parent, hasDefaultRegion))
  {
    return 0;
  }

  if (masterReader->GetTimeValues()->GetNumberOfTuples() == 0)
  {
    vtkErrorMacro(<< this->FileName << " contains no timestep data.");
    return 0;
  }

  if (hasDefaultRegion)
  {
    this->Readers->AddItem(masterReader);
  }

  // Add subregions
  for (const auto& regionName : regionNames)
  {
    auto subReader = vtkSmartPointer<vtkOpenFOAMReaderPrivate>::New();
    subReader->SetupInformation(casePath, regionName, procName, masterReader);
    this->Readers->AddItem(subReader);
  }

  this->Parent->NumberOfReaders += this->Readers->GetNumberOfItems();

  if (outputVector != nullptr)
  {
    this->SetTimeInformation(outputVector, masterReader->GetTimeValues());
  }
  if (this->Parent == this)
  {
    this->CreateCharArrayFromString(this->CasePath, "CasePath", casePath);
  }

  return 1;
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReader::CreateCasePath(vtkStdString& casePath, vtkStdString& controlDictPath)
{
#if defined(_WIN32)
  const std::string pathFindSeparator = "/\\", pathSeparator = "\\";
#else
  const std::string pathFindSeparator = "/", pathSeparator = "/";
#endif
  controlDictPath = this->FileName;

  // determine the case directory and path to controlDict
  auto pos = controlDictPath.find_last_of(pathFindSeparator);
  if (pos == std::string::npos)
  {
    // if there's no prepending path, prefix with the current directory
    controlDictPath = "." + pathSeparator + controlDictPath;
    pos = 1;
  }
  if (controlDictPath.substr(pos + 1, 11) == "controlDict")
  {
    // remove trailing "/controlDict*"
    casePath = controlDictPath.substr(0, pos - 1);
    if (casePath == ".")
    {
      casePath = ".." + pathSeparator;
    }
    else
    {
      pos = casePath.find_last_of(pathFindSeparator);
      if (pos == std::string::npos)
      {
        casePath = "." + pathSeparator;
      }
      else
      {
        // remove trailing "system" (or any other directory name)
        casePath.erase(pos + 1); // preserve the last "/"
      }
    }
  }
  else
  {
    // if the file is named other than controlDict*, use the directory
    // containing the file as case directory
    casePath = controlDictPath.substr(0, pos + 1);
    controlDictPath = casePath + "system" + pathSeparator + "controlDict";
  }
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReader::AddSelectionNames(
  vtkDataArraySelection* selections, vtkStringArray* objects)
{
  objects->Squeeze();
  vtkSortDataArray::Sort(objects);
  for (int nameI = 0; nameI < objects->GetNumberOfValues(); nameI++)
  {
    selections->AddArray(objects->GetValue(nameI).c_str());
  }
  objects->Delete();
}

//------------------------------------------------------------------------------
bool vtkOpenFOAMReader::SetTimeValue(const double timeValue)
{
  bool modified = false;
  vtkOpenFOAMReaderPrivate* reader;
  this->Readers->InitTraversal();
  while ((reader = vtkOpenFOAMReaderPrivate::SafeDownCast(this->Readers->GetNextItemAsObject())) !=
    nullptr)
  {
    vtkMTimeType mTime = reader->GetMTime();
    reader->SetTimeValue(timeValue);
    if (reader->GetMTime() != mTime)
    {
      modified = true;
    }
  }
  return modified;
}

//------------------------------------------------------------------------------
vtkDoubleArray* vtkOpenFOAMReader::GetTimeValues()
{
  if (this->Readers->GetNumberOfItems() <= 0)
  {
    return nullptr;
  }
  vtkOpenFOAMReaderPrivate* reader =
    vtkOpenFOAMReaderPrivate::SafeDownCast(this->Readers->GetItemAsObject(0));
  return reader != nullptr ? reader->GetTimeValues() : nullptr;
}

//------------------------------------------------------------------------------
int vtkOpenFOAMReader::MakeMetaDataAtTimeStep(const bool listNextTimeStep)
{
  vtkStringArray* cellSelectionNames = vtkStringArray::New();
  vtkStringArray* pointSelectionNames = vtkStringArray::New();
  vtkStringArray* lagrangianSelectionNames = vtkStringArray::New();
  int ret = 1;
  vtkOpenFOAMReaderPrivate* reader;
  this->Readers->InitTraversal();
  while ((reader = vtkOpenFOAMReaderPrivate::SafeDownCast(this->Readers->GetNextItemAsObject())) !=
    nullptr)
  {
    ret *= reader->MakeMetaDataAtTimeStep(
      cellSelectionNames, pointSelectionNames, lagrangianSelectionNames, listNextTimeStep);
  }
  this->AddSelectionNames(this->Parent->CellDataArraySelection, cellSelectionNames);
  this->AddSelectionNames(this->Parent->PointDataArraySelection, pointSelectionNames);
  this->AddSelectionNames(this->Parent->LagrangianDataArraySelection, lagrangianSelectionNames);

  return ret;
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReader::CreateCharArrayFromString(
  vtkCharArray* array, const char* name, vtkStdString& string)
{
  array->Initialize();
  array->SetName(name);
  const size_t len = string.length();
  char* ptr = array->WritePointer(0, static_cast<vtkIdType>(len + 1));
  memcpy(ptr, string.c_str(), len);
  ptr[len] = '\0';
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReader::UpdateStatus()
{
  // update selection MTimes
  this->PatchSelectionMTimeOld = this->PatchDataArraySelection->GetMTime();
  this->CellSelectionMTimeOld = this->CellDataArraySelection->GetMTime();
  this->PointSelectionMTimeOld = this->PointDataArraySelection->GetMTime();
  this->LagrangianSelectionMTimeOld = this->LagrangianDataArraySelection->GetMTime();
  this->CreateCellToPointOld = this->CreateCellToPoint;
  this->DecomposePolyhedraOld = this->DecomposePolyhedra;
  this->PositionsIsIn13FormatOld = this->PositionsIsIn13Format;
  this->ReadZonesOld = this->ReadZones;
  this->SkipZeroTimeOld = this->SkipZeroTime;
  this->ListTimeStepsByControlDictOld = this->ListTimeStepsByControlDict;
  this->AddDimensionsToArrayNamesOld = this->AddDimensionsToArrayNames;
  this->Use64BitLabelsOld = this->Use64BitLabels;
  this->Use64BitFloatsOld = this->Use64BitFloats;
}

//------------------------------------------------------------------------------
void vtkOpenFOAMReader::UpdateProgress(double amount)
{
  this->vtkAlgorithm::UpdateProgress(
    (static_cast<double>(this->Parent->CurrentReaderIndex) + amount) /
    static_cast<double>(this->Parent->NumberOfReaders));
}
