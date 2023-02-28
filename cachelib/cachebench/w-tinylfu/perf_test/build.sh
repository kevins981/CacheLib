#g++ -std=c++14 -g -O3 perf_lfu.cpp -o perf_lfu
g++ -pthread microbench.cpp -g -O3 -lnuma -fopenmp -o microbench

