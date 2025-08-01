// Compile and run with:
// ```
// nvq++ vqe_h2.cpp -o vqe.x && ./vqe.x
// ```

#include <cudaq.h>
#include <cudaq/algorithm.h>
#include <cudaq/builder.h>
#include <cudaq/gradients.h>
#include <cudaq/optimizers.h>

// Here we build up a CUDA-Q kernel with N layers and each
// layer containing an arrangement of random SO(4) rotations. The algorithm
// leverages the CUDA-Q VQE support to compute the ground state of the
// Hydrogen atom.

// The SO4 random entangler written as a CUDA-Q kernel free function
// since this is a pure-device quantum kernel
__qpu__ void so4(cudaq::qubit &q, cudaq::qubit &r,
                 const std::vector<double> &thetas) {
  ry(thetas[0], q);
  ry(thetas[1], r);

  h(r);
  x<cudaq::ctrl>(q, r);
  h(r);

  ry(thetas[2], q);
  ry(thetas[3], r);

  h(r);
  x<cudaq::ctrl>(q, r);
  h(r);

  ry(thetas[4], q);
  ry(thetas[5], r);

  h(r);
  x<cudaq::ctrl>(q, r);
  h(r);
}

// The SO4 fabric CUDA-Q kernel. Keeps track of simple
// arithmetic class members controlling the number of qubits and
// entangling layers.
struct so4_fabric {
  void operator()(std::vector<double> params, int n_qubits,
                  int n_layers) __qpu__ {
    cudaq::qvector q(n_qubits);

    x(q[0]);
    x(q[2]);

    const int block_size = 2;
    int counter = 0;
    for (int i = 0; i < n_layers; i++) {
      // first layer of so4 blocks (even)
      for (int k = 0; k < n_qubits; k += 2) {
        auto subq = q.slice(k, block_size);
        auto so4_params = cudaq::slice_vector(params, counter, 6);
        so4(subq[0], subq[1], so4_params);
        counter += 6;
      }

      // second layer of so4 blocks (odd)
      for (int k = 1; k + block_size < n_qubits; k += 2) {
        auto subq = q.slice(k, block_size);
        auto so4_params = cudaq::slice_vector(params, counter, 6);
        so4(subq[0], subq[1], so4_params);
        counter += 6;
      }
    }
  }
};

int main() {
  // Read in the spin op from file
  std::vector<double> h2_data{
      15, -0.10647701149499994, 0, 4, 0, 0, 1, 0, 2, 0, 3,
      0,  0.0454063328691,      0, 4, 0, 2, 1, 2, 2, 2, 3,
      2,  0.0454063328691,      0, 4, 0, 2, 1, 2, 2, 3, 3,
      3,  0.0454063328691,      0, 4, 0, 3, 1, 3, 2, 2, 3,
      2,  0.0454063328691,      0, 4, 0, 3, 1, 3, 2, 3, 3,
      3,  0.170280101353,       0, 4, 0, 1, 1, 0, 2, 0, 3,
      0,  0.120200490713,       0, 4, 0, 1, 1, 1, 2, 0, 3,
      0,  0.168335986252,       0, 4, 0, 1, 1, 0, 2, 1, 3,
      0,  0.165606823582,       0, 4, 0, 1, 1, 0, 2, 0, 3,
      1,  -0.22004130022499996, 0, 4, 0, 0, 1, 1, 2, 0, 3,
      0,  0.165606823582,       0, 4, 0, 0, 1, 1, 2, 1, 3,
      0,  0.174072892497,       0, 4, 0, 0, 1, 1, 2, 0, 3,
      1,  0.170280101353,       0, 4, 0, 0, 1, 0, 2, 1, 3,
      0,  0.120200490713,       0, 4, 0, 0, 1, 0, 2, 1, 3,
      1,  -0.22004130022499996, 0, 4, 0, 0, 1, 0, 2, 0, 3,
      1};
  cudaq::spin_op H(h2_data);
  // For 8 qubits, 36 parameters per layer
  int n_layers = 2, n_qubits = H.num_qubits(), block_size = 2, p_counter = 0;
  int n_blocks_per_layer = 2 * (n_qubits / block_size) - 1;
  int n_params = n_layers * 6 * n_blocks_per_layer;
  printf("%d qubit Hamiltonian -> %d parameters\n", n_qubits, n_params);

  // Define the initial parameters and ansatz.
  auto init_params =
      cudaq::random_vector(-1, 1, n_params, std::mt19937::default_seed);

  so4_fabric ansatz;

  // Run VQE.
  cudaq::optimizers::lbfgs optimizer;
  optimizer.initial_parameters = init_params;
  optimizer.max_eval = 20;
  optimizer.max_line_search_trials = 10;
  cudaq::gradients::central_difference gradient;
  auto [opt_val, opt_params] =
      cudaq::vqe(ansatz, gradient, H, optimizer, n_params, n_qubits, n_layers);

  printf("Optimal value = %.16lf\n", opt_val);
}
