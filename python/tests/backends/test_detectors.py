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

    dets = cudaq.detectors(mykernel)
    assert dets == [[0, 1], [2, 3]]


def test_detector_stdvec():

    @cudaq.kernel
    def mykernel():
        q = cudaq.qubit()
        r = cudaq.qubit()
        mz(q)
        mz(r)
        detector([-2, -1])
        mz(q)
        mz(r)
        detector([-2, -1])

    dets = cudaq.detectors(mykernel)
    assert dets == [[0, 1], [2, 3]]


def test_detector_stdvec_single_element():

    @cudaq.kernel
    def mykernel():
        q = cudaq.qubit()
        r = cudaq.qubit()
        mz(q)
        mz(r)
        detector([-2])

    dets = cudaq.detectors(mykernel)
    assert dets == [[0]]


def test_detector_loops():

    @cudaq.kernel
    def mykernel():
        q = cudaq.qubit()
        r = cudaq.qubit()
        mz(q)
        mz(r)
        for i in range(10):
            mz(q)
            mz(r)
            detector(-4, -2)
            detector(-3, -1)

    dets = cudaq.detectors(mykernel)
    assert len(dets) == 20
    for i in range(20):
        # 2 measurements per detector
        assert len(dets[i]) == 2
        # E.g. 0,2 / 1,3 / 4,6 / 5,7 / ...
        assert dets[i][0] == i
        assert dets[i][1] == i + 2


# leave for gdb debugging
if __name__ == "__main__":
    loc = os.path.abspath(__file__)
    pytest.main([loc, "-s"])
