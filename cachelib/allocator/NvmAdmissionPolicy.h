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

#include <folly/Range.h>

#include "cachelib/common/ApproxSplitSet.h"
#include "cachelib/common/AtomicCounter.h"
#include "cachelib/common/PercentileStats.h"
#include "cachelib/common/Time.h"

namespace facebook {
namespace cachelib {

// Base class for admission policy into nvm cache
// It provides:
// 1. Interface function for testing whether an item should be admitted.
// 2. Stats gathering functionalities.
// 3. minTTL functionality: when set, items with configured TTL smaller than
// minTTL will be rejected.
template <typename Cache>
class NvmAdmissionPolicy {
 public:
  using Item = typename Cache::Item;
  using ChainedItemIter = typename Cache::ChainedItemIter;
  virtual ~NvmAdmissionPolicy() = default;

  // The method that the outside class calls to get the admission decision.
  // It captures the common logics (e.g statistics) then
  // delicates the detailed implementation to subclasses in acceptImpl.
  virtual bool accept(const Item& item,
                      folly::Range<ChainedItemIter> chainItem) final {
    util::LatencyTracker overallTracker(overallLatency_);
    overallCount_.inc();

    auto minTTL = minTTL_.load(std::memory_order_relaxed);
    auto itemTTL = static_cast<uint64_t>(item.getConfiguredTTL().count());
    if (minTTL > 0 && itemTTL < minTTL) {
      ttlRejected_.inc();
      return false;
    }

    const bool decision = acceptImpl(item, chainItem);
    if (decision) {
      accepted_.inc();
    } else {
      rejected_.inc();
    }
    return decision;
  }

  // same as the above, but makes admission decisions given just the key and
  // not the entire item. This can be used when we can infer the information
  // needed for admission decision from only the key.
  // Note: minTTL will be ignored if a callsite uses this function to test
  // a key.
  virtual bool accept(typename Item::Key key) final {
    util::LatencyTracker overallTracker(overallLatency_);
    overallCount_.inc();

    const bool decision = acceptImpl(key);
    if (decision) {
      accepted_.inc();
    } else {
      rejected_.inc();
    }
    return decision;
  }

  // The method that exposes stats.
  virtual std::unordered_map<std::string, double> getCounters() final {
    auto ctrs = getCountersImpl();
    ctrs["ap.called"] = overallCount_.get();
    ctrs["ap.accepted"] = accepted_.get();
    ctrs["ap.rejected"] = rejected_.get();

    auto visitorUs = [&ctrs](folly::StringPiece name, double count) {
      ctrs[name.toString()] = count / 1000;
    };
    overallLatency_.visitQuantileEstimator(visitorUs, "ap.latency_us");
    ctrs["ap.ttlRejected"] = ttlRejected_.get();
    return ctrs;
  }

  // Track access for an item.
  // This is useful when the admission policy requires access pattern to make
  // admission decision.
  // @param key   key corresponding to the item
  virtual void trackAccess(typename Item::Key) {}

  // Set minTTL. This method should be called only once.
  void initMinTTL(uint64_t minTTL) {
    auto initValue = minTTL_.load(std::memory_order_relaxed);
    if (initValue > 0) {
      throw std::invalid_argument(
          "minTTL should be initialized to non-zero only once.");
    }
    auto success = minTTL_.compare_exchange_strong(initValue, minTTL);
    if (!success) {
      throw std::invalid_argument("Setting minTTL failed.");
    }
  }

  uint64_t getMinTTL() const { return minTTL_; }

  // Implement this method for the detailed admission decision logic.
  // By default this accepts all items.
  virtual bool acceptImpl(const Item&, folly::Range<ChainedItemIter>) {
    return true;
  }

  // Implement this method for the detailed admission decision logic.
  // By default this accepts all items.
  virtual bool acceptImpl(typename Item::Key) { return true; }

  // Implementation specific statistics.
  // Please include a prefix/postfix with the name of implementation to avoid
  // collision with base level stats.
  virtual std::unordered_map<std::string, double> getCountersImpl() {
    return {};
  }

 private:
  util::PercentileStats overallLatency_;
  AtomicCounter overallCount_{0};
  AtomicCounter accepted_{0};
  AtomicCounter rejected_{0};
  AtomicCounter ttlRejected_{0};
  // The minimum TTL to an item need to have to be accepted.
  std::atomic<uint64_t> minTTL_{0};
};

// an admission policy that keeps track of a number (numEntries) of unique keys
// recently observed and rejects an item if the key is not recently observed,
// backed by cachelib/common/ApproxSplitSet
template <typename Cache>
class RejectFirstAP final : public NvmAdmissionPolicy<Cache> {
 public:
  using Item = typename Cache::Item;
  using ChainedItemIter = typename Cache::ChainedItemIter;

  // @param numEntries number of items to be tracked
  // @param numSplits number of splits in the ApporxSplitSet
  // @param suffixIgnoreLength  length of the suffix to be ignored in the key.
  // Useful when an admission policy treates items with the same key prefix as
  // the same item.
  RejectFirstAP(uint64_t numEntries,
                uint32_t numSplits,
                size_t suffixIgnoreLength,
                bool useDramHitSignal)
      : tracker_{numEntries, numSplits},
        suffixIgnoreLength_{suffixIgnoreLength},
        useDramHitSignal_{useDramHitSignal} {}

 protected:
  bool acceptImpl(const Item& it,
                  folly::Range<ChainedItemIter>) final override {
    const bool wasDramHit =
        useDramHitSignal_ && it.getLastAccessTime() > it.getCreationTime();

    const auto seenBefore = trackAndCheckIfSeenBefore(it.getKey());
    if (!seenBefore && wasDramHit) {
      admitsByDramHits_.inc();
    }

    return seenBefore || wasDramHit;
  }

  bool acceptImpl(typename Item::Key key) final override {
    return trackAndCheckIfSeenBefore(key);
  }

  std::unordered_map<std::string, double> getCountersImpl() final override {
    std::unordered_map<std::string, double> ctrs;
    ctrs["ap.reject_first_keys_tracked"] = tracker_.numKeysTracked();
    ctrs["ap.reject_first_tracking_window_secs"] =
        tracker_.trackingWindowDurationSecs();
    ctrs["ap.reject_first_admits_by_dram_hit"] = admitsByDramHits_.get();
    return ctrs;
  }

 private:
  // @param key   the key to check and start tracking
  // @return true if the key was seen before, false otherwise.
  bool trackAndCheckIfSeenBefore(typename Item::Key key) {
    const size_t len = key.size() > suffixIgnoreLength_
                           ? key.size() - suffixIgnoreLength_
                           : key.size();
    const auto keyHash = folly::hash::SpookyHashV2::Hash64(key.data(), len, 0);

    // if key already existed, return true. If we are inserting it for
    // the first time, we insert to track and return false
    bool seenBefore = tracker_.insert(keyHash);
    return seenBefore;
  }

  ApproxSplitSet tracker_;
  const size_t suffixIgnoreLength_;
  AtomicCounter admitsByDramHits_{0};
  const bool useDramHitSignal_{true};
};
} // namespace cachelib
} // namespace facebook
