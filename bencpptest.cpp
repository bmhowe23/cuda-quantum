/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include <iostream>
#include <stdio.h>
#include <chrono>
#include <mpi.h>
#include <time.h>

int g_rank = -1;

void mycppfunc() {
  std::cout << "printing from within myfunc()\n";
  MPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
  printf("Hello from within C++ using printf (rank = %d)!\n", g_rank);
//   const auto t0 = std::chrono::system_clock::now();
  struct timespec t0, t1;
  const int n_iter = 1000000;
  MPI_Barrier(MPI_COMM_WORLD);
  clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
  for (int i = 0; i < n_iter; i++) {
    MPI_Barrier(MPI_COMM_WORLD);
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
  
  // This is done very ugly and complicated to avoid this downstream error:
  //  error: unhandled intrinsic:   %7 = call double @llvm.fmuladd.f64(double %conv, double 1.000000e+06, double %div)
  long uSec1 = t1.tv_sec - t0.tv_sec;
  uSec1 *= 1000000.0;
  long uSec2 = t1.tv_nsec - t0.tv_nsec;
  uSec2 /= 1000.0;
  double uSec = uSec1 + uSec2;

  printf("Done with %d barriers (took %.3f us per barrier)\n", n_iter,
         uSec / n_iter);
}

extern "C" {
int myfunc() {
  mycppfunc();
  return 1;
}

// int main(int argc, char *argv[]) {
//   mycppfunc();
// }
}