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

#include "cachelib/cachebench/workload/WorkloadGenerator.h"

#include <algorithm>
#include <chrono>
#include <iostream>
namespace facebook {
namespace cachelib {
namespace cachebench {

WorkloadGenerator::WorkloadGenerator(const StressorConfig& config)
    : config_{config} {
  for (const auto& c : config.poolDistributions) {
    if (c.keySizeRange.size() != c.keySizeRangeProbability.size() + 1) {
      throw std::invalid_argument(
          "Key size range and their probabilities do not match up. Check your "
          "test config.");
    }
    workloadDist_.push_back(WorkloadDistribution(c));
  }

  if (config_.numKeys > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument(folly::sformat(
        "Too many keys specified: {}. Maximum allowed is 4 Billion.",
        config_.numKeys));
  }

  generateReqs();
  generateKeyDistributions();
}

const Request& WorkloadGenerator::getReq(uint8_t poolId,
                                         std::mt19937_64& gen,
                                         std::optional<uint64_t>) {
  XDCHECK_LT(poolId, keyIndicesForPool_.size());
  XDCHECK_LT(poolId, keyGenForPool_.size());

  size_t idx = keyIndicesForPool_[poolId][keyGenForPool_[poolId](gen)];
  if (hotKeyAccessed_[idx] == 1) {
    //std::cout << folly::sformat("already seen this key, index is {}. ttl is {}", idx, reqs_[idx].ttlSecs) << std::endl;
    reqs_[idx].ttlSecs = 0; // set TTL back to 0 so we dont print its address again.
  }
  // check if this key is one of the hot keys
  if (keyAccessFreq_[idx] > hotItemThresh && hotKeyAccessed_[idx] == 0) {
    // this hot key's address has not been printed 
    //std::cout << folly::sformat("[DEBUG] hot key found. Index is {}, key is {} or {}", idx, keys_[idx], reqs_[idx].key) << std::endl;
    reqs_[idx].ttlSecs = 12345; // using TTL as indication that this key is hot
    hotKeyAccessed_[idx] = 1; // record that we have already seen this key once
  } 
  
  auto op =
      static_cast<OpType>(workloadDist_[workloadIdx(poolId)].sampleOpDist(gen));
  reqs_[idx].setOp(op);
  return reqs_[idx];
}

void WorkloadGenerator::generateKeys() {
  uint8_t pid = 0;
  auto fn = [pid, this](size_t start, size_t end) {
    // All keys are printable lower case english alphabet.
    std::uniform_int_distribution<char> charDis('a', 'z');
    std::mt19937_64 gen(folly::Random::rand64());
    for (uint64_t i = start; i < end; i++) {
      size_t keySize =
          util::narrow_cast<size_t>(workloadDist_[pid].sampleKeySizeDist(gen));
      keys_[i].resize(keySize);
      for (auto& c : keys_[i]) {
        c = charDis(gen);
      }
    }
  };

  size_t totalKeys(0);
  std::chrono::seconds keyGenDuration(0);
  keys_.resize(config_.numKeys);
  for (size_t i = 0; i < config_.keyPoolDistribution.size(); i++) {
    pid = util::narrow_cast<uint8_t>(workloadIdx(i));
    size_t numKeysForPool =
        firstKeyIndexForPool_[i + 1] - firstKeyIndexForPool_[i];
    totalKeys += numKeysForPool;
    keyGenDuration += detail::executeParallel(
        fn, config_.numThreads, numKeysForPool, firstKeyIndexForPool_[i]);
  }

  auto startTime = std::chrono::steady_clock::now();
  for (size_t i = 0; i < config_.keyPoolDistribution.size(); i++) {
    auto poolKeyBegin = keys_.begin() + firstKeyIndexForPool_[i];
    // past the end iterator
    auto poolKeyEnd = keys_.begin() + (firstKeyIndexForPool_[i + 1]);
    std::sort(poolKeyBegin, poolKeyEnd);
    auto newEnd = std::unique(poolKeyBegin, poolKeyEnd);
    // update pool key boundary before invalidating iterators
    for (size_t j = i + 1; j < firstKeyIndexForPool_.size(); j++) {
      firstKeyIndexForPool_[j] -= std::distance(newEnd, poolKeyEnd);
    }
    totalKeys -= std::distance(newEnd, poolKeyEnd);
    keys_.erase(newEnd, poolKeyEnd);
  }
  auto sortDuration = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - startTime);

  // initialize all elements to 0
  keyAccessFreq_.resize(config_.numKeys, 0);
  hotKeyAccessed_.resize(config_.numKeys, 0);

  std::cout << folly::sformat("Created {:,} keys in {:.2f} mins",
                              totalKeys,
                              (keyGenDuration + sortDuration).count() / 60.)
            << std::endl;
}

void WorkloadGenerator::generateReqs() {
  generateFirstKeyIndexForPool();
  generateKeys();
  std::mt19937_64 gen(folly::Random::rand64());
  for (size_t i = 0; i < config_.keyPoolDistribution.size(); i++) {
    size_t idx = workloadIdx(i);
    for (size_t j = firstKeyIndexForPool_[i]; j < firstKeyIndexForPool_[i + 1];
         j++) {
      std::vector<size_t> chainSizes;
      chainSizes.push_back(
          util::narrow_cast<size_t>(workloadDist_[idx].sampleValDist(gen)));
      int chainLen =
          util::narrow_cast<int>(workloadDist_[idx].sampleChainedLenDist(gen));
      for (int k = 0; k < chainLen; k++) {
        chainSizes.push_back(util::narrow_cast<size_t>(
            workloadDist_[idx].sampleChainedValDist(gen)));
      }
      sizes_.emplace_back(chainSizes);
      auto reqSizes = sizes_.end() - 1;
      reqs_.emplace_back(keys_[j], reqSizes->begin(), reqSizes->end());
    }
  }
}

void WorkloadGenerator::generateFirstKeyIndexForPool() {
  auto sumProb = std::accumulate(config_.keyPoolDistribution.begin(),
                                 config_.keyPoolDistribution.end(), 0.);
  auto accumProb = 0.;
  firstKeyIndexForPool_.push_back(0);
  for (auto prob : config_.keyPoolDistribution) {
    accumProb += prob;
    firstKeyIndexForPool_.push_back(
        util::narrow_cast<uint32_t>(config_.numKeys * accumProb / sumProb));
  }
}

void WorkloadGenerator::generateKeyDistributions() {
  // We are trying to generate a gaussian distribution for each pool's part
  // in the overall cache ops. To keep the amount of memory finite, we only
  // generate a max of 4 billion op traces across all the pools and replay
  // the same when we need longer traces.
  std::chrono::seconds duration{0};
  for (uint64_t i = 0; i < config_.opPoolDistribution.size(); i++) {
    auto left = firstKeyIndexForPool_[i];
    auto right = firstKeyIndexForPool_[i + 1] - 1;
    size_t idx = workloadIdx(i);

    size_t numOpsForPool = std::min<size_t>(
        util::narrow_cast<size_t>(config_.numOps * config_.numThreads *
                                  config_.opPoolDistribution[i]),
        std::numeric_limits<uint32_t>::max());
    std::cout << folly::sformat("Generating {:.2f}M sampled accesses",
                                numOpsForPool / 1e6)
              << std::endl;
    keyGenForPool_.push_back(std::uniform_int_distribution<uint32_t>(
        0, util::narrow_cast<uint32_t>(numOpsForPool) - 1));
    keyIndicesForPool_.push_back(std::vector<uint32_t>(numOpsForPool));

    duration += detail::executeParallel(
        [&, this](size_t start, size_t end) {
          std::mt19937_64 gen(folly::Random::rand64());
          auto popDist = workloadDist_[idx].getPopDist(left, right);
          for (uint64_t j = start; j < end; j++) {
            keyIndicesForPool_[i][j] =
                util::narrow_cast<uint32_t>((*popDist)(gen));
          }
        },
        config_.numThreads, numOpsForPool);

    // scan keyIndicesForPool_ and record the # of accesses of each key.
    // assuming only one pool.
    for (uint64_t j = 0; j < numOpsForPool; j++) {
      int keyIndex = keyIndicesForPool_[0][j];
      (keyAccessFreq_[keyIndex])++;
    }

    long numHotItems = 0;
    // figure out how many keys are considered hot.
    for (uint64_t j = 0; j < config_.numKeys; j++) {
      if (keyAccessFreq_[j] >= hotItemThresh) {
        numHotItems++;
      }
    }
    std::cout << folly::sformat("[DEBUG] {} items are hot, having >= {} accesses generated.", numHotItems, hotItemThresh) << std::endl;
  }
  
  std::cout << folly::sformat("Generated access patterns in {:.2f} mins",
                              duration.count() / 60.)
            << std::endl;

  //std::cout << folly::sformat("scanning the accesses generated.") << std::endl;
  //std::cout << folly::sformat("keyindex, key, #accesses") << std::endl;
  //for (int i=0; i < keyAccessFreq_.size(); i++) {
  //    //std::cout << folly::sformat("index {}, key {}, frequency {} ", i, keys_[i], keyAccessFreq_[i]) << std::endl;
  //    std::cout << folly::sformat("{},{},{} ", i, keys_[i], keyAccessFreq_[i]) << std::endl;
  //}

  
}

} // namespace cachebench
} // namespace cachelib
} // namespace facebook
