//  Copyright (c) 2025-present, the CoFDP authors. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// CoFDP P-Directory placement shim (see docs/COFDP.md).
//
// On a CoFDP file system (PiDir-FS), the FDP placement IDs (PIDs) of a file
// follow the P-Directory the file is created in. To give the WAL and the SSTs
// of different LSM levels different PIDs without changing RocksDB's file
// layout, the operator places P-Directories (usually bind mounts of PiDir-FS
// P-Directories) inside the DB directory, and PosixFileSystem moves WAL and
// SST files into them:
//
//   <dir>/<name>.log    ->  <dir>/<pdir[0]>/<name>.log
//   <dir>/<number>.sst  ->  <dir>/<pdir[min(L + 1, N - 1)]>/<number>.sst
//
// pdir[0..N-1] are the entries of the first DB path whose names start with
// 'p', in byte order, and L is the level the SST was created for (an unknown
// level selects pdir[0]). All other files (MANIFEST, CURRENT, OPTIONS, LOG,
// ...) stay in the DB directory. The shim does nothing while N == 0 or before
// the first DB has been opened on the file system.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

// Values of DirFsyncOptions::pdir_class. Class 0 fsyncs the directory itself,
// as upstream RocksDB does. When the file system has P-Directories, class
// k >= 1 fsyncs P-Directory k - 1 instead: kPDirClassWal is the WAL
// P-Directory and PDirClassForSstLevel(L) the P-Directory of level-L SSTs.
constexpr int kPDirClassOwnDir = 0;
constexpr int kPDirClassWal = 1;
constexpr int PDirClassForSstLevel(int level) { return level + 2; }

class PDirPlacement {
 public:
  PDirPlacement();
  ~PDirPlacement();

  PDirPlacement(const PDirPlacement&) = delete;
  PDirPlacement& operator=(const PDirPlacement&) = delete;

  // Replaces the P-Directories with the 'p*' entries of `db_path` and opens
  // them for FsyncPDir(). Prints one "<db_path>/<pdir> starts with p" line per
  // P-Directory; these lines are the experiment logs' record of the layout.
  void Discover(const std::string& db_path);

  // Enables path rewriting. Called whenever a DB is opened; never undone.
  void NotifyDbOpened() { db_opened_.store(true, std::memory_order_relaxed); }

  // Records the level an SST file is created for. Must be called before the
  // file is created.
  void RecordSstCreationLevel(uint64_t file_number, int level);

  // Returns the path at which `fname` is stored. The first result for a path
  // is cached for the lifetime of the process.
  std::string Resolve(const std::string& fname);

  // Removes `path` from the Resolve() cache.
  void ForgetPath(const std::string& path);

  size_t num_pdirs() const {
    return num_pdirs_.load(std::memory_order_relaxed);
  }

  // fsyncs the P-Directory of `pdir_class` (>= 1). Errors are ignored.
  void FsyncPDir(int pdir_class);

 private:
  // File numbers covered by the creation-level table. Larger file numbers
  // have an unknown level.
  static constexpr uint64_t kMaxSstFileNumber = uint64_t{1} << 20;

  std::atomic<bool> db_opened_{false};
  std::atomic<size_t> num_pdirs_{0};

  std::mutex mu_;
  // The fields below are guarded by mu_.
  std::vector<std::string> pdir_names_;
  std::vector<int> pdir_fds_;
  // Logical path -> resolved path.
  std::unordered_map<std::string, std::string> resolved_paths_;
  // SST file number -> creation level, -1 if unknown.
  std::vector<int> sst_creation_level_;
};

}  // namespace ROCKSDB_NAMESPACE
