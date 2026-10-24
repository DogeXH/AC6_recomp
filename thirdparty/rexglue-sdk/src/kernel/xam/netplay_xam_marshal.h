/**
 * @file        netplay_xam_marshal.h
 * @brief       Guest <-> host marshalling for the AC6 netplay XAM glue (XGI, stats, sessions).
 *
 * Pure functions over raw guest bytes (big-endian, as they sit in guest memory) and the ac6net
 * host types. No rex runtime dependencies, so the layouts are unit-tested without a kernel
 * (netplay_xam_test.cpp). The caller validates guest ranges and hands in host pointers.
 *
 * Guest layouts follow Xenia Canary Netplay (src/xenia/kernel/xnet.h, xsession.h,
 * xam/user_data.h; Copyright 2026 Xenia Canary, BSD-3-Clause):
 *   XNADDR                        0x24  ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
 *   XSESSION_INFO                 0x3C  XNKID, XNADDR, XNKEY
 *   XUSER_DATA                    0x10  u8 type, pad[7], 8-byte union
 *   XUSER_PROPERTY                0x18  u32 id, pad, XUSER_DATA
 *   XUSER_CONTEXT                 0x08  u32 id, u32 value
 *   XSESSION_SEARCHRESULT_HEADER  0x08  count, results_ptr
 *   XSESSION_SEARCHRESULT         0x5C  info, open/filled public/private, nprops, nctx, ptrs
 *   XSESSION_REGISTRATION_RESULTS 0x08  count, registrants_ptr
 *   XSESSION_REGISTRANT           0x18  u64 machine_id, trust, num_users, users_ptr, pad
 *   X_USER_STATS_READ_RESULTS     0x08  num_views, views_ptr
 *   X_USER_STATS_VIEW             0x10  view_id, total_rows, num_rows, rows_ptr
 *   X_USER_STATS_ROW              0x30  u64 xuid, rank, pad, i64 rating, gamertag[16], ncol, ptr
 *   X_USER_STATS_COLUMN           0x18  u16 id, pad, XUSER_DATA
 *
 * @license     BSD 3-Clause License
 */
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ac6net/types.h>
#include <ac6net/xlive.h>

namespace rex::netplay::marshal {

inline constexpr uint32_t kXnAddrSize = 0x24;
inline constexpr uint32_t kSessionInfoSize = 0x3C;
inline constexpr uint32_t kUserDataSize = 0x10;
inline constexpr uint32_t kUserPropertySize = 0x18;
inline constexpr uint32_t kUserContextSize = 0x08;
inline constexpr uint32_t kSearchHeaderSize = 0x08;
inline constexpr uint32_t kSearchResultSize = 0x5C;
inline constexpr uint32_t kRegistrationResultsSize = 0x08;
inline constexpr uint32_t kRegistrantSize = 0x18;
inline constexpr uint32_t kStatsReadResultsSize = 0x08;
inline constexpr uint32_t kStatsViewSize = 0x10;
inline constexpr uint32_t kStatsRowSize = 0x30;
inline constexpr uint32_t kStatsColumnSize = 0x18;
inline constexpr uint32_t kMaxUserDataBytes = 1000;  // Xenia kMaxUserDataSize
inline constexpr uint32_t kStatsMaxColumns = 64;     // X_USER_STATS_ATTRIBUTES_IN_SPEC

// --- Big-endian access ------------------------------------------------------------------------

template <typename T>
inline T LoadBE(const uint8_t* p) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
  using U = std::conditional_t<sizeof(T) == 8, uint64_t,
                               std::conditional_t<sizeof(T) == 4, uint32_t,
                                                  std::conditional_t<sizeof(T) == 2, uint16_t,
                                                                     uint8_t>>>;
  U v = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    v = static_cast<U>((v << 8) | p[i]);
  }
  return std::bit_cast<T>(v);
}

template <typename T>
inline void StoreBE(uint8_t* p, T value) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
  using U = std::conditional_t<sizeof(T) == 8, uint64_t,
                               std::conditional_t<sizeof(T) == 4, uint32_t,
                                                  std::conditional_t<sizeof(T) == 2, uint16_t,
                                                                     uint8_t>>>;
  U v = std::bit_cast<U>(value);
  for (size_t i = sizeof(T); i-- > 0;) {
    p[i] = static_cast<uint8_t>(v & 0xFF);
    v = static_cast<U>(v >> 8);
  }
}

// --- Addresses and session info ---------------------------------------------------------------

inline ac6net::XnAddr ReadXnAddr(const uint8_t* g) {
  ac6net::XnAddr a;
  a.ina = LoadBE<uint32_t>(g + 0);
  a.ina_online = LoadBE<uint32_t>(g + 4);
  a.port_online = LoadBE<uint16_t>(g + 8);
  std::memcpy(a.ab_enet.data(), g + 10, a.ab_enet.size());
  std::memcpy(a.ab_online.data(), g + 16, a.ab_online.size());
  return a;
}

inline void WriteXnAddr(uint8_t* g, const ac6net::XnAddr& a) {
  StoreBE<uint32_t>(g + 0, a.ina);
  StoreBE<uint32_t>(g + 4, a.ina_online);
  StoreBE<uint16_t>(g + 8, a.port_online);
  std::memcpy(g + 10, a.ab_enet.data(), a.ab_enet.size());
  std::memcpy(g + 16, a.ab_online.data(), a.ab_online.size());
}

inline ac6net::SessionInfo ReadSessionInfo(const uint8_t* g) {
  ac6net::SessionInfo s;
  std::memcpy(s.session_id.b.data(), g + 0, s.session_id.b.size());
  s.host = ReadXnAddr(g + 8);
  std::memcpy(s.key.b.data(), g + 8 + kXnAddrSize, s.key.b.size());
  return s;
}

inline void WriteSessionInfo(uint8_t* g, const ac6net::SessionInfo& s) {
  std::memcpy(g + 0, s.session_id.b.data(), s.session_id.b.size());
  WriteXnAddr(g + 8, s.host);
  std::memcpy(g + 8 + kXnAddrSize, s.key.b.data(), s.key.b.size());
}

// --- XUSER_DATA -------------------------------------------------------------------------------

inline ac6net::DataType DataTypeFromByte(uint8_t type) {
  return type <= 7 ? static_cast<ac6net::DataType>(type) : ac6net::DataType::kNull;
}

// The property/setting ID carries its type in the top nibble (Xenia AttributeKey).
inline ac6net::DataType DataTypeFromPropertyId(uint32_t id) {
  return DataTypeFromByte(static_cast<uint8_t>(id >> 28));
}

inline bool NeedsExtraData(ac6net::DataType type) {
  return type == ac6net::DataType::kUnicode || type == ac6net::DataType::kBinary;
}

// Size in guest bytes of a fixed-size value; 0 for unicode/binary/null.
inline uint32_t FixedValueSize(ac6net::DataType type) {
  switch (type) {
    case ac6net::DataType::kContext:
    case ac6net::DataType::kInt32:
    case ac6net::DataType::kFloat:
      return 4;
    case ac6net::DataType::kInt64:
    case ac6net::DataType::kDouble:
    case ac6net::DataType::kDateTime:
      return 8;
    default:
      return 0;
  }
}

// Decodes a fixed-size value stored big-endian at `value` (the XUSER_DATA union, or the raw
// bytes an XGIUserSetPropertyEx points at).
inline void DecodeFixedValue(ac6net::DataType type, const uint8_t* value, ac6net::UserData* out) {
  out->type = type;
  switch (type) {
    case ac6net::DataType::kContext:
      out->i = LoadBE<uint32_t>(value);
      break;
    case ac6net::DataType::kInt32:
      out->i = LoadBE<int32_t>(value);
      break;
    case ac6net::DataType::kInt64:
    case ac6net::DataType::kDateTime:
      out->i = LoadBE<int64_t>(value);
      break;
    case ac6net::DataType::kDouble:
      out->d = LoadBE<double>(value);
      break;
    case ac6net::DataType::kFloat:
      out->d = LoadBE<float>(value);
      break;
    default:
      break;
  }
}

// --- Guest output arena -----------------------------------------------------------------------

// Bump allocator over one guest output buffer (guest address `guest_base`, mapped at `host`).
// Every allocation is bounds-checked; nothing is ever written outside [guest_base, +size).
class GuestArena {
 public:
  GuestArena() = default;
  GuestArena(uint32_t guest_base, uint8_t* host, uint32_t size)
      : guest_base_(guest_base), host_(host), size_(host ? size : 0) {}

  // Guest address of `size` zeroed bytes aligned to `align` (a power of two), or 0 if they do not
  // fit. A zero-byte allocation returns the aligned cursor (never 0 for a valid arena).
  uint32_t Alloc(uint32_t size, uint32_t align = 4) {
    if (!host_ || !align || (align & (align - 1))) {
      return 0;
    }
    // Align the guest address, not the offset: the guest buffer itself may be unaligned.
    const uint64_t cursor = uint64_t(guest_base_) + used_;
    const uint64_t aligned = (cursor + (align - 1)) & ~uint64_t(align - 1);
    const uint64_t start = aligned - guest_base_;
    const uint64_t end = start + size;
    if (end > size_ || aligned > 0xFFFFFFFFull) {
      return 0;
    }
    used_ = static_cast<uint32_t>(end);
    std::memset(host_ + start, 0, size);
    return static_cast<uint32_t>(aligned);
  }

  // Host pointer for a guest address previously returned by Alloc().
  uint8_t* Host(uint32_t guest_ptr) const { return host_ + (guest_ptr - guest_base_); }

  void Reset() {
    used_ = 0;
    if (host_) {
      std::memset(host_, 0, size_);
    }
  }

  uint32_t guest_base() const { return guest_base_; }
  uint32_t used() const { return used_; }
  uint32_t size() const { return size_; }

 private:
  uint32_t guest_base_ = 0;
  uint8_t* host_ = nullptr;
  uint32_t size_ = 0;
  uint32_t used_ = 0;
};

// Writes an XUSER_DATA at guest address `dst` (16 bytes, already allocated in `arena`).
// Unicode/binary payloads (at most kMaxUserDataBytes) go into `arena`. Returns false if they do
// not fit; the value is then left empty.
inline bool WriteUserData(GuestArena& arena, uint32_t dst, const ac6net::UserData& data) {
  uint8_t* p = arena.Host(dst);
  std::memset(p, 0, kUserDataSize);
  p[0] = static_cast<uint8_t>(data.type);
  uint8_t* value = p + 8;
  switch (data.type) {
    case ac6net::DataType::kContext:
      StoreBE<uint32_t>(value, static_cast<uint32_t>(data.i));
      break;
    case ac6net::DataType::kInt32:
      StoreBE<int32_t>(value, static_cast<int32_t>(data.i));
      break;
    case ac6net::DataType::kInt64:
    case ac6net::DataType::kDateTime:
      StoreBE<int64_t>(value, data.i);
      break;
    case ac6net::DataType::kDouble:
      StoreBE<double>(value, data.d);
      break;
    case ac6net::DataType::kFloat:
      StoreBE<float>(value, static_cast<float>(data.d));
      break;
    case ac6net::DataType::kUnicode:
    case ac6net::DataType::kBinary: {
      const uint32_t size =
          static_cast<uint32_t>(std::min<size_t>(data.bytes.size(), kMaxUserDataBytes));
      if (!size) {
        break;
      }
      const uint32_t data_ptr = arena.Alloc(size, 4);
      if (!data_ptr) {
        return false;
      }
      std::memcpy(arena.Host(data_ptr), data.bytes.data(), size);
      StoreBE<uint32_t>(value + 0, size);
      StoreBE<uint32_t>(value + 4, data_ptr);
      break;
    }
    case ac6net::DataType::kNull:
      break;
  }
  return true;
}

// Writes an XUSER_PROPERTY at guest address `dst` (24 bytes, already allocated).
inline bool WriteUserProperty(GuestArena& arena, uint32_t dst, const ac6net::Property& property) {
  uint8_t* p = arena.Host(dst);
  StoreBE<uint32_t>(p + 0, property.id);
  StoreBE<uint32_t>(p + 4, 0);
  return WriteUserData(arena, dst + 8, property.value);
}

// --- XSessionSearchEx results -------------------------------------------------------------------

// Lays out XSESSION_SEARCHRESULT_HEADER, then `count` results at stride 0x5C, then each result's
// XUSER_PROPERTY and XUSER_CONTEXT arrays (and unicode/binary payloads). Returns false if they
// do not all fit; the arena is then partly written and should be Reset().
inline bool LayoutSearchResults(GuestArena& arena,
                                const std::vector<ac6net::xlive::SearchResult>& results,
                                size_t count) {
  count = std::min(count, results.size());
  const uint32_t header = arena.Alloc(kSearchHeaderSize, 4);
  if (!header) {
    return false;
  }
  const uint32_t array = arena.Alloc(kSearchResultSize * static_cast<uint32_t>(count), 4);
  if (!array) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    const auto& r = results[i];
    uint32_t properties = 0;
    if (!r.properties.empty()) {
      properties = arena.Alloc(kUserPropertySize * static_cast<uint32_t>(r.properties.size()), 8);
      if (!properties) {
        return false;
      }
      for (size_t p = 0; p < r.properties.size(); ++p) {
        if (!WriteUserProperty(arena, properties + static_cast<uint32_t>(p) * kUserPropertySize,
                               r.properties[p])) {
          return false;
        }
      }
    }
    uint32_t contexts = 0;
    if (!r.contexts.empty()) {
      contexts = arena.Alloc(kUserContextSize * static_cast<uint32_t>(r.contexts.size()), 4);
      if (!contexts) {
        return false;
      }
      for (size_t c = 0; c < r.contexts.size(); ++c) {
        uint8_t* out = arena.Host(contexts + static_cast<uint32_t>(c) * kUserContextSize);
        StoreBE<uint32_t>(out + 0, r.contexts[c].id);
        StoreBE<uint32_t>(out + 4, r.contexts[c].value);
      }
    }
    uint8_t* out = arena.Host(array + static_cast<uint32_t>(i) * kSearchResultSize);
    WriteSessionInfo(out, r.info);
    StoreBE<uint32_t>(out + 60, r.open_public);
    StoreBE<uint32_t>(out + 64, r.open_private);
    StoreBE<uint32_t>(out + 68, r.filled_public);
    StoreBE<uint32_t>(out + 72, r.filled_private);
    StoreBE<uint32_t>(out + 76, static_cast<uint32_t>(r.properties.size()));
    StoreBE<uint32_t>(out + 80, static_cast<uint32_t>(r.contexts.size()));
    StoreBE<uint32_t>(out + 84, properties);
    StoreBE<uint32_t>(out + 88, contexts);
  }
  StoreBE<uint32_t>(arena.Host(header) + 0, static_cast<uint32_t>(count));
  // Like Xenia, the header points at the array that follows it even when there are no results.
  StoreBE<uint32_t>(arena.Host(header) + 4, array);
  return true;
}

// Fits as many whole results as the arena holds (Xenia writes past a short buffer; this never
// does). Returns the number written, or -1 if not even the header fits.
inline int LayoutSearchResultsFitting(GuestArena& arena,
                                      const std::vector<ac6net::xlive::SearchResult>& results,
                                      size_t max_results) {
  for (size_t count = std::min(results.size(), max_results);; --count) {
    arena.Reset();
    if (LayoutSearchResults(arena, results, count)) {
      return static_cast<int>(count);
    }
    if (!count) {
      arena.Reset();
      return -1;
    }
  }
}

// --- XSessionArbitrationRegister results --------------------------------------------------------

// XSESSION_REGISTRATION_RESULTS, the XSESSION_REGISTRANT array, then each registrant's XUIDs.
// Returns false (arena partly written) if they do not fit.
inline bool LayoutRegistrationResults(GuestArena& arena,
                                      const std::vector<ac6net::xlive::Registrant>& registrants) {
  const uint32_t header = arena.Alloc(kRegistrationResultsSize, 4);
  if (!header) {
    return false;
  }
  const uint32_t array = arena.Alloc(kRegistrantSize * static_cast<uint32_t>(registrants.size()), 8);
  if (!array) {
    return false;
  }
  for (size_t i = 0; i < registrants.size(); ++i) {
    const auto& registrant = registrants[i];
    const uint32_t users = arena.Alloc(8 * static_cast<uint32_t>(registrant.users.size()), 8);
    if (!users) {
      return false;
    }
    for (size_t u = 0; u < registrant.users.size(); ++u) {
      StoreBE<uint64_t>(arena.Host(users + static_cast<uint32_t>(u) * 8), registrant.users[u]);
    }
    uint8_t* out = arena.Host(array + static_cast<uint32_t>(i) * kRegistrantSize);
    StoreBE<uint64_t>(out + 0, registrant.machine_id);
    StoreBE<uint32_t>(out + 8, registrant.trustworthiness);
    StoreBE<uint32_t>(out + 12, static_cast<uint32_t>(registrant.users.size()));
    StoreBE<uint32_t>(out + 16, registrant.users.empty() ? 0 : users);
  }
  StoreBE<uint32_t>(arena.Host(header) + 0, static_cast<uint32_t>(registrants.size()));
  StoreBE<uint32_t>(arena.Host(header) + 4, registrants.empty() ? 0 : array);
  return true;
}

// --- Stats enumerator results -------------------------------------------------------------------

// Xenia's buffer size for XamUserCreateStatsEnumerator, including its 4 spare bytes per column.
inline uint32_t StatsEnumeratorBufferSize(uint32_t num_rows,
                                          const std::vector<ac6net::xlive::StatsSpec>& specs) {
  uint64_t size = uint64_t(specs.size()) * (uint64_t(kStatsRowSize) * num_rows + kStatsViewSize) +
                  kStatsReadResultsSize;
  for (const auto& spec : specs) {
    size += uint64_t(kStatsColumnSize + 4) * num_rows *
            std::min<size_t>(spec.column_ids.size(), kStatsMaxColumns);
  }
  return size > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(size);
}

// X_USER_STATS_READ_RESULTS, X_USER_STATS_VIEW[], then per view its rows and each row's columns
// (Xenia LeaderboardObjectJSON::WriteResults). Unicode/binary columns are skipped, as in Xenia.
// Rows that do not fit are dropped. Returns false if anything was truncated; the output is still
// a consistent structure.
inline bool LayoutStatsResults(GuestArena& arena,
                               const std::vector<ac6net::xlive::StatsView>& views) {
  const uint32_t results = arena.Alloc(kStatsReadResultsSize, 8);
  if (!results) {
    return false;
  }
  const uint32_t out_views = arena.Alloc(kStatsViewSize * static_cast<uint32_t>(views.size()), 8);
  if (!out_views) {
    return false;
  }
  StoreBE<uint32_t>(arena.Host(results) + 0, static_cast<uint32_t>(views.size()));
  StoreBE<uint32_t>(arena.Host(results) + 4, views.empty() ? 0 : out_views);
  bool complete = true;
  for (size_t v = 0; v < views.size(); ++v) {
    const auto& view = views[v];
    uint8_t* out_view = arena.Host(out_views + static_cast<uint32_t>(v) * kStatsViewSize);
    StoreBE<uint32_t>(out_view + 0, view.view_id);
    StoreBE<uint32_t>(out_view + 4, view.total_rows);
    size_t rows_count = view.rows.size();
    uint32_t rows = 0;
    while (rows_count &&
           !(rows = arena.Alloc(kStatsRowSize * static_cast<uint32_t>(rows_count), 8))) {
      --rows_count;
      complete = false;
    }
    for (size_t r = 0; r < rows_count; ++r) {
      const auto& row = view.rows[r];
      uint8_t* out_row = arena.Host(rows + static_cast<uint32_t>(r) * kStatsRowSize);
      StoreBE<uint64_t>(out_row + 0, row.xuid);
      StoreBE<uint32_t>(out_row + 8, row.rank);
      StoreBE<int64_t>(out_row + 16, row.rating);
      const size_t tag = std::min<size_t>(row.gamertag.size(), 15);
      std::memcpy(out_row + 24, row.gamertag.data(), tag);  // NUL-padded by Alloc
      std::vector<const ac6net::xlive::StatsColumn*> columns;
      for (const auto& column : row.columns) {
        if (!NeedsExtraData(column.value.type) && columns.size() < kStatsMaxColumns) {
          columns.push_back(&column);
        }
      }
      uint32_t out_columns = 0;
      if (!columns.empty()) {
        out_columns = arena.Alloc(kStatsColumnSize * static_cast<uint32_t>(columns.size()), 8);
        if (!out_columns) {
          complete = false;
          columns.clear();
        }
      }
      for (size_t c = 0; c < columns.size(); ++c) {
        const uint32_t dst = out_columns + static_cast<uint32_t>(c) * kStatsColumnSize;
        StoreBE<uint16_t>(arena.Host(dst), columns[c]->column_id);
        WriteUserData(arena, dst + 8, columns[c]->value);  // fixed-size: always fits
      }
      StoreBE<uint32_t>(out_row + 40, static_cast<uint32_t>(columns.size()));
      StoreBE<uint32_t>(out_row + 44, out_columns);
    }
    StoreBE<uint32_t>(out_view + 8, static_cast<uint32_t>(rows_count));
    StoreBE<uint32_t>(out_view + 12, rows_count ? rows : 0);
  }
  return complete;
}

}  // namespace rex::netplay::marshal
