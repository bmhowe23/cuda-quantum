/*******************************************************************************
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include <cudaq.h>

#define N 5
#define N_SQUARED (N*N)

struct Qubit {
  Qubit() : row(0), col(0), nRows(0), nCols(0), type(DATA) {}
  Qubit(int row, int col, int nRows, int nCols)
      : row(row), col(col), nRows(nRows), nCols(nCols), type(DATA) {}

  int id() { return row * nCols + col; }
  int north() {
    if (row == 0)
      return -1;
    else
      return (row - 1) * nCols + col;
  }
  int south() {
    if (row == nRows - 1)
      return -1;
    else
      return (row + 1) * nCols + col;
  }
  int east() {
    if (col == nCols - 1)
      return -1;
    else
      return row * nCols + (col + 1);
  }
  int west() {
    if (col == 0)
      return -1;
    else
      return row * nCols + (col - 1);
  }

  enum QubitType {
    DATA,
    MEASUREMENT_X,
    MEASUREMENT_Z,
  };

  int row;
  int col;
  int nRows;
  int nCols;
  QubitType type;
};

struct Surface {
  std::vector<Qubit> qubits;
  std::vector<int> data_ids;
  std::vector<int> mx_ids;
  std::vector<int> mz_ids;
  std::vector<int> meas_ids;

  // Need a grid
  Surface(int dim) {
    qubits.reserve(dim * dim);
    for (int r = 0; r < dim; r++) {
      for (int c = 0; c < dim; c++) {
        Qubit q(r, c, dim, dim);
        if (r % 2 == 0) {
          // Even row
          if (c % 2 == 0) {
            // Even column
            q.type = Qubit::DATA;
          } else {
            // Odd column
            q.type = Qubit::MEASUREMENT_Z;
          }
        } else {
          // Odd row
          if (c % 2 == 0) {
            // Even column
            q.type = Qubit::MEASUREMENT_X;
          } else {
            // Odd column
            q.type = Qubit::DATA;
          }
        }
        auto tmp_id = q.id();
        if (q.type == Qubit::DATA)
          data_ids.push_back(tmp_id);
        if (q.type == Qubit::MEASUREMENT_X) {
          mx_ids.push_back(tmp_id);
          meas_ids.push_back(tmp_id);
        }
        if (q.type == Qubit::MEASUREMENT_Z) {
          mz_ids.push_back(tmp_id);
          meas_ids.push_back(tmp_id);
        }
        qubits.push_back(q);
      }
    }
  }
};

struct myround {
  // All of these gates need to happen in parallel
  auto step1(Surface &s, cudaq::qubit q[]) {
    for (auto m : s.meas_ids) {
      if (s.qubits[m].type == Qubit::MEASUREMENT_X)
        reset(q[m]);
      else
        ry(0., q[m]);
    }
  }

  // All of these gates need to happen in parallel
  auto step2(Surface &s, cudaq::qubit q[]) {
    for (auto m : s.meas_ids) {
      if (s.qubits[m].type == Qubit::MEASUREMENT_X)
        h(q[m]);
      else
        reset(q[m]);
    }
  }

  // All of these gates need to happen in parallel
  auto step3(Surface &s, cudaq::qubit q[]) {
    for (auto m : s.meas_ids) {
      auto neighbor = s.qubits[m].north();
      if (neighbor >= 0) {
        if (s.qubits[m].type == Qubit::MEASUREMENT_X)
          x<cudaq::ctrl>(q[m], q[neighbor]);
        else
          x<cudaq::ctrl>(q[neighbor], q[m]);
      } else {
        ry(0., q[m]);
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step4(Surface &s, cudaq::qubit q[]) {
    for (auto m : s.meas_ids) {
      auto neighbor = s.qubits[m].west();
      if (neighbor >= 0) {
        if (s.qubits[m].type == Qubit::MEASUREMENT_X)
          x<cudaq::ctrl>(q[m], q[neighbor]);
        else
          x<cudaq::ctrl>(q[neighbor], q[m]);
      } else {
        ry(0., q[m]);
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step5(Surface &s, cudaq::qubit q[]) {
    for (auto m : s.meas_ids) {
      auto neighbor = s.qubits[m].east();
      if (neighbor >= 0) {
        if (s.qubits[m].type == Qubit::MEASUREMENT_X)
          x<cudaq::ctrl>(q[m], q[neighbor]);
        else
          x<cudaq::ctrl>(q[neighbor], q[m]);
      } else {
        ry(0., q[m]);
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step6(Surface &s, cudaq::qubit q[]) {
    for (auto m : s.meas_ids) {
      auto neighbor = s.qubits[m].south();
      if (neighbor >= 0) {
        if (s.qubits[m].type == Qubit::MEASUREMENT_X)
          x<cudaq::ctrl>(q[m], q[neighbor]);
        else
          x<cudaq::ctrl>(q[neighbor], q[m]);
      } else {
        ry(0., q[m]);
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step7(Surface &s, cudaq::qubit q[], int results[]) {
    for (auto m : s.meas_ids) {
      if (s.qubits[m].type == Qubit::MEASUREMENT_X)
        h(q[m]);
      else {
        char mydat[256];
        snprintf(mydat, sizeof(mydat), "meas_id%02d", m);
        //results[m] = mz(q[m]);
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step8(Surface &s, cudaq::qubit q[], int results[]) {
    for (auto m : s.meas_ids) {
      if (s.qubits[m].type == Qubit::MEASUREMENT_X) {
        char mydat[256];
        snprintf(mydat, sizeof(mydat), "meas_id%02d", m);
        //h(q[m]);
        //results[m] = mz(q[m]);
      } else {
        ry(0., q[m]);
      }
    }
  }

  auto operator()(int n, bool performHFirst) __qpu__ {
    cudaq::qubit q[N_SQUARED];
    int results[N_SQUARED] = {0};
    for (int i = 0; i < N_SQUARED; i++)
      results[i] = -1;
    Surface s(N);
    if (performHFirst)
      for (auto qi : s.data_ids)
        h(q[qi]);
    step1(s, q);
    step2(s, q);
    step3(s, q);
    step4(s, q);
    step5(s, q);
    step6(s, q);
    step7(s, q, results);
    step8(s, q, results);

    // step1(s, q);
    // step2(s, q);
    // step3(s, q);
    // step4(s, q);
    // step5(s, q);
    // step6(s, q);
    // step7(s, q, results);
    // step8(s, q, results);
  }
};

void dumpSingleShot(cudaq::sample_result &counts) {
  printf("  ");
  for (auto &[bits, _] : counts) {
    const char *tmp = bits.c_str();
    for (int i = 0; i < bits.length(); i++) {
      printf("|%c|", tmp[i]);
    }
  }
  printf("\n");
}

int main() {

  Surface s(N);
  int i = 0;
  for (int r = 0; r < N; r++) {
    for (int c = 0; c < N; c++) {
      printf("%s",
             s.qubits[i].type == Qubit::DATA
                 ? " D "
                 : (s.qubits[i].type == Qubit::MEASUREMENT_X ? " X " : " Z "));
      i++;
    }
    printf("\n");
  }
  printf("  ");
  i = 0;
  for (int r = 0; r < N; r++) {
    for (int c = 0; c < N; c++) {
      printf("%s",
             s.qubits[i].type == Qubit::DATA
                 ? "|D|"
                 : (s.qubits[i].type == Qubit::MEASUREMENT_X ? "|X|" : "|Z|"));
      i++;
    }
  }
  printf("\n");

  cudaq::set_random_seed(13);
  bool performHFirst = false;
  for (int i = 0; i < 10; i++) {
    auto counts = cudaq::sample(/*shots=*/1, myround{}, N, performHFirst);
    dumpSingleShot(counts);
  }

  return 0;
}
