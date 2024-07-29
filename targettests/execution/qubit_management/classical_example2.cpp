/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// clang-format off
// RUN: CUDAQ_MLIR_PASS_STATISTICS=true nvq++ %cpp_std --target ionq --emulate %s -o %t && %t |& FileCheck %s
// RUN: CUDAQ_MLIR_PASS_STATISTICS=true nvq++ %cpp_std --target oqc  --emulate %s -o %t && %t |& FileCheck %s
// RUN: nvq++ -std=c++17 --enable-mlir %s -o %t

#include <cudaq.h>

struct run_test {
  __qpu__ auto operator()() {
    cudaq::qubit q,p;
    double d = 1.;

    rx(d, q);
    x(q);
    y(q);
    rx(d,p);
    x<cudaq::ctrl>(q,p);
    mz(q);
    mz(p);
  }
};

int main() {
  auto counts = cudaq::sample(run_test{});
  return 0;
}

// CHECK: (S) 5 num-cycles
// CHECK: (S) 2 num-physical-qubits
// CHECK: (S) 2 num-virtual-qubits