# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import os


class NVQCInputFile:
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


class NVQCOutputFile:
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


class NVQCRequest:
    requestId: str
    result: str
    parent_client: 'NVQCClient'

    def __init__(self, parent_client: 'NVQCClient'):
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

    def dumpOutputFiles(self):
        """
        When the server provides output files back to the client, call this
        function to write them out to local disk.
        """
        pass


class NVQCClient:
    """
    A client to access Nvidia Quantum Cloud
    """

    ngpus: int
    """The number of GPUs this client will use on the server"""

    token: str
    """The NVQC API KEY that starts with `nvcf-`."""

    requests: list[NVQCRequest]
    """List of outstanding requests (`NVQCRequest`)"""

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

    def addInputFile(self, f: NVQCInputFile):
        """
        Add a local file to the client so that the file can be uploaded when
        running a main program.

        Args:
            `f` (`NVQCInputFile`): A single file to add to the client
        """
        pass

    def addInputFiles(self, f: list[NVQCInputFile]):
        """
        Add a list of local file to the client so that the files can be uploaded
        when running a main program.

        Args:
            `f` (`list[NVQCInputFile]`): A list of files to add to the client
        """
        pass

    def addOutputFile(self, f: NVQCOutputFile):
        """
        When your NVQC job submission is complete, download a file that your
        program created to your local host.

        Args:
            `f` (`NVQCOutputFile`) : A file that your program produced that you
            want to download from NVQC once your job completes execution.
        """
        pass

    def addOutputFiles(self, f: list[NVQCOutputFile]):
        """
        When your NVQC job submission is complete, download a list of files that
        your program created to your local host.

        Args:
            `f` (`list[NVQCOutputFile]`) : A list of files that your program
            produced that you want to download from NVQC once your job completes
            execution.
        """
        pass

    def addCustomEnv(self, envDict: dict):
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
        asynchronously and returns an `NVQCRequest`.

        Args:
            `cliArgs` (`list[str]`) : A list of command-line arguments
        
        Returns:
            `NVQCRequest`: 
        """
        pass


client = NVQCClient(
    token=os.environ.get("NVQC_API_KEY", "invalid"),
    ngpus=1,  # default to 1
    # These aren't needed but can be overriden
    functionID=os.environ.get("NVQC_FUNCTION_ID", "invalid"),
    versionID=os.environ.get("NVQC_FUNCTION_VERSION_ID", "invalid"),
)

# Or
client.setAPIKey("nvapi-...")
client.setNumGPUs(1)

f1 = NVQCInputFile("/local/path/file.py", remote_path="file.py")
f2 = NVQCInputFile("another_file.py")
client.addInputFiles([f1, f2])  # or perhaps addSourceFiles()
client.addInputFilesFromDir("/path/to/source/dir/with/many/files",
                            dst="remote/path/")

f3 = NVQCOutputFile(
    remote_path="data.dat",
    local_path="data.dat")  # local file won't exist until job execution
client.addOutputFiles(f3)

customEnv = dict()
customEnv["CUDAQ_LOG_LEVEL"] = "info"
client.addCustomEnv(customEnv)

reqHandle = client.run("top_file.py")
jobs = client.get_jobs()  # returns a list of active request handles
print("Request ID =", reqHandle.requestId)
result = reqHandle.result()
result.dumpOutputFiles()  # writes f3
print("Job stddout =", result["stdout"])
print("Job retcode =", result["retcode"])

jobs = client.get_jobs()
