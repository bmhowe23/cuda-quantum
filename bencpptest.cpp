/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

//#include <iostream>
#include <stdio.h>
#include <chrono>
#include <mpi.h>
#include <time.h>

int g_rank = -1;

void mycppfunc() {
// //   std::cout << "printing from within myfunc()\n";
//   MPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
//   printf("Hello from within C++ using printf (rank = %d)!\n", g_rank);
// //   const auto t0 = std::chrono::system_clock::now();
//   struct timespec t0, t1;
//   const int n_iter = 1000000;
//   MPI_Barrier(MPI_COMM_WORLD);
//   clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
//   for (int i = 0; i < n_iter; i++) {
//     MPI_Barrier(MPI_COMM_WORLD);
//   }
//   clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
//   printf("Done with barrier (took %.3f us)\n", ((t1.tv_sec - t0.tv_sec)*1000000.0 + (t1.tv_nsec - t0.tv_nsec)/1000.0)/n_iter);
  printf("Hello from a brand new world\n");
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