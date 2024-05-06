# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import os
import requests
import re


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
        self.local_path = local_path
        self.remote_path = remote_path


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
        self.local_path = local_path
        self.remote_path = remote_path


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

    functionID: str
    """The selected function ID (either overriden by user or selected from ngpus)"""

    versionID: str
    """The selected function version ID (either overriden by user or selected from ngpus)"""

    def __init__(self,
                 token=None,
                 ngpus=None,
                 functionID=None,
                 versionID=None,
                 ncaID=None):
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
            `ncaID` (`str`): Override the NVIDIA Client ID (for dev testing)
        """
        self.token = token
        self.ngpus = ngpus
        self.functionID = functionID
        self.versionID = versionID
        self.ncaID = ncaID

        # Select the appropriate function
        if self.token is None:
            self.token = os.environ.get("NVQC_API_KEY", "invalid")
        if self.functionID is None:
            self.functionID = os.environ.get("NVQC_FUNCTION_ID")

        if not self.token.startswith("nvapi-"):
            raise ValueError(
                'Invalid NVQC token specified; must start with `nvapi-`')

        if self.ncaID is None:
            #self.ncaID = 'audj0Ow_82RT0BbiewKaIryIdZWiSrOqiiDSaA8w7a8'
            self.ncaID = 'mZraB3k06kOd8aPhD6MVXJwBVZ67aXDLsfmDo4MYXDs'

        if self.functionID is None:
            self._selectFunctionAndVersion()

        if self.versionID is None:
            # User probably provided a function ID but not a version. Need to
            # select the version automatically now.
            self._selectVersion()

        pass

    def _fetchActiveFunctions(self):
        # Fetch a list of functions
        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        r = requests.get('https://api.nvcf.nvidia.com/v2/nvcf/functions',
                         headers=headers)
        rj = r.json()
        self.allActiveFunctions = list()
        for func in rj["functions"]:
            if func['ncaId'] == self.ncaID and func['status'] == 'ACTIVE':
                #print(func)
                self.allActiveFunctions.append(func)

    def _selectFunctionAndVersion(self):
        if not hasattr(self, 'allActiveFunctions'):
            self._fetchActiveFunctions()

        # Select the one with the right number of GPUs
        valid_versions = list()
        num_gpus_avail = dict()
        regex_str = "cuda_quantum_v(\d+)_t(\d+)_(\d)x"
        for func in self.allActiveFunctions:
            matches = re.search(regex_str, func['name'])
            if matches:
                func_ver, func_timeout, func_ngpu = matches.groups()
                func_ver = int(func_ver)
                func_timeout = int(func_timeout)
                func_ngpu = int(func_ngpu)
                if func_ver == 1 and func_ngpu == self.ngpus:
                    valid_versions.append(func)
                    num_gpus_avail[func_ngpu] = 1

        if len(valid_versions) == 0:
            raise Exception(
                'No valid functions found matching all criteria during search. Try overriding the function ID if you think it is up.'
            )
        sorted_versions = sorted(valid_versions,
                                 key=lambda x: x['createdAt'],
                                 reverse=True)
        self.functionID = sorted_versions[0]["id"]
        self.versionID = sorted_versions[0]["versionId"]
        self.selectedFunction = sorted_versions[0]
        print("Selected function: ", sorted_versions[0])

    def _selectVersion(self):
        if not hasattr(self, 'allActiveFunctions'):
            self._fetchActiveFunctions()
        valid_versions = list()
        for func in self.allActiveFunctions:
            if func["id"] == self.functionID:
                valid_versions.append(func)
        if len(valid_versions) == 0:
            raise Exception('No valid function versions found for function ' +
                            self.functionID)
        sorted_versions = sorted(valid_versions,
                                 key=lambda x: x['createdAt'],
                                 reverse=True)
        self.versionID = sorted_versions[0]["versionId"]
        self.selectedFunction = sorted_versions[0]
        print("Selected function: ", sorted_versions[0])

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
    token=os.environ.get("NVQC_API_KEY"),
    ngpus=1,  # default to 1
    # These aren't needed but can be overriden
    functionID=os.environ.get("NVQC_FUNCTION_ID"),
    versionID=os.environ.get("NVQC_FUNCTION_VERSION_ID"),
)

exit()

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
