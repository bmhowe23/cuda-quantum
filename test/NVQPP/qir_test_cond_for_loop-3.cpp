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
  void operator()(const int n_iter) __qpu__ {
    cudaq::qubit q0;
    cudaq::qubit q1;
    std::vector<cudaq::measure_result> resultVector(n_iter);
    for (int i = 0; i < n_iter; i++) {
      h(q0);
      resultVector[i] = mz(q0);
      if (resultVector[i])
        x(q1); // toggle q1 on every q0 coin toss that lands heads
    }
    auto q1result = mz(q1); // the measured q1 should contain the parity bit for
                            // the q0 measurements
  }
};

int main() {

  int nShots = 100;
  // Sample
  auto counts = cudaq::sample(/*shots=*/nShots, kernel{}, /*n_iter=*/5);
  counts.dump();
  // No assertions yet (FIXME)
}