#!/bin/bash

# import common functions
if [ "$BIGMEMBENCH_COMMON_PATH" = "" ] ; then
  echo "ERROR: bigmembench_common script not found. BIGMEMBENCH_COMMON_PATH is $BIGMEMBENCH_COMMON_PATH"
  echo "Have you set BIGMEMBENCH_COMMON_PATH correctly? Are you using sudo -E instead of just sudo?"
  exit 1
fi
source ${BIGMEMBENCH_COMMON_PATH}/run_exp_common.sh

RESULT_DIR="/ssd1/songxin8/thesis/cache/vtune/exp_endToEnd/"
#RESULT_DIR="/ssd1/songxin8/thesis/cache/vtune/test/"
WORKING_DIR="/ssd1/songxin8/thesis/cache/CacheLib/"
CONFIG_DIR="${WORKING_DIR}/cachelib/cachebench/test_configs/ecosys_medium/"

#declare -a CONFIG_LIST=("graph_cache_leader_assocs" "cdn" "kvcache_reg" "ssd_graph_cache_leader")
declare -a CONFIG_LIST=("graph_cache_leader_assocs" "cdn" "kvcache_reg")
#declare -a CONFIG_LIST=("graph_cache_leader_assocs") 

VTUNE_EXE="/opt/intel/oneapi/vtune/latest/bin64/vtune"

export LD_PRELOAD="/usr/lib/x86_64-linux-gnu/debug/libstdc++.so.6.0.28"
echo "LD_PRELOAD: $LD_PRELOAD"

run_vtune () { 
  OUTFILE_NAME=$1
  CACHE_CONFIG=$2
  CONFIG=$3

  OUTFILE_PATH="${RESULT_DIR}/${OUTFILE_NAME}"

  #VTUNE_MEMACC_COMMON="${VTUNE_EXE} -collect memory-access -start-paused \
  VTUNE_MEMACC_COMMON="${VTUNE_EXE} -collect memory-access \
      -knob sampling-interval=10 -knob analyze-mem-objects=true -knob analyze-openmp=true -knob mem-object-size-min-thres=256 \
      -data-limit=0 -result-dir ${OUTFILE_PATH}-memacc \
      --app-working-dir=${WORKING_DIR}"

  #VTUNE_HOTSPOT_COMMON="${VTUNE_EXE} -collect hotspots -start-paused \
  VTUNE_HOTSPOT_COMMON="${VTUNE_EXE} -collect hotspots \
      -data-limit=0 -result-dir ${OUTFILE_PATH}-hotspot \
      --app-working-dir=${WORKING_DIR}"

  VTUNE_UARCH_COMMON="${VTUNE_EXE} -collect uarch-exploration -start-paused \
      -knob sampling-interval=10 -knob collect-memory-bandwidth=true
      -data-limit=0 -result-dir ${OUTFILE_PATH}-uarch \
      --app-working-dir=${WORKING_DIR}" 
  if [[ "$CONFIG" == "ALL_LOCAL" ]]; then
    # All local config: place both data and compute on node 1
    COMMAND_COMMON="/usr/bin/numactl --membind=1 --cpunodebind=1"
  elif [[ "$CONFIG" == "EDGES_ON_REMOTE" ]]; then
    # place edges array on node 1, rest on node 0
    COMMAND_COMMON="/usr/bin/numactl --membind=0 --cpunodebind=0"
  elif [[ "$CONFIG" == "TPP" ]]; then
    # only use node 0 CPUs and let TPP decide how memory is placed
    COMMAND_COMMON="/usr/bin/numactl --cpunodebind=0"
  elif [[ "$CONFIG" == "AUTONUMA" ]]; then
    COMMAND_COMMON="/usr/bin/numactl --cpunodebind=0"
  else
    echo "Error! Undefined configuration $CONFIG"
    exit 1
  fi


  CACHELIB_KERNEL_CMD="${COMMAND_COMMON} ./opt/cachelib/bin/cachebench \
                       --json_test_config ${CONFIG_DIR}/${CACHE_CONFIG}/config.json --progress=5"

  echo "Starting analysis. Log file is ${OUTFILE_PATH}_hotspot_log"
  ${VTUNE_HOTSPOT_COMMON} -- ${CACHELIB_KERNEL_CMD} &> ${OUTFILE_PATH}_hotspot_log
  clean_cache
  #echo "Starting analysis. Log file is ${OUTFILE_PATH}-memaccLog"
  #echo "${VTUNE_MEMACC_COMMON} -- ${CACHELIB_KERNEL_CMD}" > ${OUTFILE_PATH}-memaccLog
  #${VTUNE_MEMACC_COMMON} -- ${CACHELIB_KERNEL_CMD} &>> ${OUTFILE_PATH}-memaccLog
  #clean_cache
  #echo "Starting analysis. Log file is ${OUTFILE_PATH}_uarch_log"
  #${VTUNE_UARCH_COMMON} -- ${CACHELIB_KERNEL_CMD} &> ${OUTFILE_PATH}_uarch_log
}


##############
# Script start
##############
trap clean_up SIGHUP SIGINT SIGTERM

mkdir -p $RESULT_DIR

setup_vtune

## AutoNUMA
#enable_autonuma
#for cache_config in "${CONFIG_LIST[@]}"
#do
#  clean_cache
#  LOGFILE_NAME=$(gen_file_name "cachelib" "${cache_config}" "${MEMCONFIG}_autonuma")
#  run_vtune $LOGFILE_NAME $cache_config "AUTONUMA"
#done

# All allocations on node 0
disable_numa
for cache_config in "${CONFIG_LIST[@]}"
do
  clean_cache
  LOGFILE_NAME=$(gen_file_name "cachelib" "${cache_config}" "${MEMCONFIG}_allLocal")
  run_vtune $LOGFILE_NAME $cache_config "ALL_LOCAL"
done


