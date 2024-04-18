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
#echo "OPTIND is $OPTIND ${args[$((OPTIND-1))]}"

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
curl --location localhost:3030/job --header "Content-Length: ${#DATA}" --data "${DATA}"
echo