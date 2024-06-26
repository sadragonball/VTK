// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @class   vtkCocoaTkUtilities
 * @brief   Internal Tk Routines for Cocoa
 *
 *
 * vtkCocoaTkUtilities provide access to the Tk internals for Cocoa
 * implementations of Tk.  These internals must be implemented in a .mm
 * file, since Cocoa is Objective-C, but the header file itself is
 * pure C++ so that it can be included by other VTK classes.
 *
 * @sa
 * vtkCocoaGLView
 *
 * @warning
 * This header must be in C++ only because it is included by .cxx files.
 * That means no Objective-C may be used. That's why some instance variables
 * are void* instead of what they really should be.
 */

#ifndef vtkCocoaTkUtilities_h
#define vtkCocoaTkUtilities_h

#include "vtkObject.h"

struct Tk_Window_;

class vtkCocoaTkUtilities : public vtkObject
{
public:
  static vtkCocoaTkUtilities* New();
  vtkTypeMacro(vtkCocoaTkUtilities, vtkObject);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  /**
   * Return the NSView for a Tk_Window.  It is returned as a void pointer
   * so that users of this function don't need to compile as Objective C.
   */
  static void* GetDrawableView(Tk_Window_* window);

protected:
  vtkCocoaTkUtilities() {}
  ~vtkCocoaTkUtilities() override {}

private:
  vtkCocoaTkUtilities(const vtkCocoaTkUtilities&) = delete;
  void operator=(const vtkCocoaTkUtilities&) = delete;
};

#endif
// VTK-HeaderTest-Exclude: vtkCocoaTkUtilities.h
