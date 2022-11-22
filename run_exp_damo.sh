#!/bin/bash

RESULT_DIR="exp/exp_damo/"
WORKING_DIR="/ssd1/songxin8/thesis/cache/CacheLib"
CONFIG_DIR="${WORKING_DIR}/cachelib/cachebench/test_configs/ecosys/"

DAMO_EXE="/ssd1/songxin8/anaconda3/envs/py36_damo/bin/damo"

declare -a CONFIG_LIST=("graph_cache_leader_assocs" "cdn" "kvcache_reg" "ssd_graph_cache_leader")

clean_up () {
    echo "Cleaning up. Kernel PID is $EXE_PID"
    # Perform program exit housekeeping
    kill $EXE_PID
    exit
}

clean_cache () { 
  echo "Clearing caches..."
  # clean CPU caches 
  ./tools/clear_cpu_cache
  # clean page cache
  echo 3 > /proc/sys/vm/drop_caches
}

run_gap () { 
  OUTFILE=$1 #first argument
  CONFIG=$2
  NODE=$3
  echo "Start" > $OUTFILE
  
  /usr/bin/time -v /usr/bin/numactl --membind=${NODE} --cpunodebind=0 \
    ./opt/cachelib/bin/cachebench --json_test_config $CONFIG_DIR/$config/config.json --progress=5  &>> $OUTFILE &
  # PID of time command
  TIME_PID=$! 
  # get PID of actual kernel, which is a child of time. 
  EXE_PID=$(pgrep -P $TIME_PID)

  echo "EXE PID is ${EXE_PID}"

  # wait until the cache hits steady state
  # Dont have a mechanism to ensure we are at steady state. Just putting a safe number for now."
  echo "sleeping for 2min to wait for steady state. "
  sleep 120

  echo "Running damo."
  ${DAMO_EXE} record $EXE_PID --out ${OUTFILE}_damo.data &
  DAMO_PID=$!

  echo "running DAMO for 10 minutes. DAMO data trace file is ${OUTFILE}_damo.data"
  sleep 600

  kill $EXE_PID
  echo "DAMO measurement done."
}


##############
# Script start
##############
trap clean_up SIGHUP SIGINT SIGTERM

[[ $EUID -ne 0 ]] && echo "This script must be run using sudo or as root." && exit 1

mkdir -p $RESULT_DIR

# All allocations on node 0
for config in "${CONFIG_LIST[@]}"
do
  clean_cache
  run_gap "${RESULT_DIR}/${config}_allnode0" $config 0
done

