// Compile and run with:
// ```
// nvq++ --target iqm iqm.cpp --iqm-machine Crystal_5 -o out.x && ./out.x
// ```
// Assumes a valid set of credentials have been stored.

#include <cudaq.h>
#include <fstream>

// Define a simple quantum kernel to execute on IQM Server.
struct crystal_5_ghz {
  // Maximally entangled state between 5 qubits on Crystal_5 QPU.
  //       QB1
  //        |
  // QB2 - QB3 - QB4
  //        |
  //       QB5

  void operator()() __qpu__ {
    cudaq::qvector q(5);
    h(q[0]);

    // Note that the CUDA-Q compiler will automatically generate the
    // necessary instructions to swap qubits to satisfy the required
    // connectivity constraints for the Crystal_5 QPU. In this program, that
    // means that despite QB1 not being physically connected to QB2, the user
    // can still perform joint operations q[0] and q[1] because the compiler
    // will automatically (and transparently) inject the necessary swap
    // instructions to execute the user's program without the user having to
    // worry about the physical constraints.
    for (int i = 0; i < 4; i++) {
      x<cudaq::ctrl>(q[i], q[i + 1]);
    }
    auto result = mz(q);
  }
};

int main() {
  // Submit to IQM Server asynchronously. E.g, continue executing
  // code in the file until the job has been returned.
  auto future = cudaq::sample_async(crystal_5_ghz{});
  // ... classical code to execute in the meantime ...

  // Can write the future to file:
  {
    std::ofstream out("saveMe.json");
    out << future;
  }

  // Then come back and read it in later.
  cudaq::async_result<cudaq::sample_result> readIn;
  std::ifstream in("saveMe.json");
  in >> readIn;

  // Get the results of the read in future.
  auto async_counts = readIn.get();
  async_counts.dump();

  // OR: Submit to IQM Server synchronously. E.g, wait for the job
  // result to be returned before proceeding.
  auto counts = cudaq::sample(crystal_5_ghz{});
  counts.dump();
}
