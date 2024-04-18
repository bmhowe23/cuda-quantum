# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

useRawCommand=false
verbose=false

declare -A ENV_VARS # environment variables provided by the user

add_to_json() {
  local var_name=$(echo "$1" | cut -d '=' -f 1)
  local var_value=$(echo "$1" | cut -d '=' -f 2)
  ENV_VARS["$var_name"]="$var_value"
}

__optind__=$OPTIND
OPTIND=1
while getopts ":c:e:v" opt; do
  case $opt in
    c) rawCommand="$OPTARG"
    useRawCommand=true
    ;;
    # Custom environment variables
    e) add_to_json "$OPTARG"
    ;;
    v) verbose=true
    ;;
    \?) echo "Invalid command line option -$OPTARG" >&2
    exit 1
    #if $is_sourced; then return 1; else exit 1; fi
    ;;
  esac
done

# Convert associative array to JSON format
jsonEnvVars="{"
first=1
for k in "${!ENV_VARS[@]}"; do
  if [ $first -eq 1 ]; then
    first=0
  else
    jsonEnvVars+=","
  fi
  jsonEnvVars+="\"$k\":\"${ENV_VARS[$k]}\""
done
jsonEnvVars+="}"

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

DATA='{"rawPython":"'$CMD'","envVars":'$jsonEnvVars'}'
if $verbose; then
  echo "JSON data to submit:"
  echo $DATA | jq
fi

USE_LOCAL=true

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
  
  # Check the queue depth
  #res=$(curl -s --location "https://api.nvcf.nvidia.com/v2/nvcf/queues/functions/${NVQC_FUNCTION_ID}/versions/${NVQC_FUNCTION_VERSION_ID}"
  #--no-progress-meter \
  #--header 'Content-Type: application/json' \
  #--header "Authorization: Bearer $NVQC_API_KEY")

  #echo $res | jq -r '.queues[0].queueDepth'

  DATA='{ "requestBody": '$DATA' }'
  # -w '%{http_code}'
  # -w '%{time_total}' \
  res=$(curl -s --location "https://api.nvcf.nvidia.com/v2/nvcf/exec/functions/${NVQC_FUNCTION_ID}/versions/${NVQC_FUNCTION_VERSION_ID}" \
  --no-progress-meter \
  --header 'Content-Type: application/json' \
  --header "Authorization: Bearer $NVQC_API_KEY" \
  --data '{ "requestBody": { "rawPython": "'$CMD'" }}')
  #echo $res
  reqId=$(echo $res | jq -r ".reqId")
  if [ "$reqId" == "null" ]; then
    echo "Error in return value: $res"
    exit 1
  fi
  needNewline=false
  executing=false
  while [ "$(echo $res | jq -r '.status')" == "pending-evaluation" ]; do
    # We need to poll the results
    lastPosition=999

    # Poll the position until we're at the front of the queue
    while ! $executing; do
      res=$(curl -s --location "https://api.nvcf.nvidia.com/v2/nvcf/queues/${reqId}/position" \
              --no-progress-meter \
              --header 'Content-Type: application/json' \
              --header "Authorization: Bearer $NVQC_API_KEY")
      posInQueue=$(echo $res | jq -r '.positionInQueue')
      if [ "$posInQueue" == "null" ]; then
        # Maybe it already finished in the time we were waiting
        # echo "Error parsing position in queue for result: $res"
        executing=true
        break
      fi
      if [ "$posInQueue" == "0" ] && [ "$lastPosition" == "999" ]; then
        # No need to pring anything ... execution had started right away
        break
      fi
      if [ "$lastPosition" != "$posInQueue" ]; then
        if [ "$lastPosition" == "999" ]; then
          echo "Position in queue: $posInQueue"
        else
          echo "Position in queue changed from $lastPosition to $posInQueue"
        fi
        lastPosition=$posInQueue
      fi
      if [ "$posInQueue" == "0" ]; then
        # Done polling
        echo "Your job has started executing now"
        executing=true
        break
      else
        # Wait before polling again
        echo -n "x"
        needNewline=true
        sleep 5
      fi
    done

    echo -n "."
    needNewline=true
    res=$(curl -s --location "https://api.nvcf.nvidia.com/v2/nvcf/exec/status/${reqId}" \
              --no-progress-meter \
              --header 'Content-Type: application/json' \
              --header "Authorization: Bearer $NVQC_API_KEY")
    if [ "$(echo $res | jq -r '.reqId')" == "null" ]; then
      echo "Error in return value: $res"
      exit 1
    fi
  done
  if $needNewline; then
    echo
  fi
  if true && [ "$(echo $res | jq -r '.response.returncode')" = "0" ]; then
    # Just print stdout
    echo $res | jq -r '.response.stdout'
  else
    # Print everything
    echo $res | jq
  fi
fi
