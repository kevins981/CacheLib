#!/bin/bash

# import common functions
if [ "$BIGMEMBENCH_COMMON_PATH" = "" ] ; then
  echo "ERROR: bigmembench_common script not found. BIGMEMBENCH_COMMON_PATH is $BIGMEMBENCH_COMMON_PATH"
  echo "Have you set BIGMEMBENCH_COMMON_PATH correctly? Are you using sudo -E instead of just sudo?"
  exit 1
fi
source ${BIGMEMBENCH_COMMON_PATH}/run_exp_common.sh

RESULT_DIR="exp/exp_itemSizeTest/"
WORKING_DIR="/ssd1/songxin8/thesis/cache/CacheLib"
CONFIG_DIR="${WORKING_DIR}/cachelib/cachebench/test_configs/item_size_test/"

MEMCONFIG=""

#declare -a CACHE_CONFIG_LIST=("2k" "4k" "3k" "5k")
#declare -a CACHE_CONFIG_LIST=("8k" "12k" "6k" "10k")
#declare -a CACHE_CONFIG_LIST=("128" "256" "512" "1k")
declare -a CACHE_CONFIG_LIST=("64")

clean_up () {
    echo "Cleaning up. Kernel PID is $EXE_PID, numastat PID is $NUMASTAT_PID."
    # Perform program exit housekeeping
    kill $EXE_PID
    exit
}

run_app () {
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
  elif [[ "$CONFIG" == "MULTICLOCK" ]]; then
    COMMAND_COMMON="/usr/bin/time -v /usr/bin/numactl --cpunodebind=0"
  else
    echo "Error! Undefined configuration $CONFIG"
    exit 1
  fi

  echo "Start" > $OUTFILE_PATH

  echo "NUMA hardware config is: " >> $OUTFILE_PATH
  NUMACTL_OUT=$(numactl -H)
  echo "$NUMACTL_OUT" >> $OUTFILE_PATH

  echo "${COMMAND_COMMON} ./opt/cachelib/bin/cachebench --json_test_config \
    $CONFIG_DIR/$CACHE_CONFIG/config.json --progress=5"  >> $OUTFILE_PATH
  ${COMMAND_COMMON} ./opt/cachelib/bin/cachebench --json_test_config \
    $CONFIG_DIR/$CACHE_CONFIG/config.json --progress=5 &>> $OUTFILE_PATH &

  # PID of time command
  TIME_PID=$!
  # get PID of actual kernel, which is a child of time.
  # This PID is needed for the numastat command
  EXE_PID=$(pgrep -P $TIME_PID)

  # TODO: if the kernel command fails, we are still sleeping here

  echo "EXE PID is ${EXE_PID}"
  sleep 600 # wait for 10 minutes. Assume we reached steady state
  # run numastat ONCE. numastat has non-negligible performance overhead if ran too many times
  numastat -p $EXE_PID >> $OUTFILE_PATH
  # run top once.
  top -b -1 -n3 >> $OUTFILE_PATH

  echo "Waiting for cachelib workload to complete (PID is ${EXE_PID}). numastat is logged into ${OUTFILE_PATH}-numastat, PID is ${NUMASTAT_PID}. Top PID is ${TOP_PID}"
  wait $TIME_PID
  echo "Cachelib workload complete."
}


##############
# Script start
##############
trap clean_up SIGHUP SIGINT SIGTERM

mkdir -p $RESULT_DIR


# All allocations on local
disable_numa
for cache_config in "${CACHE_CONFIG_LIST[@]}"
do
  clean_cache
  LOGFILE_NAME=$(gen_file_name "cachelib" "${cache_config}" "${MEMCONFIG}_allLocal")
  run_app $LOGFILE_NAME $cache_config "ALL_LOCAL"
done

# AutoNUMA
enable_autonuma
for cache_config in "${CACHE_CONFIG_LIST[@]}"
do
  clean_cache
  LOGFILE_NAME=$(gen_file_name "cachelib" "${cache_config}" "${MEMCONFIG}_autonuma")
  run_app $LOGFILE_NAME $cache_config "AUTONUMA"
done

## TPP
#enable_tpp
#for cache_config in "${CACHE_CONFIG_LIST[@]}"
#do
#  clean_cache
#  LOGFILE_NAME=$(gen_file_name "cachelib" "${cache_config}" "${MEMCONFIG}_tpp")
#  run_app $LOGFILE_NAME $cache_config "TPP"
#done

