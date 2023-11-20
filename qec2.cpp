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

// Grid coordinates
struct dim2 {
  float x = 0.0f;
  float y = 0.0f;
};

// Forward declarations
struct DataQubit;
struct StabilizerQubit;

struct StabilizerQubit {
  StabilizerQubit() = default;
  StabilizerQubit(dim2 c) : grid_coord(c), type(X) {}

  enum StabilizerType { X, Z };

  // Note: coordinates for stabilizer qubits are expected to be a.5, where a is
  // an integer. Values like -0.5 are acceptable, too.
  dim2 grid_coord{0.5f, 0.5f};
  StabilizerType type = X;

  // Qubit ID. These IDs start at `X` (where `X` is any integer >= 0) for a
  // logical qubit, and increment for every physical qubit inside that logical
  // qubit.
  int global_id = -1;

  // Stabilizer-type-specific ID (IDs are duplicated across X and Z
  // stabilizers). These always start at 0 for a given LogicalQubit.
  int stab_id = -1;

  DataQubit *NE = nullptr;
  DataQubit *NW = nullptr;
  DataQubit *SE = nullptr;
  DataQubit *SW = nullptr;
  bool enabled = false;
};

struct DataQubit {
  DataQubit() = default;

  dim2 grid_coord{0.0f, 0.0f};

  // Qubit ID. These IDs start at `X` (where `X` is any integer >= 0) for a
  // logical qubit, and increment for every physical qubit inside that logical
  // qubit.
  int global_id = -1;

  // These don't seem to be needed
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

    XErrorLUT.resize(1 << ((distance * distance - 1) / 2), -1);
    ZErrorLUT.resize(1 << ((distance * distance - 1) / 2), -1);

    int x_id = 0;
    int z_id = 0;

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
        if (xzToggle == StabilizerQubit::X)
          stabilizerQubits[i].stab_id = x_id++;
        else
          stabilizerQubits[i].stab_id = z_id++;
        stabilizerVec.push_back(&stabilizerQubits[i]);

        // Set neighbor pointers. This lambda function checks the bounds on the
        // row,col of the corresponding data qubit.
        auto calcNeighborIx = [&](int row, int col) {
          if (row >= 0 && row < distance && col >= 0 && col < distance)
            return row * distance + col;
          return -1;
        };
        //      
        // NW -> (r+1,c)----------------------------(r+1,c+1) <- NE
        //           |                                  |
        //           |         Stabilizer Qubit         |
        //           |          (r+.5,c+.5)             |
        //           |                                  |
        // SW -> (r  ,c)----------------------------(r  ,c+1) <- SE
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

    buildDecodingLUTs();
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

  bool xCorrection(const std::vector<int> &stabilizerFlips) {
    int idx = 0;
    for (auto i : stabilizerFlips)
      if (this->stabilizerQubits[i].type == StabilizerQubit::X)
        idx |= (1 << i);
    auto qb = XErrorLUT[idx];
    if (qb >= 0)
      return true;
    return false;
  }

  bool zCorrection(const std::vector<int> &stabilizerFlips) {
    int idx = 0;
    for (auto i : stabilizerFlips)
      if (this->stabilizerQubits[i].type == StabilizerQubit::Z)
        idx |= (1 << i);
    auto qb = ZErrorLUT[idx];
    if (qb >= 0)
      return true;
    return false;
  }

private:
  DataQubit *dataQubits = nullptr;
  StabilizerQubit *stabilizerQubits = nullptr;
  int distance;
  std::vector<int> XErrorLUT;
  std::vector<int> ZErrorLUT;

  // Set bits in XErrorIdx and ZErrorIdx based on the stabilizer type and
  // stabilizer ID
  void setBitsForErrors(StabilizerQubit *stabQubit, std::size_t *XErrorIdx,
                        std::size_t *ZErrorIdx) {
    if (stabQubit) {
      if (stabQubit->type == StabilizerQubit::X) {
        if (XErrorIdx)
          *XErrorIdx |= (1ull << stabQubit->stab_id);
      } else {
        if (ZErrorIdx)
          *ZErrorIdx |= (1ull << stabQubit->stab_id);
      }
    }
  }

  // Build error-decoding lookup tables
  void buildDecodingLUTs() {
    // Loop through data qubits and identify error signature if this *one* qubit
    // were to flip (in either X or Z)
    for (auto s : dataVec) {
      std::size_t XErrorIdx = 0;
      std::size_t ZErrorIdx = 0;
      setBitsForErrors(s->NE, &XErrorIdx, &ZErrorIdx);
      setBitsForErrors(s->NW, &XErrorIdx, &ZErrorIdx);
      setBitsForErrors(s->SE, &XErrorIdx, &ZErrorIdx);
      setBitsForErrors(s->SW, &XErrorIdx, &ZErrorIdx);

      // Set the LUT[idx] such that it reveals the ID of the qubit that likely
      // flipped to produce this error signature.
      XErrorLUT[XErrorIdx] = s->global_id;
      ZErrorLUT[ZErrorIdx] = s->global_id;
    }
  }

public:
  std::vector<DataQubit *> dataVec;
  std::vector<StabilizerQubit *> stabilizerVec;
};

#define N_ROUNDS 10
int g_results[N_ROUNDS][NUM_PHY_QUBITS];

struct performRounds {
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

  auto operator()(int n, bool performLogicalXFirst,
                  int numRoundsToInjectSingleError) {
    cudaq::qubit q[NUM_PHY_QUBITS];
    LogicalQubit s(N);

    if (performLogicalXFirst) {
      if (1) {
        for (auto qi : s.dataVec) {
          // Perform X on column 0. This has the effect of performing a "logical
          // X" ($X_L$).
          if (qi->grid_coord.x == 0) {
            x(q[qi->global_id]);
          }
        }
      } else {
        for (auto qi : s.dataVec) {
          // Perform Z on row 0. This has the effect of performing a "logical
          // Z" ($Z_L$).
          if (qi->grid_coord.y == 0) {
            z(q[qi->global_id]);
          }
        }
      }
    }

    for (int round = 0; round < N_ROUNDS; round++) {
      step1(s, q);
      step2(s, q);
      step3(s, q);
      step4(s, q);
      step5(s, q);
      step6(s, q);
      step7(s, q, g_results[round]);
      step8(s, q, g_results[round]);

      // Randomly apply errors in either X or Z
      if (numRoundsToInjectSingleError > 0) {
        if (rand() & 1)
          x(q[rand()%(N*N)]);
        else
          z(q[rand()%(N*N)]);
        numRoundsToInjectSingleError--;
      }

      // As long as N_ROUNDS is even, the logical X_L and Z_L operations on the
      // logical qubit (below) will have no effect on the final result.

      // Demonstration of applying logical qubit operations on the surface. The
      // interesting thing about these is that the stabilizer measurements do
      // not change because the state vector changes are orthogonal to the
      // stabilizer subspace.
      for (auto qi : s.dataVec) {
        // Perform Z on row 0. This has the effect of performing a "logical Z"
        // ($Z_L$). Note that the stabilizer measurements do NOT change.
        if (qi->grid_coord.y == 0) {
          z(q[qi->global_id]);
        }
      }
      for (auto qi : s.dataVec) {
        // Perform X on column 0. This has the effect of performing a "logical
        // X" ($X_L$). Note that the stabilizer measurements do NOT change.
        if (qi->grid_coord.x == 0) {
          x(q[qi->global_id]);
        }
      }
    }
    // Perform a final measurement on the data qubits
    for (int i = 0; i < s.dataVec.size(); i++) {
      g_results[N_ROUNDS - 1][s.dataVec[i]->global_id] =
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
  for (int round = 0; round < N_ROUNDS; round++) {
    printf("  ");
    for (int j = 0; j < NUM_PHY_QUBITS; j++) {
      printf("|%d|", g_results[round][j]);
    }
    printf("\n");
  }
}

bool check_repeatable_stabilizers(int numRoundsToInjectSingleError) {
  // Make sure all stabilizer measurements in every iteration are the same as
  // the prior round. Or more specifically, make sure the number of rounds where
  // they don't match the prior round is the same as the number of rounds that
  // errors were injected.
  int numMismatchedRounds = 0;
  for (int round = 1; round < N_ROUNDS; round++) {
    int numMismatchesThisRound = 0;
    for (int j = N * N; j < NUM_PHY_QUBITS; j++)
      if (g_results[round][j] != g_results[round - 1][j])
        numMismatchesThisRound++;
    if (numMismatchesThisRound > 0)
      numMismatchedRounds++;
  }
  return numMismatchedRounds == numRoundsToInjectSingleError;
}

void analyze_results(LogicalQubit &s, bool performLogicalXFirst,
                     int numRoundsToInjectSingleError) {
  int parity = 0;
  int sum = 0;
  for (int j = 0; j < N * N; j++) {
    parity ^= g_results[N_ROUNDS - 1][j] ? 1 : 0;
    sum += g_results[N_ROUNDS - 1][j] ? 1 : 0;
  }

  // Perform error correction by analyzing the syndromes
  int x_flip = 0;
  int z_flip = 0;
  for (int round = 1; round < N_ROUNDS; round++) {
    std::vector<int> stabToggled;
    for (auto m : s.stabilizerVec) {
      const int id = m->global_id;
      if (g_results[round][id] != g_results[round - 1][id]) {
        // FIXME if using more than one logical qubit
        stabToggled.push_back(id - s.dataVec.size());
      } 
    }
    if (s.xCorrection(stabToggled))
      x_flip ^= 1;
    if (s.zCorrection(stabToggled))
      z_flip ^= 1;
  }

  if (z_flip)
    parity ^= 1;

  // The logical qubit measurement is simply the parity of the final result
  // measurements. (I can't find exactly where that's stated in the literature,
  // but that seems to hold true.)
  printf("Logical qubit init = %d; Error-corrected logical qubit measurement = "
         "%d (%s); x_flip = %d, z_flip = %d, Sum %d; Number of errors injected "
         "= %d; Stabilizers as expected? %s\n",
         performLogicalXFirst, parity,
         parity == performLogicalXFirst ? "expected" : "UNEXPECTED", x_flip,
         z_flip, sum, numRoundsToInjectSingleError,
         check_repeatable_stabilizers(numRoundsToInjectSingleError) ? "yes"
                                                                    : "no");
}

int main() {
  LogicalQubit s(N);

  srand(13);
  cudaq::set_random_seed(13);

  for (int i = 0; i < 30; i++) {
    bool performLogicalXFirst = rand() & 1 ? true : false;
    int numRoundsToInjectSingleError = rand() % (N_ROUNDS-1);
    performRounds{}(N, performLogicalXFirst, numRoundsToInjectSingleError);
    // print_heading(s);
    // dump_g_results();
    // printf("\n");
    analyze_results(s, performLogicalXFirst, numRoundsToInjectSingleError);
  }

  return 0;
}
