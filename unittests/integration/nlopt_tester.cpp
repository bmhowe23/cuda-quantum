/*******************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include "CUDAQTestUtils.h"
#include <cudaq/algorithm.h>
#include <cudaq/builder.h>

#include <cudaq/algorithms/gradients/central_difference.h>
#include <cudaq/optimizers.h>

// Rotational gates not supported in Stim.
#ifndef CUDAQ_BACKEND_STIM

// Skip these Nlopt optimizer tests for slow backends to reduce test time.
// Note: CUDA-Q API level tests (e.g., `cudaq::observe`) should cover all
// backend-specific functionalities required to interface with optimizers.
#if !defined CUDAQ_BACKEND_DM && !defined CUDAQ_BACKEND_TENSORNET

struct deuteron_n3_ansatz {
  void operator()(double x0, double x1) __qpu__ {
    cudaq::qvector q(3);
    x(q[0]);
    ry(x0, q[1]);
    ry(x1, q[2]);
    x<cudaq::ctrl>(q[2], q[0]);
    x<cudaq::ctrl>(q[0], q[1]);
    ry(-x0, q[1]);
    x<cudaq::ctrl>(q[0], q[1]);
    x<cudaq::ctrl>(q[1], q[0]);
  }
};

CUDAQ_TEST(NloptTester, checkSimple) {

  cudaq::spin_op h =
      5.907 - 2.1433 * cudaq::spin_op::x(0) * cudaq::spin_op::x(1) -
      2.1433 * cudaq::spin_op::y(0) * cudaq::spin_op::y(1) +
      .21829 * cudaq::spin_op::z(0) - 6.125 * cudaq::spin_op::z(1);
  cudaq::spin_op h3 = h + 9.625 - 9.625 * cudaq::spin_op::z(2) -
                      3.913119 * cudaq::spin_op::x(1) * cudaq::spin_op::x(2) -
                      3.913119 * cudaq::spin_op::y(1) * cudaq::spin_op::y(2);

  printf("\nOptimize with gradients.\n");

  cudaq::optimizers::lbfgs optimizer;
  cudaq::gradients::central_difference gradient(
      deuteron_n3_ansatz{},
      [](std::vector<double> x) { return std::make_tuple(x[0], x[1]); });

  auto [opt_val_0, optpp] = optimizer.optimize(
      2, [&](const std::vector<double> &x, std::vector<double> &grad_vec) {
        double e = cudaq::observe(deuteron_n3_ansatz{}, h3, x[0], x[1]);
        gradient.compute(x, grad_vec, h3, e);
        printf("<H>(%lf, %lf) = %lf\n", x[0], x[1], e);
        return e;
      });

  EXPECT_NEAR(opt_val_0, -2.0453, 1e-2);
}

CUDAQ_TEST(NloptTester, checkOtherSignatures) {

  cudaq::spin_op h =
      5.907 - 2.1433 * cudaq::spin_op::x(0) * cudaq::spin_op::x(1) -
      2.1433 * cudaq::spin_op::y(0) * cudaq::spin_op::y(1) +
      .21829 * cudaq::spin_op::z(0) - 6.125 * cudaq::spin_op::z(1);
  cudaq::spin_op h3 = h + 9.625 - 9.625 * cudaq::spin_op::z(2) -
                      3.913119 * cudaq::spin_op::x(1) * cudaq::spin_op::x(2) -
                      3.913119 * cudaq::spin_op::y(1) * cudaq::spin_op::y(2);

  printf("\nOptimize with gradients.\n");

  cudaq::optimizers::cobyla optimizer;
  auto [opt_val_0, optpp] =
      optimizer.optimize(2, [&](const std::vector<double> &x) {
        double e = cudaq::observe(deuteron_n3_ansatz{}, h3, x[0], x[1]);
        printf("<H>(%lf, %lf) = %lf\n", x[0], x[1], e);
        return e;
      });

  EXPECT_NEAR(opt_val_0, -2.0453, 1e-2);

  auto [opt_val_1, optpp1] = optimizer.optimize(2, [&](std::vector<double> x) {
    double e = cudaq::observe(deuteron_n3_ansatz{}, h3, x[0], x[1]);
    printf("<H>(%lf, %lf) = %lf\n", x[0], x[1], e);
    return e;
  });

  EXPECT_NEAR(opt_val_1, -2.0453, 1e-2);

  // This should throw an exception
  cudaq::optimizers::lbfgs grad_optimizer;
  EXPECT_ANY_THROW({
    grad_optimizer.optimize(2, [&](std::vector<double> x) {
      double e = cudaq::observe(deuteron_n3_ansatz{}, h3, x[0], x[1]);
      printf("<H>(%lf, %lf) = %lf\n", x[0], x[1], e);
      return e;
    });
  });
}

#endif

#endif
