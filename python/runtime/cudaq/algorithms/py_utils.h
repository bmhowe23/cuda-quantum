/****************************************************************-*- C++ -*-****
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#pragma once

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace cudaq {

/// @brief Perform base64 encoding of a dictionary.
std::string b64encode_dict(py::dict serializable_dict);

/// @brief Get a Base64-encoded pickled dictionary of a combination of all local
/// and global variables that are pickle-able.
py::dict get_serializable_var_dict();

/// @brief Fetch the Python source code from a `py::function`
std::string get_source_code(const py::function &func);

/// @brief Get a Python string of currently imported modules
/// @returns A string like `"import cudaq\nimport os\nimport numpy as np"`
std::string get_imports();

/// @brief Find the variable name for a given Python object handle. It searches
/// locally first, walks up the call stack, and finally checks the global
/// namespace. If not found, it returns an empty string.
std::string get_var_name_for_handle(const py::handle &h);

} // namespace cudaq
