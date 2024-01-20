/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

//#include <iostream>
#include <stdio.h>

void mycppfunc() {
//   std::cout << "printing from within myfunc()\n";
  printf("Hello from within C++ using printf!\n");
}

extern "C" {
int myfunc() {
  mycppfunc();
  return 1;
}
}