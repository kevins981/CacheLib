#!/bin/bash

# import common functions
if [ "$BIGMEMBENCH_COMMON_PATH" = "" ] ; then
  echo "ERROR: bigmembench_common script not found. BIGMEMBENCH_COMMON_PATH is $BIGMEMBENCH_COMMON_PATH"
  echo "Have you set BIGMEMBENCH_COMMON_PATH correctly? Are you using sudo -E instead of just sudo?"
  exit 1
fi
source ${BIGMEMBENCH_COMMON_PATH}/run_exp_common.sh

RESULT_DIR="exp/exp_damo/"
WORKING_DIR="/ssd1/songxin8/thesis/cache/CacheLib"
CONFIG_DIR="${WORKING_DIR}/cachelib/cachebench/test_configs/ecosys_medium/"

DAMO_EXE="/ssd1/songxin8/anaconda3/envs/py36_damo/bin/damo"
MEMCONFIG=""


#declare -a CACHE_CONFIG_LIST=("graph_cache_leader_assocs" "cdn" "kvcache_reg" "ssd_graph_cache_leader")
declare -a CACHE_CONFIG_LIST=("cdn" "kvcache_reg")
#declare -a CACHE_CONFIG_LIST=("graph_cache_leader_assocs")

clean_up () {
    echo "Cleaning up. Kernel PID is $EXE_PID"
    # Perform program exit housekeeping
    kill $EXE_PID
    exit
}

run_damo () { 
  OUTFILE_NAME=$1 
  CACHE_CONFIG=$2
  CONFIG=$3
  OUTFILE_PATH="${RESULT_DIR}/${OUTFILE_NAME}"

  if [[ "$CONFIG" == "ALL_LOCAL" ]]; then
    # All local config: place both data and compute on node 1
    COMMAND_COMMON="/usr/bin/time -v /usr/bin/numactl --membind=1 --cpunodebind=1"
  elif [[ "$CONFIG" == "EDGES_ON_REMOTE" ]]; then
    # place edges array on node 1, rest on node 0
    COMMAND_COMMON="/usr/bin/time -v /usr/bin/numactl --membind=0 --cpunodebind=0"
  elif [[ "$CONFIG" == "TPP" ]]; then
    # only use node 0 CPUs and let TPP decide how memory is placed
    COMMAND_COMMON="/usr/bin/time -v /usr/bin/numactl --cpunodebind=0"
  elif [[ "$CONFIG" == "AUTONUMA" ]]; then
    COMMAND_COMMON="/usr/bin/time -v /usr/bin/numactl --cpunodebind=0"
  else
    echo "Error! Undefined configuration $CONFIG"
    exit 1
  fi

  echo "Start" > $OUTFILE_PATH

  echo "NUMA hardware config is: " >> $OUTFILE_PATH
  NUMACTL_OUT=$(numactl -H)
  echo "$NUMACTL_OUT" >> $OUTFILE_PATH

  
  ${COMMAND_COMMON} ./opt/cachelib/bin/cachebench \
      --json_test_config $CONFIG_DIR/$CACHE_CONFIG/config.json --progress=5  &>> $OUTFILE_PATH &
  # PID of time command
  TIME_PID=$! 
  # get PID of actual kernel, which is a child of time. 
  EXE_PID=$(pgrep -P $TIME_PID)

  echo "EXE PID is ${EXE_PID}"

  # wait until the cache hits steady state
  # Dont have a mechanism to ensure we are at steady state. Just putting a safe number for now.
  echo "sleeping for 60s to wait for steady state. "
  sleep 60

  echo "Running damo."
  ${DAMO_EXE} record $EXE_PID --out ${OUTFILE_PATH}-damo.data &
  DAMO_PID=$!

  echo "running DAMO for 10 minutes. DAMO data trace file is ${OUTFILE_PATH}-damo.data"
  sleep 600

  kill $EXE_PID
  echo "DAMO measurement done."
}


##############
# Script start
##############
trap clean_up SIGHUP SIGINT SIGTERM

mkdir -p $RESULT_DIR

# All allocations on local node
enable_damo
for cache_config in "${CACHE_CONFIG_LIST[@]}"
do
  LOGFILE_NAME=$(gen_file_name "cachelib" "${cache_config}" "${MEMCONFIG}_allLocal")
  clean_cache
  run_damo $LOGFILE_NAME $cache_config "ALL_LOCAL"
done

