/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include "nvqir/CircuitSimulator.h"
#include "nvqir/Gates.h"
#include "stim.h"

#include <bit>
#include <iostream>
#include <set>
#include <span>

using namespace cudaq;

namespace nvqir {

/// @brief The StimCircuitSimulator implements the CircuitSimulator
/// base class to provide a simulator delegating to the Stim library from
/// https://github.com/quantumlib/Stim.
class StimCircuitSimulator : public nvqir::CircuitSimulatorBase<double> {
protected:
  stim::Circuit fullStimCircuit;
  stim::Circuit partialCircuit;
  std::mt19937_64 randomEngine;
  std::unique_ptr<stim::TableauSimulator<stim::MAX_BITWORD_WIDTH>> simulator;
  bool midCircuit = false;
  stim::simd_bits<stim::MAX_BITWORD_WIDTH> ref_sample;

  /// @brief Grow the state vector by one qubit.
  void addQubitToState() override { addQubitsToState(1); }

  /// @brief Override the default sized allocation of qubits
  /// here to be a bit more efficient than the default implementation
  void addQubitsToState(std::size_t qubitCount,
                        const void *stateDataIn = nullptr) override {
    if (stateDataIn)
      throw std::runtime_error("The Stim simulator does not support "
                               "initialization of qubits from state data.");
    return;
  }

  /// @brief Reset the qubit state.
  void deallocateStateImpl() override {
    fullStimCircuit.clear();
    partialCircuit.clear();
    simulator.reset();
  }

  void safe_append_ua(const std::string &gate_name,
                      const std::vector<uint32_t> &targets,
                      double singleton_arg) {
    fullStimCircuit.safe_append_ua(gate_name, targets, singleton_arg);
    partialCircuit.safe_append_ua(gate_name, targets, singleton_arg);
  }

  void safe_append_u(const std::string &gate_name,
                     const std::vector<uint32_t> &targets) {
    fullStimCircuit.safe_append_u(gate_name, targets);
    partialCircuit.safe_append_u(gate_name, targets);
  }

  /// @brief Apply the noise channel on \p qubits
  void applyNoiseChannel(const std::string_view gateName,
                         const std::vector<std::size_t> &controls,
                         const std::vector<std::size_t> &targets,
                         const std::vector<double> &params) override {
    // Do nothing if no execution context
    if (!executionContext)
      return;

    // Do nothing if no noise model
    if (!executionContext->noiseModel)
      return;

    // Get the name as a string
    std::string gName(gateName);

    // Cast size_t to uint32_t
    std::vector<std::uint32_t> stimTargets;
    stimTargets.reserve(controls.size() + targets.size());
    for (auto q : controls)
      stimTargets.push_back(static_cast<std::uint32_t>(q));
    for (auto q : targets)
      stimTargets.push_back(static_cast<std::uint32_t>(q));

    // Get the Kraus channels specified for this gate and qubits
    auto krausChannels = executionContext->noiseModel->get_channels(
        gName, targets, controls, params);

    // If none, do nothing
    if (krausChannels.empty())
      return;

    cudaq::info("Applying {} kraus channels to qubits {}", krausChannels.size(),
                stimTargets);

    for (auto &channel : krausChannels) {
      if (channel.noise_type == cudaq::noise_model_type::bit_flip_channel)
        safe_append_ua("X_ERROR", stimTargets, channel.parameters[0]);
      else if (channel.noise_type ==
               cudaq::noise_model_type::phase_flip_channel)
        safe_append_ua("Z_ERROR", stimTargets, channel.parameters[0]);
      else if (channel.noise_type ==
               cudaq::noise_model_type::depolarization_channel)
        safe_append_ua("DEPOLARIZE1", stimTargets, channel.parameters[0]);
    }
  }

  void applyGate(const GateApplicationTask &task) override {
    std::string gateName(task.operationName);
    std::transform(gateName.begin(), gateName.end(), gateName.begin(),
                   ::toupper);
    std::vector<std::uint32_t> stimTargets;

    // These CUDA-Q rotation gates have the same name as Stim "reset" gates.
    // Stim is a Clifford simulator, so it doesn't actually support rotational
    // gates. Throw exceptions if they are encountered here.
    // TODO - consider adding support for specific rotations (e.g. pi/2).
    if (gateName == "RX" || gateName == "RY" || gateName == "RZ")
      throw std::runtime_error(
          fmt::format("Gate not supported by Stim simulator: {}. Note that "
                      "Stim can only simulate Clifford gates.",
                      task.operationName));

    if (task.controls.size() > 1)
      throw std::runtime_error(
          "Gates with >1 controls not supported by stim simulator");
    if (task.controls.size() >= 1)
      gateName = "C" + gateName;
    for (auto c : task.controls)
      stimTargets.push_back(c);
    for (auto t : task.targets)
      stimTargets.push_back(t);
    try {
      safe_append_u(gateName, stimTargets);
    } catch (std::out_of_range &e) {
      throw std::runtime_error(
          fmt::format("Gate not supported by Stim simulator: {}. Note that "
                      "Stim can only simulate Clifford gates.",
                      e.what()));
    }
  }

  /// @brief Set the current state back to the |0> state.
  void setToZeroState() override { return; }

  /// @brief Override the calculateStateDim because this is not a state vector
  /// simulator.
  std::size_t calculateStateDim(const std::size_t numQubits) override {
    return 0;
  }

  /// @brief Measure the qubit and return the result.
  bool measureQubit(const std::size_t index) override {
    // This is a mid-circuit measurement of a single qubit.
    // TODO - if this is in a loop of measurements, it is fairly inefficient
    // because each sample call is a fresh entry into Stim. Considering trying
    // to batch these somehow.
    midCircuit = true;
    auto execResult = sample({index}, /*shots=*/1);
    midCircuit = false;
    // The measurement value is the first measurement of the first (and only)
    // shot.
    bool result = execResult.sequentialData[0][0] == '1';
    if (index == 0) {
    cudaq::info("Measured qubit {} -> {}", index, result);
    static bool falseSeen = false;
    static bool trueSeen = false;
    if (!falseSeen && !result) {
      // falseSeen = true;
      printf("BMH result was %d circuit size %s\n", result, fullStimCircuit.operations[1].str().c_str());
    } else if (!trueSeen && result) {
      // trueSeen = true;
      printf("BMH result was %d circuit size %s\n", result, fullStimCircuit.operations[1].str().c_str());
    }
    }
    return result;
  }

  QubitOrdering getQubitOrdering() const override { return QubitOrdering::msb; }

public:
  StimCircuitSimulator() : ref_sample(0), randomEngine(std::random_device{}()) {
    // Populate the correct name so it is printed correctly during
    // deconstructor.
    summaryData.name = name();
  }
  virtual ~StimCircuitSimulator() = default;

  void setRandomSeed(std::size_t seed) override {
    randomEngine = std::mt19937_64(seed);
  }

  bool canHandleObserve() override { return false; }

  /// @brief Reset the qubit
  /// @param index 0-based index of qubit to reset
  void resetQubit(const std::size_t index) override {
    flushGateQueue();
    safe_append_u(
        "R", std::vector<std::uint32_t>{static_cast<std::uint32_t>(index)});
  }

  /// @brief Sample the multi-qubit state.
  cudaq::ExecutionResult sample(const std::vector<std::size_t> &qubits,
                                const int shots) override {
    std::vector<std::uint32_t> stimTargetQubits(qubits.begin(), qubits.end());

    if (!simulator) {
      cudaq::info("BMH Allocating new simulator");
      simulator =
          std::make_unique<stim::TableauSimulator<stim::MAX_BITWORD_WIDTH>>(
              std::move(randomEngine));
      ref_sample = stim::simd_bits<stim::MAX_BITWORD_WIDTH>(0);
      // randomEngine = std::mt19937_64();
    }

    randomEngine = std::mt19937_64(std::random_device{}());

    if (midCircuit)
      safe_append_u("M", stimTargetQubits);
      // partialCircuit.safe_append_u("M", stimTargetQubits);
    else
      fullStimCircuit.safe_append_u("M", stimTargetQubits);

    if (cudaq::details::should_log(cudaq::details::LogLevel::trace)) {
      std::stringstream ss;
      ss << fullStimCircuit << '\n';
      cudaq::details::trace(fmt::format("Stim circuit is\n{}", ss.str()));
    }

    // simulator->measurement_record.storage.clear();
    if (midCircuit) {
      simulator->safe_do_circuit(partialCircuit);
      const std::vector<bool> &v = simulator->measurement_record.storage;
      // ref_sample = stim::simd_bits<stim::MAX_BITWORD_WIDTH>(v.size());
      // for (size_t k = 0; k < v.size(); k++)
      //   ref_sample[k] = v[k];
      auto qubitNum = *stimTargetQubits.crbegin();
      auto measResult = *v.crbegin();
      cudaq::info("BMH postselect_z v.size {} qubit {} value {}", v.size(), qubitNum, measResult);
      simulator->postselect_z(
          std::vector<stim::GateTarget>{stim::GateTarget::qubit(qubitNum)},
          measResult);
      partialCircuit.clear();
      std::string shotResults(measResult ? "1" : "0");
      CountsDictionary counts;
      counts.insert({shotResults, 1});
      ExecutionResult execResult(counts);
      execResult.sequentialData.push_back(std::move(shotResults));
      return execResult;
    } /*else if (ref_sample.num_simd_words == 0) {
      exit(0); // BMH shouldn't get here for now, fix later
      ref_sample = stim::TableauSimulator<
          stim::MAX_BITWORD_WIDTH>::reference_sample_circuit(fullStimCircuit);
    }*/
    // safe_append_u("M", stimTargetQubits);

    const std::vector<bool> &v = simulator->measurement_record.storage;
    ref_sample = stim::simd_bits<stim::MAX_BITWORD_WIDTH>(v.size());
    for (size_t k = 0; k < v.size(); k++)
      ref_sample[k] = v[k];

    // FIXME - how do I get a Tableau into a Circuit?
    auto newCircuit = tableau_to_circuit_elimination_method(simulator->inv_state.inverse());
    newCircuit.safe_append_u("M", stimTargetQubits);
    cudaq::info("BMH newCircuit is {}", newCircuit.str());
    cudaq::info("BMH newCircuit2 is {}", tableau_to_circuit_elimination_method(simulator->inv_state).str());
    cudaq::info("BMH fullStimCircuit is {}", fullStimCircuit.str());
    auto ref_sample = stim::TableauSimulator<
        stim::MAX_BITWORD_WIDTH>::reference_sample_circuit(newCircuit);
    stim::simd_bit_table<stim::MAX_BITWORD_WIDTH> sample =
        stim::sample_batch_measurements(newCircuit, ref_sample, shots,
                                        randomEngine, false);
    size_t bits_per_sample = newCircuit.count_measurements();

    // randomEngine = std::mt19937_64(std::random_device{}());
    // // auto ref_sample = stim::TableauSimulator<
    // //     stim::MAX_BITWORD_WIDTH>::reference_sample_circuit(fullStimCircuit);
    // stim::simd_bit_table<stim::MAX_BITWORD_WIDTH> sample =
    //     stim::sample_batch_measurements(fullStimCircuit, ref_sample, shots,
    //                                     randomEngine, false);
    // size_t bits_per_sample = fullStimCircuit.count_measurements();
    std::vector<std::string> sequentialData;
    // Only retain the final "qubits.size()" measurements. All other
    // measurements were mid-circuit measurements that have been previously
    // accounted for and saved.
    assert(bits_per_sample >= qubits.size());
    std::size_t first_bit_to_save = bits_per_sample - qubits.size();
    CountsDictionary counts;
    for (std::size_t shot = 0; shot < shots; shot++) {
      std::string aShot(qubits.size(), '0');
      for (std::size_t b = first_bit_to_save; b < bits_per_sample; b++) {
        aShot[b - first_bit_to_save] = sample[b][shot] ? '1' : '0';
      }
      counts[aShot]++;
      if (shots == 1 && cudaq::details::should_log(cudaq::details::LogLevel::trace)) {
        cudaq::info("BMH qubits {} are {}", stimTargetQubits, aShot);
      }
      sequentialData.push_back(std::move(aShot));
    }
    ExecutionResult result(counts);
    result.sequentialData = std::move(sequentialData);
    return result;
  }

  bool isStateVectorSimulator() const override { return false; }

  std::string name() const override { return "stim"; }
  NVQIR_SIMULATOR_CLONE_IMPL(StimCircuitSimulator)
};

} // namespace nvqir

#ifndef __NVQIR_QPP_TOGGLE_CREATE
/// Register this Simulator with NVQIR.
NVQIR_REGISTER_SIMULATOR(nvqir::StimCircuitSimulator, stim)
#endif
