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
import hashlib
import json
import time


class nvqc_input_file:
    local_path: str
    remote_path: str
    asset_id: str

    def __init__(self, local_path, remote_path=None):
        """
        Constructor

        Args:
            `local_path` (`str`): Local path to a file (required)
            `remote_path` (`str`): Remote path to the file once uploaded (optional)
        """
        self.local_path = local_path
        self.asset_id = ""
        if remote_path:
            self.remote_path = remote_path
        else:
            self.remote_path = local_path  # if not provided, use same as local path


class nvqc_output_file:
    local_path: str
    remote_path: str
    asset_id: str

    def __init__(self, remote_path, local_path=None):
        """
        Constructor

        Args:
            `remote_path` (`str`): Remote path to the file on NVQC servers (required)
            `local_path` (`str`): Local path to the file once downloaded (optional)
        """
        self.local_path = local_path
        self.remote_path = remote_path
        self.asset_id = ""


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
    """The selected function ID (either overridden by user or selected from ngpus)"""

    versionID: str
    """The selected function version ID (either overridden by user or selected from ngpus)"""

    input_assets: list[nvqc_input_file]
    """List of assets needed for this session"""

    in_context: bool
    """Whether or not we're in a "with" context right now"""

    verbose: bool
    """Verbose printing"""

    env_dict: dict
    """Environment variable dictionary"""

    # Set this to true to do local testing
    LOCAL_SERVER = False
    LOCAL_URL = "https://localhost:3031"
    NVCF_URL = "https://api.nvcf.nvidia.com/v2/nvcf"

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
        self.input_assets = list()
        self.in_context = False
        self.verbose = False
        self.env_dict = dict()

        if self.token is None:
            self.token = os.environ.get("NVQC_API_KEY", "invalid")

        if not self.token.startswith("nvapi-"):
            raise ValueError(
                'Invalid NVQC token specified; must start with `nvapi-`')

        if self.ncaID is None:
            #self.ncaID = 'audj0Ow_82RT0BbiewKaIryIdZWiSrOqiiDSaA8w7a8'
            self.ncaID = 'mZraB3k06kOd8aPhD6MVXJwBVZ67aXDLsfmDo4MYXDs'

        # Select the appropriate function
        if self.functionID is None:
            self.functionID = os.environ.get("NVQC_FUNCTION_ID")
        if self.versionID is None:
            self.versionID = os.environ.get("NVQC_FUNCTION_VERSION_ID")

        if self.functionID is None:
            self._selectFunctionAndVersion()

        if self.versionID is None:
            # User probably provided a function ID but not a version. Need to
            # select the version automatically now.
            self._selectVersion()

        if not hasattr(self, 'selectedFunction'):
            # Note: this was not fetched via API but might be nice to have
            self.selectedFunction = dict()
            self.selectedFunction["id"] = self.functionID
            self.selectedFunction["versionId"] = self.versionID
        print("Selected function: ", self.selectedFunction)

        pass

    def _fetchActiveFunctions(self):
        if self.LOCAL_SERVER:
            return

        # Fetch a list of functions
        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        r = requests.get(f'{self.NVCF_URL}/functions', headers=headers)
        rj = r.json()
        self.allActiveFunctions = list()
        for func in rj["functions"]:
            if func['ncaId'] == self.ncaID and func['status'] == 'ACTIVE':
                self.allActiveFunctions.append(func)

    def _selectFunctionAndVersion(self):
        if self.LOCAL_SERVER:
            return

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

    def _selectVersion(self):
        if self.LOCAL_SERVER:
            return

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

    def _fetchAssets(self):
        # Fetch a list of assets visible with this key
        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        r = requests.get(f'{self.NVCF_URL}/assets', headers=headers)
        assert r.status_code == 200
        rj = r.json()
        if self.verbose:
            print('_fetchAssets:', rj)
        self.nvqcAssets = list()
        for asset in rj["assets"]:
            self.nvqcAssets.append(asset)

    def _hashFile(self, filename):
        h = hashlib.sha256()
        b = bytearray(128 * 1024)
        mv = memoryview(b)
        with open(filename, 'rb', buffering=0) as f:
            while n := f.readinto(mv):
                h.update(mv[:n])
        return h.hexdigest()

    def add_input_file(self, f: nvqc_input_file):
        """
        Add a local file to the client so that the file can be uploaded when
        running a main program. You must be in a "with" context to call this
        function.

        Args:
            `f` (`nvqc_input_file`): A single file to add to the client
        """
        assert self.in_context

        if not hasattr(self, 'nvqcAssets'):
            self._fetchAssets()

        start = time.perf_counter()

        h = self._hashFile(f.local_path)

        headers_nvcf = dict()
        headers_nvcf['Authorization'] = 'Bearer ' + self.token
        headers_nvcf['Content-Type'] = 'application/json'
        headers_nvcf["accept"] = "application/json"
        data_nvcf = dict()
        data_nvcf["contentType"] = "application/octet-stream"
        data_nvcf["description"] = "cudaq-nvqc-file-" + h
        r = requests.post(url=f'{self.NVCF_URL}/assets',
                          data=json.dumps(data_nvcf),
                          headers=headers_nvcf)
        if self.verbose:
            print('r for assets post', r)
        assert (r.status_code == 200)
        assetResponse = r.json()

        if self.verbose:
            print('Uploading file...')
        with open(f.local_path, 'rb') as fh:
            data_upload = fh.read()
        headers_upload = dict()
        headers_upload['Content-Type'] = data_nvcf["contentType"]
        headers_upload['x-amz-meta-nvcf-asset-description'] = assetResponse[
            'description']
        if self.verbose:
            print('Uploading to', assetResponse['uploadUrl'])
        r = requests.put(url=assetResponse['uploadUrl'],
                         data=data_upload,
                         headers=headers_upload)
        if self.verbose:
            print('Done uploading file...', r)
        assert r.status_code == 200

        # Assign the asset id
        f.asset_id = assetResponse["assetId"]

        # Add to internal tracking list
        self.input_assets.append(f)

        end = time.perf_counter()

        if self.verbose:
            print(
                f'_add_input_file({f.local_path} completed in {end-start} seconds.'
            )

    def _deleteAllAssets(self):
        # This function is essentially disabled by default since it deletes all
        # of the assets owned by the NCA ID.
        return
        if not hasattr(self, 'nvqcAssets'):
            self._fetchAssets()

        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        for asset in self.nvqcAssets:
            assetId = asset['assetId']
            r = requests.delete(url=f'{self.NVCF_URL}/assets/{assetId}',
                                headers=headers)
            if self.verbose:
                print('Deleting asset', assetId, 'had return code', r)
            assert r.status_code == 204

    def _fetchAssetInfo(self, assetId=None):
        assetList = list()
        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        if assetId is None:
            # Return information about all the assets
            if not hasattr(self, 'nvqcAssets'):
                self._fetchAssets()
            for asset in self.nvqcAssets:
                assetId = asset['assetId']
                r = requests.get(url=f'{self.NVCF_URL}/assets/{assetId}',
                                 headers=headers)
                assert r.status_code == 200
                if self.verbose:
                    print('_fetchAssetInfo:', r.json())
                assetList.append(r.json())
        else:
            r = requests.get(url=f'{self.NVCF_URL}/assets/{assetId}',
                             headers=headers)
            assert r.status_code == 200
            if self.verbose:
                print('_fetchAssetInfo:', r.json())
            assetList.append(r.json())
        return assetList

    def add_input_files(self, f: list[nvqc_input_file]):
        """
        Add a list of local file to the client so that the files can be uploaded
        when running a main program.

        Args:
            `f` (`list[nvqc_input_file]`): A list of files to add to the client
        """
        # Add each item
        for it in f:
            self.add_input_file(f)

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
        for var, val in envDict.items():
            assert type(var) == str
            assert type(val) == str
            self.env_dict[var] = val
        pass

    def run(self, cli_args: list[str]):
        """
        Run a program on NVIDIA Quantum Cloud. This submits the job
        asynchronously and returns an `nvqc_request`.

        Args:
            `cli_args` (`list[str]`) : A list of command-line arguments
        
        Returns:
            `nvqc_request`: 
        """
        req_body = dict()
        req_body["cli_args"] = cli_args
        req_body["env_dict"] = self.env_dict

        asset_str = ''
        asset_str = ','.join(map(lambda x: x.asset_id, self.input_assets))
        assetFileMap = dict()
        for a in self.input_assets:
            assetFileMap[a.asset_id] = a.remote_path
        req_body["asset_map"] = assetFileMap
        print(json.dumps(req_body, indent=2))

        data = dict()
        data['requestBody'] = req_body

        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        headers['Content-Type'] = 'application/json'
        headers['NVCF-INPUT-ASSET-REFERENCES'] = asset_str
        headers['NVCF-POLL-SECONDS'] = '5'  # FIXME

        r = requests.post(
            url=
            f'{self.NVCF_URL}/pexec/functions/{self.functionID}/versions/{self.versionID}',
            data=json.dumps(data),
            headers=headers)
        print(r.request.headers)
        print(r.request.body)
        #if self.verbose:
        print('r for function post', r)

        #assert r.status_code == 200 or r.status_code == 202
        funcResponse = r.json()
        print(funcResponse)

        # The stateless server will place the files in the appropriate locations
        # and then run the commands specified in cli_args.

        pass

    def __enter__(self):
        self.in_context = True
        pass

    def __exit__(self, exc_type, exc_value, traceback):
        # For each input file that was added as an asset, let's delete it
        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        start = time.perf_counter()
        for f in self.input_assets:
            assetId = f.asset_id
            if len(assetId) > 0:
                r = requests.delete(url=f'{self.NVCF_URL}/assets/{assetId}',
                                    headers=headers)
            print('Deleting asset', assetId, 'had response', r)
            assert r.status_code == 204
        end = time.perf_counter()
        if self.verbose:
            print(f'Deleting assets took {end-start} seconds.')
        self.input_assets = list()
        self.in_context = False


# Typical workflow for the user
client = nvqc_client(
    token=os.environ.get("NVQC_API_KEY"),
    ngpus=1,  # default to 1
    # These aren't needed but can be overridden
    functionID='e53f57ed-6e04-4e42-b491-5c75b2132148',
    versionID='9b987d02-feec-427b-98cc-5d548c97319a')

with client:
    #client.verbose = True
    #client.add_input_file(nvqc_input_file('test.py'))
    #client._fetchAssets()
    #client._deleteAllAssets()
    #client._fetchAssetInfo()
    client.add_custom_env({"xxxCUDAQ_LOG_LEVEL": "info"})
    #print(client.nvqcAssets)
    client.run(['python3', 'test.py'])

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
