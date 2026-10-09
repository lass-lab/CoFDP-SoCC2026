//  Copyright (c) 2025-present, the CoFDP authors. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "env/pdir_placement.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {

namespace {

// Returns the last run of decimal digits in `path` (the file number of an SST
// path), or 0 if there is none.
uint64_t ExtractFileNumber(const std::string& path) {
  static const char kDigits[] = "0123456789";
  const size_t last = path.find_last_of(kDigits);
  if (last == std::string::npos) {
    return 0;
  }
  size_t first = path.find_last_not_of(kDigits, last);
  first = (first == std::string::npos) ? 0 : first + 1;
  return std::stoull(path.substr(first, last - first + 1));
}

}  // namespace

PDirPlacement::PDirPlacement() : sst_creation_level_(kMaxSstFileNumber, -1) {}

PDirPlacement::~PDirPlacement() {
  for (int fd : pdir_fds_) {
    if (fd >= 0) {
      close(fd);
    }
  }
}

void PDirPlacement::Discover(const std::string& db_path) {
  std::lock_guard<std::mutex> lock(mu_);
  // A DB that is opened again registers its paths again.
  for (int fd : pdir_fds_) {
    if (fd >= 0) {
      close(fd);
    }
  }
  pdir_names_.clear();
  pdir_fds_.clear();

  DIR* dir = opendir(db_path.c_str());
  if (dir != nullptr) {
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      // Any entry starting with 'p' counts, also non-directories; see
      // KNOWN_ISSUES (RDB-5).
      if (entry->d_name[0] == 'p') {
        pdir_names_.push_back(entry->d_name);
      }
    }
    // Byte order, so p10 sorts before p2; see KNOWN_ISSUES (RDB-4).
    std::sort(pdir_names_.begin(), pdir_names_.end());
    for (const std::string& name : pdir_names_) {
      // NOTE: FsyncPDir() can read past the end of pdir_fds_, and what it
      // finds there depends on the heap; see KNOWN_ISSUES (RDB-1).
      std::string path = std::string(db_path) + "/" + name;
      printf("%s starts with p\n", path.c_str());
      pdir_fds_.push_back(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    }
    closedir(dir);
  }
  num_pdirs_.store(pdir_names_.size(), std::memory_order_relaxed);
}

void PDirPlacement::RecordSstCreationLevel(uint64_t file_number, int level) {
  if (file_number < kMaxSstFileNumber) {
    std::lock_guard<std::mutex> lock(mu_);
    sst_creation_level_[file_number] = level;
  }
}

std::string PDirPlacement::Resolve(const std::string& fname) {
  if (num_pdirs() == 0 || !db_opened_.load(std::memory_order_relaxed)) {
    return fname;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (pdir_names_.empty()) {
    return fname;
  }
  auto cached = resolved_paths_.find(fname);
  if (cached != resolved_paths_.end()) {
    return cached->second;
  }

  size_t pdir;
  if (EndsWith(fname, ".log")) {
    pdir = 0;
  } else if (EndsWith(fname, ".sst")) {
    const uint64_t file_number = ExtractFileNumber(fname);
    const int level =
        file_number < kMaxSstFileNumber ? sst_creation_level_[file_number] : -1;
    // Level L uses P-Directory L + 1 (an unknown level, -1, the WAL one);
    // the last P-Directory also takes all deeper levels.
    const size_t level_pdir = static_cast<size_t>(level + 1);
    pdir =
        level_pdir < pdir_names_.size() ? level_pdir : pdir_names_.size() - 1;
  } else {
    return fname;
  }

  // The P-Directory is looked up in the file's own directory, whether or not
  // that is the DB directory; see KNOWN_ISSUES (RDB-6).
  const size_t last_slash = fname.find_last_of('/');
  if (last_slash == std::string::npos) {
    return fname;
  }
  std::string resolved = fname.substr(0, last_slash) + "/" + pdir_names_[pdir] +
                         "/" + fname.substr(last_slash + 1);
  resolved_paths_[fname] = resolved;
  return resolved;
}

void PDirPlacement::ForgetPath(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  resolved_paths_.erase(path);
}

void PDirPlacement::FsyncPDir(int pdir_class) {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // NOTE: the index is not bounds-checked, unlike placement in Resolve().
    // A compaction into a level whose P-Directory index (level + 1) is >= N
    // reads past the last P-Directory, and the value read depends on the
    // heap; see KNOWN_ISSUES (RDB-1).
    fd = pdir_fds_[pdir_class - 1];
  }
  if (fd > 0) {
    // NOTE: errors are ignored; see KNOWN_ISSUES (RDB-2).
    fsync(fd);
  }
}

}  // namespace ROCKSDB_NAMESPACE
