#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <sys/mman.h>
#include <cassert>
#include <pthread.h>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <string>
#include <fstream>
#include <set>
#include <numa.h>
#include <numaif.h>
#include <errno.h>
#include <syscall.h>


#include "../wtinylfu.hpp"
#include "../bloom_filter.hpp"

// Perf related 
//#define PERF_PAGES	(1 + (1 << 16))	// Has to be == 1+2^n, 
//#define PERF_PAGES	(1 + (1 << 7))	// Has to be == 1+2^n, 
#define PERF_PAGES	(1 + (1 << 9))	// Has to be == 1+2^n, 
#define NPROC 64
#define NPBUFTYPES 2
#define LOCAL_DRAM_LOAD 0
#define REMOTE_DRAM_LOAD 1
//#define OUTFILE "perf_cpp.txt"

// TinyLFU related
//#define NUM_ENTRIES 108000000/16
//#define NUM_ENTRIES 86000000/16
#define NUM_ENTRIES 540000000/16

int fd[NPROC][NPBUFTYPES];
static struct perf_event_mmap_page *perf_page[NPROC][NPBUFTYPES];

struct perf_sample {
  struct perf_event_header header;
  __u64	ip;
  __u32 pid, tid;    /* if PERF_SAMPLE_TID */
  __u64 addr;        /* if PERF_SAMPLE_ADDR */
};

static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                int cpu, int group_fd, unsigned long flags)
{
    int ret;
    ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
                   group_fd, flags);
    return ret;
}

struct perf_event_mmap_page* perf_setup(__u64 config, __u64 cpu, __u64 type) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));

    pe.type = PERF_TYPE_RAW;
    pe.size = sizeof(pe);
    pe.config = config;
    pe.sample_period = 20;
    //pe.sample_period = 200;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
    pe.disabled = 0;
    pe.freq = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 0;
    pe.exclude_callchain_kernel = 1;
    //pe.exclude_callchain_user = 1;
    pe.precise_ip = 1;
    pe.inherit = 1; 
    pe.task = 1; 
    pe.sample_id_all = 1;

    // perf_event_open args: perf_event_attr, pid, cpu, group_fd, flags.
    // pid == 0 && cpu == -1: measures the calling process/thread on any CPU.
    // returns a file descriptor, for use in subsequent system calls.
    //fd = perf_event_open(&pe, 0, -1, -1, 0);
    fd[cpu][type] = perf_event_open(&pe, -1, cpu, -1, 0);
    if (fd[cpu][type] == -1) {
       std::perror("failed");
       fprintf(stderr, "Error opening leader %llx\n", pe.config);
       exit(EXIT_FAILURE);
    }

    size_t mmap_size = sysconf(_SC_PAGESIZE) * PERF_PAGES;

    // mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
    // prot: protection. How the page may be used.
    // flags: whether updates to the mapping are visible to other processes mapping the same region.
    // fd: file descriptor.
    // offset: offset into the file.
    struct perf_event_mmap_page *p = (perf_event_mmap_page *)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd[cpu][type], 0);
    if(p == MAP_FAILED) {
      perror("mmap");
    }
    assert(p != MAP_FAILED);
    return p;
}

void* perf_func(void*) {
    pthread_t tid = pthread_self();
    int sid = syscall(SYS_gettid);

    uint64_t throttle_cnt = 0;
    uint64_t unthrottle_cnt = 0;
    uint64_t unknown_cnt = 0;
    uint64_t sample_cnt = 0;
    uint64_t nodata_cnt = 0;

    for (int i = 0; i < NPROC; i++) {
      perf_page[i][LOCAL_DRAM_LOAD]  = perf_setup(0x1d3, i, LOCAL_DRAM_LOAD); // MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM
      perf_page[i][REMOTE_DRAM_LOAD] = perf_setup(0x2d3, i, REMOTE_DRAM_LOAD); // MEM_LOAD_L3_MISS_RETIRED.REMOTE_DRAM
    }

    std::cout << "start perf recording." << std::endl;

    //std::ofstream outfile;
    //outfile.open(OUTFILE);
    //std::cout << "Writing perf results to " << OUTFILE << std::endl;

    // setup TinyLFU
    uint32_t hot_thresh = 6;
    frequency_sketch<uint64_t> lfu(NUM_ENTRIES);
    uint64_t max_page_migrate = 7800000;
    //std::set<uint64_t> hot_pages;
    std::cout << "==== TinyLFU debug info" <<  std::endl;
    std::cout << std::dec << "hot threshold = " << hot_thresh << std::endl;
    std::cout << "max number of pages to be migrated = " << max_page_migrate << std::endl;

    uint64_t stat_hits = 0;
    uint64_t stat_misses = 0;
    uint64_t stat_prev_hits = 0;
    uint64_t stat_prev_misses = 0;
    uint64_t page_addr;
    uint64_t incr_hits;
    uint64_t incr_misses;
    float hit_rate;


    // NUMA migration vars
    int** migrate_pages = new int*[1];  // array of pointer, with only 1 element
    int migrate_nodes[1] = {0}; // migrate to node 0
    int migrate_status[1] = {99};
    long pages_migrated = 0;
    long pages_already_in_node0 = 0;
    int move_page_ret;

    // start perf monitoring.
    for(;;){
      for (int i = 0; i < NPROC; i++) {
        for(int j = 0; j < NPBUFTYPES; j++) {
          struct perf_event_mmap_page *p = perf_page[i][j];
          char *pbuf = (char *)p + p->data_offset;
          __sync_synchronize();

          // this probably keeps looping if no new perf data is collected. 
          // interrupt might be a better idea.
          if(p->data_head == p->data_tail) {
            //std::cout << "no data." << std::endl;
            nodata_cnt++;
            continue;
          }
          struct perf_event_header *ph = (perf_event_header *)((void *)(pbuf + (p->data_tail % p->data_size)));
          struct perf_sample* ps;
          //p == header
          //perf_sample == perf_event_sample
          //perf_sample = base + data_tail % buffer_size;
          //            = header + page_size + data_tail % buffer_size;
          //            = p + page_size + data_tail % buffer_size;

        	//mmap_size = page_size * (page_cnt + 1); 

        	//__u64 buffer_size = page_cnt * page_size;


          //mine
          //perf_sample = p + p->data_offset + data_tail % p->data_size
        
          if ( (char*)(__u64(ph) + sizeof(struct perf_sample)) > (char*)(__u64(p) + p->data_offset + p->data_size)) {
            // this sample overflowed/exceeded the mmap region. Reading this sample would cause
            // segfault. Skipping this sample. After the next p->data_tail += ph->size, the overflow
            // should be resolved, as we are back to the head of the circular buffer.
            std::cout << "[INFO] skipping overflow sample. sample start: " << ph << ", size of sample: " << sizeof(ps) << std::endl;
          } else {
            switch(ph->type) {
              case PERF_RECORD_SAMPLE:
                ps = (struct perf_sample*)ph;
                //assert(ps != NULL);
                if (ps->addr != 0) { // sometimes the sample address is 0. Not sure why
                  page_addr = ps->addr & ~(0xFFF); // get virtual page address from address
                  lfu.record_access(page_addr);
                  if (lfu.frequency(page_addr) >= hot_thresh) {
                    migrate_pages[0] = (int*)page_addr;
                    // first check if this page is already in node 0.
                    //std::cout << "66:  " << page_addr << std::endl;
                    numa_move_pages(0, 1, (void **)migrate_pages, NULL, migrate_status, MPOL_MF_MOVE_ALL);
                    if (migrate_status[0] == 0){
                      // page is already in node 0
                      pages_already_in_node0++;
                    } else {
                      // promote hot page to fast tier memory
                      move_page_ret = numa_move_pages(0, 1, (void **)migrate_pages, migrate_nodes, migrate_status, MPOL_MF_MOVE_ALL);
                      if (move_page_ret) { 
                        // a non zero value is returned.
                        std::cout << "WARNING: migrating page " << migrate_pages[0] << " failed with " << strerror(errno) << std::endl;
                      } else if (migrate_status[0] < 0) {
                        std::cout << "migration status is -ve:  " << strerror(-1*migrate_status[0]) << std::hex << ". addr " <<  migrate_pages[0] << std::endl;
                      } else {
                        pages_migrated++;
                      }
                    }
                  }
                  if (sample_cnt % 100000 == 0){
                    std::cout << std::dec << "pages migrated: " << pages_migrated << std::endl;
                    //std::cout << "pages already in node 0: " << pages_already_in_node0 << std::endl;
                    //std::cout << "samples: " << sample_cnt << std::endl;
                  }
                  if (pages_migrated  > max_page_migrate) {
                    std::cout << "migrated " << max_page_migrate <<  " pages. Stop. " << std::endl;
                    return NULL;
                  }
                  sample_cnt++;
                }
                break;
              case PERF_RECORD_THROTTLE:
                throttle_cnt++;
                break;
              case PERF_RECORD_UNTHROTTLE:
                unthrottle_cnt++;
                break;
              default:
                //fprintf(stderr, "Unknown type %u\n", ph->type);
                unknown_cnt++;
                break;
            }
          }
          // When the mapping is PROT_WRITE, the data_tail value should
          // be written by user space to reflect the last read data.
          // In this case, the kernel will not overwrite unread data.
          p->data_tail += ph->size;
        }
      }
    }
  //outfile.close();
  return NULL;
}

