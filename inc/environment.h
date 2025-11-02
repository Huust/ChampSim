/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

#include <functional>
#include <vector>

#include "cache.h"
#include "shim_layer.h"
#include "dram_controller.h"
#include "cxl_memory.h"
#include "ooo_cpu.h"
#include "operable.h"
#include "ptw.h"
#include "vmem.h"

// Forward declaration for Ramulator controller
class RAMULATOR_CONTROLLER;

namespace champsim
{
struct environment {
  virtual std::vector<std::reference_wrapper<operable>> operable_view() = 0;
  virtual std::vector<std::reference_wrapper<O3_CPU>> cpu_view() = 0;
  virtual std::vector<std::reference_wrapper<CACHE>> cache_view() = 0;
  virtual std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() = 0;
  virtual SHIM_LAYER& router_view() = 0;

  // Memory subsystem presence flags
  // Note: has_dram() returns true if physical memory subsystem exists (regardless of implementation)
  // Note: has_cxl() returns true if CXL memory subsystem exists
  virtual bool has_dram() const { return false; }
  virtual bool has_cxl() const { return false; }

  // Memory simulator type flag
  // Note: uses_ramulator() returns true if using Ramulator for DRAM simulation (applies to all DRAM components)
  virtual bool uses_ramulator() const { return false; }

  // ChampSim builtin DRAM controller views
  // Note: Returns nullptr when DRAM disabled or when using Ramulator
  virtual MEMORY_CONTROLLER* builtin_dram_view() { return nullptr; }
  virtual MEMORY_CONTROLLER* builtin_cxl_dram_view() { return nullptr; }

  // Ramulator DRAM controller views
  // Note: Returns nullptr when DRAM disabled or when using builtin implementation
  virtual RAMULATOR_CONTROLLER* ramulator_dram_view() { return nullptr; }
  virtual RAMULATOR_CONTROLLER* ramulator_cxl_dram_view() { return nullptr; }

  // CXL controller view
  // Note: Returns nullptr when CXL disabled
  virtual CXL_CONTROLLER* cxl_view() { return nullptr; }
};

namespace configured
{
template <unsigned long long ID>
struct generated_environment;
}
} // namespace champsim

#endif
