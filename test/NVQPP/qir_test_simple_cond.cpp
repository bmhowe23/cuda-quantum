/*******************************************************************************
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// RUN: nvq++ --target quantinuum --emulate %s -o %basename_t.x && ./%basename_t.x

// The test here is the assert statement. 

#include <cudaq.h>

struct kernel {
  void operator()() __qpu__ {
    cudaq::qubit q0;
    cudaq::qubit q1;
    h(q0);
    auto q0result = mz(q0);
    if (q0result)
      x(q1);
    auto q1result = mz(q1); // Every q1 measurement will be the same as q0
  }
};

int main() {

  int nShots = 100;
  // Sample
  auto counts = cudaq::sample(/*shots=*/nShots, kernel{});
  counts.dump();
  // No assertions yet (FIXME)
}