/*******************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// RUN: cudaq-quake %cpp_std -verify %s -o /dev/null

#include <cudaq.h>
#include <set>

struct InvalidKernel1 {
  void operator()(void *m) __qpu__ { // expected-error{{kernel argument type not supported}}
    cudaq::qvector reg(4);
    x(reg);
  }
};

struct InvalidKernel2 {
  void operator()(int *m) __qpu__ { // expected-error{{kernel argument type not supported}}
    cudaq::qvector reg(4);
    x(reg);
  }
};

struct InvalidKernel2_1 {
  void operator()(int &m) __qpu__ { // expected-error{{kernel argument type not supported}}
    cudaq::qvector reg(4);
    x(reg);
  }
};

struct InvalidKernel2_2 {
  void operator()(const int &m) __qpu__ { // expected-error{{kernel argument type not supported}}
    cudaq::qvector reg(4);
    x(reg);
  }
};

struct InvalidKernel3_1 {
  void operator()(const std::set<int> &m) __qpu__ { // expected-error{{kernel argument type not supported}}
    cudaq::qvector reg(4);
    x(reg);
  }
};

struct InvalidKernel4 {
   std::vector<int*> operator()() __qpu__ { // expected-error{{kernel result type not supported}}
    cudaq::qvector reg(4);
    x(reg);
    return {};
  }
};
