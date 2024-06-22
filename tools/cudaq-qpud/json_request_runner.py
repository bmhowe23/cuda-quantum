# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import cudaq
import ast
import sys
import json
import subprocess
import importlib
from datetime import datetime


class ASTNodeVisitor(ast.NodeVisitor):

    def __init__(self):
        self.valid = True

    def visit_ClassDef(self, node):
        self.valid = False
        self.generic_visit(node)

    def visit_Name(self, node):
        # Check for invalid builtin functions
        if node.id in ('__import__', 'compile', 'dir', 'eval', 'exec',
                       'getattr', 'memoryview', 'open'):
            self.valid = False
        self.generic_visit(node)

    def visit_Import(self, node):
        self.valid = False
        self.generic_visit(node)

    def visit_ImportFrom(self, node):
        self.valid = False
        self.generic_visit(node)

    def visit_Attribute(self, node):
        # Check for invalid numpy functions
        if node.attr in ('fromfile', 'genfromtxt', 'open_mmap', 'load',
                         'loadtxt', 'memmap', 'open', 'save', 'savetxt',
                         'savez', 'savez_compressed'):
            self.valid = False
        # Check for invalid builtin functions
        if node.attr in ('__import__', 'compile', 'dir', 'eval', 'exec',
                         'memoryview', 'getattr', 'open'):
            self.valid = False
        # pathlib.Path
        if node.attr in ('read_text'):
            self.valid = False
        # Pandas (TODO -there are more)
        if node.attr in ('read_csv', 'read_excel', 'read_html', 'read_json',
                         'read_xml'):
            self.valid = False
        # mmap.mmap
        if node.attr in ('mmap'):
            self.valid = False
        self.generic_visit(node)


def get_deserialized_dict(scoped_dict):
    deserialized_dict = {}

    # If the scoped_dict is one big JSON string, then load it into a
    # dictionary-like object.
    if isinstance(scoped_dict, str):
        scoped_dict = json.loads(scoped_dict)

    for key, val in scoped_dict.items():
        try:
            if "/" in key:
                key, val_type = key.split('/')
                if val_type.startswith('cudaq.'):
                    module_name, type_name = val_type.rsplit('.', 1)
                    module = importlib.import_module(module_name)
                    type_class = getattr(module, type_name)
                    result = type_class.from_json(json.dumps(val))
                    deserialized_dict[key] = result
                else:
                    raise Exception(f'Invalid val_type in key: {val_type}')
            else:
                deserialized_dict[key] = val
        except Exception as e:
            raise Exception(f"Error deserializing key '{key}': {e}")

    return deserialized_dict


if __name__ == "__main__":
    try:
        # Read request
        if len(sys.argv) < 2:
            raise (Exception('Too few command-line arguments'))
        jsonFile = sys.argv[1]
        with open(jsonFile, 'rb') as fp:
            request = json.load(fp)

        serialized_ctx = request['serializedCodeExecutionContext']
        imports_code = serialized_ctx['imports']
        source_code = serialized_ctx['source_code']
        imports_code += '\nfrom typing import List, Tuple\n'

        # Be sure to do this before running any code from `serialized_ctx`
        globals_dict = get_deserialized_dict(serialized_ctx['scoped_var_dict'])

        # Determine which target to set
        sim2target = {
            'qpp': 'qpp-cpu',
            'custatevec_fp32': 'nvidia',
            'custatevec_fp64': 'nvidia-fp64',
            'tensornet': 'tensornet',
            'tensornet_mps': 'tensornet-mps',
            'dm': 'density-matrix-cpu',
            'nvidia_mgpu': 'nvidia-mgpu'
        }
        simulator_name = request['simulator']
        simulator_name = simulator_name.replace('-', '_')
        target_name = sim2target[simulator_name]

        # Validate the source_code.
        # TODO - imports should probably be handled differently.
        tree = ast.parse(source_code)
        visitor = ASTNodeVisitor()
        visitor.visit(tree)

        if not visitor.valid:
            raise Exception(f'Invalid source code discoverd by ASTNodeVisitor')

        full_source = f'{imports_code}\n{source_code}'

        # Execute imports
        exec(imports_code, globals_dict)

        # Perform setup
        exec(f'cudaq.set_target("{target_name}")', globals_dict)
        seed_num = int(request['seed'])
        if seed_num > 0:
            exec(f'cudaq.set_random_seed({seed_num})', globals_dict)

        # Initialize output dictionary
        result = {
            "status": "success",
            "executionContext": {
                "shots": 0,
                "hasConditionalsOnMeasureResults": False
            }
        }
        globals_dict['_json_request_result'] = result

        # Execute main source_code
        simulationStart = int(datetime.now().timestamp() * 1000)
        exec(source_code, globals_dict)
        simulationEnd = int(datetime.now().timestamp() * 1000)

        # Collect results
        result = globals_dict['_json_request_result']
        try:
            cmd_result = subprocess.run(['cudaq-qpud', '--cuda-properties'],
                                        capture_output=True,
                                        text=True)
            deviceProps = json.loads(cmd_result.stdout)
        except:
            deviceProps = dict()

        # We don't have visibility into the difference between `requestStart`
        # and `simulationStart`, so simply use `simulationStart` for both
        executionInfo = {
            'requestStart': simulationStart,
            'simulationStart': simulationStart,
            'simulationEnd': simulationEnd,
            'deviceProps': deviceProps
        }
        result['executionInfo'] = executionInfo

        # Only rank 0 prints the result
        if not (cudaq.mpi.is_initialized()) or (cudaq.mpi.rank() == 0):
            print('\n' + json.dumps(result))

    except Exception as e:
        result = {
            'status': 'Failed to process incoming request',
            'errorMessage': str(e)
        }
        # Only rank 0 prints the result
        if not (cudaq.mpi.is_initialized()) or (cudaq.mpi.rank() == 0):
            print('\n' + json.dumps(result))
