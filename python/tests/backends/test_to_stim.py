# ============================================================================ #
# Copyright (c) 2025 NVIDIA Corporation & Affiliates.                          #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import cudaq
import pytest


@pytest.fixture(scope="session", autouse=True)
def setTarget():
    old_target = cudaq.get_target()
    cudaq.set_target('stim')
    yield
    cudaq.set_target(old_target)


def test_basic():

    @cudaq.kernel
    def mykernel():
        q = cudaq.qubit()
        r = cudaq.qubit()
        mz(q)
        mz(r)
        detector(-2, -1)
        mz(q)
        mz(r)
        detector(-2, -1)

    stim_circuit_str = cudaq.to_stim(mykernel)
    assert stim_circuit_str == "M 0\nM 1\nDETECTOR rec[-2] rec[-1]\nM 0\nM 1\nDETECTOR rec[-2] rec[-1]\n"

def test_with_noise():
    @cudaq.kernel
    def with_noise():
        q = cudaq.qubit()
        r = cudaq.qubit()
        cudaq.apply_noise(cudaq.XError, 0.1, q)
        mz(q)
        mz(r)
        detector(-2, -1)
        mz(q)
        mz(r)
        detector(-2, -1)

    noise_model = cudaq.NoiseModel()
    stim_circuit_str = cudaq.to_stim(with_noise, noise_model=noise_model)
    assert stim_circuit_str == "X_ERROR(0.1) 0\nM 0\nM 1\nDETECTOR rec[-2] rec[-1]\nM 0\nM 1\nDETECTOR rec[-2] rec[-1]\n"

def test_with_noise_and_arguments():
    @cudaq.kernel
    def with_noise_and_arguments(p : float):
        q = cudaq.qubit()
        r = cudaq.qubit()
        cudaq.apply_noise(cudaq.XError, p, q)
        mz(q)
        mz(r)
        detector(-2, -1)
        mz(q)
        mz(r)
        detector(-2, -1)

    noise_model = cudaq.NoiseModel()
    stim_circuit_str = cudaq.to_stim(with_noise_and_arguments, 0.25, noise_model=noise_model)
    assert stim_circuit_str == "X_ERROR(0.25) 0\nM 0\nM 1\nDETECTOR rec[-2] rec[-1]\nM 0\nM 1\nDETECTOR rec[-2] rec[-1]\n"
