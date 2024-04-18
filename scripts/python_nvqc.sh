# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

build_configuration=Release
useRawCommand=false
verbose=false
compiler_rt=false
llvm_runtimes=""

__optind__=$OPTIND
OPTIND=1
while getopts "c:" opt; do
  case $opt in
    c) rawCommand="$OPTARG"
    useRawCommand=true
    ;;
    \?) echo "Invalid command line option -$OPTARG" >&2
    exit 1
    #if $is_sourced; then return 1; else exit 1; fi
    ;;
  esac
done

args=("$@")

if $useRawCommand; then
  CMD=$(echo -n $rawCommand | base64 --wrap=0)
else
  FILENAME=${args[$((OPTIND-1))]}
  if [ -z "$FILENAME" ]; then
    echo "No filename provided...exiting"
    exit 1
  fi
  if [ ! -e "$FILENAME" ]; then
    echo "Filename $FILENAME doesn't exist ... exiting"
    exit 1
  fi
  CMD=$(cat $FILENAME | base64 --wrap=0)
fi

DATA='{"rawPython":"'$CMD'"}'

USE_LOCAL=false

# Use this when running locally
if $USE_LOCAL; then
  curl --location localhost:3030/job --header "Content-Length: ${#DATA}" --data "${DATA}"
else
  # Use this when running with NVCF
  if [ -z "$NVQC_API_KEY" ]; then
    echo "You need to set your NVQC_API_KEY environment variable"
    exit 1
  fi
  if [ -z "$NVQC_FUNCTION_ID" ]; then
    NVQC_FUNCTION_ID=e53f57ed-6e04-4e42-b491-5c75b2132148
    #echo "You need to set the NVQC_FUNCTION_ID environment variable"
    #exit 1
  fi
  if [ -z "$NVQC_FUNCTION_VERSION_ID" ]; then
    NVQC_FUNCTION_VERSION_ID=fa29c725-7a19-41e7-b530-f74fc7e1b61e
    #echo "You need to set the NVQC_FUNCTION_VERSION_ID environment variable"
    #exit 1
  fi
  DATA='{ "requestBody": '$DATA' }'
  # FIXME - set function ID
  curl --location "https://api.nvcf.nvidia.com/v2/nvcf/exec/functions/${NVQC_FUNCTION_ID}/versions/${NVQC_FUNCTION_VERSION_ID}" \
  --header 'Content-Type: application/json' \
  --header "Authorization: Bearer $NVQC_API_KEY" \
  --data '{ "requestBody": { "rawPython": "'$CMD'" }}'
fi

echo
