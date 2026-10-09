//  Copyright (c) 2025-present, the CoFDP authors. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// CoFDP experiment instrumentation (see docs/COFDP.md). Every PosixFileSystem
// owns one CoFDPStats and publishes it through FileSystem::cofdp_stats(); for
// any other FileSystem that pointer is nullptr and nothing is recorded.
//
// I/O latency breakdown. IoTimer measures the POSIX calls made by
// PosixFileSystem and its files. When a DB is closed, ~DBImpl prints one line
// per call site that was timed at least once, sorted by name:
//
//   <name> : <total us>/<calls> = <average us>
//
// Each call adds its duration truncated to whole microseconds.
//
// Ops timeline. A thread started with the file system wakes up every second
// and, every 10th time, prints the cumulative number of DB::Write() batches,
// Get() calls and NewIterator() calls and the sum of their increase since the
// previous row:
//
//   <seconds>\t<writes>\t<gets>\t<iterators>\t<delta>\t
//
// followed by one line per operation, labelled "PUT ", "GET " and "SCAN",
// with the latency of the operations finished in that interval
// ("  <label>(<n> ops): avg=..us p50=..us p90=..us p95=..us p99=..us
// p99.9=..us p99.99=..us"), or "  <label>: no ops". The average is exact;
// each percentile is the lower bound of the power-of-two nanosecond bucket
// that contains it (KNOWN_ISSUES RDB-9). SCAN times the creation of the
// iterator only, not Seek() or Next() (KNOWN_ISSUES RDB-8).
//
// These lines are measurement output; keep their format.

#pragma once

#include <time.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <thread>
#include <vector>

#include "rocksdb/rocksdb_namespace.h"
#include "util/math.h"

namespace ROCKSDB_NAMESPACE {

class CoFDPStats {
 public:
  // Timed POSIX call sites of the I/O latency breakdown.
  enum IoCall : int {
    kMeta,                  // open/unlink/stat/rename/lock of PosixFileSystem
    kWrite,                 // write() of PosixWritableFile::Append()
    kPositionedWrite,       // pwrite() of PositionedAppend()
    kSequentialFileRead,    // pread() of PosixSequentialFile
    kRandomAccessFileRead,  // pread() of PosixRandomAccessFile::Read()
    kWritableFileSync,      // fdatasync() of PosixWritableFile::Sync()
    kRandomRWFileRead,      // pread() of PosixRandomRWFile::Read()
    kRandomRWFileSync,      // fdatasync() of PosixRandomRWFile::Sync()
    kNumIoCalls,
  };

  // DB operations of the ops timeline.
  enum DbOp : int {
    kDbWrite,        // DBImpl::Write(), printed as PUT
    kDbGet,          // DBImpl::GetImpl(), printed as GET
    kDbNewIterator,  // DBImpl::NewIterator(), printed as SCAN
    kNumDbOps,
  };

  // Times one POSIX call; does nothing if `stats` is nullptr. The destructor
  // preserves errno, so the caller can inspect the errno of the timed call
  // after the timer has gone out of scope.
  class IoTimer {
   public:
    IoTimer(CoFDPStats* stats, IoCall call) : stats_(stats), call_(call) {
      if (stats_ != nullptr) {
        clock_gettime(CLOCK_MONOTONIC, &start_);
      }
    }
    ~IoTimer() {
      if (stats_ != nullptr) {
        const int saved_errno = errno;
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        const int64_t elapsed_ns = (end.tv_sec - start_.tv_sec) * 1000000000 +
                                   (end.tv_nsec - start_.tv_nsec);
        stats_->RecordIoCall(call_, static_cast<uint64_t>(elapsed_ns / 1000));
        errno = saved_errno;
      }
    }

    IoTimer(const IoTimer&) = delete;
    IoTimer& operator=(const IoTimer&) = delete;

   private:
    CoFDPStats* const stats_;
    const IoCall call_;
    struct timespec start_ = {};
  };

  // Times one DB operation from construction until Stop() or destruction;
  // does nothing if `stats` is nullptr.
  class OpTimer {
   public:
    OpTimer(CoFDPStats* stats, DbOp op)
        : stats_(stats),
          op_(op),
          start_ns_(stats != nullptr ? NowNanos() : 0) {}
    ~OpTimer() { Stop(); }

    OpTimer(const OpTimer&) = delete;
    OpTimer& operator=(const OpTimer&) = delete;

    void Stop() {
      if (stats_ != nullptr) {
        stats_->RecordDbOpLatency(op_, NowNanos() - start_ns_);
        stats_ = nullptr;
      }
    }

   private:
    CoFDPStats* stats_;
    const DbOp op_;
    const uint64_t start_ns_;
  };

  CoFDPStats() = default;
  // Stops the ops timeline, then prints the timeline rows collected so far
  // and the I/O latency breakdown. FileSystem::Default() is only destroyed
  // in builds that free static objects (e.g. ASAN); other PosixFileSystem
  // instances are destroyed normally.
  ~CoFDPStats();

  CoFDPStats(const CoFDPStats&) = delete;
  CoFDPStats& operator=(const CoFDPStats&) = delete;

  // CLOCK_MONOTONIC in nanoseconds.
  static uint64_t NowNanos() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  void RecordIoCall(IoCall call, uint64_t micros) {
    io_calls_[call].count.fetch_add(1, std::memory_order_relaxed);
    io_calls_[call].micros.fetch_add(micros, std::memory_order_relaxed);
  }

  // Prints the I/O latency breakdown described above.
  void PrintIoBreakdown() const;

  // Counts one DB operation for the timeline rows; does nothing if `stats` is
  // nullptr.
  static void CountDbOp(CoFDPStats* stats, DbOp op) {
    if (stats != nullptr) {
      stats->db_op_count_[op]++;
    }
  }

  void RecordDbOpLatency(DbOp op, uint64_t nanos) {
    db_op_latency_sum_ns_[op].fetch_add(nanos, std::memory_order_relaxed);
    const int bucket = nanos == 0 ? 0 : FloorLog2(nanos);
    db_op_latency_hist_[op][bucket].fetch_add(1, std::memory_order_relaxed);
  }

  // Starts the ops timeline thread.
  void StartOpsTimeline();

 private:
  // Bucket b of a latency histogram counts operations that took
  // [2^b, 2^(b+1)) ns; 0 ns counts in bucket 0.
  static constexpr int kLog2LatencyBuckets = 64;

  struct IoCallStats {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> micros{0};
  };

  struct TimelineRow {
    int seconds;
    uint64_t writes;
    uint64_t gets;
    uint64_t new_iterators;
  };

  void RunOpsTimeline();
  // Prints and resets the latency histogram of `op`.
  void PrintIntervalLatency(const char* label, DbOp op);

  IoCallStats io_calls_[kNumIoCalls];

  std::atomic<uint64_t> db_op_count_[kNumDbOps]{};
  std::atomic<uint64_t> db_op_latency_hist_[kNumDbOps][kLog2LatencyBuckets]{};
  std::atomic<uint64_t> db_op_latency_sum_ns_[kNumDbOps]{};

  std::atomic<bool> timeline_running_{false};
  std::thread timeline_thread_;
  // Appended to by the timeline thread only; read after it has been joined.
  std::vector<TimelineRow> timeline_rows_;
};

}  // namespace ROCKSDB_NAMESPACE
