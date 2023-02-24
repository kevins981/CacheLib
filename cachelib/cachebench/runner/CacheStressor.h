/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/Random.h>
#include <folly/TokenBucket.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_set>
#include <ittnotify.h>  // vtune pause and resume

#include "cachelib/cachebench/cache/Cache.h"
#include "cachelib/cachebench/cache/TimeStampTicker.h"
#include "cachelib/cachebench/runner/Stressor.h"
#include "cachelib/cachebench/util/Config.h"
#include "cachelib/cachebench/util/Exceptions.h"
#include "cachelib/cachebench/util/Parallel.h"
#include "cachelib/cachebench/util/Request.h"
#include "cachelib/cachebench/workload/GeneratorBase.h"

namespace facebook {
namespace cachelib {
namespace cachebench {

constexpr uint32_t kNvmCacheWarmUpCheckRate = 1000;

// Implementation of stressor that uses a workload generator to stress an
// instance of the cache.  All item's value in CacheStressor follows CacheValue
// schema, which contains a few integers for sanity checks use. So it is invalid
// to use item.getMemory and item.getSize APIs.
template <typename Allocator>
class CacheStressor : public Stressor {
 public:
  using CacheT = Cache<Allocator>;
  using Key = typename CacheT::Key;
  using WriteHandle = typename CacheT::WriteHandle;

  // @param cacheConfig   the config to instantiate the cache instance
  // @param config        stress test config
  // @param generator     workload  generator
  CacheStressor(CacheConfig cacheConfig,
                StressorConfig config,
                std::unique_ptr<GeneratorBase>&& generator)
      : config_(std::move(config)),
        throughputStats_(config_.numThreads),
        wg_(std::move(generator)),
        hardcodedString_(genHardcodedString()),
        endTime_{std::chrono::system_clock::time_point::max()} {
    // if either consistency check is enabled or if we want to move
    // items during slab release, we want readers and writers of chained
    // allocs to be synchronized
    //std::cout << "DEBUG: hardcode string is " << hardcodedString_ << std::endl;
    typename CacheT::ChainedItemMovingSync movingSync;
    if (config_.usesChainedItems() &&
        (cacheConfig.moveOnSlabRelease || config_.checkConsistency)) {
      lockEnabled_ = true;

      struct CacheStressSyncObj : public CacheT::SyncObj {
        std::unique_lock<folly::SharedMutex> lock;

        CacheStressSyncObj(CacheStressor& s, std::string itemKey)
            : lock{s.chainedItemAcquireUniqueLock(itemKey)} {}
      };
      movingSync = [this](typename CacheT::Item::Key key) {
        return std::make_unique<CacheStressSyncObj>(*this, key.str());
      };
    }

    if (cacheConfig.useTraceTimeStamp &&
        cacheConfig.tickerSynchingSeconds > 0) {
      // When using trace based replay for generating the workload,
      // TimeStampTicker allows syncing the notion of time between the
      // cache and the workload generator based on timestamps in the trace.
      ticker_ = std::make_shared<TimeStampTicker>(
          config.numThreads, cacheConfig.tickerSynchingSeconds,
          [wg = wg_.get()](double elapsedSecs) {
            wg->renderWindowStats(elapsedSecs, std::cout);
          });
      cacheConfig.ticker = ticker_;
    }

    cache_ = std::make_unique<CacheT>(cacheConfig, movingSync,
                                      cacheConfig.cacheDir, config_.touchValue);
    if (config_.opPoolDistribution.size() > cache_->numPools()) {
      throw std::invalid_argument(folly::sformat(
          "more pools specified in the test than in the cache. "
          "test: {}, cache: {}",
          config_.opPoolDistribution.size(), cache_->numPools()));
    }
    if (config_.keyPoolDistribution.size() != cache_->numPools()) {
      throw std::invalid_argument(folly::sformat(
          "different number of pools in the test from in the cache. "
          "test: {}, cache: {}",
          config_.keyPoolDistribution.size(), cache_->numPools()));
    }

    if (config_.checkConsistency) {
      cache_->enableConsistencyCheck(wg_->getAllKeys());
    }
    if (config_.opRatePerSec > 0) {
      rateLimiter_ = std::make_unique<folly::BasicTokenBucket<>>(
          config_.opRatePerSec, config_.opRatePerSec);
    }
  }

  ~CacheStressor() override { finish(); }

  // Start the stress test by spawning the worker threads and waiting for them
  // to finish the stress operations.
  void start() override {
    {
      std::lock_guard<std::mutex> l(timeMutex_);
      startTime_ = std::chrono::system_clock::now();
    }
    std::cout << folly::sformat("Total {:.2f}M ops to be run",
                                config_.numThreads * config_.numOps / 1e6)
              << std::endl;

    stressWorker_ = std::thread([this] {
      std::vector<std::thread> workers;
      for (uint64_t i = 0; i < config_.numThreads; ++i) {
        workers.push_back(
            std::thread([this, throughputStats = &throughputStats_.at(i), thread_num = i]() {
              stressByDiscreteDistribution(*throughputStats, thread_num);
            }));
      }
      for (auto& worker : workers) {
        worker.join();
      }
      {
        std::lock_guard<std::mutex> l(timeMutex_);
        endTime_ = std::chrono::system_clock::now();
      }
    });
  }

  // Block until all stress workers are finished.
  void finish() override {
    if (stressWorker_.joinable()) {
      stressWorker_.join();
    }
    wg_->markShutdown();
    cache_->clearCache(config_.maxInvalidDestructorCount);
  }

  // abort the stress run by indicating to the workload generator and
  // delegating to the base class abort() to stop the test.
  void abort() override {
    wg_->markShutdown();
    Stressor::abort();
  }

  // obtain stats from the cache instance.
  Stats getCacheStats() const override { return cache_->getStats(); }

  // obtain aggregated throughput stats for the stress run so far.
  ThroughputStats aggregateThroughputStats() const override {
    ThroughputStats res{};
    for (const auto& stats : throughputStats_) {
      res += stats;
    }

    return res;
  }

  void renderWorkloadGeneratorStats(uint64_t elapsedTimeNs,
                                    std::ostream& out) const override {
    wg_->renderStats(elapsedTimeNs, out);
  }

  void renderWorkloadGeneratorStats(
      uint64_t elapsedTimeNs, folly::UserCounters& counters) const override {
    wg_->renderStats(elapsedTimeNs, counters);
  }

  uint64_t getTestDurationNs() const override {
    std::lock_guard<std::mutex> l(timeMutex_);
    return std::chrono::nanoseconds{
        std::min(std::chrono::system_clock::now(), endTime_) - startTime_}
        .count();
  }

 private:
  static std::string genHardcodedString() {
    const std::string s = "The quick brown fox jumps over the lazy dog. ";
    std::string val;
    for (int i = 0; i < 4 * 1024 * 1024; i += s.size()) {
      val += s;
    }
    return val;
  }

  folly::SharedMutex& getLock(Key key) {
    auto bucket = MurmurHash2{}(key.data(), key.size()) % locks_.size();
    return locks_[bucket];
  }

  // TODO maintain state on whether key has chained allocs and use it to only
  // lock for keys with chained items.
  auto chainedItemAcquireSharedLock(Key key) {
    using Lock = std::shared_lock<folly::SharedMutex>;
    return lockEnabled_ ? Lock{getLock(key)} : Lock{};
  }
  auto chainedItemAcquireUniqueLock(Key key) {
    using Lock = std::unique_lock<folly::SharedMutex>;
    return lockEnabled_ ? Lock{getLock(key)} : Lock{};
  }

  // populate the input item handle according to the stress setup.
  void populateItem(WriteHandle& handle) {
    if (!config_.populateItem) {
      return;
    }
    XDCHECK(handle);
    XDCHECK_LE(cache_->getSize(handle), 4ULL * 1024 * 1024);
    if (cache_->consistencyCheckEnabled()) {
      cache_->setUint64ToItem(handle, folly::Random::rand64(rng));
    } else {
      cache_->setStringItem(handle, hardcodedString_);
    }
  }

  // Runs a number of operations on the cache allocator. The actual
  // operations and key/value used are determined by the workload generator
  // initialized.
  //
  // Throughput and Hit/Miss rates are tracked here as well
  //
  // @param stats       Throughput stats
  void stressByDiscreteDistribution(ThroughputStats& stats, int thread_num = -1) {
    // thread_num is used to select one worker thread to perform vtune pause and resume,
    // which pause and resume the entire program.
    //std::cout << "[DEBUG] Worker thread number is " << thread_num << std::endl;
    //std::mt19937_64 gen(folly::Random::rand64());
    std::mt19937_64 gen(0xdeadbeef);
    std::discrete_distribution<> opPoolDist(config_.opPoolDistribution.begin(),
                                            config_.opPoolDistribution.end());
    const uint64_t opDelayBatch = config_.opDelayBatch;
    const uint64_t opDelayNs = config_.opDelayNs;
    const std::chrono::nanoseconds opDelay(opDelayNs);

    const bool needDelay = opDelayBatch != 0 && opDelayNs != 0;
    uint64_t opCounter = 0;
    auto throttleFn = [&] {
      if (needDelay && ++opCounter == opDelayBatch) {
        opCounter = 0;
        std::this_thread::sleep_for(opDelay);
      }
      // Limit the rate if specified.
      limitRate();
    };

    bool vtune_running_warmup = false;
    bool vtune_running_cleanup = false;
    Stats prevCacheStats;
    Stats currCacheStats;
    float currHitRatio = 100;
    float prevHitRatio = -100;
    uint64_t newMisses, newGets;
    //if (thread_num == 0){
    //    std::cout << "[INFO: VTUNE] Vtune analysis will start after the first 1/8 of operations are completed and \
    //        will be paused before the last 1/8 of ops." << std::endl;
    //    std::cout << "[INFO: VTUNE] 1/8th of all operations (per thread) is " << config_.numOps/8 << std::endl;
    //}
    //char *tmpVal = new char[16777215]; // max allowed value size. Used to read the retrieved item.
    std::string tmpVal;
    tmpVal.resize(16777215);

    std::optional<uint64_t> lastRequestId = std::nullopt;
    for (uint64_t i = 0;
         i < config_.numOps &&
         cache_->getInconsistencyCount() < config_.maxInconsistencyCount &&
         cache_->getInvalidDestructorCount() <
             config_.maxInvalidDestructorCount &&
         !cache_->isNvmCacheDisabled() && !shouldTestStop();
         ++i) {
        // start vtune profiling after the difference between two consecutive 
        // iterations is less than 0.5%.
        // Only thread 0 will perform this.
        if (thread_num == 0 && (i % 524288 == 0)) {
            // only check this for every x ops
            currCacheStats = this->getCacheStats();
            newMisses = currCacheStats.numCacheGetMiss - prevCacheStats.numCacheGetMiss;
            newGets = currCacheStats.numCacheGets -  prevCacheStats.numCacheGets;
            currHitRatio = 100 - (newGets == 0
                           ? 100.0
                           : 100.0*static_cast<double>(newMisses)/static_cast<double>(newGets));
            //std::cout << "[DEBUG: VTUNE] hit ratio is " << currHitRatio << ". diff is " << currHitRatio - prevHitRatio << std::endl;
            
            if (!vtune_running_warmup && (currHitRatio - prevHitRatio < 0) && (currHitRatio - prevHitRatio > -0.5)){
                 __itt_resume();
                 std::cout << "[INFO: VTUNE] Vtune analysis resumed." << std::endl;
                 vtune_running_warmup = true;
            }
            if (!vtune_running_cleanup && i >= (config_.numOps - 1048576)){
                // Stop vtunes collection before the last 1M operations to skip clean up
                 __itt_pause();
                 std::cout << "[INFO: VTUNE] Vtune analysis paused to skip cleanup." << std::endl;
                 vtune_running_cleanup = true;
            }
            prevCacheStats = currCacheStats;
            prevHitRatio = currHitRatio ;
        }
      try {
        // at the end of every operation, throttle per the config.
        SCOPE_EXIT { throttleFn(); };
          // detect refcount leaks when run in  debug mode.
#ifndef NDEBUG
        auto checkCnt = [](int cnt) {
          if (cnt != 0) {
            throw std::runtime_error(folly::sformat("Refcount leak {}", cnt));
          }
        };
        checkCnt(cache_->getHandleCountForThread());
        SCOPE_EXIT { checkCnt(cache_->getHandleCountForThread()); };
#endif
        ++stats.ops;

        const auto pid = static_cast<PoolId>(opPoolDist(gen));
        const Request& req(getReq(pid, gen, lastRequestId));
        OpType op = req.getOp();
        const std::string* key = &(req.key);
        std::string oneHitKey;
        if (op == OpType::kLoneGet || op == OpType::kLoneSet) {
          oneHitKey = Request::getUniqueKey();
          key = &oneHitKey;
        }

        OpResultType result(OpResultType::kNop);
        switch (op) {
        case OpType::kLoneSet:
        case OpType::kSet: {
          if (config_.onlySetIfMiss) {
            auto it = cache_->find(*key);
            if (it != nullptr) {
              continue;
            }
          }
          auto lock = chainedItemAcquireUniqueLock(*key);
          result = setKey(pid, stats, key, *(req.sizeBegin), req.ttlSecs,
                          req.admFeatureMap);

          break;
        }
        case OpType::kLoneGet:
        case OpType::kGet: {
          ++stats.get;

          auto slock = chainedItemAcquireSharedLock(*key);
          auto xlock = decltype(chainedItemAcquireUniqueLock(*key)){};

          if (ticker_) {
            ticker_->updateTimeStamp(req.timestamp);
          }
          // TODO currently pure lookaside, we should
          // add a distribution over sequences of requests/access patterns
          // e.g. get-no-set and set-no-get
          cache_->recordAccess(*key);
          auto it = cache_->find(*key);
          if (it == nullptr) {
            ++stats.getMiss;
            result = OpResultType::kGetMiss;

            if (config_.enableLookaside) {
              // allocate and insert on miss
              // upgrade access privledges, (lock_upgrade is not
              // appropriate here)
              slock = {};
              xlock = chainedItemAcquireUniqueLock(*key);
              setKey(pid, stats, key, *(req.sizeBegin), req.ttlSecs,
                     req.admFeatureMap);
            }
          } else {
            result = OpResultType::kGetHit;
            // storing the retrieved key into a variable so that the content of the item is actually 
            // accessed from memory. Otherwise only the pointer to the item is accessed.
            //std::memcpy(tmpVal, it->getMemory(), it->getSize());
            std::memcpy(&tmpVal[0], it->getMemory(), it->getSize());
          }

          break;
        }
        case OpType::kDel: {
          ++stats.del;
          auto lock = chainedItemAcquireUniqueLock(*key);
          auto res = cache_->remove(*key);
          if (res == CacheT::RemoveRes::kNotFoundInRam) {
            ++stats.delNotFound;
          }
          break;
        }
        case OpType::kAddChained: {
          ++stats.get;
          auto lock = chainedItemAcquireUniqueLock(*key);
          auto it = cache_->findToWrite(*key);
          if (!it) {
            ++stats.getMiss;

            ++stats.set;
            it = cache_->allocate(pid, *key, *(req.sizeBegin), req.ttlSecs);
            if (!it) {
              ++stats.setFailure;
              break;
            }
            populateItem(it);
            cache_->insertOrReplace(it);
          }
          XDCHECK(req.sizeBegin + 1 != req.sizeEnd);
          bool chainSuccessful = false;
          for (auto j = req.sizeBegin + 1; j != req.sizeEnd; j++) {
            ++stats.addChained;

            const auto size = *j;
            auto child = cache_->allocateChainedItem(it, size);
            if (!child) {
              ++stats.addChainedFailure;
              continue;
            }
            chainSuccessful = true;
            populateItem(child);
            cache_->addChainedItem(it, std::move(child));
          }
          if (chainSuccessful && cache_->consistencyCheckEnabled()) {
            cache_->trackChainChecksum(it);
          }
          break;
        }
        case OpType::kUpdate: {
          ++stats.get;
          ++stats.update;
          auto lock = chainedItemAcquireUniqueLock(*key);
          if (ticker_) {
            ticker_->updateTimeStamp(req.timestamp);
          }
          auto it = cache_->findToWrite(*key);
          if (it == nullptr) {
            ++stats.getMiss;
            ++stats.updateMiss;
            break;
          }
          cache_->updateItemRecordVersion(it);
          break;
        }
        case OpType::kCouldExist: {
          ++stats.couldExistOp;
          if (!cache_->couldExist(*key)) {
            ++stats.couldExistOpFalse;
          }
          break;
        }
        default:
          throw std::runtime_error(
              folly::sformat("invalid operation generated: {}", (int)op));
          break;
        }

        lastRequestId = req.requestId;
        if (req.requestId) {
          // req might be deleted after calling notifyResult()
          wg_->notifyResult(*req.requestId, result);
        }
      } catch (const cachebench::EndOfTrace& ex) {
        break;
      }
    }
    wg_->markFinish();
    // reading tmp value here so hopefully the compiler does not optimize it out
    //std::cout << folly::sformat("[DEBUG] Last read value is {}", tmpVal) << std::endl;
    std::cout << "[DEBUG] Last read value is " <<  tmpVal.substr(0, 100) << std::endl; // printing the first 100 chars
  }

  // inserts key into the cache if the admission policy also indicates the
  // key is worthy to be cached.
  //
  // @param pid         pool id to insert the key
  // @param stats       reference to the stats structure.
  // @param key         the key to be inserted
  // @param size        size of the cache value
  // @param ttlSecs     ttl for the value
  // @param featureMap  feature map for admission policy decisions.
  OpResultType setKey(
      PoolId pid,
      ThroughputStats& stats,
      const std::string* key,
      size_t size,
      uint32_t ttlSecs,
      const std::unordered_map<std::string, std::string>& featureMap) {
    // check the admission policy first, and skip the set operation
    // if the policy returns false
    if (config_.admPolicy && !config_.admPolicy->accept(featureMap)) {
      return OpResultType::kSetSkip;
    }

    ++stats.set;
    auto it = cache_->allocate(pid, *key, size, ttlSecs);
    if (it == nullptr) {
      ++stats.setFailure;
      return OpResultType::kSetFailure;
    } else {
      populateItem(it);
      cache_->insertOrReplace(it);
      return OpResultType::kSetSuccess;
    }
  }

  // fetch a request from the workload generator for a particular pool
  // @param pid             the pool id chosen for the request.
  // @param gen             the thread local random number generator to be
  // fed
  //                        to the workload generator  for constructing the
  //                        request.
  // @param lastRequestId   optional information about the last request id
  // that
  //                        was given to this thread by the workload
  //                        generator. This is used to provide continuity by
  //                        some generator implementations.

  const Request& getReq(const PoolId& pid,
                        std::mt19937_64& gen,
                        std::optional<uint64_t>& lastRequestId) {
    while (true) {
      const Request& req(wg_->getReq(pid, gen, lastRequestId));
      if (config_.checkConsistency && cache_->isInvalidKey(req.key)) {
        continue;
      }
      // TODO: allow callback on nvm eviction instead of checking it repeatedly.
      if (config_.checkNvmCacheWarmUp &&
          folly::Random::oneIn(kNvmCacheWarmUpCheckRate)) {
        checkNvmCacheWarmedUp(req.timestamp);
      }
      return req;
    }
  }

  void limitRate() {
    if (!rateLimiter_) {
      return;
    }
    rateLimiter_->consumeWithBorrowAndWait(1);
  }

  void checkNvmCacheWarmedUp(uint64_t requestTimestamp) {
    if (hasNvmCacheWarmedUp_) {
      // already notified, nothing to do
      return;
    }
    if (cache_->isNvmCacheDisabled()) {
      return;
    }
    if (cache_->hasNvmCacheWarmedUp()) {
      wg_->setNvmCacheWarmedUp(requestTimestamp);
      XLOG(INFO) << "NVM cache has been warmed up";
      hasNvmCacheWarmedUp_ = true;
    }
  }

  const StressorConfig config_; // config for the stress run

  std::vector<ThroughputStats> throughputStats_; // thread local stats

  std::unique_ptr<GeneratorBase> wg_; // workload generator

  // locks when using chained item and moving.
  std::array<folly::SharedMutex, 1024> locks_;

  // if locking is enabled.
  std::atomic<bool> lockEnabled_{false};

  // memorize rng to improve random performance
  folly::ThreadLocalPRNG rng;

  // string used for generating random payloads
  const std::string hardcodedString_;

  std::unique_ptr<CacheT> cache_;

  // Ticker that syncs the time according to trace timestamp.
  std::shared_ptr<TimeStampTicker> ticker_;

  // main stressor thread
  std::thread stressWorker_;

  // mutex to protect reading the timestamps.
  mutable std::mutex timeMutex_;

  // start time for the stress test
  std::chrono::time_point<std::chrono::system_clock> startTime_;

  // time when benchmark finished. This is set once the benchmark finishes
  std::chrono::time_point<std::chrono::system_clock> endTime_;

  // Token bucket used to limit the operations per second.
  std::unique_ptr<folly::BasicTokenBucket<>> rateLimiter_;

  // Whether flash cache has been warmed up
  bool hasNvmCacheWarmedUp_{false};
};
} // namespace cachebench
} // namespace cachelib
} // namespace facebook
