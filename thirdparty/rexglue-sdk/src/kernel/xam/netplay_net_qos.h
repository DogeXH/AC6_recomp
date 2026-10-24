/**
 * @file        netplay_net_qos.h
 * @brief       Guest XNQOS results for the AC6 netplay XNetQosLookup / XNetQosServiceLookup glue.
 *
 * A QosResults object owns one guest XNQOS allocation plus the data blobs it points to, and
 * turns ac6net QoS callbacks into big-endian guest writes. No rex runtime dependencies: guest
 * memory is reached through the GuestHeap interface, so the logic is unit-testable.
 *
 * XNQOS / XNQOSINFO layout and flag values follow Xenia (BSD-3, Copyright Ben Vanik and the
 * Xenia Canary team), src/xenia/kernel/xam/xam_net.cc:77-131.
 *
 * @license     BSD 3-Clause License
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <ac6net/netplay.h>

#include "netplay_net_marshal.h"

namespace rex::kernel::xam::netplay_net {

// Guest memory as seen by the QoS glue. Alloc returns zero-filled memory or 0.
class GuestHeap {
 public:
  virtual ~GuestHeap() = default;
  virtual uint32_t Alloc(uint32_t size) = 0;
  virtual void Free(uint32_t guest_addr) = 0;
  virtual uint8_t* Translate(uint32_t guest_addr) = 0;
};

// XNQOS: u32 BE cxnqos, u32 BE cxnqosPending, then XNQOSINFO[cxnqos].
// XNQOSINFO (24 bytes): u8 bFlags, u8 reserved, u16 cProbesXmit, u16 cProbesRecv, u16 cbData,
// u32 pbData, u16 wRttMinInMsecs, u16 wRttMedInMsecs, u32 dwUpBitsPerSec, u32 dwDnBitsPerSec.
inline constexpr uint32_t kXnQosHeaderBytes = 8;
inline constexpr uint32_t kXnQosInfoBytes = 24;
inline constexpr uint32_t kQosInfoFlags = 0;
inline constexpr uint32_t kQosInfoProbesXmit = 2;
inline constexpr uint32_t kQosInfoProbesRecv = 4;
inline constexpr uint32_t kQosInfoDataLen = 6;
inline constexpr uint32_t kQosInfoDataPtr = 8;
inline constexpr uint32_t kQosInfoRttMin = 12;
inline constexpr uint32_t kQosInfoRttMed = 14;
inline constexpr uint32_t kQosInfoUpBps = 16;
inline constexpr uint32_t kQosInfoDownBps = 20;

inline constexpr uint8_t kQosComplete = 0x01;
inline constexpr uint8_t kQosTargetContacted = 0x02;
inline constexpr uint8_t kQosTargetDisabled = 0x04;
inline constexpr uint8_t kQosDataReceived = 0x08;
inline constexpr uint8_t kQosPartialComplete = 0x10;

// Nominal bandwidth reported when the core gives none (10 Mbit/s gives AC6 tier 5).
inline constexpr uint32_t kQosNominalBps = 10000000;
// cfgQosDataLimitDiv4 is a u8, so no title can publish more than 255 * 4 bytes.
inline constexpr uint32_t kMaxQosDataBytes = 255 * 4;
// Upper bound on cxna + cina for one lookup (AC6 uses 1).
inline constexpr uint32_t kMaxQosEntries = 64;
inline constexpr uint32_t kMaxQosProbes = 64;

// Size of the guest XNQOS for `count` entries (XNQOS declares info[1], so at least one).
inline uint32_t XnQosBytes(uint32_t count) {
  return kXnQosHeaderBytes + kXnQosInfoBytes * (count ? count : 1);
}

class QosResults {
 public:
  using Signal = std::function<void()>;

  // `xnqos_addr` is a zero-filled guest block of XnQosBytes(count) bytes, owned from now on.
  // Entries [0, targets) are probed; entries [targets, count) (security gateways, not
  // supported) complete at once without contact. `signal` (may be empty) runs, outside the
  // internal lock, after each entry completes.
  QosResults(GuestHeap* heap, uint32_t xnqos_addr, uint32_t count, uint32_t targets, Signal signal)
      : heap_(heap),
        addr_(xnqos_addr),
        count_(count),
        targets_(targets < count ? targets : count),
        signal_(std::move(signal)),
        entries_(count) {}

  QosResults(const QosResults&) = delete;
  QosResults& operator=(const QosResults&) = delete;

  uint32_t guest_address() const { return addr_; }

  void set_lookup_id(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    lookup_id_ = id;
  }
  uint64_t lookup_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lookup_id_;
  }

  // Writes cxnqos = count and cxnqosPending = targets, and completes the gateway entries.
  // Call before the guest can see the block.
  void Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t* base = heap_->Translate(addr_);
    for (uint32_t i = targets_; i < count_; ++i) {
      entries_[i].complete = true;
      base[InfoOffset(i) + kQosInfoFlags] = kQosComplete;
    }
    pending_ = targets_;
    StoreBe32(base + 0, count_);
    std::atomic_thread_fence(std::memory_order_release);
    StoreBe32(base + 4, pending_);
  }

  // Body of the ac6net::QosCallback. Every info field is written before bFlags, and bFlags
  // before cxnqosPending drops, so a guest that polls cxnqosPending sees complete entries.
  void OnResult(size_t index, const ac6net::QosTargetResult& r, bool all_done) {
    bool signal = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (released_ || index >= targets_)
        return;
      uint8_t* base = heap_->Translate(addr_);
      Entry& e = entries_[index];
      uint8_t* info = base + InfoOffset(static_cast<uint32_t>(index));

      StoreBe16(info + kQosInfoProbesXmit, r.probes_sent);
      StoreBe16(info + kQosInfoProbesRecv, r.probes_recv);
      if (r.contacted) {
        e.flags |= kQosTargetContacted;
        StoreBe16(info + kQosInfoRttMin, r.rtt_min_ms);
        StoreBe16(info + kQosInfoRttMed, r.rtt_med_ms);
        StoreBe32(info + kQosInfoUpBps, r.up_bps ? r.up_bps : kQosNominalBps);
        StoreBe32(info + kQosInfoDownBps, r.dn_bps ? r.dn_bps : kQosNominalBps);
      }
      if (r.data_received && !e.data_addr && !r.data.empty() && r.data.size() <= kMaxQosDataBytes) {
        const uint32_t size = static_cast<uint32_t>(r.data.size());
        const uint32_t data_addr = heap_->Alloc(size);
        if (data_addr) {
          std::memcpy(heap_->Translate(data_addr), r.data.data(), size);
          e.data_addr = data_addr;
          e.data_len = static_cast<uint16_t>(size);
          e.flags |= kQosDataReceived;
        }
      }
      if (e.data_addr) {
        StoreBe16(info + kQosInfoDataLen, e.data_len);
        StoreBe32(info + kQosInfoDataPtr, e.data_addr);
      }

      uint32_t newly_complete = 0;
      if ((r.complete || all_done) && !e.complete) {
        e.complete = true;
        ++newly_complete;
      }
      if (e.complete)
        e.flags |= kQosComplete;
      std::atomic_thread_fence(std::memory_order_release);
      info[kQosInfoFlags] = e.flags;

      if (all_done) {
        // The core reports all_done only once every target is complete; make sure the guest
        // view agrees even if an earlier per-target completion was never delivered.
        for (uint32_t i = 0; i < targets_; ++i) {
          Entry& other = entries_[i];
          if (other.complete)
            continue;
          other.complete = true;
          other.flags |= kQosComplete;
          base[InfoOffset(i) + kQosInfoFlags] = other.flags;
          ++newly_complete;
        }
      }
      if (newly_complete) {
        pending_ = newly_complete > pending_ ? 0 : pending_ - newly_complete;
        std::atomic_thread_fence(std::memory_order_release);
        StoreBe32(base + 4, pending_);
        signal = true;
      }
    }
    if (signal && signal_)
      signal_();
  }

  // Stops all guest writes and hands back every guest allocation (data blobs, then the XNQOS)
  // for the caller to free. Later calls return an empty list.
  std::vector<uint32_t> Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> out;
    if (released_)
      return out;
    released_ = true;
    for (const Entry& e : entries_) {
      if (e.data_addr)
        out.push_back(e.data_addr);
    }
    out.push_back(addr_);
    return out;
  }

  bool released() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return released_;
  }

 private:
  struct Entry {
    bool complete = false;
    uint8_t flags = 0;
    uint16_t data_len = 0;
    uint32_t data_addr = 0;
  };

  static uint32_t InfoOffset(uint32_t index) { return kXnQosHeaderBytes + kXnQosInfoBytes * index; }

  GuestHeap* heap_;
  const uint32_t addr_;
  const uint32_t count_;
  const uint32_t targets_;
  const Signal signal_;

  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  uint32_t pending_ = 0;
  uint64_t lookup_id_ = 0;
  bool released_ = false;
};

// XNetQosServiceLookup result: one entry, complete at once.
inline ac6net::QosTargetResult MakeServiceResult(bool ok, uint32_t rtt_ms, uint32_t up_bps,
                                                 uint32_t dn_bps) {
  ac6net::QosTargetResult r;
  r.complete = true;
  r.contacted = ok;
  if (ok) {
    const uint16_t rtt = static_cast<uint16_t>(rtt_ms > 0xFFFF ? 0xFFFF : rtt_ms);
    r.rtt_min_ms = rtt;
    r.rtt_med_ms = rtt;
    r.up_bps = up_bps;
    r.dn_bps = dn_bps;
    r.probes_sent = 1;
    r.probes_recv = 1;
  }
  return r;
}

}  // namespace rex::kernel::xam::netplay_net
