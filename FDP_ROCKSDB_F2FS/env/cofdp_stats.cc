//  Copyright (c) 2025-present, the CoFDP authors. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "env/cofdp_stats.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace ROCKSDB_NAMESPACE {

namespace {

// Printed names of CoFDPStats::IoCall.
const char* const kIoCallNames[CoFDPStats::kNumIoCalls] = {
    "Meta",
    "PosixWrite",
    "PosixPositionedWrite",
    "PosixSequentialFile::PositionedRead",
    "PosixRandomAccessFile::PositionedRead",
    "PosixWritableFile::Sync",
    "PosixRandomRWFile::Read",
    "PosixRandomRWFile::Sync",
};

// Returns the lower bound, in nanoseconds, of the histogram bucket that holds
// the operation of rank max(1, floor(total * p)).
double Log2PercentileLowerBoundNs(const uint64_t* hist, int num_buckets,
                                  uint64_t total, double p) {
  if (total == 0) {
    return 0.0;
  }
  uint64_t target = static_cast<uint64_t>(total * p);
  if (target == 0) {
    target = 1;
  }
  uint64_t cumulative = 0;
  for (int b = 0; b < num_buckets; b++) {
    cumulative += hist[b];
    if (cumulative >= target) {
      return static_cast<double>(uint64_t{1} << b);
    }
  }
  return static_cast<double>(uint64_t{1} << (num_buckets - 1));
}

}  // namespace

CoFDPStats::~CoFDPStats() {
  timeline_running_.store(false);
  if (timeline_thread_.joinable()) {
    timeline_thread_.join();
  }
  for (const TimelineRow& row : timeline_rows_) {
    printf("%d\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t\n", row.seconds,
           row.writes, row.gets, row.new_iterators);
  }
  PrintIoBreakdown();
}

void CoFDPStats::PrintIoBreakdown() const {
  // Sorted by name (byte order).
  std::array<int, kNumIoCalls> order;
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [](int a, int b) {
    return std::strcmp(kIoCallNames[a], kIoCallNames[b]) < 0;
  });
  for (int call : order) {
    const uint64_t count = io_calls_[call].count.load();
    if (count == 0) {
      continue;
    }
    const uint64_t micros = io_calls_[call].micros.load();
    printf("%s : %" PRIu64 "/%" PRIu64 " = %" PRIu64 "\n", kIoCallNames[call],
           micros, count, micros / count);
  }
}

void CoFDPStats::StartOpsTimeline() {
  if (timeline_thread_.joinable()) {
    return;
  }
  timeline_running_.store(true);
  timeline_thread_ = std::thread(&CoFDPStats::RunOpsTimeline, this);
}

void CoFDPStats::RunOpsTimeline() {
  int seconds = 0;
  uint64_t prev_total = 0;
  printf("--------------BackgroundStatTimeLapse------------------------\n");
  while (timeline_running_.load()) {
    sleep(1);
    seconds++;
    if (seconds % 10 != 0) {
      continue;
    }
    const uint64_t writes = db_op_count_[kDbWrite].load();
    const uint64_t gets = db_op_count_[kDbGet].load();
    const uint64_t new_iterators = db_op_count_[kDbNewIterator].load();
    const uint64_t total = writes + gets + new_iterators;
    timeline_rows_.push_back({seconds, writes, gets, new_iterators});
    printf("%d\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t\n",
           seconds, writes, gets, new_iterators, total - prev_total);
    PrintIntervalLatency("PUT ", kDbWrite);
    PrintIntervalLatency("GET ", kDbGet);
    PrintIntervalLatency("SCAN", kDbNewIterator);
    prev_total = total;
    fflush(stdout);
  }
  printf(
      "--------------bye BackgroundStatTimeLapse------------------------ "
      "bye\n");
}

void CoFDPStats::PrintIntervalLatency(const char* label, DbOp op) {
  uint64_t hist[kLog2LatencyBuckets];
  uint64_t total = 0;
  for (int b = 0; b < kLog2LatencyBuckets; b++) {
    hist[b] = db_op_latency_hist_[op][b].exchange(0, std::memory_order_relaxed);
    total += hist[b];
  }
  const uint64_t sum_ns =
      db_op_latency_sum_ns_[op].exchange(0, std::memory_order_relaxed);
  if (total == 0) {
    printf("  %s: no ops\n", label);
    return;
  }
  const double avg_us = static_cast<double>(sum_ns) / total / 1000.0;
  auto percentile_us = [&](double p) {
    return Log2PercentileLowerBoundNs(hist, kLog2LatencyBuckets, total, p) /
           1000.0;
  };
  printf("  %s(%" PRIu64
         " ops): avg=%.1fus p50=%.1fus p90=%.1fus p95=%.1fus"
         " p99=%.1fus p99.9=%.1fus p99.99=%.1fus\n",
         label, total, avg_us, percentile_us(0.50), percentile_us(0.90),
         percentile_us(0.95), percentile_us(0.99), percentile_us(0.999),
         percentile_us(0.9999));
}

}  // namespace ROCKSDB_NAMESPACE
