/* TorFS: RocksDB Storage Backend for FDP SSDs
 *
 * Copyright 2024 Samsung Electronics
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#pragma once
#include <liburing.h>

#include <memory>
#include <queue>
#include <string>

#include "io_torfs.h"

namespace rocksdb {

// Management operations of the NVMe I/O Management Receive command.
enum NvmeIomr2Mo {
  NVME_IOMR_MO_NOP = 0x0,
  NVME_IOMR_MO_RUH_STATUS = 0x1,
};

struct nvme_fdp_ruh_status_desc {
  uint16_t pid;
  uint16_t ruhid;
  uint32_t earutr;
  uint64_t ruamw;
  uint8_t rsvd16[16];
};

// Reclaim Unit Handle Status, returned for NVME_IOMR_MO_RUH_STATUS.
// NOTE: the anonymous struct (vendor statistics that no device fills) pads the
// 14 reserved header bytes to 20, so nruhsd and ruhss[] are read at offsets 20
// and 24 instead of 14 and 16 (KNOWN_ISSUES TORFS-3). The layout is kept
// because sizeof() of this struct (8216 bytes) is the transfer size of the
// command issued in XNvmeBackend::Init().
struct nvme_fdp_ruh_status {
  union {
    struct {
      uint8_t free_space_ratio;
      uint32_t copied_page;
      uint32_t block_erased;
      uint8_t rsvd0_tmp[5];
    };
    uint8_t rsvd0[14];
  };
  uint16_t nruhsd;
  struct nvme_fdp_ruh_status_desc ruhss[256];
};

class XNvmeBackend : public Backend {
 public:
  XNvmeBackend() {}
  std::queue<struct xnvme_queue*> xnvme_queues_;
  ~XNvmeBackend() { Exit(); }
  std::string dev;
  std::string async_io;
  port::Mutex xnvme_mutex_;
  struct xnvme_dev* dev_ = nullptr;
  struct nvme_fdp_ruh_status ruh_status_;
  const struct xnvme_geo* geo_ = nullptr;
  struct xnvme_queue* queues_[MAX_NR_QUEUE];
  std::queue<void*> big_buf_queue_;
  port::Mutex big_buf_mutex_;
  void* AllocBuf(uint32_t size);
  void FreeBuf(void* buf);
  // for SPDK API
  void FreeBuf(void* buf, uint32_t size);
  int DeallocateImpl(const DeviceGeometry& geo);
  void SetDev(const std::string& name) { dev = name; }
  void SetAsyncType(const std::string& type) { async_io = type; }
  void GetPidInfo(std::vector<uint64_t>& info) override {
    info.clear();
    info.push_back(max_ph_id_);
    info.push_back(max_rg_id_);
  }

 private:
  int Async(const DeviceGeometry& geo, TorfsDIO dio);
  virtual struct xnvme_opts GetOpt() = 0;
  int Init();
  void Exit();
  uint64_t GetNlb();
  uint32_t GetLbaShift();
  static void async_cb(struct xnvme_cmd_ctx* ctx, void* cb_arg);
  int SubmitXNvmeAsyncCmd(struct xnvme_queue* xqueue, const DeviceGeometry& geo,
                          TorfsDIO dio);
  int SubmitDeallocate(const DeviceGeometry& geo);

  // Largest placement handle and reclaim group in the RUH status.
  uint64_t max_ph_id_ = 0;
  uint64_t max_rg_id_ = 0;
};

class XNvmeBlock : public XNvmeBackend {
 public:
  XNvmeBlock() {}
  ~XNvmeBlock() {}

 private:
  struct xnvme_opts GetOpt();
};

class XNvmeChar : public XNvmeBackend {
 public:
  XNvmeChar() {}
  ~XNvmeChar() {}

 private:
  struct xnvme_opts GetOpt();
};

class XNvmeSpdk : public XNvmeBackend {
 public:
  XNvmeSpdk() {}
  ~XNvmeSpdk() {}

 private:
  struct xnvme_opts GetOpt();
};

}  // namespace rocksdb
