/****************************************************************-*- C++ -*-****
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#pragma once

#include "MeasureCounts.h"
#include "cudaq/operators.h"

#include <cassert>

namespace cudaq {

/// @brief The observe_result encapsulates all data generated from a
/// cudaq"::"observe call. This includes all measurement counts from the
/// execution of each ansatz + measure circuit, and the global expected
/// value of the spin_op passed to observe.
class observe_result {
protected:
  // The global expectation for the given spinOp
  double expVal = 0.0;

  // The spin_op measured
  spin_op spinOp;

  // Contains ExecutionResults for all terms measured
  sample_result data;

public:
  observe_result() 
  : spinOp(cudaq::spin_op::empty()){}

  /// @brief Constructor, takes the precomputed expectation value for
  /// <psi(x) | H | psi(x)> for the given spin_op.
  observe_result(double e, const spin_op &H) : expVal(e), spinOp(H) {}

  /// @brief Constructor, takes the precomputed expectation value for
  /// <psi(x) | H | psi(x)> for the given spin_op. If this execution
  /// was shots based, also provide the sample_result data containing counts
  /// for each term in H.
  observe_result(double e, const spin_op &H, sample_result counts)
      : expVal(e), spinOp(H), data(counts) {}

  /// @brief Return the raw counts data for all terms
  /// @return
  sample_result raw_data() { return data; };

  /// @brief Conversion operator for this `observe_result` to `double`. Simply
  /// returns the precomputed expectation value for the given spin_op. This
  /// enables one to ignore the fine-grain sample_result data, and explicitly
  /// request the expected value: double exp = cudaq"::"observe(...); as opposed
  /// to cudaq"::"observe_data data = cudaq::observe(...); auto exp =
  /// data.expectation();
  operator double() { return expVal; }

  /// @brief Return the expected value for the provided spin_op
  /// @return
  double expectation() { return expVal; }

  /// @brief Return the expectation value for a sub-term in the provided
  /// spin_op.
  template <typename SpinOpType>
  double expectation(SpinOpType term) {
    static_assert(std::is_same_v<spin_op_term, std::remove_reference_t<SpinOpType>>,
                  "Must provide a spin_op_term");
    // Pauli == Pauli II..III
    // e.g. someone might check for <Z>, which
    // on more than 1 qubit can be <ZIII...III>
    auto numQubits = spinOp.num_qubits();
    auto termStr = term.to_string(false);
    // FIXME: MAKE THIS CLEANER
    // FIXME: THE PROBLEM IS THAT IF WE HAVE AN EXPLICIT PRODUCT WITH I, 
    // THEN THE TO_STRING(FALSE) WILL STILL PRODUCT IT AND IT WILL BE STORED ACCORDINGLY
    // -> SO WE MAY HAVE ANY NUMBER OF I'S APPENDED...
    while (termStr.size() > 0 && termStr[termStr.size() - 1] == 'I')
      termStr.pop_back();
    // FIXME: THIS LOGIC DOESN'T SEE SOUND...
    // Expand the string representation of the term to match the number of
    // qubits of the overall spin_op this result represents by appending
    // identity ops.
    if (!data.has_expectation(termStr))
      for (std::size_t i = termStr.size(); i < numQubits; i++)
        termStr += "I";
    return data.expectation(termStr);
  }

  /// @brief Return the counts data for the given spin_op
  /// @param term
  /// @return
  template <typename SpinOpType>
  sample_result counts(SpinOpType term) {
    static_assert(std::is_same_v<spin_op_term, std::remove_reference_t<SpinOpType>>,
                  "Must provide a spin_op_term");
    auto numQubits = spinOp.num_qubits();
    auto termStr = term.to_string(false);
    if (!data.has_expectation(termStr) && termStr.size() == 1 && numQubits > 1)
      for (std::size_t i = 1; i < numQubits; i++)
        termStr += "I";
    auto counts = data.to_map(termStr);
    ExecutionResult result(counts);
    return sample_result(result);
  }

  /// @brief Return the coefficient of the identity term.
  /// Assumes there is at more one identity term. 
  /// Returns 0 if no identity term exists.
  double id_coefficient() {
    auto terms = spinOp.get_terms();
    for (const auto &term : terms)
      if (term.is_identity())
        return term.get_coefficient().evaluate().real();
    return 0.0;
  }

  spin_op get_spin() { return spinOp; }

  /// @brief Dump the counts data to standard out.
  void dump() { data.dump(); }
};

} // namespace cudaq
