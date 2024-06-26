// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/*
Test NIFTI support in VTK by reading a file, writing it, and
then re-reading it to ensure that the contents are identical.
*/

#include "vtkNew.h"
#include "vtkRegressionTestImage.h"
#include "vtkTestUtilities.h"

#include "vtkCamera.h"
#include "vtkImageData.h"
#include "vtkImageMathematics.h"
#include "vtkImageProperty.h"
#include "vtkImageSlice.h"
#include "vtkImageSliceMapper.h"
#include "vtkMatrix4x4.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkRenderer.h"
#include "vtkStringArray.h"

#include "vtkNIFTIImageHeader.h"
#include "vtkNIFTIImageReader.h"
#include "vtkNIFTIImageWriter.h"

#include <string>

static const char* testfiles[7][2] = { { "Data/minimal.nii.gz", "out_minimal.nii.gz" },
  { "Data/minimal.img.gz", "out_minimal.hdr" }, { "Data/planar_rgb.nii.gz", "out_planar_rgb.nii" },
  { "Data/nifti_rgb.nii.gz", "out_nifti_rgb.nii" },
  { "Data/filtered_func_data.nii.gz", "out_filtered_func_data.nii.gz" },
  { "Data/minimal.hdr.gz", "out_minimal_2.nii" }, { "Data/minimal.img.gz", "" } };

static const char* dispfile = "Data/avg152T1_RL_nifti.nii.gz";

static void TestDisplay(vtkRenderWindow* renwin, const char* infile)
{
  vtkNew<vtkNIFTIImageReader> reader;
  reader->SetFileName(infile);
  reader->Update();

  int size[3];
  double center[3], spacing[3];
  reader->GetOutput()->GetDimensions(size);
  reader->GetOutput()->GetCenter(center);
  reader->GetOutput()->GetSpacing(spacing);
  double center1[3] = { center[0], center[1], center[2] };
  double center2[3] = { center[0], center[1], center[2] };
  if (size[2] % 2 == 1)
  {
    center1[2] += 0.5 * spacing[2];
  }
  if (size[0] % 2 == 1)
  {
    center2[0] += 0.5 * spacing[0];
  }
  double vrange[2];
  reader->GetOutput()->GetScalarRange(vrange);

  vtkNew<vtkImageSliceMapper> map1;
  map1->BorderOn();
  map1->SliceAtFocalPointOn();
  map1->SliceFacesCameraOn();
  map1->SetInputConnection(reader->GetOutputPort());
  vtkNew<vtkImageSliceMapper> map2;
  map2->BorderOn();
  map2->SliceAtFocalPointOn();
  map2->SliceFacesCameraOn();
  map2->SetInputConnection(reader->GetOutputPort());

  vtkNew<vtkImageSlice> slice1;
  slice1->SetMapper(map1);
  slice1->GetProperty()->SetColorWindow(vrange[1] - vrange[0]);
  slice1->GetProperty()->SetColorLevel(0.5 * (vrange[0] + vrange[1]));

  vtkNew<vtkImageSlice> slice2;
  slice2->SetMapper(map2);
  slice2->GetProperty()->SetColorWindow(vrange[1] - vrange[0]);
  slice2->GetProperty()->SetColorLevel(0.5 * (vrange[0] + vrange[1]));

  double ratio = size[0] * 1.0 / (size[0] + size[2]);

  vtkNew<vtkRenderer> ren1;
  ren1->SetViewport(0, 0, ratio, 1.0);

  vtkNew<vtkRenderer> ren2;
  ren2->SetViewport(ratio, 0.0, 1.0, 1.0);
  ren1->AddViewProp(slice1);
  ren2->AddViewProp(slice2);

  vtkCamera* cam1 = ren1->GetActiveCamera();
  cam1->ParallelProjectionOn();
  cam1->SetParallelScale(0.5 * spacing[1] * size[1]);
  cam1->SetFocalPoint(center1[0], center1[1], center1[2]);
  cam1->SetPosition(center1[0], center1[1], center1[2] - 100.0);

  vtkCamera* cam2 = ren2->GetActiveCamera();
  cam2->ParallelProjectionOn();
  cam2->SetParallelScale(0.5 * spacing[1] * size[1]);
  cam2->SetFocalPoint(center2[0], center2[1], center2[2]);
  cam2->SetPosition(center2[0] + 100.0, center2[1], center2[2]);

  renwin->SetSize((size[0] + size[2]) / 2 * 2, size[1] / 2 * 2); // keep size even
  renwin->AddRenderer(ren1);
  renwin->AddRenderer(ren2);
}

static double TestReadWriteRead(
  const char* infile, const char* infile2, const char* outfile, bool planarRGB)
{
  // read a NIFTI file
  vtkNew<vtkNIFTIImageReader> reader;
  if (infile2 == nullptr)
  {
    reader->SetFileName(infile);
  }
  else
  {
    vtkNew<vtkStringArray> filenames;
    filenames->InsertNextValue(infile);
    filenames->InsertNextValue(infile2);
    reader->SetFileNames(filenames);
  }
  reader->TimeAsVectorOn();
  if (planarRGB)
  {
    reader->PlanarRGBOn();
  }
  reader->Update();

  vtkNew<vtkNIFTIImageWriter> writer;
  writer->SetInputConnection(reader->GetOutputPort());
  writer->SetFileName(outfile);
  // copy most information directly from the header
  vtkNIFTIImageHeader* header = writer->GetNIFTIHeader();
  header->DeepCopy(reader->GetNIFTIHeader());
  header->SetDescrip("VTK Test Data");
  // this information will override the reader's header
  writer->SetQFac(reader->GetQFac());
  writer->SetTimeDimension(reader->GetTimeDimension());
  writer->SetTimeSpacing(reader->GetTimeSpacing());
  writer->SetRescaleSlope(reader->GetRescaleSlope());
  writer->SetRescaleIntercept(reader->GetRescaleIntercept());
  writer->SetQFormMatrix(reader->GetQFormMatrix());
  if (reader->GetSFormMatrix())
  {
    writer->SetSFormMatrix(reader->GetSFormMatrix());
  }
  else
  {
    writer->SetSFormMatrix(reader->GetQFormMatrix());
  }
  if (planarRGB)
  {
    writer->PlanarRGBOn();
  }
  writer->Write();

  // to exercise PrintSelf
  vtkOStrStreamWrapper s;
  reader->Print(s);
  header->Print(s);
  writer->Print(s);

  vtkNew<vtkNIFTIImageReader> reader2;
  reader2->SetFileName(outfile);
  reader2->TimeAsVectorOn();
  if (planarRGB)
  {
    reader2->PlanarRGBOn();
  }
  reader2->Update();

  // the images should be identical
  vtkNew<vtkImageMathematics> diff;
  diff->SetOperationToSubtract();
  diff->SetInputConnection(0, reader->GetOutputPort());
  diff->SetInputConnection(1, reader2->GetOutputPort());
  diff->Update();
  double diffrange[2];
  diff->GetOutput()->GetScalarRange(diffrange);
  double differr = diffrange[0] * diffrange[0] + diffrange[1] * diffrange[1];

  // the matrices should be within tolerance
  if (writer->GetQFormMatrix())
  {
    vtkNew<vtkMatrix4x4> m;
    m->DeepCopy(writer->GetQFormMatrix());
    m->Invert();
    vtkMatrix4x4::Multiply4x4(m, reader2->GetQFormMatrix(), m);
    double sqdiff = 0.0;
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        double d = (m->GetElement(i, j) - (i == j));
        sqdiff += d * d;
      }
    }
    if (sqdiff > 1e-10)
    {
      cerr << "Mismatched read/write QFormMatrix:\n";
      m->Print(cerr);
      differr = 1.0;
    }
  }

  if (writer->GetSFormMatrix())
  {
    vtkNew<vtkMatrix4x4> m;
    m->DeepCopy(writer->GetSFormMatrix());
    m->Invert();
    vtkMatrix4x4::Multiply4x4(m, reader2->GetSFormMatrix(), m);
    double sqdiff = 0.0;
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        double d = (m->GetElement(i, j) - (i == j));
        sqdiff += d * d;
      }
    }
    if (sqdiff > 1e-10)
    {
      cerr << "Mismatched read/write SFormMatrix:\n";
      m->Print(cerr);
      differr = 1.0;
    }
  }

  return differr;
}

static int TestNIFTIHeader()
{
  vtkNew<vtkNIFTIImageHeader> header1;
  vtkNew<vtkNIFTIImageHeader> header2;

  header1->SetIntentCode(vtkNIFTIImageHeader::IntentZScore);
  header1->SetIntentName("ZScore");
  header1->SetIntentP1(1.0);
  header1->SetIntentP2(2.0);
  header1->SetIntentP3(3.0);
  header1->SetSclSlope(2.0);
  header1->SetSclInter(1024.0);
  header1->SetCalMin(-1024.0);
  header1->SetCalMax(3072.0);
  header1->SetSliceDuration(1.0);
  header1->SetSliceStart(2);
  header1->SetSliceEnd(14);
  header1->SetXYZTUnits(vtkNIFTIImageHeader::UnitsMM | vtkNIFTIImageHeader::UnitsSec);
  header1->SetDimInfo(0);
  header1->SetDescrip("Test header");
  header1->SetAuxFile("none");
  header1->SetQFormCode(vtkNIFTIImageHeader::XFormScannerAnat);
  header1->SetQuaternB(0.0);
  header1->SetQuaternC(1.0);
  header1->SetQuaternD(0.0);
  header1->SetQOffsetX(10.0);
  header1->SetQOffsetY(30.0);
  header1->SetQOffsetZ(20.0);
  header1->SetSFormCode(vtkNIFTIImageHeader::XFormAlignedAnat);
  double matrix[16];
  vtkMatrix4x4::Identity(matrix);
  header1->SetSRowX(matrix);
  header1->SetSRowY(matrix + 4);
  header1->SetSRowZ(matrix + 8);

  header2->DeepCopy(header1);
  bool success = true;
  success &= (header2->GetIntentCode() == vtkNIFTIImageHeader::IntentZScore);
  success &= (strcmp(header2->GetIntentName(), "ZScore") == 0);
  success &= (header2->GetIntentP1() == 1.0);
  success &= (header2->GetIntentP2() == 2.0);
  success &= (header2->GetIntentP3() == 3.0);
  success &= (header2->GetSclSlope() == 2.0);
  success &= (header2->GetSclInter() == 1024.0);
  success &= (header2->GetCalMin() == -1024.0);
  success &= (header2->GetCalMax() == 3072.0);
  success &= (header2->GetSliceDuration() == 1.0);
  success &= (header2->GetSliceStart() == 2);
  success &= (header2->GetSliceEnd() == 14);
  success &=
    (header2->GetXYZTUnits() == (vtkNIFTIImageHeader::UnitsMM | vtkNIFTIImageHeader::UnitsSec));
  success &= (header2->GetDimInfo() == 0);
  success &= (strcmp(header2->GetDescrip(), "Test header") == 0);
  success &= (strcmp(header2->GetAuxFile(), "none") == 0);
  success &= (header2->GetQFormCode() == vtkNIFTIImageHeader::XFormScannerAnat);
  success &= (header2->GetQuaternB() == 0.0);
  success &= (header2->GetQuaternC() == 1.0);
  success &= (header2->GetQuaternD() == 0.0);
  success &= (header2->GetQOffsetX() == 10.0);
  success &= (header2->GetQOffsetY() == 30.0);
  success &= (header2->GetQOffsetZ() == 20.0);
  header2->GetSRowX(matrix);
  header2->GetSRowY(matrix + 4);
  header2->GetSRowZ(matrix + 8);
  success &= (matrix[0] == 1.0 && matrix[5] == 1.0 && matrix[10] == 1.0);

  return success;
}

int TestNIFTIReaderWriter(int argc, char* argv[])
{
  // perform the header test
  if (!TestNIFTIHeader())
  {
    cerr << "Failed TestNIFTIHeader\n";
    return 1;
  }

  // perform the read/write test
  for (int i = 0; i < 5; i++)
  {
    char* infile2 = nullptr;
    char* infile = vtkTestUtilities::ExpandDataFileName(argc, argv, testfiles[i][0]);
    bool planarRGB = (i == 2);
    if (i == 5)
    {
      infile2 = vtkTestUtilities::ExpandDataFileName(argc, argv, testfiles[6][0]);
    }
    if (!infile)
    {
      cerr << "Could not locate input file " << testfiles[i][0] << "\n";
      return 1;
    }

    char* tempDir =
      vtkTestUtilities::GetArgOrEnvOrDefault("-T", argc, argv, "VTK_TEMP_DIR", "Testing/Temporary");
    if (!tempDir)
    {
      cerr << "Could not determine temporary directory.\n";
      return 1;
    }

    std::string outpath = tempDir;
    outpath += "/";
    outpath += testfiles[i][1];
    delete[] tempDir;

    vtkNew<vtkNIFTIImageReader> testReader;
    testReader->GetFileExtensions();
    testReader->GetDescriptiveName();
    if (!testReader->CanReadFile(infile))
    {
      cerr << "CanReadFile() failed for " << infile << "\n";
      return 1;
    }

    double err = TestReadWriteRead(infile, infile2, outpath.c_str(), planarRGB);
    if (err != 0.0)
    {
      cerr << "Input " << infile << " differs from output " << outpath << "\n";
      return 1;
    }
    delete[] infile;
    delete[] infile2;
  }

  // perform the display test
  char* infile = vtkTestUtilities::ExpandDataFileName(argc, argv, dispfile);
  if (!infile)
  {
    cerr << "Could not locate input file " << dispfile << "\n";
    return 1;
  }

  vtkNew<vtkRenderWindow> renwin;
  vtkNew<vtkRenderWindowInteractor> iren;
  iren->SetRenderWindow(renwin);

  TestDisplay(renwin, infile);
  delete[] infile;

  int retVal = vtkRegressionTestImage(renwin);
  if (retVal == vtkRegressionTester::DO_INTERACTOR)
  {
    renwin->Render();
    iren->Start();
    retVal = vtkRegressionTester::PASSED;
  }

  return (retVal != vtkRegressionTester::PASSED);
}
