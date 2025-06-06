# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import cudaq, pytest, os
from cudaq import spin
import numpy as np
from typing import List
from multiprocessing import Process
from network_utils import check_server_connection
try:
    from utils.mock_qpu.ionq import startServer
except:
    print("Mock qpu not available, skipping IonQ tests.")
    pytest.skip("Mock qpu not available.", allow_module_level=True)

# Define the port for the mock server
port = 62441


def assert_close(got) -> bool:
    return got < -1.5 and got > -1.9


@pytest.fixture(scope="session", autouse=True)
def startUpMockServer():
    os.environ["IONQ_API_KEY"] = "00000000000000000000000000000000"

    # Set the targeted QPU
    cudaq.set_target("ionq", url="http://localhost:{}".format(port))

    # Launch the Mock Server
    p = Process(target=startServer, args=(port,))
    p.start()

    if not check_server_connection(port):
        p.terminate()
        pytest.exit("Mock server did not start in time, skipping tests.",
                    returncode=1)

    yield "Server started."

    # Kill the server
    p.terminate()


@pytest.fixture(scope="function", autouse=True)
def configureTarget():

    # Set the targeted QPU
    cudaq.set_target("ionq", url="http://localhost:{}".format(port))
    yield "Running the test."
    cudaq.reset_target()


def test_ionq_sample():
    # Create the kernel we'd like to execute on IonQ
    kernel = cudaq.make_kernel()
    qubits = kernel.qalloc(2)
    kernel.h(qubits[0])
    kernel.cx(qubits[0], qubits[1])
    kernel.mz(qubits)
    print(kernel)

    # Run sample synchronously, this is fine
    # here in testing since we are targeting a mock
    # server. In reality you'd probably not want to
    # do this with the remote job queue.
    counts = cudaq.sample(kernel)
    assert len(counts) == 2
    assert "00" in counts
    assert "11" in counts

    # Run sample, but do so asynchronously. This enters
    # the execution job into the remote IonQ job queue.
    future = cudaq.sample_async(kernel)
    # We could go do other work, but since this
    # is a mock server, get the result
    counts = future.get()
    assert len(counts) == 2
    assert "00" in counts
    assert "11" in counts

    # Ok now this is the most likely scenario, launch the
    # job asynchronously, this puts it in the queue, now
    # you can take the future and persist it to file for later.
    future = cudaq.sample_async(kernel)
    print(future)

    # Persist the future to a file (or here a string,
    # could write this string to file for later)
    futureAsString = str(future)

    # Later you can come back and read it in and get
    # the results, which are now present because the job
    # made it through the queue
    futureReadIn = cudaq.AsyncSampleResult(futureAsString)
    counts = futureReadIn.get()
    assert len(counts) == 2
    assert "00" in counts
    assert "11" in counts


def test_ionq_observe():
    # Create the parameterized ansatz
    kernel, theta = cudaq.make_kernel(float)
    qreg = kernel.qalloc(2)
    kernel.x(qreg[0])
    kernel.ry(theta, qreg[1])
    kernel.cx(qreg[1], qreg[0])

    # Define its spin Hamiltonian.
    hamiltonian = (5.907 - 2.1433 * spin.x(0) * spin.x(1) -
                   2.1433 * spin.y(0) * spin.y(1) + 0.21829 * spin.z(0) -
                   6.125 * spin.z(1))

    # Run the observe task on IonQ synchronously
    res = cudaq.observe(kernel, hamiltonian, 0.59)
    assert assert_close(res.expectation())

    # Launch it asynchronously, enters the job into the queue
    future = cudaq.observe_async(kernel, hamiltonian, 0.59)
    # Retrieve the results (since we're on a mock server)
    res = future.get()
    assert assert_close(res.expectation())

    # Launch the job async, job goes in the queue, and
    # we're free to dump the future to file
    future = cudaq.observe_async(kernel, hamiltonian, 0.59)
    print(future)
    futureAsString = str(future)

    # Later you can come back and read it in
    # You must provide the spin_op so we can reconstruct
    # the results from the term job ids.
    futureReadIn = cudaq.AsyncObserveResult(futureAsString, hamiltonian)
    res = futureReadIn.get()
    assert assert_close(res.expectation())


def test_ionq_u3_decomposition():

    @cudaq.kernel
    def kernel():
        qubit = cudaq.qubit()
        u3(0.0, np.pi / 2, np.pi, qubit)

    result = cudaq.sample(kernel)


def test_ionq_u3_ctrl_decomposition():

    @cudaq.kernel
    def kernel():
        control = cudaq.qubit()
        target = cudaq.qubit()
        u3.ctrl(0.0, np.pi / 2, np.pi, control, target)

    result = cudaq.sample(kernel)


def test_ionq_state_preparation():

    @cudaq.kernel
    def kernel(vec: List[complex]):
        qubits = cudaq.qvector(vec)

    state = [1. / np.sqrt(2.), 1. / np.sqrt(2.), 0., 0.]
    counts = cudaq.sample(kernel, state)
    assert '00' in counts
    assert '10' in counts
    assert not '01' in counts
    assert not '11' in counts


def test_ionq_state_preparation_builder():
    kernel, state = cudaq.make_kernel(List[complex])
    qubits = kernel.qalloc(state)

    state = [1. / np.sqrt(2.), 1. / np.sqrt(2.), 0., 0.]
    counts = cudaq.sample(kernel, state)
    assert '00' in counts
    assert '10' in counts
    assert not '01' in counts
    assert not '11' in counts


def test_ionq_state_synthesis_from_simulator():

    @cudaq.kernel
    def kernel(state: cudaq.State):
        qubits = cudaq.qvector(state)

    state = cudaq.State.from_data(
        np.array([1. / np.sqrt(2.), 1. / np.sqrt(2.), 0., 0.], dtype=complex))

    counts = cudaq.sample(kernel, state)
    assert "00" in counts
    assert "10" in counts
    assert len(counts) == 2

    synthesized = cudaq.synthesize(kernel, state)
    counts = cudaq.sample(synthesized)
    assert '00' in counts
    assert '10' in counts
    assert len(counts) == 2


def test_ionq_state_synthesis_from_simulator_builder():

    kernel, state = cudaq.make_kernel(cudaq.State)
    qubits = kernel.qalloc(state)

    state = cudaq.State.from_data(
        np.array([1. / np.sqrt(2.), 1. / np.sqrt(2.), 0., 0.], dtype=complex))

    counts = cudaq.sample(kernel, state)
    assert "00" in counts
    assert "10" in counts
    assert len(counts) == 2


def test_Ionq_state_synthesis():

    @cudaq.kernel
    def init(n: int):
        q = cudaq.qvector(n)
        x(q[0])

    @cudaq.kernel
    def kernel(s: cudaq.State):
        q = cudaq.qvector(s)
        x(q[1])

    s = cudaq.get_state(init, 2)
    s = cudaq.get_state(kernel, s)
    counts = cudaq.sample(kernel, s)
    assert '10' in counts
    assert len(counts) == 1


def test_Ionq_state_synthesis_builder():

    init, n = cudaq.make_kernel(int)
    qubits = init.qalloc(n)
    init.x(qubits[0])

    s = cudaq.get_state(init, 2)

    kernel, state = cudaq.make_kernel(cudaq.State)
    qubits = kernel.qalloc(state)
    kernel.x(qubits[1])

    s = cudaq.get_state(kernel, s)
    counts = cudaq.sample(kernel, s)
    assert '10' in counts
    assert len(counts) == 1


def test_exp_pauli():

    @cudaq.kernel
    def test():
        q = cudaq.qvector(2)
        exp_pauli(1.0, q, "XX")

    counts = cudaq.sample(test)
    print(test, counts)
    assert '00' in counts
    assert '11' in counts
    assert not '01' in counts
    assert not '10' in counts


def test_1q_unitary_synthesis():

    cudaq.register_operation("custom_h",
                             1. / np.sqrt(2.) * np.array([1, 1, 1, -1]))
    cudaq.register_operation("custom_x", np.array([0, 1, 1, 0]))

    @cudaq.kernel
    def basic_x():
        qubit = cudaq.qubit()
        custom_x(qubit)

    counts = cudaq.sample(basic_x)
    counts.dump()
    assert len(counts) == 1 and "1" in counts

    @cudaq.kernel
    def basic_h():
        qubit = cudaq.qubit()
        custom_h(qubit)

    counts = cudaq.sample(basic_h)
    counts.dump()
    assert "0" in counts and "1" in counts

    @cudaq.kernel
    def bell():
        qubits = cudaq.qvector(2)
        custom_h(qubits[0])
        custom_x.ctrl(qubits[0], qubits[1])

    counts = cudaq.sample(bell)
    counts.dump()
    assert len(counts) == 2
    assert "00" in counts and "11" in counts

    cudaq.register_operation("custom_s", np.array([1, 0, 0, 1j]))
    cudaq.register_operation("custom_s_adj", np.array([1, 0, 0, -1j]))

    @cudaq.kernel
    def kernel():
        q = cudaq.qubit()
        h(q)
        custom_s.adj(q)
        custom_s_adj(q)
        h(q)

    counts = cudaq.sample(kernel)
    counts.dump()
    assert counts["1"] == 1000


def test_2q_unitary_synthesis():

    cudaq.register_operation(
        "custom_cnot",
        np.array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0]))

    @cudaq.kernel
    def bell_pair():
        qubits = cudaq.qvector(2)
        h(qubits[0])
        custom_cnot(qubits[0], qubits[1])

    counts = cudaq.sample(bell_pair)
    assert len(counts) == 2
    assert "00" in counts and "11" in counts

    cudaq.register_operation(
        "custom_cz", np.array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0,
                               -1]))

    @cudaq.kernel
    def ctrl_z_kernel():
        qubits = cudaq.qvector(5)
        controls = cudaq.qvector(2)
        custom_cz(qubits[1], qubits[0])
        x(qubits[2])
        custom_cz(qubits[3], qubits[2])
        x(controls)

    counts = cudaq.sample(ctrl_z_kernel)
    assert counts["0010011"] == 1000


# leave for gdb debugging
if __name__ == "__main__":
    loc = os.path.abspath(__file__)
    pytest.main([loc, "-s"])
