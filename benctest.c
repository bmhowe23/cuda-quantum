/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

//#include <stdio.h>

int myfunc() {
//   printf("Hello from C\n");
//   fflush(0);
  int i = 0;
  i = i + 1;
  return i;
}

// void myfunc() {
//   std::cout << "printing from within myfunc()\n";
// }

// extern "C" {
// void mycfunc() {myfunc();}
// }