# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import os


class nvqc_input_file:
    local_path: str
    remote_path: str

    def __init__(self, local_path, remote_path=None):
        """
        Constructor

        Args:
            `local_path` (`str`): Local path to a file (required)
            `remote_path` (`str`): Remote path to the file once uploaded (optional)
        """
        pass


class nvqc_output_file:
    local_path: str
    remote_path: str

    def __init__(self, remote_path, local_path=None):
        """
        Constructor

        Args:
            `remote_path` (`str`): Remote path to the file on NVQC servers (required)
            `local_path` (`str`): Local path to the file once downloaded (optional)
        """
        pass


class nvqc_request:
    requestId: str
    result: str
    parent_client: 'nvqc_client'

    def __init__(self, parent_client: 'nvqc_client'):
        pass

    def result(self):
        """
        Poll until the job is complete and return the final result as a JSON object
        """
        # Once done, remove it from parent_client
        pass

    def status(self):
        """
        Perform a single remote call to see what the result status is.

        Returns:
            String status of `"waiting"`, `"complete"`, etc. (FIXME)
        """
        pass

    def dump_output_files(self):
        """
        When the server provides output files back to the client, call this
        function to write them out to local disk.
        """
        pass


class nvqc_client:
    """
    A client to access Nvidia Quantum Cloud
    """

    ngpus: int
    """The number of GPUs this client will use on the server"""

    token: str
    """The NVQC API KEY that starts with `nvcf-`."""

    requests: list[nvqc_request]
    """List of outstanding requests (`nvqc_request`)"""

    def __init__(self, token=None, ngpus=None, functionID=None, versionID=None):
        """
        Constructor
        
        Args:
            `token` (`str`): NVQC API key. If None, then use the `NVQC_API_KEY`
            environment variable
            `ngpus` (`int`): The number of GPUs your main program will use (defaults
            to 1)
            `functionID` (`str`): The NVQC function ID (defaults to automatically
            searching)
            `versionID` (`str`): The NVQC function version ID (defaults to
            automatically searching)
        """
        self.ngpus = 1
        pass

    def add_input_file(self, f: nvqc_input_file):
        """
        Add a local file to the client so that the file can be uploaded when
        running a main program.

        Args:
            `f` (`nvqc_input_file`): A single file to add to the client
        """
        pass

    def add_input_files(self, f: list[nvqc_input_file]):
        """
        Add a list of local file to the client so that the files can be uploaded
        when running a main program.

        Args:
            `f` (`list[nvqc_input_file]`): A list of files to add to the client
        """
        pass

    def add_output_file(self, f: nvqc_output_file):
        """
        When your NVQC job submission is complete, download a file that your
        program created to your local host.

        Args:
            `f` (`nvqc_output_file`) : A file that your program produced that you
            want to download from NVQC once your job completes execution.
        """
        pass

    def add_output_files(self, f: list[nvqc_output_file]):
        """
        When your NVQC job submission is complete, download a list of files that
        your program created to your local host.

        Args:
            `f` (`list[nvqc_output_file]`) : A list of files that your program
            produced that you want to download from NVQC once your job completes
            execution.
        """
        pass

    def add_custom_env(self, envDict: dict):
        """
        Add a custom dictionary of environment variables to be set on the remote
        server prior to executing your program on NVQC.

        Args:
            `envDict` (`dict`) : A string dictionary of string environment variables
        """
        pass

    def run(self, cliArgs: list[str]):
        """
        Run a program on NVIDIA Quantum Cloud. This submits the job
        asynchronously and returns an `nvqc_request`.

        Args:
            `cliArgs` (`list[str]`) : A list of command-line arguments
        
        Returns:
            `nvqc_request`: 
        """
        pass


# Typical workflow for the user
client = nvqc_client(
    token=os.environ.get("NVQC_API_KEY", "invalid"),
    ngpus=1,  # default to 1
    # These aren't needed but can be overriden
    functionID=os.environ.get("NVQC_FUNCTION_ID", "invalid"),
    versionID=os.environ.get("NVQC_FUNCTION_VERSION_ID", "invalid"),
)

# Or
client.set_api_key("nvapi-...")
client.set_num_gpus(1)

# User loads input files
f1 = nvqc_input_file("/local/path/file.py", remote_path="file.py")
f2 = nvqc_input_file("another_file.py")
client.add_input_files([f1, f2])  # or perhaps add_source_files()??
client.add_input_files_from_dir("/path/to/source/dir/with/many/files",
                                dst="remote/path/")

# User specifies expected output files and where they can be copied
f3 = nvqc_output_file(
    remote_path="data.dat",
    local_path="data.dat")  # local file won't exist until job execution
client.add_output_files(f3)

# User can specify remote environment variables
customEnv = dict()
customEnv["CUDAQ_LOG_LEVEL"] = "info"
client.add_custom_env(customEnv)

req_handle = client.run(["top_file.py", "--target", "nvidia-fp64"])

jobs = client.get_jobs()  # returns a list of active request handles
print("Request ID =", req_handle.requestId)
result = req_handle.result()
result.dump_output_files()  # writes f3
print("Job stddout =", result["stdout"])
print("Job retcode =", result["retcode"])
