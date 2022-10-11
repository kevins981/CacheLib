#!/bin/bash

RESULT_DIR="/ssd1/songxin8/thesis/cache/vtune/sweep_normal/"
WORKING_DIR="/ssd1/songxin8/thesis/cache/CacheLib/"
CONFIG_DIR="${WORKING_DIR}/cachelib/cachebench/test_configs/ecosys/"

declare -a CONFIG_LIST=("graph_cache_leader_assocs" "cdn" "kvcache_reg" "ssd_graph_cache_leader")

##clean_up () {
##    echo "Cleaning up. Kernel PID is $EXE_PID, numastat PID is $LOG_PID."
##    # Perform program exit housekeeping
##    kill $LOG_PID ##    kill $EXE_PID
##    exit
##}

clean_cache () { 
  echo "Clearing caches..."
  # clean CPU caches
  ./tools/clear_cpu_cache
  # clean page cache
  echo 3 > /proc/sys/vm/drop_caches
}

run_vtune () { 
  CONFIG=$1
  MEMNODE=$2

  LD_PRELOAD=/usr/lib/x86_64-linux-gnu/debug/libstdc++.so.6.0.28 /opt/intel/oneapi/vtune/2022.3.0/bin64/vtune -collect hotspots -data-limit=5000 -result-dir ${RESULT_DIR}/${config}_hotspot_node${MEMNODE} --app-working-dir=${WORKING_DIR} -- /usr/bin/numactl --membind=${MEMNODE} --cpunodebind=0 ./opt/cachelib/bin/cachebench --json_test_config $CONFIG_DIR/$config/config.json --progress=5

  LD_PRELOAD=/usr/lib/x86_64-linux-gnu/debug/libstdc++.so.6.0.28 /opt/intel/oneapi/vtune/2022.3.0/bin64/vtune -collect memory-access -knob sampling-interval=10 -knob analyze-mem-objects=true -knob mem-object-size-min-thres=256 -knob analyze-openmp=true -data-limit=5000 -result-dir ${RESULT_DIR}/${config}_memacc_node${MEMNODE} --app-working-dir=${WORKING_DIR} -- /usr/bin/numactl --membind=${MEMNODE} --cpunodebind=0 ./opt/cachelib/bin/cachebench --json_test_config $CONFIG_DIR/$config/config.json --progress=5

  #LD_PRELOAD=/usr/lib/x86_64-linux-gnu/debug/libstdc++.so.6.0.28 /opt/intel/oneapi/vtune/2022.3.0/bin64/vtune -collect memory-consumption -knob mem-object-size-min-thres=1024 -data-limit=5000 -result-dir ${RESULT_DIR}/${config}_test_memconsump_node0 --app-working-dir=${WORKING_DIR} -- /usr/bin/numactl --membind=${MEMNODE} --cpunodebind=0 ./opt/cachelib/bin/cachebench --json_test_config $CONFIG_DIR/$config/config.json --progress=5

}


##############
# Script start
##############
#trap clean_up SIGHUP SIGINT SIGTERM

[[ $EUID -ne 0 ]] && echo "This script must be run using sudo or as root." && exit 1

# All allocations on node 0
for config in "${CONFIG_LIST[@]}"
do
  clean_cache
  run_vtune $config 0
  clean_cache
  run_vtune $config 1
done

