/*******************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include "common/Resources.h"
#include "runtime/cudaq/platform/py_alt_launch_kernel.h"
#include "utils/LinkedLibraryHolder.h"
#include "utils/OpaqueArguments.h"
#include "mlir/Bindings/Python/PybindAdaptors.h"
#include "mlir/CAPI/IR.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"

#include <fmt/core.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace cudaq {
void bindPyDetectors(py::module &mod) {
  mod.def(
      "detectors",
      [&](py::object kernel, py::args args) {
        if (py::hasattr(kernel, "compile"))
          kernel.attr("compile")();
        auto &platform = cudaq::get_platform();
        auto kernelName = kernel.attr("name").cast<std::string>();
        auto kernelMod = kernel.attr("module").cast<MlirModule>();
        args = simplifiedValidateInputArguments(args);
        std::unique_ptr<OpaqueArguments> argData(
            toOpaqueArgs(args, kernelMod, kernelName));

        auto ctx = std::make_unique<ExecutionContext>("detectors", 1);
        ctx->kernelName = kernelName;
        // Indicate that this is not an async exec
        ctx->asyncExec = false;

        // Set the platform
        platform.set_exec_ctx(ctx.get());

        pyAltLaunchKernel(kernelName, kernelMod, *argData, {});

        platform.reset_exec_ctx();

        if (!ctx->detector_measurement_indices) {
          throw std::runtime_error("No detector measurement indices found");
        }
        return *ctx->detector_measurement_indices;
      },
      py::arg("kernel"), py::kw_only(),
      R"#(Identify detectors in the given quantum kernel expression and returns
a list of detector measurement indices.

Args:
  kernel (:class:`Kernel`): The :class:`Kernel` to identify detectors on
  *arguments (Optional[Any]): The concrete values to evaluate the kernel 
    function at. Leave empty if the kernel doesn't accept any arguments.

Returns:
  :class:`list`:
  A list of detector measurement indices for the :class:`Kernel`.)#");
}
} // namespace cudaq
