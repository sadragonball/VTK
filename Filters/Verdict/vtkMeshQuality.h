/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkMeshQuality.h

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

  Copyright 2003-2008 Sandia Corporation.
  Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
  license for use of this work by or on behalf of the
  U.S. Government. Redistribution and use in source and binary forms, with
  or without modification, are permitted provided that this Notice and any
  statement of authorship are reproduced on all copies.

  Contact: dcthomp@sandia.gov,pppebay@sandia.gov

=========================================================================*/
/**
 * @class   vtkMeshQuality
 * @brief   Calculate functions of quality of the elements of a mesh
 *
 * vtkMeshQuality computes one or more functions of (geometric)
 * quality for each 2-D and 3-D cell (triangle, quadrilateral, tetrahedron,
 * or hexahedron) of a mesh. These functions of quality are then averaged
 * over the entire mesh. The minimum, average, maximum, and unbiased variance
 * of quality for each type of cell is stored in the output mesh's FieldData.
 * The FieldData arrays are named "Mesh Triangle Quality,"
 * "Mesh Quadrilateral Quality," "Mesh Tetrahedron Quality,"
 * and "Mesh Hexahedron Quality." Each array has a single tuple
 * with 5 components. The first 4 components are the quality statistics
 * mentioned above; the final value is the number of cells of the given type.
 * This final component makes aggregation of statistics for distributed
 * mesh data possible.
 *
 * By default, the per-cell quality is added to the mesh's cell data, in
 * an array named "Quality." Cell types not supported by
 * this filter will have an entry of 0. Use SaveCellQualityOff() to
 * store only the final statistics.
 *
 * This version of the filter written by Philippe Pebay and David Thompson
 * overtakes an older version written by Leila Baghdadi, Hanif Ladak, and
 * David Steinman at the Imaging Research Labs, Robarts Research Institute.
 * That version only supported tetrahedral radius ratio. See the
 * CompatibilityModeOn() member for information on how to make this filter
 * behave like the previous implementation.
 * For more information on the triangle quality functions of this class, cf.
 * Pebay & Baker 2003, Analysis of triangle quality measures, Math Comp 72:244.
 * For more information on the quadrangle quality functions of this class, cf.
 * Pebay 2004, Planar Quadrangle Quality Measures, Eng Comp 20:2.
 *
 * @warning
 * While more general than before, this class does not address many
 * cell types, including wedges and pyramids in 3D and triangle strips
 * and fans in 2D (among others).
 * Most quadrilateral quality functions are intended for planar quadrilaterals
 * only.
 * The minimal angle is not, strictly speaking, a quality function, but it is
 * provided because of its usage by many authors.
 */

#ifndef vtkMeshQuality_h
#define vtkMeshQuality_h

#include "vtkDataSetAlgorithm.h"
#include "vtkDeprecation.h"          // For deprecation
#include "vtkFiltersVerdictModule.h" // For export macro

class vtkCell;
class vtkDataArray;
class vtkDoubleArray;

class VTKFILTERSVERDICT_EXPORT vtkMeshQuality : public vtkDataSetAlgorithm
{
public:
  void PrintSelf(ostream& os, vtkIndent indent) override;
  vtkTypeMacro(vtkMeshQuality, vtkDataSetAlgorithm);
  static vtkMeshQuality* New();

  ///@{
  /**
   * This variable controls whether or not cell quality is stored as
   * cell data in the resulting mesh or discarded (leaving only the
   * aggregate quality average of the entire mesh, recorded in the
   * FieldData).
   */
  vtkSetMacro(SaveCellQuality, vtkTypeBool);
  vtkGetMacro(SaveCellQuality, vtkTypeBool);
  vtkBooleanMacro(SaveCellQuality, vtkTypeBool);
  ///@}

  ///@{
  /**
   * If set to true, then this filter will output 2 quality arrays instead of one.
   * The second array is names "Quality (Linear Approx)" and features measure for all non-linear
   * cells in addition to the linear ones, but treated like if they were linear.
   *
   * @note In the array "Quality", any non-linear cell quality is set to NaN.
   */
  vtkSetMacro(LinearApproximation, bool);
  vtkGetMacro(LinearApproximation, bool);
  vtkBooleanMacro(LinearApproximation, bool);
  ///@}

  /**
   * Enum which lists the Quality Measures Types
   */
  enum QualityMeasureTypes
  {
    AREA = 28,
    ASPECT_FROBENIUS = 3,
    ASPECT_GAMMA = 27,
    ASPECT_RATIO = 1,
    COLLAPSE_RATIO = 7,
    CONDITION = 9,
    DIAGONAL = 21,
    DIMENSION = 22,
    DISTORTION = 15,
    EDGE_RATIO = 0,
    JACOBIAN = 25,
    MAX_ANGLE = 8,
    MAX_ASPECT_FROBENIUS = 5,
    MAX_EDGE_RATIO = 16,
    MED_ASPECT_FROBENIUS = 4,
    MIN_ANGLE = 6,
    ODDY = 23,
    RADIUS_RATIO = 2,
    RELATIVE_SIZE_SQUARED = 12,
    SCALED_JACOBIAN = 10,
    SHAPE = 13,
    SHAPE_AND_SIZE = 14,
    SHEAR = 11,
    SHEAR_AND_SIZE = 24,
    SKEW = 17,
    STRETCH = 20,
    TAPER = 18,
    VOLUME = 19,
    WARPAGE = 26,
    TOTAL_QUALITY_MEASURE_TYPES = 29,
    NONE = TOTAL_QUALITY_MEASURE_TYPES
  };

  /**
   * Array which lists the Quality Measures Names.
   */
  static const char* QualityMeasureNames[];

  ///@{
  /**
   * Set/Get the particular estimator used to function the quality of triangles.
   * The default is RADIUS_RATIO and valid values also include
   * ASPECT_RATIO, ASPECT_FROBENIUS, and EDGE_RATIO, MIN_ANGLE, MAX_ANGLE, CONDITION,
   * SCALED_JACOBIAN, RELATIVE_SIZE_SQUARED, SHAPE, SHAPE_AND_SIZE, and DISTORTION.
   */
  vtkSetMacro(TriangleQualityMeasure, int);
  vtkGetMacro(TriangleQualityMeasure, int);
  void SetTriangleQualityMeasureToArea() { this->SetTriangleQualityMeasure(AREA); }
  void SetTriangleQualityMeasureToEdgeRatio() { this->SetTriangleQualityMeasure(EDGE_RATIO); }
  void SetTriangleQualityMeasureToAspectRatio() { this->SetTriangleQualityMeasure(ASPECT_RATIO); }
  void SetTriangleQualityMeasureToRadiusRatio() { this->SetTriangleQualityMeasure(RADIUS_RATIO); }
  void SetTriangleQualityMeasureToAspectFrobenius()
  {
    this->SetTriangleQualityMeasure(ASPECT_FROBENIUS);
  }
  void SetTriangleQualityMeasureToMinAngle() { this->SetTriangleQualityMeasure(MIN_ANGLE); }
  void SetTriangleQualityMeasureToMaxAngle() { this->SetTriangleQualityMeasure(MAX_ANGLE); }
  void SetTriangleQualityMeasureToCondition() { this->SetTriangleQualityMeasure(CONDITION); }
  void SetTriangleQualityMeasureToScaledJacobian()
  {
    this->SetTriangleQualityMeasure(SCALED_JACOBIAN);
  }
  void SetTriangleQualityMeasureToRelativeSizeSquared()
  {
    this->SetTriangleQualityMeasure(RELATIVE_SIZE_SQUARED);
  }
  void SetTriangleQualityMeasureToShape() { this->SetTriangleQualityMeasure(SHAPE); }
  void SetTriangleQualityMeasureToShapeAndSize()
  {
    this->SetTriangleQualityMeasure(SHAPE_AND_SIZE);
  }
  void SetTriangleQualityMeasureToDistortion() { this->SetTriangleQualityMeasure(DISTORTION); }
  ///@}

  ///@{
  /**
   * Set/Get the particular estimator used to measure the quality of quadrilaterals.
   * The default is EDGE_RATIO and valid values also include
   * RADIUS_RATIO, ASPECT_RATIO, MAX_EDGE_RATIO SKEW, TAPER, WARPAGE, AREA,
   * STRETCH, MIN_ANGLE, MAX_ANGLE, ODDY, CONDITION, JACOBIAN, SCALED_JACOBIAN,
   * SHEAR, SHAPE, RELATIVE_SIZE_SQUARED, SHAPE_AND_SIZE, SHEAR_AND_SIZE, and DISTORTION.
   *
   * Scope: Except for EDGE_RATIO, these estimators are intended for planar
   * quadrilaterals only; use at your own risk if you really want to assess non-planar
   * quadrilateral quality with those.
   */
  vtkSetMacro(QuadQualityMeasure, int);
  vtkGetMacro(QuadQualityMeasure, int);
  void SetQuadQualityMeasureToEdgeRatio() { this->SetQuadQualityMeasure(EDGE_RATIO); }
  void SetQuadQualityMeasureToAspectRatio() { this->SetQuadQualityMeasure(ASPECT_RATIO); }
  void SetQuadQualityMeasureToRadiusRatio() { this->SetQuadQualityMeasure(RADIUS_RATIO); }
  void SetQuadQualityMeasureToMedAspectFrobenius()
  {
    this->SetQuadQualityMeasure(MED_ASPECT_FROBENIUS);
  }
  void SetQuadQualityMeasureToMaxAspectFrobenius()
  {
    this->SetQuadQualityMeasure(MAX_ASPECT_FROBENIUS);
  }
  void SetQuadQualityMeasureToMaxEdgeRatios() { this->SetQuadQualityMeasure(MAX_EDGE_RATIO); }
  void SetQuadQualityMeasureToSkew() { this->SetQuadQualityMeasure(SKEW); }
  void SetQuadQualityMeasureToTaper() { this->SetQuadQualityMeasure(TAPER); }
  void SetQuadQualityMeasureToWarpage() { this->SetQuadQualityMeasure(WARPAGE); }
  void SetQuadQualityMeasureToArea() { this->SetQuadQualityMeasure(AREA); }
  void SetQuadQualityMeasureToStretch() { this->SetQuadQualityMeasure(STRETCH); }
  void SetQuadQualityMeasureToMinAngle() { this->SetQuadQualityMeasure(MIN_ANGLE); }
  void SetQuadQualityMeasureToMaxAngle() { this->SetQuadQualityMeasure(MAX_ANGLE); }
  void SetQuadQualityMeasureToOddy() { this->SetQuadQualityMeasure(ODDY); }
  void SetQuadQualityMeasureToCondition() { this->SetQuadQualityMeasure(CONDITION); }
  void SetQuadQualityMeasureToJacobian() { this->SetQuadQualityMeasure(JACOBIAN); }
  void SetQuadQualityMeasureToScaledJacobian() { this->SetQuadQualityMeasure(SCALED_JACOBIAN); }
  void SetQuadQualityMeasureToShear() { this->SetQuadQualityMeasure(SHEAR); }
  void SetQuadQualityMeasureToShape() { this->SetQuadQualityMeasure(SHAPE); }
  void SetQuadQualityMeasureToRelativeSizeSquared()
  {
    this->SetQuadQualityMeasure(RELATIVE_SIZE_SQUARED);
  }
  void SetQuadQualityMeasureToShapeAndSize() { this->SetQuadQualityMeasure(SHAPE_AND_SIZE); }
  void SetQuadQualityMeasureToShearAndSize() { this->SetQuadQualityMeasure(SHEAR_AND_SIZE); }
  void SetQuadQualityMeasureToDistortion() { this->SetQuadQualityMeasure(DISTORTION); }
  ///@}

  ///@{
  /**
   * Set/Get the particular estimator used to measure the quality of tetrahedra.
   * The default is RADIUS_RATIO and valid values also include
   * ASPECT_RATIO, ASPECT_FROBENIUS, EDGE_RATIO, COLLAPSE_RATIO, ASPECT_GAMMA, VOLUME,
   * CONDITION, JACOBIAN, SCALED_JACOBIAN, SHAPE, RELATIVE_SIZE_SQUARED, SHAPE_AND_SIZE, and
   * DISTORTION.
   */
  vtkSetMacro(TetQualityMeasure, int);
  vtkGetMacro(TetQualityMeasure, int);
  void SetTetQualityMeasureToEdgeRatio() { this->SetTetQualityMeasure(EDGE_RATIO); }
  void SetTetQualityMeasureToAspectRatio() { this->SetTetQualityMeasure(ASPECT_RATIO); }
  void SetTetQualityMeasureToRadiusRatio() { this->SetTetQualityMeasure(RADIUS_RATIO); }
  void SetTetQualityMeasureToAspectFrobenius() { this->SetTetQualityMeasure(ASPECT_FROBENIUS); }
  void SetTetQualityMeasureToMinAngle() { this->SetTetQualityMeasure(MIN_ANGLE); }
  void SetTetQualityMeasureToCollapseRatio() { this->SetTetQualityMeasure(COLLAPSE_RATIO); }
  void SetTetQualityMeasureToAspectGamma() { this->SetTetQualityMeasure(ASPECT_GAMMA); }
  void SetTetQualityMeasureToVolume() { this->SetTetQualityMeasure(VOLUME); }
  void SetTetQualityMeasureToCondition() { this->SetTetQualityMeasure(CONDITION); }
  void SetTetQualityMeasureToJacobian() { this->SetTetQualityMeasure(JACOBIAN); }
  void SetTetQualityMeasureToScaledJacobian() { this->SetTetQualityMeasure(SCALED_JACOBIAN); }
  void SetTetQualityMeasureToShape() { this->SetTetQualityMeasure(SHAPE); }
  void SetTetQualityMeasureToRelativeSizeSquared()
  {
    this->SetTetQualityMeasure(RELATIVE_SIZE_SQUARED);
  }
  void SetTetQualityMeasureToShapeAndSize() { this->SetTetQualityMeasure(SHAPE_AND_SIZE); }
  void SetTetQualityMeasureToDistortion() { this->SetTetQualityMeasure(DISTORTION); }
  ///@}

  ///@{
  /**
   * Set/Get the particular estimator used to measure the quality of hexahedra.
   * The default is MAX_ASPECT_FROBENIUS and valid values also include
   * EDGE_RATIO, MAX_ASPECT_FROBENIUS, MAX_EDGE_RATIO, SKEW, TAPER, VOLUME,
   * STRETCH, DIAGONAL, DIMENSION, ODDY, CONDITION, JACOBIAN,
   * SCALED_JACOBIAN, SHEAR, SHAPE, RELATIVE_SIZE_SQUARED, SHAPE_AND_SIZE,
   * SHEAR_AND_SIZE, and DISTORTION.
   */
  vtkSetMacro(HexQualityMeasure, int);
  vtkGetMacro(HexQualityMeasure, int);
  void SetHexQualityMeasureToEdgeRatio() { this->SetHexQualityMeasure(EDGE_RATIO); }
  void SetHexQualityMeasureToMedAspectFrobenius()
  {
    this->SetHexQualityMeasure(MED_ASPECT_FROBENIUS);
  }
  void SetHexQualityMeasureToMaxAspectFrobenius()
  {
    this->SetHexQualityMeasure(MAX_ASPECT_FROBENIUS);
  }
  void SetHexQualityMeasureToMaxEdgeRatios() { this->SetHexQualityMeasure(MAX_EDGE_RATIO); }
  void SetHexQualityMeasureToSkew() { this->SetHexQualityMeasure(SKEW); }
  void SetHexQualityMeasureToTaper() { this->SetHexQualityMeasure(TAPER); }
  void SetHexQualityMeasureToVolume() { this->SetHexQualityMeasure(VOLUME); }
  void SetHexQualityMeasureToStretch() { this->SetHexQualityMeasure(STRETCH); }
  void SetHexQualityMeasureToDiagonal() { this->SetHexQualityMeasure(DIAGONAL); }
  void SetHexQualityMeasureToDimension() { this->SetHexQualityMeasure(DIMENSION); }
  void SetHexQualityMeasureToOddy() { this->SetHexQualityMeasure(ODDY); }
  void SetHexQualityMeasureToCondition() { this->SetHexQualityMeasure(CONDITION); }
  void SetHexQualityMeasureToJacobian() { this->SetHexQualityMeasure(JACOBIAN); }
  void SetHexQualityMeasureToScaledJacobian() { this->SetHexQualityMeasure(SCALED_JACOBIAN); }
  void SetHexQualityMeasureToShear() { this->SetHexQualityMeasure(SHEAR); }
  void SetHexQualityMeasureToShape() { this->SetHexQualityMeasure(SHAPE); }
  void SetHexQualityMeasureToRelativeSizeSquared()
  {
    this->SetHexQualityMeasure(RELATIVE_SIZE_SQUARED);
  }
  void SetHexQualityMeasureToShapeAndSize() { this->SetHexQualityMeasure(SHAPE_AND_SIZE); }
  void SetHexQualityMeasureToShearAndSize() { this->SetHexQualityMeasure(SHEAR_AND_SIZE); }
  void SetHexQualityMeasureToDistortion() { this->SetHexQualityMeasure(DISTORTION); }
  ///@}

  /**
   * This is a static function used to calculate the area of a triangle.
   */
  static double TriangleArea(vtkCell* cell);

  /**
   * This is a static function used to calculate the edge ratio of a triangle.
   * The edge ratio of a triangle \f$t\f$ is:
   * \f$\frac{|t|_\infty}{|t|_0}\f$,
   * where \f$|t|_\infty\f$ and \f$|t|_0\f$ respectively denote the greatest and
   * the smallest edge lengths of \f$t\f$.
   */
  static double TriangleEdgeRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the aspect ratio of a triangle.
   * The aspect ratio of a triangle \f$t\f$ is:
   * \f$\frac{|t|_\infty}{2\sqrt{3}r}\f$,
   * where \f$|t|_\infty\f$ and \f$r\f$ respectively denote the greatest edge
   * length and the inradius of \f$t\f$.
   */
  static double TriangleAspectRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the radius ratio of a triangle.
   * The radius ratio of a triangle \f$t\f$ is:
   * \f$\frac{R}{2r}\f$,
   * where \f$R\f$ and \f$r\f$ respectively denote the circumradius and
   * the inradius of \f$t\f$.
   */
  static double TriangleRadiusRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the Frobenius condition number
   * of the transformation matrix from an equilateral triangle to a triangle.
   * The Frobenius aspect of a triangle \f$t\f$, when the reference element is
   * equilateral, is:
   * \f$\frac{|t|^2_2}{2\sqrt{3}{\cal A}}\f$,
   * where \f$|t|^2_2\f$ and \f$\cal A\f$ respectively denote the sum of the
   * squared edge lengths and the area of \f$t\f$.
   */
  static double TriangleAspectFrobenius(vtkCell* cell);

  /**
   * This is a static function used to calculate the minimal (nonoriented) angle
   * of a triangle, expressed in degrees.
   */
  static double TriangleMinAngle(vtkCell* cell);

  /**
   * This is a static function used to calculate the maximal (nonoriented) angle
   * of a triangle, expressed in degrees.
   */
  static double TriangleMaxAngle(vtkCell* cell);

  /**
   * This is a static function used to calculate the condition number
   * of a triangle.
   */
  static double TriangleCondition(vtkCell* cell);

  /**
   * This is a static function used to calculate the scaled Jacobian of a triangle.
   */
  static double TriangleScaledJacobian(vtkCell* cell);

  /**
   * This is a static function used to calculate the square of the relative size of a triangle.
   *
   * Note: TriangleRelativeSizeSquared will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average triangle size.
   */
  static double TriangleRelativeSizeSquared(vtkCell* cell);

  /**
   * This is a static function used to calculate the shape of a triangle.
   */
  static double TriangleShape(vtkCell* cell);

  /**
   * This is a static function used to calculate the product of shape and relative size of a
   * triangle.
   *
   * Note: TriangleShapeAndSize will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average triangle size.
   */
  static double TriangleShapeAndSize(vtkCell* cell);

  /**
   * This is a static function used to calculate the distortion of a triangle.
   */
  static double TriangleDistortion(vtkCell* cell);

  /**
   * This is a static function used to calculate the edge ratio of a quadrilateral.
   * The edge ratio of a quadrilateral \f$q\f$ is:
   * \f$\frac{|q|_\infty}{|q|_0}\f$,
   * where \f$|q|_\infty\f$ and \f$|q|_0\f$ respectively denote the greatest and
   * the smallest edge lengths of \f$q\f$.
   */
  static double QuadEdgeRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the aspect ratio of a planar quadrilateral.
   * The aspect ratio of a planar quadrilateral \f$q\f$ is:
   * \f$\frac{|q|_1|q|_\infty}{4{\cal A}}\f$,
   * where \f$|q|_1\f$, \f$|q|_\infty\f$ and \f${\cal A}\f$ respectively denote the
   * perimeter, the greatest edge length and the area of \f$q\f$.
   */
  static double QuadAspectRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the radius ratio of a planar
   * quadrilateral. The name is only used by analogy with the triangle radius
   * ratio, because in general a quadrilateral does not have a circumcircle nor
   * an incircle.
   * The radius ratio of a planar quadrilateral \f$q\f$ is:
   * \f$\frac{|q|_2h_{\max}}{\min_i{\cal A}_i}\f$,
   * where \f$|q|_2\f$, \f$h_{\max}\f$ and \f$\min{\cal A}_i\f$ respectively denote
   * the sum of the squared edge lengths, the greatest amongst diagonal and edge
   * lengths and the smallest area of the 4 triangles extractable from \f$q\f$.
   */
  static double QuadRadiusRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the average Frobenius aspect of
   * the 4 corner triangles of a planar quadrilateral, when the reference
   * triangle elements are right isosceles at the quadrangle vertices.
   * The Frobenius aspect of a triangle \f$t\f$, when the reference element is
   * right isosceles at vertex \f$V\f$, is:
   * \f$\frac{f^2+g^2}{4{\cal A}}\f$,
   * where \f$f^2+g^2\f$ and \f$\cal A\f$ respectively denote the sum of the
   * squared lengths of the edges attached to \f$V\f$ and the area of \f$t\f$.
   */
  static double QuadMedAspectFrobenius(vtkCell* cell);

  /**
   * This is a static function used to calculate the maximal Frobenius aspect of
   * the 4 corner triangles of a planar quadrilateral, when the reference
   * triangle elements are right isosceles at the quadrangle vertices.
   * The Frobenius aspect of a triangle \f$t\f$, when the reference element is
   * right isosceles at vertex \f$V\f$, is:
   * \f$\frac{f^2+g^2}{4{\cal A}}\f$,
   * where \f$f^2+g^2\f$ and \f$\cal A\f$ respectively denote the sum of the
   * squared lengths of the edges attached to \f$V\f$ and the area of \f$t\f$.
   */
  static double QuadMaxAspectFrobenius(vtkCell* cell);

  /**
   * This is a static function used to calculate the minimal (nonoriented) angle
   * of a quadrilateral, expressed in degrees.
   */
  static double QuadMinAngle(vtkCell* cell);

  /**
   * This is a static function used to calculate the maximum edge length ratio of a quadrilateral
   * at quad center.
   */
  static double QuadMaxEdgeRatios(vtkCell* cell);

  /**
   * This is a static function used to calculate the skew of a quadrilateral.
   * The skew of a quadrilateral is the maximum |cos A|, where A is the angle
   * between edges at the quad center.
   */
  static double QuadSkew(vtkCell* cell);

  /**
   * This is a static function used to calculate the taper of a quadrilateral.
   * The taper of a quadrilateral is the ratio of lengths derived from opposite edges.
   */
  static double QuadTaper(vtkCell* cell);

  /**
   * This is a static function used to calculate the warpage of a quadrilateral.
   * The warpage of a quadrilateral is the cosine of Minimum Dihedral Angle formed by
   * Planes Intersecting in Diagonals.
   */
  static double QuadWarpage(vtkCell* cell);

  /**
   * This is a static function used to calculate the area of a quadrilateral.
   * The area of a quadrilateral is the Jacobian at quad center.
   */
  static double QuadArea(vtkCell* cell);

  /**
   * This is a static function used to calculate the stretch of a quadrilateral.
   * The stretch of a quadrilateral is Sqrt(2) * minimum edge length / maximum diagonal length.
   */
  static double QuadStretch(vtkCell* cell);

  /**
   * This is a static function used to calculate the maximum (nonoriented) angle
   * of a quadrilateral, expressed in degrees.
   */
  static double QuadMaxAngle(vtkCell* cell);

  /**
   * This is a static function used to calculate the oddy of a quadrilateral.
   * The oddy of a quadrilateral is the general distortion measure based on left
   * Cauchy-Green Tensor.
   */
  static double QuadOddy(vtkCell* cell);

  /**
   * This is a static function used to calculate the condition number of a quadrilateral.
   * The condition number of a quadrilateral is the (maximum) condition number of the
   * Jacobian matrix at the 4 corners.
   */
  static double QuadCondition(vtkCell* cell);

  /**
   * This is a static function used to calculate the Jacobian of a quadrilateral.
   * The Jacobian of a quadrilateral is the minimum point-wise volume of local map
   * at 4 corners & center of quad.
   */
  static double QuadJacobian(vtkCell* cell);

  /**
   * This is a static function used to calculate the scaled Jacobian of a quadrilateral.
   * The scaled Jacobian of a quadrilateral is the minimum Jacobian divided by the lengths
   * of the 2 edge vectors.
   */
  static double QuadScaledJacobian(vtkCell* cell);

  /**
   * This is a static function used to calculate the shear of a quadrilateral.
   * The shear of a quadrilateral is 2 / Condition number of Jacobian Skew matrix.
   */
  static double QuadShear(vtkCell* cell);

  /**
   * This is a static function used to calculate the shear of a quadrilateral.
   * The shear of a quadrilateral is 2 / Condition number of weighted Jacobian matrix.
   */
  static double QuadShape(vtkCell* cell);

  /**
   * This is a static function used to calculate the relative size squared of a quadrilateral.
   * The relative size squared of a quadrilateral is the Min(J, 1 / J), where J is the
   * determinant of weighted Jacobian matrix.
   *
   * Note: QuadRelativeSizeSquared will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average quad size.
   */
  static double QuadRelativeSizeSquared(vtkCell* cell);

  /**
   * This is a static function used to calculate the shape and size of a quadrilateral.
   * The shape and size of a quadrilateral is product of shape and average size.
   *
   * Note: QuadShapeAndSize will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average triangle size.
   */
  static double QuadShapeAndSize(vtkCell* cell);

  /**
   * This is a static function used to calculate the shear and size of a quadrilateral.
   * The shear and size of a quadrilateral is product of shear and average size.
   *
   * Note: QuadShearAndSize will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average triangle size.
   */
  static double QuadShearAndSize(vtkCell* cell);

  /**
   * This is a static function used to calculate the distortion of a quadrilateral.
   * The distortion of a quadrilateral is {min(|J|)/actual area} * parent area,
   * parent area = 4 for quad.
   */
  static double QuadDistortion(vtkCell* cell);

  /**
   * This is a static function used to calculate the edge ratio of a tetrahedron.
   * The edge ratio of a tetrahedron \f$K\f$ is:
   * \f$\frac{|K|_\infty}{|K|_0}\f$,
   * where \f$|K|_\infty\f$ and \f$|K|_0\f$ respectively denote the greatest and
   * the smallest edge lengths of \f$K\f$.
   */
  static double TetEdgeRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the aspect ratio of a tetrahedron.
   * The aspect ratio of a tetrahedron \f$K\f$ is:
   * \f$\frac{|K|_\infty}{2\sqrt{6}r}\f$,
   * where \f$|K|_\infty\f$ and \f$r\f$ respectively denote the greatest edge
   * length and the inradius of \f$K\f$.
   */
  static double TetAspectRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the radius ratio of a tetrahedron.
   * The radius ratio of a tetrahedron \f$K\f$ is:
   * \f$\frac{R}{3r}\f$,
   * where \f$R\f$ and \f$r\f$ respectively denote the circumradius and
   * the inradius of \f$K\f$.
   */
  static double TetRadiusRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the Frobenius condition number
   * of the transformation matrix from a regular tetrahedron to a tetrahedron.
   * The Frobenius aspect of a tetrahedron \f$K\f$, when the reference element is
   * regular, is:
   * \f$\frac{\frac{3}{2}(l_{11}+l_{22}+l_{33}) - (l_{12}+l_{13}+l_{23})}
   * {3(\sqrt{2}\det{T})^\frac{2}{3}}\f$,
   * where \f$T\f$ and \f$l_{ij}\f$ respectively denote the edge matrix of \f$K\f$
   * and the entries of \f$L=T^t\,T\f$.
   */
  static double TetAspectFrobenius(vtkCell* cell);

  /**
   * This is a static function used to calculate the minimal (nonoriented) dihedral
   * angle of a tetrahedron, expressed in degrees.
   */
  static double TetMinAngle(vtkCell* cell);

  /**
   * This is a static function used to calculate the collapse ratio of a tetrahedron.
   * The collapse ratio is a dimensionless number defined as the smallest ratio of the
   * height of a vertex above its opposing triangle to the longest edge of that opposing
   * triangle across all vertices of the tetrahedron.
   */
  static double TetCollapseRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the aspect gamma of a tetrahedron.
   * The aspect gamma of a tetrahedron is:
   * Srms**3 / (8.479670*V) where Srms = sqrt(Sum(Si**2)/6), Si = edge length.
   */
  static double TetAspectGamma(vtkCell* cell);

  /**
   * This is a static function used to calculate the volume of a tetrahedron.
   * The volume of a tetrahedron is (1/6) * Jacobian at corner node.
   */
  static double TetVolume(vtkCell* cell);

  /**
   * This is a static function used to calculate the condition number of a tetrahedron.
   * The condition number of a tetrahedron is Condition number of the Jacobian matrix at any corner.
   */
  static double TetCondition(vtkCell* cell);

  /**
   * This is a static function used to calculate the Jacobian of a tetrahedron.
   * The jacobian of a tetrahedron is the minimum point-wise volume at any corner.
   */
  static double TetJacobian(vtkCell* cell);

  /**
   * This is a static function used to calculate the scaled Jacobian of a tetrahedron.
   * The scaled jacobian of a tetrahedron is the minimum Jacobian divided
   * by the lengths of 3 edge vectors.
   */
  static double TetScaledJacobian(vtkCell* cell);

  /**
   * This is a static function used to calculate the shape of a tetrahedron.
   * The shape of a tetrahedron is 3 / Mean Ratio of weighted Jacobian matrix.
   */
  static double TetShape(vtkCell* cell);

  /**
   * This is a static function used to calculate the relative size squared of a tetrahedron.
   * The relative size squared of a tetrahedron is Min(J, 1 / J), where J is determinant
   * of weighted Jacobian matrix.
   *
   * Note: TetRelativeSizeSquared will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average tet size.
   */
  static double TetRelativeSizeSquared(vtkCell* cell);

  /**
   * This is a static function used to calculate the shape and size of a tetrahedron.
   * The shape and size of a tetrahedron is product of shape and average size.
   *
   * Note: TetShapeAndSize will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average tet size.
   */
  static double TetShapeAndSize(vtkCell* cell);

  /**
   * This is a static function used to calculate the distortion of a tetrahedron.
   * The distortion of a quadrilateral is {min(|J|)/actual volume} * parent volume,
   * parent volume = 1 / 6 for a tetrahedron.
   */
  static double TetDistortion(vtkCell* cell);

  /**
   * This is a static function used to calculate the edge ratio of a hexahedron.
   * The edge ratio of a hexahedron \f$H\f$ is:
   * \f$\frac{|H|_\infty}{|H|_0}\f$,
   * where \f$|H|_\infty\f$ and \f$|H|_0\f$ respectively denote the greatest and
   * the smallest edge lengths of \f$H\f$.
   */
  static double HexEdgeRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the average Frobenius aspect of
   * the 8 corner tetrahedra of a hexahedron, when the reference
   * tetrahedral elements are right isosceles at the hexahedron vertices.
   */
  static double HexMedAspectFrobenius(vtkCell* cell);

  /**
   * This is a static function used to calculate the maximal Frobenius aspect of
   * the 8 corner tetrahedra of a hexahedron, when the reference
   * tetrahedral elements are right isosceles at the hexahedron vertices.
   */
  static double HexMaxAspectFrobenius(vtkCell* cell);

  /**
   * This is a static function used to calculate the maximum edge ratio of a hexahedron
   * at its center.
   */
  static double HexMaxEdgeRatio(vtkCell* cell);

  /**
   * This is a static function used to calculate the skew of a hexahedron.
   * The skew of a hexahedron is the maximum |cos A|, where A is the angle
   * between edges at the hexahedron center.
   */
  static double HexSkew(vtkCell* cell);

  /**
   * This is a static function used to calculate the taper of a hexahedron.
   * The taper of a hexahedron is the ratio of lengths derived from opposite edges.
   */
  static double HexTaper(vtkCell* cell);

  /**
   * This is a static function used to calculate the volume of a hexahedron.
   * The volume of a hexahedron is the Jacobian at the hexahedron center.
   */
  static double HexVolume(vtkCell* cell);

  /**
   * This is a static function used to calculate the stretch of a hexahedron.
   * The stretch of a hexahedron is Sqrt(3) * minimum edge length / maximum diagonal length.
   */
  static double HexStretch(vtkCell* cell);

  /**
   * This is a static function used to calculate the diagonal of a hexahedron.
   * The diagonal of a hexahedron Minimum diagonal length / maximum diagonal length.
   */
  static double HexDiagonal(vtkCell* cell);

  /**
   * This is a static function used to calculate the dimension of a hexahedron.
   * The dimension of a hexahedron is the Pronto-specific characteristic length
   * for stable time step calculation, where characteristic length = Volume / 2 grad Volume.
   */
  static double HexDimension(vtkCell* cell);

  /**
   * This is a static function used to calculate the oddy of a hexahedron.
   * The oddy of a hexahedron is the general distortion measure based on left
   * Cauchy-Green Tensor.
   */
  static double HexOddy(vtkCell* cell);

  /**
   * This is a static function used to calculate the condition of a hexahedron.
   * The condition of a hexahedron is equivalent to HexMaxAspectFrobenius.
   */
  static double HexCondition(vtkCell* cell);

  /**
   * This is a static function used to calculate the Jacobian of a hexahedron.
   * The jacobian of a hexahedron is the minimum point-wise of local map at
   * 8 corners & center of the hexahedron.
   */
  static double HexJacobian(vtkCell* cell);

  /**
   * This is a static function used to calculate the scaled Jacobian of a hexahedron.
   * The scaled jacobian of a hexahedron is the minimum Jacobian divided
   * by the lengths of 3 edge vectors.
   */
  static double HexScaledJacobian(vtkCell* cell);

  /**
   * This is a static function used to calculate the shear of a hexahedron.
   * The shear of a hexahedron is 3 / Mean Ratio of Jacobian Skew matrix.
   */
  static double HexShear(vtkCell* cell);

  /**
   * This is a static function used to calculate the shape of a hexahedron.
   * The shape of a hexahedron is 3 / Mean Ratio of weighted Jacobian matrix.
   */
  static double HexShape(vtkCell* cell);

  /**
   * This is a static function used to calculate the relative size squared of a hexahedron.
   * The relative size squared of a hexahedron is Min(J, 1 / J), where J is determinant
   * of weighted Jacobian matrix.
   *
   * Note: HexRelativeSizeSquared will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average hex size.
   */
  static double HexRelativeSizeSquared(vtkCell* cell);

  /**
   * This is a static function used to calculate the shape and size of a hexahedron.
   * The shape and size of a hexahedron is product of shape and average size.
   *
   * Note: HexShapeAndSize will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average hex size.
   */
  static double HexShapeAndSize(vtkCell* cell);

  /**
   * This is a static function used to calculate the shear and size of a hexahedron.
   * The shear and size of a hexahedron is product of shear and average size.
   *
   * Note: HexShearAndSize will return 0.0 if the MeshQuality filter has NOT
   * been executed, because it relies on the average hex size.
   */
  static double HexShearAndSize(vtkCell* cell);

  /**
   * This is a static function used to calculate the distortion of a hexahedron.
   * The distortion of a hexahedron is {min(|J|)/actual volume} * parent volume,
   * parent volume = 8 for a hexahedron.
   */
  static double HexDistortion(vtkCell* cell);

  /**
   * These methods are deprecated. Use Get/SetSaveCellQuality() instead.
   *
   * Formerly, SetRatio could be used to disable computation
   * of the tetrahedral radius ratio so that volume alone could be computed.
   * Now, cell quality is always computed, but you may decide not
   * to store the result for each cell.
   * This allows average cell quality of a mesh to be
   * calculated without requiring per-cell storage.
   */
  virtual void SetRatio(vtkTypeBool r) { this->SetSaveCellQuality(r); }
  vtkTypeBool GetRatio() { return this->GetSaveCellQuality(); }
  vtkBooleanMacro(Ratio, vtkTypeBool);

  ///@{
  /**
   * These methods are deprecated. The functionality of computing cell
   * volume is being removed until it can be computed for any 3D cell.
   * (The previous implementation only worked for tetrahedra.)

   * For now, turning on the volume computation will put this
   * filter into "compatibility mode," where tetrahedral cell
   * volume is stored in first component of each output tuple and
   * the radius ratio is stored in the second component. You may
   * also use CompatibilityModeOn()/Off() to enter this mode.
   * In this mode, cells other than tetrahedra will have report
   * a volume of 0.0 (if volume computation is enabled).

   * By default, volume computation is disabled and compatibility
   * mode is off, since it does not make a lot of sense for
   * meshes with non-tetrahedral cells.
   */
  VTK_DEPRECATED_IN_9_2_0("Part of deprecating compatibility mode for this filter")
  virtual void SetVolume(vtkTypeBool cv)
  {
    if (!((cv != 0) ^ (this->Volume != 0)))
    {
      return;
    }
    this->Modified();
    this->Volume = cv;
    if (this->Volume)
    {
      this->CompatibilityMode = 1;
    }
  }
  VTK_DEPRECATED_IN_9_2_0("Part of deprecating compatibility mode for this filter")
  vtkTypeBool GetVolume() { return this->Volume; }
  VTK_DEPRECATED_IN_9_2_0("Part of deprecating compatibility mode for this filter")
  void VolumeOn()
  {
    if (!this->Volume)
    {
      this->Volume = 1;
      this->Modified();
    }
  }
  VTK_DEPRECATED_IN_9_2_0("Part of deprecating compatibility mode for this filter")
  void VolumeOff()
  {
    if (this->Volume)
    {
      this->Volume = 0;
      this->Modified();
    }
  }
  ///@}

  ///@{
  /**
   * CompatibilityMode governs whether, when both a quality function
   * and cell volume are to be stored as cell data, the two values
   * are stored in a single array. When compatibility mode is off
   * (the default), two separate arrays are used -- one labeled
   * "Quality" and the other labeled "Volume".
   * When compatibility mode is on, both values are stored in a
   * single array, with volume as the first component and quality
   * as the second component.

   * Enabling CompatibilityMode changes the default tetrahedral
   * quality function to RADIUS_RATIO and turns volume
   * computation on. (This matches the default behavior of the
   * initial implementation of vtkMeshQuality.) You may change
   * quality function and volume computation without leaving
   * compatibility mode.

   * Disabling compatibility mode does not affect the current
   * volume computation or tetrahedral quality function settings.

   * The final caveat to CompatibilityMode is that regardless of
   * its setting, the resulting array will be of type vtkDoubleArray
   * rather than the original vtkFloatArray.
   * This is a safety function to keep the authors from
   * diving off of the Combinatorial Coding Cliff into
   * Certain Insanity.
   */
  VTK_DEPRECATED_IN_9_2_0("Deprecating compatibility mode for this filter")
  virtual void SetCompatibilityMode(vtkTypeBool cm)
  {
    if (!((cm != 0) ^ (this->CompatibilityMode != 0)))
    {
      return;
    }
    this->CompatibilityMode = cm;
    this->Modified();
    if (this->CompatibilityMode)
    {
      this->Volume = 1;
      this->TetQualityMeasure = RADIUS_RATIO;
    }
  }
  VTK_DEPRECATED_IN_9_2_0("Deprecating compatibility mode for this filter")
  vtkGetMacro(CompatibilityMode, vtkTypeBool);
  VTK_DEPRECATED_IN_9_2_0("Deprecating compatibility mode for this filter")
  void CompatibilityModeOn()
  {
    if (!this->CompatibilityMode)
    {
      this->CompatibilityMode = 1;
      this->Modified();
    }
  }
  VTK_DEPRECATED_IN_9_2_0("Part of deprecating compatibility mode for this filter")
  void CompatibilityModeOff()
  {
    if (this->CompatibilityMode)
    {
      this->CompatibilityMode = 0;
      this->Modified();
    }
  }
  ///@}

protected:
  vtkMeshQuality();
  ~vtkMeshQuality() override = default;

  int RequestData(vtkInformation*, vtkInformationVector**, vtkInformationVector*) override;

  vtkTypeBool SaveCellQuality;
  int TriangleQualityMeasure;
  int QuadQualityMeasure;
  int TetQualityMeasure;
  int HexQualityMeasure;
  bool LinearApproximation;

  // VTK_DEPRECATED_IN_9_2_0 Those 2 attributes need to be removed, and instance in the code as
  // well.
  vtkTypeBool CompatibilityMode;
  vtkTypeBool Volume;

  // Variables used to store the average size (2D: area / 3D: volume)
  static double TriangleAverageSize;
  static double QuadAverageSize;
  static double TetAverageSize;
  static double HexAverageSize;

private:
  vtkMeshQuality(const vtkMeshQuality&) = delete;
  void operator=(const vtkMeshQuality&) = delete;
};

#endif // vtkMeshQuality_h
