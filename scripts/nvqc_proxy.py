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

# This reverse proxy application is needed to span the small gaps when
# `cudaq-qpud` is shutting down and starting up again. This small reverse proxy
# allows the NVCF port (3030) to remain up while allowing the main `cudaq-qpud`
# application to restart if necessary.
PROXY_PORT = 3030
QPUD_PORT = 3031  # see `docker/build/cudaq.nvqc.Dockerfile`


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """Handle requests in a separate thread."""


class Server(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    default_request_version = 'HTTP/1.1'

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

    def do_POST(self):
        if self.path == '/job':
            content_length = int(self.headers['Content-Length'])
            if content_length > 0:
                post_body = self.rfile.read(content_length)
                #print(post_body)
                json_data = json.loads(post_body)
                #print("Hello world!", post_body)
                #print(json_data)
                # TODO - Have to handle the case where we're supposed to fetch the data from assets instead
                # Run this Linux command to invoke this:
                # CMD='print("hello from python")'; CMD2=$(echo -n $CMD | base64 --wrap=0); DATA='{"rawPython":"'$CMD2'"}'; curl --location localhost:3030/job --header "Content-Length: ${#DATA}" --data "${DATA}"
                if "rawPython" in json_data:
                    cmd_str_b64 = json_data["rawPython"]
                    #print(cmd_str_b64)
                    cmd_str = base64.b64decode(cmd_str_b64)
                    if json_data["gzip"] > 0:
                        cmd_str = gzip.decompress(cmd_str)
                    cmd_str = cmd_str.decode('utf-8')

                    # Add environment variables
                    custom_env = os.environ.copy()
                    if "envVars" in json_data:
                        envVars = json_data["envVars"]
                        for var, val in envVars.items():
                            custom_env[var] = val
                    cliArgs = list()
                    if "cliArgs" in json_data:
                        cliArgs = json_data["cliArgs"]

                    with tempfile.TemporaryDirectory() as tmpdir:
                        # Write files for the incoming JSON data
                        incomingFiles = list()
                        if "files" in json_data:
                            files = json_data["files"]
                            for fileName, fileContents in files.items():
                                # Create additional temp files and clean them up later
                                newName = os.path.normpath(tmpdir + "/" +
                                                           fileName)
                                if not newName.startswith(tmpdir):
                                    print(
                                        'Skipping', newName,
                                        'because it doesn\'t start with the correct prefix'
                                    )
                                    continue
                                if not os.path.exists(newName):
                                    print('Creating', newName)
                                    fileContentsBytes = gzip.decompress(
                                        base64.b64decode(fileContents))
                                    incomingFiles.append(newName)
                                    os.makedirs(os.path.dirname(newName),
                                                exist_ok=True)
                                    with open(newName, "wb") as fd:
                                        fd.write(fileContentsBytes)

                        with tempfile.NamedTemporaryFile(dir=tmpdir) as tmp:
                            tmp.write(cmd_str.encode('utf-8'))
                            tmp.flush()
                            print('Wrote data to', tmp.name)
                            # Should this be asynchronous?
                            result = subprocess.run(["/usr/bin/python3"] +
                                                    [tmp.name] + cliArgs,
                                                    capture_output=True,
                                                    cwd=tmpdir,
                                                    env=custom_env,
                                                    text=True)
                            print(result.stdout)

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
