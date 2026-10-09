/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include "cachelib/cachebench/runner/ProgressTracker.h"

namespace facebook {
namespace cachelib {
namespace cachebench {
ProgressTracker::ProgressTracker(const Stressor& s,
                                 const std::string& detailedStatsFile)
    : stressor_(s) {
  if (!detailedStatsFile.empty()) {
    statsFile_.open(detailedStatsFile, std::ios::app);
  }
}

ProgressTracker::~ProgressTracker() {
  try {
    if (statsFile_.is_open()) {
      statsFile_.close();
    }
    stop();
  } catch (const std::exception&) {
  }
}

void ProgressTracker::work() {
  auto now = std::chrono::system_clock::now();
  auto nowTimeT = std::chrono::system_clock::to_time_t(now);
  char buf[16];
  struct tm time;
  ::localtime_r(&nowTimeT, &time);
  ::strftime(buf, sizeof(buf), "%H:%M:%S", &time);
  auto throughputStats = stressor_.aggregateThroughputStats();
  // CoFDP: progress is reported in thousands of ops (upstream: millions).
  auto kOpsCompleted = throughputStats.ops / 1e3;

  const auto currCacheStats = stressor_.getCacheStats();
  auto [overallHitRatio, ramHitRatio, nvmHitRatio] =
      currCacheStats.getHitRatios(prevStats_);
  auto thStr = folly::sformat(
      "{} {:>10.2f}K ops completed. Hit Ratio {:6.2f}% "
      "(RAM {:6.2f}%, NVM {:6.2f}%)",
      buf,
      kOpsCompleted,
      overallHitRatio,
      ramHitRatio,
      nvmHitRatio);

  // CoFDP: latency lines. The estimates come from util::PercentileStats, i.e.
  // a sliding window of 60 x 1s (about the last minute), not only the last
  // progress interval.
  // NVM device latency (always tracked by Navy)
  auto fmtLat = [](double v) { return folly::sformat("{:.1f}", v); };
  std::string nvmReadStr = folly::sformat(
      "  NVM  Read Lat(us): avg={} p50={} p90={} p95={} p99={} p99.9={} "
      "p99.99={}",
      fmtLat(currCacheStats.nvmReadLatencyMicrosAvg),
      fmtLat(currCacheStats.nvmReadLatencyMicrosP50),
      fmtLat(currCacheStats.nvmReadLatencyMicrosP90),
      fmtLat(currCacheStats.nvmReadLatencyMicrosP95),
      fmtLat(currCacheStats.nvmReadLatencyMicrosP99),
      fmtLat(currCacheStats.nvmReadLatencyMicrosP999),
      fmtLat(currCacheStats.nvmReadLatencyMicrosP9999));
  std::string nvmWriteStr = folly::sformat(
      "  NVM Write Lat(us): avg={} p50={} p90={} p95={} p99={} p99.9={} "
      "p99.99={}",
      fmtLat(currCacheStats.nvmWriteLatencyMicrosAvg),
      fmtLat(currCacheStats.nvmWriteLatencyMicrosP50),
      fmtLat(currCacheStats.nvmWriteLatencyMicrosP90),
      fmtLat(currCacheStats.nvmWriteLatencyMicrosP95),
      fmtLat(currCacheStats.nvmWriteLatencyMicrosP99),
      fmtLat(currCacheStats.nvmWriteLatencyMicrosP999),
      fmtLat(currCacheStats.nvmWriteLatencyMicrosP9999));

  // "SET": latency of CacheAllocator::allocate (always tracked)
  const auto& setLat = currCacheStats.cacheAllocateLatencyNs;
  std::string setLatStr = folly::sformat(
      "  SET Lat(ns):       avg={} p50={} p90={} p95={} p99={} p99.9={} "
      "p99.99={}",
      setLat.avg, setLat.p50, setLat.p90, setLat.p95, setLat.p99, setLat.p999,
      setLat.p9999);

  // "GET": latency of the find API (only with --report_api_latency)
  const auto& getLat = currCacheStats.cacheFindLatencyNs;
  std::string getLatStr;
  if (getLat.avg > 0) {
    getLatStr = folly::sformat(
        "  GET Lat(ns):       avg={} p50={} p90={} p95={} p99={} p99.9={} "
        "p99.99={}",
        getLat.avg, getLat.p50, getLat.p90, getLat.p95, getLat.p99, getLat.p999,
        getLat.p9999);
  }

  // log this always to stdout
  std::cout << thStr << "\n"
            << nvmReadStr << "\n"
            << nvmWriteStr << "\n"
            << setLatStr;
  if (!getLatStr.empty()) {
    std::cout << "\n" << getLatStr;
  }
  std::cout << std::endl;

  // additionally log into the stats file
  if (statsFile_.is_open()) {
    statsFile_ << thStr << std::endl;
    statsFile_ << "== Allocator Stats ==" << std::endl;
    currCacheStats.render(statsFile_);

    statsFile_ << "== Hit Ratio Stats Since Last ==" << std::endl;
    currCacheStats.render(prevStats_, statsFile_);

    statsFile_ << "== Throughput Stats ==" << std::endl;
    auto elapsedTimeNs = stressor_.getTestDurationNs();
    throughputStats.render(elapsedTimeNs, statsFile_);

    stressor_.renderWorkloadGeneratorStats(elapsedTimeNs, statsFile_);
    statsFile_ << std::endl;
  }

  prevStats_ = currCacheStats;
}
} // namespace cachebench
} // namespace cachelib
} // namespace facebook
