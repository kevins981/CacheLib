#!/bin/bash

RESULT_DIR="exp/all_sweep_exp1/"
WORKING_DIR="/ssd1/songxin8/thesis/cache/CacheLib"
CONFIG_DIR="${WORKING_DIR}/cachelib/cachebench/test_configs/ecosys/"

declare -a CONFIG_LIST=("graph_cache_leader_assocs" "cdn" "kvcache_reg" "ssd_graph_cache_leader")

clean_up () {
    echo "Cleaning up. Kernel PID is $EXE_PID, numastat PID is $LOG_PID."
    # Perform program exit housekeeping
    kill $LOG_PID
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
    ./opt/cachelib/bin/cachebench --json_test_config $CONFIG_DIR/$config/config.json --progress=5 &>> $OUTFILE &
  # PID of time command
  TIME_PID=$! 
  # get PID of actual kernel, which is a child of time. 
  # This PID is needed for the numastat command
  EXE_PID=$(pgrep -P $TIME_PID)

  echo "EXE PID is ${EXE_PID}"
  echo "start" > ${OUTFILE}_numastat 
  while true; do numastat -p $EXE_PID >> ${OUTFILE}_numastat; sleep 5; done &
  LOG_PID=$!

  echo "Waiting for cachelib workload to complete (PID is ${EXE_PID}). numastat is logged into ${OUTFILE}_numastat, PID is ${LOG_PID}" 
  wait $TIME_PID
  echo "Cachelib workload complete."
  kill $LOG_PID
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
  clean_cache
  run_gap "${RESULT_DIR}/${config}_allnode1" $config 1
done

