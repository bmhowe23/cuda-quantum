# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

# from llvm import *
# from llvm.core import *
import cudaq
import os
import ctypes
import time
# from multiprocessing import Process, Queue
from mpi4py import MPI

# Initialize LLVM
# llvm.initialize()
# llvm.initialize_native_target()
# llvm.initialize_native_asmprinter()

# lib_path = "/usr/lib/x86_64-linux-gnu/libstdc++.so.6"
# libstdcpp = ctypes.CDLL(lib_path)

# def process_task(bitcode, results_queue):
#     print("Hello")
#     # mod = llvm.parse_assembly(bitcode)
#     mod = llvm.parse_bitcode(bitcode)
#     target = llvm.Target.from_default_triple()
#     target_machine = target.create_target_machine()

#     result = 0
#     with llvm.create_mcjit_compiler(mod, target_machine) as mcjit:
#         # llvm.add_library_path("/usr/lib/x86_64-linux-gnu")
#         # mcjit.add_dynamic_library("/usr/lib/x86_64-linux-gnu/libstdc++.so.6")
#         mcjit.finalize_object()
#         mcjit.run_static_constructors()
#         func_ptr = mcjit.get_function_address("myfunc")
#         kernel = ctypes.CFUNCTYPE(ctypes.c_int)(func_ptr)
#         print("+kernel2", func_ptr)
#         result = kernel()
#         print("-kernel2") # not getting here
#     #results_queue.put(os.getpid())
#     #results_queue.put(result)

# Run like this (assuming from "build" directory):
#   PYTHONPATH=python:$PYTHONPATH UCX_WARN_UNUSED_ENV_VARS=n mpirun -np 2 --allow-run-as-root python3 ../benpytest2.py 
if __name__ == '__main__':

    comm = MPI.COMM_WORLD
    rank = comm.Get_rank()
    print("rank = ", rank)

    if rank == 0:
        ctypes.CDLL("/workspaces/cuda-quantum/benlib1.so", mode=ctypes.RTLD_GLOBAL)
        # Compile with:
        #   clang++ bencpptest.cpp -emit-llvm -c -O0 -o bencpptest.bc
        with open("/workspaces/cuda-quantum/bencpptest.bc", "rb") as f:
            bitcode = f.read()
        for i in range(1, comm.Get_size()):
            comm.send(bitcode, dest=i, tag=11)
    else:
        ctypes.CDLL("/workspaces/cuda-quantum/benlib2.so", mode=ctypes.RTLD_GLOBAL)
        bitcode = comm.recv(source=0, tag=11)

    cudaq.parse_jit_and_run_bitcode(bitcode)
