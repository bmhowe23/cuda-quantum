/*******************************************************************************
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include <cudaq.h>

#define N 3
#define NUM_PHY_QUBITS (2*N*N - 1)

struct dim2 {
  float x = 0.0f;
  float y = 0.0f;
};

struct DataQubit;
struct StabilizerQubit;

struct StabilizerQubit {
  StabilizerQubit() = default;
  StabilizerQubit(dim2 c) : grid_coord(c), type(X) {}

  enum StabilizerType {
    X,
    Z
  };
  
  // Note: coordinates for stabilizer qubits are expected to be a.5, where a is
  // an integer. Values like -0.5 are acceptable, too.
  dim2 grid_coord{0.5f, 0.5f};
  StabilizerType type = X;
  int global_id = -1;
  DataQubit *NE = nullptr;
  DataQubit *NW = nullptr;
  DataQubit *SE = nullptr;
  DataQubit *SW = nullptr;
  bool enabled = false;
};

struct DataQubit {
  DataQubit() = default;

  dim2 grid_coord{0.0f, 0.0f};
  int global_id = -1;
  StabilizerQubit *NE = nullptr;
  StabilizerQubit *NW = nullptr;
  StabilizerQubit *SE = nullptr;
  StabilizerQubit *SW = nullptr;
};

class LogicalQubit {
public:
  LogicalQubit() = delete;
  LogicalQubit(const int distance) : distance(distance) {
    dataQubits = new DataQubit[distance * distance];
    stabilizerQubits = new StabilizerQubit[distance * distance - 1];
    dataVec.reserve(distance * distance);
    stabilizerVec.reserve(distance * distance - 1);

    // Initialize data qubits
    for (int r = 0, i = 0; r < distance; r++) {
      for (int c = 0; c < distance; c++, i++) {
        dataQubits[i].grid_coord.x = static_cast<float>(c);
        dataQubits[i].grid_coord.y = static_cast<float>(r);
        dataQubits[i].global_id = i;
        dataVec.push_back(&dataQubits[i]);
      }
    }

    // Initialize the stabilizer qubits. This drawing is for a 3x3 grid of data
    // qubits, which has 9-1=8 stabilizer qubits. This is called Surface-17.
    // Lower left and upper coordinates are labeled.
    //         +-----+-----+-----+-----+ (3,3)
    //         |     |  X  |     |     |
    //         +-----O-----O-----O-----+
    //         |     |  Z  |  X  |  Z  |
    //         +-----O-----O-----O-----+
    //         |  Z  |  X  |  Z  |     |
    //         +-----O-----O-----O-----+  O = data qubit
    //         |     |     |  X  |     |
    // (-1,-1) +-----+-----+-----+-----+
    for (int r = -1, i = 0; r < distance; r++) {
      bool firstRow = r == -1;
      bool lastRow = r == distance - 1;
      bool firstOrLastRow = firstRow || lastRow;
      for (int c = -1; c < distance; c++) {
        bool firstCol = c == -1;
        bool lastCol = c == distance - 1;
        bool firstOrLastCol = firstCol || lastCol;
        // Skip the 4 corners
        if (firstOrLastRow && firstOrLastCol)
          continue;
        // Skip every other stabilizer when we're on the outer edges
        if (firstRow && c % 2 == 0)
          continue;
        if (lastRow && c % 2 == 1)
          continue;
        if (firstCol && r % 2 == 1)
          continue;
        if (lastCol && r % 2 == 0)
          continue;

        int xzToggle = (r & 1) ^ (c & 1);

        // printf("Proceeding with row %d col %d i %d type %c\n", r, c, i,
        //        xzToggle == StabilizerQubit::X ? 'X' : 'Z');
        assert(i < distance * distance);
        stabilizerQubits[i].grid_coord.x = static_cast<float>(c) + 0.5f;
        stabilizerQubits[i].grid_coord.y = static_cast<float>(r) + 0.5f;
        stabilizerQubits[i].global_id = i + (distance * distance);
        stabilizerQubits[i].enabled = true;
        stabilizerQubits[i].type =
            static_cast<StabilizerQubit::StabilizerType>(xzToggle);
        stabilizerVec.push_back(&stabilizerQubits[i]);

        // Set neighbor pointers. This lambda function checks the bounds on the
        // row,col of the corresponding data qubit.
        auto calcNeighborIx = [&](int row, int col) {
          if (row >= 0 && row < distance && col >= 0 && col < distance)
            return row * distance + col;
          return -1;
        };
        //      
        // NW -> (r+1,c)----------------------------(r+1,c+1) < -NE
        //           |                                  |
        //           |         Stabilizer Qubit         |
        //           |          (r+.5,c+.5)             |
        //           |                                  |
        // SW -> (r  ,c)----------------------------(r  ,c+1) < -SE
        int iSW = calcNeighborIx(r, c);
        int iSE = calcNeighborIx(r, c + 1);
        int iNW = calcNeighborIx(r + 1, c);
        int iNE = calcNeighborIx(r + 1, c + 1);
        if (iSW >= 0) {
          stabilizerQubits[i].SW = &dataQubits[iSW];
          dataQubits[iSW].NE = &stabilizerQubits[i];
        }
        if (iSE >= 0) {
          stabilizerQubits[i].SE = &dataQubits[iSE];
          dataQubits[iSE].NW = &stabilizerQubits[i];
        }
        if (iNW >= 0) {
          stabilizerQubits[i].NW = &dataQubits[iNW];
          dataQubits[iNW].SE = &stabilizerQubits[i];
        }
        if (iNE >= 0) {
          stabilizerQubits[i].NE = &dataQubits[iNE];
          dataQubits[iNE].SW = &stabilizerQubits[i];
        }
        i++; // get ready for next iteration
      }
    }
  }

  // Sets data qubit IDs first, starting at startingOffset and incrementing
  // by 1. Then it assigns IDs to the stabilizer qubits picking up right after
  // the last data qubit.
  void setGlobalIds(int startingOffset) {
    int i = startingOffset;
    for (int j = 0; j < distance * distance; j++) {
      dataQubits[j].global_id = i++;
    }
    for (int j = 0; j < distance * distance - 1; j++) {
      stabilizerQubits[j].global_id = i++;
    }
  }

  ~LogicalQubit() {
    delete [] dataQubits;
    delete [] stabilizerQubits;
  }

private:
  DataQubit *dataQubits = nullptr;
  StabilizerQubit *stabilizerQubits = nullptr;
  int distance;

public:
  std::vector<DataQubit *> dataVec;
  std::vector<StabilizerQubit *> stabilizerVec;
};

#define MAX_ITER 10
int g_results[MAX_ITER][NUM_PHY_QUBITS];

struct myround {
  // All of these gates need to happen in parallel
  auto step1(LogicalQubit &s, cudaq::qubit q[]) {
    for (auto m : s.stabilizerVec) {
      if (m->enabled) {
        if (m->type == StabilizerQubit::X)
          reset(q[m->global_id]);
        else
          ry(0., q[m->global_id]);
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step2(LogicalQubit &s, cudaq::qubit q[]) {
    for (auto m : s.stabilizerVec) {
      if (m->enabled) {
        if (m->type == StabilizerQubit::X)
          h(q[m->global_id]);
        else
          reset(q[m->global_id]);
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step3(LogicalQubit &s, cudaq::qubit q[]) {
    for (auto m : s.stabilizerVec) {
      if (m->enabled) {
        auto neighbor = m->NE ? m->NE->global_id : -1;
        if (neighbor >= 0) {
          if (m->type == StabilizerQubit::X)
            x<cudaq::ctrl>(q[m->global_id], q[neighbor]);
          else
            x<cudaq::ctrl>(q[neighbor], q[m->global_id]);
        } else {
          ry(0., q[m->global_id]);
        }
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step4(LogicalQubit &s, cudaq::qubit q[]) {
    for (auto m : s.stabilizerVec) {
      if (m->enabled) {
        auto neighbor = m->NW ? m->NW->global_id : -1;
        if (neighbor >= 0) {
          if (m->type == StabilizerQubit::X)
            x<cudaq::ctrl>(q[m->global_id], q[neighbor]);
          else
            x<cudaq::ctrl>(q[neighbor], q[m->global_id]);
        } else {
          ry(0., q[m->global_id]);
        }
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step5(LogicalQubit &s, cudaq::qubit q[]) {
    for (auto m : s.stabilizerVec) {
      if (m->enabled) {
        auto neighbor = m->SE ? m->SE->global_id : -1;
        if (neighbor >= 0) {
          if (m->type == StabilizerQubit::X)
            x<cudaq::ctrl>(q[m->global_id], q[neighbor]);
          else
            x<cudaq::ctrl>(q[neighbor], q[m->global_id]);
        } else {
          ry(0., q[m->global_id]);
        }
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step6(LogicalQubit &s, cudaq::qubit q[]) {
    for (auto m : s.stabilizerVec) {
      if (m->enabled) {
        auto neighbor = m->SW ? m->SW->global_id : -1;
        if (neighbor >= 0) {
          if (m->type == StabilizerQubit::X)
            x<cudaq::ctrl>(q[m->global_id], q[neighbor]);
          else
            x<cudaq::ctrl>(q[neighbor], q[m->global_id]);
        } else {
          ry(0., q[m->global_id]);
        }
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step7(LogicalQubit &s, cudaq::qubit q[], int results[]) {
    for (auto m : s.stabilizerVec) {
      if (m->enabled) {
        if (m->type == StabilizerQubit::X)
          h(q[m->global_id]);
        else
          results[m->global_id] = mz(q[m->global_id]);
      }
    }
  }

  // All of these gates need to happen in parallel
  auto step8(LogicalQubit &s, cudaq::qubit q[], int results[]) {
    for (auto m : s.stabilizerVec) {
      if (m->enabled) {
        if (m->type == StabilizerQubit::X)
          results[m->global_id] = mz(q[m->global_id]);
        else
          ry(0., q[m->global_id]);
      }
    }
  }

  auto operator()(int n, bool performHFirst) {
    cudaq::qubit q[NUM_PHY_QUBITS];
    LogicalQubit s(N);

    if (performHFirst)
      for (auto qi : s.dataVec)
        //h(q[qi->global_id]);
        rx(0.9, q[qi->global_id]);

    for (int j = 0; j < MAX_ITER; j++) {
      step1(s, q);
      step2(s, q);
      step3(s, q);
      step4(s, q);
      step5(s, q);
      step6(s, q);
      step7(s, q, g_results[j]);
      step8(s, q, g_results[j]);

      // Demonstration of applying logical qubit operations on the surface. The
      // interesting thing about these is that the stabilizer measurements do
      // not change.
      if (j == 4) {
        for (auto qi : s.dataVec) {
          // Perform Z on row 0. This has the effect of performing a "logical Z"
          // ($Z_L$). Note that the stabilizer measurements do NOT change.
          if (qi->grid_coord.y == 0) {
            z(q[qi->global_id]);
          }
        }
        for (auto qi : s.dataVec) {
          // Perform X on column 0. This has the effect of performing a "logical
          // Z" ($X_L$). Note that the stabilizer measurements do NOT change.
          if (qi->grid_coord.x == 0) {
            x(q[qi->global_id]);
          }
        }
      }
    }
    // Perform a final measurement on the data qubits
    for (int i = 0; i < s.dataVec.size(); i++) {
      g_results[MAX_ITER - 1][s.dataVec[i]->global_id] =
          mz(q[s.dataVec[i]->global_id]);
    }
  }
};

void print_heading(LogicalQubit &s) {
  printf("  ");
  for (int i = 0; i < N*N; i++)
    printf("|D|");
  for (auto x : s.stabilizerVec)
    printf("%s", x->type == StabilizerQubit::X ? "|X|" : "|Z|");
  printf("\n");
}

void dump_g_results() {
  for (int i = 0; i < MAX_ITER; i++) {
    printf("  ");
    for (int j = 0; j < NUM_PHY_QUBITS; j++) {
      printf("|%d|", g_results[i][j]);
    }
    printf("\n");
  }
}

int main() {
  LogicalQubit s(N);

  cudaq::set_random_seed(13);
  bool performHFirst = false;
  for (int i = 0; i < 5; i++) {
    myround{}(N, performHFirst);
    print_heading(s);
    dump_g_results();
    printf("\n");
  }

  return 0;
}
