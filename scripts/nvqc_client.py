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
import sys
import logging

NVQC_CONFIG = os.environ.get("HOME") + "/.nvqc_client.json"

#logging.basicConfig(level=logging.DEBUG)


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
    request_id: str
    result_obj: dict
    parent_client: 'nvqc_client'

    def __init__(self, request_id: str, parent_client: 'nvqc_client'):
        self.request_id = request_id
        self.parent_client = parent_client
        self.result_obj = None
        pass

    def result(self):
        """
        Poll until the job is complete and return the final result as a JSON object
        """
        # Once done, remove it from parent_client
        if self.result_obj:
            return self.result_obj
        else:
            # Poll until complete
            ret = self.parent_client._fetchStatus(self.request_id,
                                                  poll_until_complete=True)
            # Remove from list now that it is complete
            del self.parent_client.requests[self.request_id]
            return ret

    def status(self):
        """
        Perform a single remote call to see what the result status is.

        Returns:
            String status of `"waiting"`, `"complete"`, etc. (FIXME)
        """
        ret = self.parent_client._fetchStatus(self.request_id,
                                              poll_until_complete=False)
        if ret:
            self.result_obj = ret
            return True
        return False

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

    requests: dict
    """Dictionary of outstanding requests (`nvqc_request`), keyed by request ID"""

    function_id: str
    """The selected function ID (either overridden by user or selected from ngpus)"""

    version_id: str
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
    LOCAL_SERVER: bool

    LOCAL_URL = "http://localhost:3030"
    NVCF_URL = "https://api.nvcf.nvidia.com/v2/nvcf"

    def __init__(self,
                 token=None,
                 ngpus=None,
                 function_id=None,
                 version_id=None,
                 ncaID=None,
                 local_server=False):
        """
        Constructor
        
        Args:
            `token` (`str`): NVQC API key. If None, then use the `NVQC_API_KEY`
            environment variable
            `ngpus` (`int`): The number of GPUs your main program will use (defaults
            to 1)
            `function_id` (`str`): The NVQC function ID (defaults to automatically
            searching)
            `version_id` (`str`): The NVQC function version ID (defaults to
            automatically searching)
            `ncaID` (`str`): Override the NVIDIA Client ID (for dev testing)
            `local_server` ('bool`): Set to true for local nvqc_proxy.py testing
        """
        self.token = token
        self.ngpus = ngpus
        self.function_id = function_id
        self.version_id = version_id
        self.ncaID = ncaID
        self.input_assets = list()
        self.in_context = False
        self.verbose = False
        self.env_dict = dict()
        self.requests = dict()
        self.LOCAL_SERVER = local_server

        if self.token is None:
            self.token = os.environ.get("NVQC_API_KEY", "invalid")

        if not self.token.startswith("nvapi-"):
            raise ValueError(
                'Invalid NVQC token specified; must start with `nvapi-`')

        if self.ncaID is None:
            self.ncaID = os.environ.get("NVQC_NCA_ID")
            if self.ncaID is None:
                #self.ncaID = 'audj0Ow_82RT0BbiewKaIryIdZWiSrOqiiDSaA8w7a8'
                self.ncaID = 'mZraB3k06kOd8aPhD6MVXJwBVZ67aXDLsfmDo4MYXDs'

        self._function_id_specified = function_id != None
        self._version_id_specified = version_id != None

        # Select the appropriate function
        if self.function_id is None:
            self.function_id = os.environ.get("NVQC_FUNCTION_ID")
            if self.function_id:
                self._function_id_specified = True
        if self.version_id is None:
            self.version_id = os.environ.get("NVQC_FUNCTION_VERSION_ID")
            if self.version_id:
                self._version_id_specified = True

        if self.function_id is None:
            # If unsuccessful on the first attempt, the local config file might
            # be corrupt. Re-fetch and try again.
            try:
                self._selectFunctionAndVersion()
            except Exception as e:
                self._fetchActiveFunctions(from_config_file=False)
                self._selectFunctionAndVersion()
            

        if self.version_id is None:
            # User probably provided a function ID but not a version. Need to
            # select the version automatically now.
            # If unsuccessful on the first attempt, the local config file might
            # be corrupt. Re-fetch and try again.
            try:
                self._selectVersion()
            except Exception as e:
                self._fetchActiveFunctions(from_config_file=False)
                self._selectVersion()
            

        if not hasattr(self, 'selectedFunction'):
            # Note: this was not fetched via API but might be nice to have
            self.selectedFunction = dict()
            self.selectedFunction["id"] = self.function_id
            self.selectedFunction["versionId"] = self.version_id

        if self.verbose:
            print("Selected function: ", self.selectedFunction)

        pass

    def _writeActiveFunctionsToFile(self):
        print("Calling _writeActiveFunctionsToFile")
        if not hasattr(self, 'allActiveFunctions'):
            return
        try:
            with open(NVQC_CONFIG, 'r') as fp:
                config = json.load(fp)
        except Exception as e:
            config = dict()
        config['allActiveFunctions'] = self.allActiveFunctions
        with open(NVQC_CONFIG, 'w') as fp:
            json.dump(config, fp)

    def _readActiveFunctionsFromFile(self):
        with open(NVQC_CONFIG, 'r') as fp:
            config = json.load(fp)
        self.allActiveFunctions = config['allActiveFunctions']

    def _fetchActiveFunctions(self, from_config_file: bool = True):
        if self.LOCAL_SERVER:
            return

        if from_config_file:
            try:
                self._readActiveFunctionsFromFile()
                return
            except Exception as e:
                # don't return, fetch remotely
                print(
                    "Was told to fetch active functions from config file, but errors occurred, so fetching functions remotely"
                )

        # Fetch a list of functions remotely
        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        r = requests.get(f'{self.NVCF_URL}/functions', headers=headers)
        rj = r.json()
        self.allActiveFunctions = list()
        for func in rj["functions"]:
            if func['ncaId'] == self.ncaID and func['status'] == 'ACTIVE':
                self.allActiveFunctions.append(func)
        self._writeActiveFunctionsToFile()

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
        self.function_id = sorted_versions[0]["id"]
        self.version_id = sorted_versions[0]["versionId"]
        self.selectedFunction = sorted_versions[0]

    def _selectVersion(self):
        if self.LOCAL_SERVER:
            return

        if not hasattr(self, 'allActiveFunctions'):
            self._fetchActiveFunctions()
        valid_versions = list()
        for func in self.allActiveFunctions:
            if func["id"] == self.function_id:
                valid_versions.append(func)
        if len(valid_versions) == 0:
            raise Exception('No valid function versions found for function ' +
                            self.function_id)
        sorted_versions = sorted(valid_versions,
                                 key=lambda x: x['createdAt'],
                                 reverse=True)
        self.version_id = sorted_versions[0]["versionId"]
        self.selectedFunction = sorted_versions[0]
        print('_selectVersion selected version_id =', self.version_id)

    def _fetchAssets(self):
        if self.LOCAL_SERVER:
            return

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
        url = f'{self.NVCF_URL}/assets'
        if self.LOCAL_SERVER:
            url = f'{self.LOCAL_URL}/assets'

        r = requests.post(url=url,
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

        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        headers['Content-Type'] = 'application/json'
        headers['NVCF-INPUT-ASSET-REFERENCES'] = asset_str
        headers['NVCF-POLL-SECONDS'] = '1'  # FIXME

        url = f'{self.NVCF_URL}/pexec/functions/{self.function_id}/versions/{self.version_id}'
        if self.LOCAL_SERVER:
            url = f'{self.LOCAL_URL}/job'

        data_json = json.dumps(req_body)
        r = requests.post(url=url, data=data_json, headers=headers)
        if r.status_code == 400 or r.status_code == 404:
            # This function/version does not exist. We probably need to refetch
            try_again = False
            if not (self._function_id_specified) and not (
                    self._version_id_specified):
                try_again = True
                self._fetchActiveFunctions(from_config_file=False)
                self._selectFunctionAndVersion()
            elif not (self._version_id_specified):
                try_again = True
                self._fetchActiveFunctions(from_config_file=False)
                self._selectVersion()
            if try_again:
                print(f'run() returned status code {r.status_code}. ' +
                      'Will re-fetch active functions and try again now.')
                url = f'{self.NVCF_URL}/pexec/functions/{self.function_id}/versions/{self.version_id}'
                # Now retry and continue on
                r = requests.post(url=url, data=data_json, headers=headers)

        if r.status_code != 200 and r.status_code != 202:
            print("Unhandled error:", r)
            return None

        request_id = r.headers.get('NVCF-REQID', 'invalid-reqid')
        request_status = r.headers.get('NVCF-STATUS', 'invalid-status')
        #percent_complete = r.headers['NVCF-PERCENT-COMPLETE']
        new_req = nvqc_request(request_id, self)
        self.requests[request_id] = new_req
        # LOCAL_SERVER doesn't populate a valid request_status
        if r.status_code == 200 and (request_status == "fulfilled" or
                                     self.LOCAL_SERVER):
            # All done
            new_req.result_obj = r.json()
            del self.requests[request_id]
            pass

        return new_req

    def _fetchStatus(self, req_id: str, poll_until_complete: bool = False):
        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        headers['Content-Type'] = 'application/json'
        headers['NVCF-POLL-SECONDS'] = '1'  # FIXME

        url = f'{self.NVCF_URL}/pexec/status/{req_id}'

        r = requests.get(url=url, headers=headers)
        request_status = r.headers['NVCF-STATUS']

        if poll_until_complete:
            while request_status != "fulfilled":
                r = requests.get(url=url, headers=headers)
                request_status = r.headers['NVCF-STATUS']

        if r.status_code == 200:
            return r.json()
        else:
            return None

    def __enter__(self):
        self.in_context = True
        pass

    def __exit__(self, exc_type, exc_value, traceback):
        # For each input file that was added as an asset, let's delete it
        headers = dict()
        headers['Authorization'] = 'Bearer ' + self.token
        start = time.perf_counter()
        base_url = self.NVCF_URL
        if self.LOCAL_SERVER:
            base_url = self.LOCAL_URL
        for f in self.input_assets:
            assetId = f.asset_id
            if len(assetId) > 0:
                r = requests.delete(url=f'{base_url}/assets/{assetId}',
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
    local_server=False,
    # These aren't needed but can be overridden
    function_id='e53f57ed-6e04-4e42-b491-5c75b2132148')

with client:
    #client.verbose = True
    #client.add_input_file(nvqc_input_file(local_path='test.py',remote_path='mydir/test.py'))
    #client.add_input_file(nvqc_input_file(local_path='test.py'))
    #client._fetchAssets()
    #client._deleteAllAssets()
    #client._fetchAssetInfo()
    #client.add_custom_env({"xxxCUDAQ_LOG_LEVEL": "info"})
    #print(client.nvqcAssets)
    #client.run(['/usr/bin/python3', '-c', 'print("hello")'])
    #client.run(['cat', '/proc/cpuinfo'])
    #print(client.run(['sleep', '0']).result()['stdout'])
    #print(client.run(['echo', 'hello']).result()['stdout'], end='')
    print(client.run(sys.argv[1:]).result()['stdout'], end='')
    #print(client.run(['cat', '/proc/cpuinfo']).result()['stdout'])
    #print(client.run(['/usr/bin/python3', 'test.py']).result()['stdout'],
    #      end='')

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
