# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import os
import llvmlite.binding as llvm
import ctypes
import time
from multiprocessing import Process, Queue


# Initialize LLVM
llvm.initialize()
llvm.initialize_native_target()
llvm.initialize_native_asmprinter()

def process_task(bitcode, results_queue):
    print("Hello")
    #mod = llvm.parse_bitcode(bitcode)
    print("+parse_assembly")
    mod = llvm.parse_assembly(bitcode)
    print("-parse_assembly")
    target = llvm.Target.from_default_triple()
    target_machine = target.create_target_machine()
    result = 0
    with llvm.create_mcjit_compiler(mod, target_machine) as mcjit:
        print("+finalize_object")
        mcjit.finalize_object()
        print("-finalize_object")
        print("+run_static_constructors")
        mcjit.run_static_constructors()
        print("-run_static_constructors")
        func_ptr = mcjit.get_function_address("myfunc")
        kernel = ctypes.CFUNCTYPE(ctypes.c_int)(func_ptr)
        print("+kernel2", func_ptr)
        result = kernel()
        print("-kernel2") # not getting here
    #results_queue.put(os.getpid())
    results_queue.put(result)

if __name__ == '__main__':
    results_queue = Queue()
    # Compile with:
    #    clang++ -fno-pic -fno-pie -Xclang -no-opaque-pointers bencpptest.cpp -S -emit-llvm -c -O0 -o bencpptest.ll
    with open("/workspaces/cuda-quantum/bencpptest.ll", "r") as f:
        bitcode = f.read()

    # Spawn two processes
    processes = []
    for _ in range(2):
        p = Process(target=process_task, args=(bitcode, results_queue))
        p.start()
        processes.append(p)

    # Wait for both processes to finish and collect results
    for p in processes:
        p.join()

    print("Done with join")
    # Retrieve results from the queue
    results = [results_queue.get() for _ in processes]
    print("Results:", results)