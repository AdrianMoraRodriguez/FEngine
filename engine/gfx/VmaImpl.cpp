/**
 * @file VmaImpl.cpp
 * @brief Single translation unit that compiles the VMA implementation.
 *
 * VMA is a header-only library that requires exactly one translation unit
 * to define VMA_IMPLEMENTATION before including the header. All other files
 * include vk_mem_alloc.h without that define and get only declarations.
 */

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
