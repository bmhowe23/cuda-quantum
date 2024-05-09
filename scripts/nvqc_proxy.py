# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

from http import HTTPStatus
import http.server
import json
import requests
import socketserver
import sys
import time
import json
import base64
import tempfile
import subprocess
import os
import gzip
import uuid

# This reverse proxy application is needed to span the small gaps when
# `cudaq-qpud` is shutting down and starting up again. This small reverse proxy
# allows the NVCF port (3030) to remain up while allowing the main `cudaq-qpud`
# application to restart if necessary.
PROXY_PORT = 3030
QPUD_PORT = 3031  # see `docker/build/cudaq.nvqc.Dockerfile`


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """Handle requests in a separate thread."""


# Global asset dictionary for local tests (not for NVCF)
global_file_dict = dict()


class Server(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    default_request_version = 'HTTP/1.1'

    # Override this function because we seem to be getting a lot of
    # ConnectionResetError exceptions in the health monitoring endpoint,
    # producing lots of ugly stack traces in the logs. Hopefully this will
    # reduce them.
    def handle_one_request(self):
        try:
            super().handle_one_request()
        except ConnectionResetError as e:
            if self.path != '/':
                print(f"Connection was reset by peer: {e}")
        except Exception as e:
            print(f"Unhandled exception: {e}")

    def log_message(self, format, *args):
        # Don't log the health endpoint queries
        if len(args) > 0 and args[0] != "GET / HTTP/1.1":
            super().log_message(format, *args)

    def do_GET(self):
        # Allow the proxy to automatically handle the health endpoint. The proxy
        # will exit if the application's /job endpoint is down.
        if self.path == '/':
            self.send_response(HTTPStatus.OK)
            self.send_header('Content-Type', 'application/json')
            message = json.dumps({"status": "OK"}).encode('utf-8')
            self.send_header("Content-Length", str(len(message)))
            self.end_headers()
            self.wfile.write(message)
        else:
            self.send_response(HTTPStatus.NOT_FOUND)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_DELETE(self):
        if self.path.startswith("/assets/"):
            #                    012345678
            assetId = self.path[8:]
            print(f"Deleting asset {assetId}")
            if assetId in global_file_dict:
                del global_file_dict[assetId]
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

    def do_PUT(self):
        if self.path.startswith("/upload/"):
            #                    012345678
            assetId = self.path[8:]
            content_length = int(self.headers['Content-Length'])
            global_file_dict[assetId] = self.rfile.read(content_length)
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

    def do_POST(self):
        if self.path == '/assets':
            content_length = int(self.headers['Content-Length'])
            if content_length > 0:
                post_body = self.rfile.read(content_length)
                json_data = json.loads(post_body)
                self.send_response(HTTPStatus.OK)
                self.send_header('Content-Type', 'application/json')
                res = dict()
                res['description'] = json_data['description']
                res['assetId'] = str(uuid.uuid4())
                res['uploadUrl'] = f'http://localhost:{str(PROXY_PORT)}/upload/{res["assetId"]}'
                message = json.dumps(res).encode('utf-8')
                self.send_header("Content-Length", str(len(message)))
                self.end_headers()
                self.wfile.write(message)
                return

        if self.path == '/job':
            content_length = int(self.headers['Content-Length'])
            if content_length > 0:
                post_body = self.rfile.read(content_length)
                json_data = json.loads(post_body)
                if "cli_args" in json_data:
                    print('Running cli_args job:', json_data)
                    custom_env = os.environ.copy()
                    if "env_dict" in json_data:
                        envVars = json_data["env_dict"]
                        for var, val in envVars.items():
                            custom_env[var] = val
                    with tempfile.TemporaryDirectory() as tmpdir:
                        # Write out the necessary assets if any are specified in
                        # the asset_map element.
                        incomingFiles = list()
                        if "asset_map" in json_data:
                            files = json_data["asset_map"]

                            for assetId, filename in files.items():
                                # Create additional temp files and clean them up later
                                newName = os.path.normpath(tmpdir + "/" +
                                                           filename)
                                if not newName.startswith(tmpdir):
                                    print(
                                        f'ERROR: Skipping {newName} because it doesn\'t start with the correct prefix'
                                    )
                                    continue
                                if not os.path.exists(newName):
                                    incomingFiles.append(newName)
                                    print('Creating', newName)
                                    os.makedirs(os.path.dirname(newName),
                                                exist_ok=True)
                                    if assetId in global_file_dict:
                                        # Non-NVCF path - write temp file
                                        with open(newName, "wb") as fd:
                                            fd.write(global_file_dict[assetId])
                                    else:
                                        # NVCF path: setup a symlink to the file
                                        src_filename = self.headers.get(
                                            "NVCF-ASSET-DIR",
                                            "") + '/' + assetId
                                        dst_filename = newName
                                        print(
                                            f'Creating a symlink from {src_filename} to {dst_filename}'
                                        )
                                        os.symlink(src=src_filename,
                                                   dst=dst_filename,
                                                   target_is_directory=False)

                        result = subprocess.run(json_data["cli_args"],
                                                capture_output=True,
                                                cwd=tmpdir,
                                                env=custom_env,
                                                text=True)

                        # Cleanup (FIXME - move to finally section)
                        for file2remove in incomingFiles:
                            print('Removing', file2remove)
                            os.remove(file2remove)

                self.send_response(HTTPStatus.OK)
                self.send_header('Content-Type', 'application/json')

                res = dict()
                res['stdout'] = result.stdout
                res['stderr'] = result.stderr
                res['returncode'] = result.returncode
                message = json.dumps(res).encode('utf-8')
                self.send_header("Content-Length", str(len(message)))
                self.end_headers()
                self.wfile.write(message)
                return
                # End if cli_args

            qpud_up = False
            retries = 0
            qpud_url = 'http://localhost:' + str(QPUD_PORT)
            while (not qpud_up):
                try:
                    ping_response = requests.get(qpud_url)
                    qpud_up = (ping_response.status_code == HTTPStatus.OK)
                except:
                    qpud_up = False
                if not qpud_up:
                    retries += 1
                    if retries > 100:
                        print("PROXY EXIT: TOO MANY RETRIES!")
                        sys.exit()
                    print(
                        "Main application is down, retrying (retry_count = {})..."
                        .format(retries))
                    time.sleep(0.1)

            content_length = int(self.headers['Content-Length'])
            if content_length:
                res = requests.request(method=self.command,
                                       url=qpud_url + self.path,
                                       headers=self.headers,
                                       data=self.rfile.read(content_length))
                self.send_response(HTTPStatus.OK)
                self.send_header('Content-Type', 'application/json')
                message = json.dumps(res.json()).encode('utf-8')
                self.send_header("Content-Length", str(len(message)))
                self.end_headers()
                self.wfile.write(message)
            else:
                self.send_response(HTTPStatus.BAD_REQUEST)
                self.send_header("Content-Length", "0")
                self.end_headers()
        else:
            self.send_response(HTTPStatus.NOT_FOUND)
            self.send_header("Content-Length", "0")
            self.end_headers()


Handler = Server
with ThreadedHTTPServer(("", PROXY_PORT), Handler) as httpd:
    print("Serving at port", PROXY_PORT)
    print("Forward to port", QPUD_PORT)
    httpd.serve_forever()
