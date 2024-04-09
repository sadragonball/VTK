//
// Created by Fujun Xue(meidofuku@hotmail.com) on 2024/4/9.
//
#pragma once
#include <vulkan/vulkan.h>

#include "vtkSmartPointer.h"

#define VTK_DISABLE_COPY(Class)                                                                    \
  Class(const Class&) = delete;                                                                    \
  Class& operator=(const Class&) = delete;

#define VTK_DISABLE_MOVE(Class)                                                                    \
  Class(Class&&) = delete;                                                                         \
  Class& operator=(Class&&) = delete;

#define VTK_DISABLE_COPY_MOVE(Class)                                                               \
  VTK_DISABLE_COPY(Class)                                                                          \
  VTK_DISABLE_MOVE(Class)

template <typename T>
using vsp = vtkSmartPointer<T>;
