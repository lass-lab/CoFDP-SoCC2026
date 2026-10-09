//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  Copyright (c) 2025-present, the CoFDP authors. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// TorFS logger for RocksDB's info LOG, derived from RocksDB's EnvLogger
// (logging/env_logger.h).

#pragma once

#include <time.h>

#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <memory>

#include "fs_torfs.h"
#include "port/port.h"
#include "rocksdb/env.h"
#include "rocksdb/perf_level.h"
#include "rocksdb/system_clock.h"

namespace rocksdb {

// Logger returned by TorFS::NewLogger(). RocksDB's info LOG and the TorFS log
// are TorFS files (PID 0), so every file of the DB lives on the FDP device.
// Adapted from RocksDB's EnvLogger (logging/env_logger.h), but appends to the
// FDPWritableFile directly: the log reaches the device when the 8 MiB file
// buffer fills and when the logger is closed.
class TorFSLogger : public Logger {
 public:
  TorFSLogger(std::unique_ptr<FSWritableFile> writable_file, Env* env,
              InfoLogLevel log_level = InfoLogLevel::ERROR_LEVEL)
      : Logger(log_level),
        env_(env),
        clock_(env_->GetSystemClock().get()),
        file_(static_cast<FDPWritableFile*>(writable_file.release())),
        last_flush_micros_(0),
        flush_pending_(false) {}

  ~TorFSLogger() override {
    if (!closed_) {
      closed_ = true;
      CloseHelper().PermitUncheckedError();
    }
  }

 private:
  // A guard to prepare file operations, such as mutex and skip
  // I/O context.
  class FileOpGuard {
   public:
    explicit FileOpGuard(TorFSLogger& logger)
        : logger_(logger), prev_perf_level_(GetPerfLevel()) {
      // Preserve iostats not to pollute writes from user writes. We might
      // need a better solution than this.
      SetPerfLevel(PerfLevel::kDisable);
      logger.mutex_.Lock();
    }
    ~FileOpGuard() {
      logger_.mutex_.Unlock();
      SetPerfLevel(prev_perf_level_);
    }

   private:
    TorFSLogger& logger_;
    PerfLevel prev_perf_level_;
  };

  // Nothing to write here: the file keeps the log in its buffer until it
  // fills or the logger is closed.
  void FlushLocked() {
    mutex_.AssertHeld();
    if (flush_pending_) {
      flush_pending_ = false;
    }
    last_flush_micros_ = clock_->NowMicros();
  }

  void Flush() override {
    FileOpGuard guard(*this);
    FlushLocked();
  }

  Status CloseImpl() override { return CloseHelper(); }

  Status CloseHelper() {
    FileOpGuard guard(*this);
    file_->Close(IOOptions(), nullptr);
    return IOStatus::OK();
  }

  using Logger::Logv;
  void Logv(const char* format, va_list ap) override {
    const uint64_t thread_id = env_->GetThreadID();

    // We try twice: the first time with a fixed-size stack allocated buffer,
    // and the second time with a much larger dynamically allocated buffer.
    char buffer[500];
    for (int iter = 0; iter < 2; iter++) {
      char* base;
      int bufsize;
      if (iter == 0) {
        bufsize = sizeof(buffer);
        base = buffer;
      } else {
        bufsize = 65536;
        base = new char[bufsize];
      }
      char* p = base;
      char* limit = base + bufsize;

      // NOTE: only tm_mon and tm_year are set and the sub-second part comes
      // from CLOCK_MONOTONIC, so the timestamps of this log carry no
      // meaningful date or time (KNOWN_ISSUES TORFS-8).
      struct timespec now_tv;
      clock_gettime(CLOCK_MONOTONIC, &now_tv);
      struct tm t;
      t.tm_mon = 3;
      t.tm_year = 2025;
      p += snprintf(p, limit - p, "%04d/%02d/%02d-%02d:%02d:%02d.%06d %llu ",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                    t.tm_min, t.tm_sec, static_cast<int>(now_tv.tv_nsec / 1000),
                    static_cast<long long unsigned int>(thread_id));

      // Print the message
      if (p < limit) {
        va_list backup_ap;
        va_copy(backup_ap, ap);
        p += vsnprintf(p, limit - p, format, backup_ap);
        va_end(backup_ap);
      }

      // Truncate to available space if necessary
      if (p >= limit) {
        if (iter == 0) {
          continue;  // Try again with larger buffer
        } else {
          p = limit - 1;
        }
      }

      // Add newline if necessary
      if (p == base || p[-1] != '\n') {
        *p++ = '\n';
      }

      assert(p <= limit);
      {
        FileOpGuard guard(*this);
        // We will ignore any error returned by Append().
        file_->Append(Slice(base, p - base), IOOptions(), nullptr);
        flush_pending_ = true;
        const uint64_t now_micros = clock_->NowMicros();
        if (now_micros - last_flush_micros_ >= flush_every_seconds_ * 1000000) {
          FlushLocked();
        }
      }
      if (base != buffer) {
        delete[] base;
      }
      break;
    }
  }

  size_t GetLogFileSize() const override {
    const FDPFile* fdp_file = file_->fdp_file();
    return fdp_file ? fdp_file->size_ : 0;
  }

  Env* env_;
  SystemClock* clock_;
  std::unique_ptr<FDPWritableFile> file_;
  mutable port::Mutex mutex_;  // Mutex to protect the shared variables below.
  const static uint64_t flush_every_seconds_ = 5;
  std::atomic_uint_fast64_t last_flush_micros_;
  std::atomic<bool> flush_pending_;
};

}  // namespace rocksdb
