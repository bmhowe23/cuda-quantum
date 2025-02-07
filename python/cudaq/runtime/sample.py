# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
from ..mlir._mlir_libs._quakeDialects import cudaq_runtime
from .utils import __isBroadcast, __createArgumentSet


def __broadcastSample(kernel, *args, shots_count=0):
    """Implement broadcasting of a single sample call over an argument set."""
    argSet = __createArgumentSet(*args)
    N = len(argSet)
    results = []
    for i, a in enumerate(argSet):
        ctx = cudaq_runtime.ExecutionContext('sample', shots_count)
        ctx.totalIterations = N
        ctx.batchIteration = i
        cudaq_runtime.setExecutionContext(ctx)
        kernel(*a)
        res = ctx.result
        cudaq_runtime.resetExecutionContext()
        results.append(res)

    return results


def sample(kernel,
           *args,
           shots_count=1000,
           noise_model=None,
           explicit_measurements=False):
    """Sample the state generated by the provided `kernel` at the given kernel 
`arguments` over the specified number of circuit executions (`shots_count`). 
Each argument in `arguments` provided can be a list or `ndarray` of arguments  
of the specified kernel argument type, and in this case, the `sample` 
functionality will be broadcasted over all argument sets and a list of 
`sample_result` instances will be returned.

Args:
  kernel (:class:`Kernel`): The :class:`Kernel` to execute `shots_count`
    times on the QPU.
  *arguments (Optional[Any]): The concrete values to evaluate the kernel
    function at. Leave empty if the kernel doesn't accept any arguments. For 
    example, if the kernel takes two `float` values as input, the `sample` call 
    should be structured as `cudaq.sample(kernel, firstFloat, secondFloat)`. For 
    broadcasting of the `sample` function, the arguments should be structured as a 
    `list` or `ndarray` of argument values of the specified kernel argument type.
  shots_count (Optional[int]): The number of kernel executions on the QPU.
    Defaults to 1000. Key-word only.
  noise_model (Optional[`NoiseModel`]): The optional :class:`NoiseModel`
    to add noise to the kernel execution on the simulator. Defaults to
    an empty noise model.
  explicit_measurements (Optional[bool]): Whether or not to concatenate 
    measurements in execution order for the returned sample result.

Returns:
  :class:`SampleResult` or `list[SampleResult]`: 
  A dictionary containing the measurement count results for the :class:`Kernel`, 
  or a list of such results in the case of `sample` function broadcasting."""

    if noise_model != None:
        cudaq_runtime.set_noise(noise_model)

    if __isBroadcast(kernel, *args):
        res = __broadcastSample(kernel, *args, shots_count=shots_count)
        cudaq_runtime.unset_noise()
        return res

    ctx = cudaq_runtime.ExecutionContext("sample", shots_count)
    cudaq_runtime.setExecutionContext(ctx)

    ctx.explicitMeasurements = explicit_measurements
    ## TODO: If using metadata to check if explicit measurement allowed or not,
    #        check it here and exit early if not supported

    if hasattr(kernel, 'metadata') and kernel.metadata.get(
            'conditionalOnMeasure', False):
        ctx.hasConditionalsOnMeasureResults = True

    platformSupportsConditionalFeedback = cudaq_runtime.supportsConditionalFeedback(
    )
    if ctx.hasConditionalsOnMeasureResults and not platformSupportsConditionalFeedback:
        counts = cudaq_runtime.SampleResult()
        for i in range(shots_count):
            kernel(*args)
            cudaq_runtime.resetExecutionContext()
            counts += ctx.result
            ctx.result.clear()
            if i < shots_count - 1:
                cudaq_runtime.setExecutionContext(ctx)
        return counts

    kernel(*args)
    res = ctx.result
    cudaq_runtime.resetExecutionContext()
    cudaq_runtime.unset_noise()
    return res
