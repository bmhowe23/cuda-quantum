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
  static constexpr std::size_t W = stim::MAX_BITWORD_WIDTH;
  std::size_t num_measurements = 0;
  std::mt19937_64 randomEngine;
  std::unique_ptr<stim::TableauSimulator<W>> tableau;
  std::unique_ptr<stim::FrameSimulator<W>> midCircuitSim;
  std::unique_ptr<stim::FrameSimulator<W>> sampleSim;

  /// @brief Grow the state vector by one qubit.
  void addQubitToState() override { addQubitsToState(1); }

  /// @brief Override the default sized allocation of qubits
  /// here to be a bit more efficient than the default implementation
  void addQubitsToState(std::size_t qubitCount,
                        const void *stateDataIn = nullptr) override {
    if (stateDataIn)
      throw std::runtime_error("The Stim simulator does not support "
                               "initialization of qubits from state data.");

    if (!tableau) {
      cudaq::info("BMH Allocating new tableau simulator");
      randomEngine.discard(
          std::uniform_int_distribution<int>(1, 30)(randomEngine));
      tableau =
          std::make_unique<stim::TableauSimulator<W>>(
            std::mt19937_64(randomEngine), /*num_qubits=*/0, /*sign_bias=*/+0);
    }
    if (!midCircuitSim) {
      cudaq::info("BMH Allocating new midCircuitSim simulator");
      randomEngine.discard(
          std::uniform_int_distribution<int>(1, 30)(randomEngine));
      midCircuitSim =
          std::make_unique<stim::FrameSimulator<W>>(
              stim::CircuitStats(),
              stim::FrameSimulatorMode::STORE_MEASUREMENTS_TO_MEMORY,
              /*batch_size=*/1, std::mt19937_64(randomEngine));
      // midCircuitSim->guarantee_anticommutation_via_frame_randomization = false;
      midCircuitSim->reset_all();
    }
    if (!sampleSim) {
      cudaq::info("BMH Allocating new sampleSim simulator");
      randomEngine.discard(
          std::uniform_int_distribution<int>(1, 30)(randomEngine));
      sampleSim =
          std::make_unique<stim::FrameSimulator<W>>(
              stim::CircuitStats(),
              stim::FrameSimulatorMode::STORE_MEASUREMENTS_TO_MEMORY,
              /*batch_size=*/10000, std::mt19937_64(randomEngine));
      // sampleSim->guarantee_anticommutation_via_frame_randomization = false;
      sampleSim->reset_all();
    }
  }

  /// @brief Reset the qubit state.
  void deallocateStateImpl() override {
    tableau.reset();
    midCircuitSim.reset();
    // Update the randomEngine so that future invocations will be different.
    if (sampleSim)
      randomEngine = std::move(sampleSim->rng);
    sampleSim.reset();
    num_measurements = 0;
  }

  // void safe_append_ua(const std::string &gate_name,
  //                     const std::vector<uint32_t> &targets,
  //                     double singleton_arg) {
  //   stim::Circuit newCircuit;
  //   newCircuit.safe_append_ua(gate_name, targets, singleton_arg);
  //   tableau->safe_do_circuit(newCircuit);
  //   frameSim->safe_do_circuit(newCircuit);
  // }

  void safe_append_u(const std::string &gate_name,
                     const std::vector<uint32_t> &targets) {
    stim::Circuit newCircuit;
    cudaq::info("Calling safe_append_u {} - {}", gate_name, targets);
    newCircuit.safe_append_u(gate_name, targets);
    tableau->safe_do_circuit(newCircuit);
    midCircuitSim->safe_do_circuit(newCircuit);
    sampleSim->safe_do_circuit(newCircuit);
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

    stim::Circuit noiseCircuit;
    for (auto &channel : krausChannels) {
      if (channel.noise_type == cudaq::noise_model_type::bit_flip_channel)
        noiseCircuit.safe_append_ua("X_ERROR", stimTargets, channel.parameters[0]);
      else if (channel.noise_type ==
               cudaq::noise_model_type::phase_flip_channel)
        noiseCircuit.safe_append_ua("Z_ERROR", stimTargets, channel.parameters[0]);
      else if (channel.noise_type ==
               cudaq::noise_model_type::depolarization_channel)
        noiseCircuit.safe_append_ua("DEPOLARIZE1", stimTargets, channel.parameters[0]);
    }
    midCircuitSim->safe_do_circuit(noiseCircuit);
    sampleSim->safe_do_circuit(noiseCircuit);
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
    stim::Circuit tempCircuit;
    std::vector<std::uint32_t> indexAsVec{static_cast<std::uint32_t>(index)};
    // tempCircuit.safe_append_u(
    //     "M", std::vector<std::uint32_t>{static_cast<std::uint32_t>(index)});

    // Before we measure, find out if the Tableau says the measurement is
    // deterministic.
    bool tableauDeterministic = false;
    int8_t peekVal = tableau->peek_z(index);
    if (tableau->is_deterministic_z(index))
      tableauDeterministic = true;

    // Collapse the tableau
    safe_append_u("M", indexAsVec);
    num_measurements++;

    // Get the tableau bit that was just generated.
    const std::vector<bool> &v = tableau->measurement_record.storage;
    const bool tableauBit = *v.crbegin();
    cudaq::info("v.size is {} and contents are {}", v.size(), v);
    stim::simd_bits<W> ref(v.size());
    for (size_t k = 0; k < v.size(); k++)
      ref[k] ^= v[k];

    // Get the mid-circuit sample to be XOR-ed with tableauBit.
    // FIXME was midCiruitSim
    stim::simd_bit_table<W> sample = sampleSim->m_record.storage;
    auto nShots = sampleSim->batch_size;
    if (ref.not_zero()) {
      sample = stim::transposed_vs_ref(nShots, sample, ref);
      sample = sample.transposed();
    }

    // bool result = tableauBit ^ static_cast<bool>(sample[num_measurements-1][/*shot=*/0]);
    bool result = sample[num_measurements-1][/*shot=*/0];
    if (tableauDeterministic &&
        ((result && peekVal < 0) || (!result && peekVal > 0))) {
      // Noise must have corrupted this measurement, so running
      // postselect_z(desired_val=result) would cause bad results.
      // TODO - verify and handle accordingly?
      // printf("FIXME %s:%d\n", __FILE__, __LINE__);
    } else {
      tableau->postselect_z(std::vector<stim::GateTarget>{index}, result);
    }

    return result;
  }

  QubitOrdering getQubitOrdering() const override { return QubitOrdering::msb; }

public:
  StimCircuitSimulator() : randomEngine(std::random_device{}()) {
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
    assert(shots <= 10000);
    std::vector<std::uint32_t> stimTargetQubits(qubits.begin(), qubits.end());
    safe_append_u("M", stimTargetQubits);
    num_measurements += stimTargetQubits.size();
    
    // Generate a reference sample
    const std::vector<bool> &v = tableau->measurement_record.storage;
    stim::simd_bits<W> ref(v.size());
    cudaq::info("v.size is {} and contents are {}", v.size(), v);
    for (size_t k = 0; k < v.size(); k++)
      ref[k] ^= v[k];

    // Now XOR results on a per-shot basis
    stim::simd_bit_table<W> sample = sampleSim->m_record.storage;
    cudaq::info("sample shots = {}", sampleSim->m_record.num_shots);
    auto nShots = sampleSim->batch_size;
    cudaq::info("batch size = {}", nShots);
    if (ref.not_zero()) {
      sample = stim::transposed_vs_ref(nShots, sample, ref);
      sample = sample.transposed();
    }

    size_t bits_per_sample = num_measurements;
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
        aShot[b - first_bit_to_save] = static_cast<bool>(sample[b][shot]) ? '1' : '0';
      }
      std::string debugString(bits_per_sample, '0');
      for (std::size_t b = 0; b < bits_per_sample; b++)
        debugString[b] = static_cast<bool>(sample[b][shot]) ? '1' : '0';
      cudaq::info("BMH shot {} debugString {}", shot, debugString);
      counts[aShot]++;
      sequentialData.push_back(std::move(aShot));
    }
    ExecutionResult result(counts);
    result.sequentialData = std::move(sequentialData);
    return result;

    // Run what we've accumulated, but don't include any measurements yet.
    // cudaq::info("BMH partialCircuit is {}", partialCircuit.str());
    // tableau->safe_do_circuit(partialCircuit);
    // partialCircuit.clear();
    // auto newCircuit = tableau_to_circuit_elimination_method(tableau->inv_state.inverse());
    // cudaq::info("BMH newCircuit is {}", newCircuit.str());

    // randomEngine = std::mt19937_64(std::random_device{}());

    // if (midCircuit)
    //   safe_append_u("M", stimTargetQubits);
    //   // partialCircuit.safe_append_u("M", stimTargetQubits);
    // else
    //   fullStimCircuit.safe_append_u("M", stimTargetQubits);

    // safe_append_u("M", stimTargetQubits);
    // partialCircuit.safe_append_u("M", stimTargetQubits);
    // cudaq::info("BMH calling safe_do_circuit on {}", partialCircuit.str());
    // tableau->safe_do_circuit(partialCircuit);
    // partialCircuit.clear();

    // if (cudaq::details::should_log(cudaq::details::LogLevel::trace)) {
    //   std::stringstream ss;
    //   ss << fullStimCircuit << '\n';
    //   cudaq::details::trace(fmt::format("Stim circuit is\n{}", ss.str()));
    // }

    // tableau->measurement_record.storage.clear();
    // if (midCircuit) {
    //   // The new way of midcircuit measurements is to peek at one of the results (not sure which sim), postselect accordingly

    //   // partialCircuit.safe_append_u("M", stimTargetQubits);
    //   tableau->safe_do_circuit(partialCircuit);
    //   partialCircuit.clear();
    //   const std::vector<bool> &v = tableau->measurement_record.storage;
    //   // ref_sample = stim::simd_bits<W>(v.size());
    //   // for (size_t k = 0; k < v.size(); k++)
    //   //   ref_sample[k] = v[k];
    //   auto qubitNum = *stimTargetQubits.crbegin();
    //   auto measResult = *v.crbegin();
    //   cudaq::info("BMH postselect_z v.size {} qubit {} value {}", v.size(), qubitNum, measResult);
    //   tableau->postselect_z(
    //       std::vector<stim::GateTarget>{stim::GateTarget::qubit(qubitNum)},
    //       measResult);
    //   // partialCircuit.clear();
    //   std::string shotResults(measResult ? "1" : "0");
    //   CountsDictionary counts;
    //   counts.insert({shotResults, 1});
    //   ExecutionResult execResult(counts);
    //   execResult.sequentialData.push_back(std::move(shotResults));
    //   return execResult;
    // } /*else if (ref_sample.num_simd_words == 0) {
    //   exit(0); // BMH shouldn't get here for now, fix later
    //   ref_sample = stim::TableauSimulator<
    //       W>::reference_sample_circuit(fullStimCircuit);
    // }*/
    // // safe_append_u("M", stimTargetQubits);

    // const std::vector<bool> &v = tableau->measurement_record.storage;
    // ref_sample = stim::simd_bits<W>(v.size());
    // for (size_t k = 0; k < v.size(); k++)
    //   ref_sample[k] = v[k];

    // // FIXME - how do I get a Tableau into a Circuit?
    // // auto newCircuit = tableau_to_circuit_elimination_method(tableau->inv_state.inverse());
    // newCircuit.safe_append_u("M", stimTargetQubits);
    // cudaq::info("BMH newCircuit is {}", newCircuit.str());
    // cudaq::info("BMH newCircuit2 is {}", tableau_to_circuit_elimination_method(tableau->inv_state).str());
    // cudaq::info("BMH fullStimCircuit is {}", fullStimCircuit.str());

    // // This works but seems to lose the noisy errors because newCircuit doesn't
    // // have things like X_ERROR in it. :(
    // auto ref_sample = stim::TableauSimulator<
    //     W>::reference_sample_circuit(newCircuit);
    // auto sample = std::move(frameSim->m_record.storage);

    // // stim::simd_bit_table<W> sample =
    // //     stim::sample_batch_measurements(newCircuit, ref_sample, shots,
    // //                                     randomEngine, false);
    // size_t bits_per_sample = newCircuit.count_measurements();

    // // randomEngine = std::mt19937_64(std::random_device{}());
    // // // auto ref_sample = stim::TableauSimulator<
    // // //     W>::reference_sample_circuit(fullStimCircuit);
    // // stim::simd_bit_table<W> sample =
    // //     stim::sample_batch_measurements(fullStimCircuit, ref_sample, shots,
    // //                                     randomEngine, false);
    // // size_t bits_per_sample = fullStimCircuit.count_measurements();
    // std::vector<std::string> sequentialData;
    // // Only retain the final "qubits.size()" measurements. All other
    // // measurements were mid-circuit measurements that have been previously
    // // accounted for and saved.
    // assert(bits_per_sample >= qubits.size());
    // std::size_t first_bit_to_save = bits_per_sample - qubits.size();
    // CountsDictionary counts;
    // for (std::size_t shot = 0; shot < shots; shot++) {
    //   std::string aShot(qubits.size(), '0');
    //   for (std::size_t b = first_bit_to_save; b < bits_per_sample; b++) {
    //     aShot[b - first_bit_to_save] = sample[b][shot] ? '1' : '0';
    //   }
    //   counts[aShot]++;
    //   if (shots == 1 && cudaq::details::should_log(cudaq::details::LogLevel::trace)) {
    //     cudaq::info("BMH qubits {} are {}", stimTargetQubits, aShot);
    //   }
    //   sequentialData.push_back(std::move(aShot));
    // }
    // ExecutionResult result(counts);
    // result.sequentialData = std::move(sequentialData);
    // return result;
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
