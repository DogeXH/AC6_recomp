// Standalone unit tests for netplay_xam_marshal.h (no rex runtime, no kernel). Not part of any
// CMake target: it has its own main(). Build and run:
//   clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=undefined \
//     -I<AC6_recomp_netplay>/netplay/core/include -I<this dir> netplay_xam_test.cpp -o t && ./t

#include "netplay_xam_marshal.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace m = rex::netplay::marshal;
namespace xlive = ac6net::xlive;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      ++g_failures;                                                       \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                     \
  } while (0)

// A fake guest buffer: guest address `base` maps to bytes[0]. Guard bytes on both sides catch
// out-of-bounds writes.
struct GuestBuffer {
  static constexpr uint32_t kGuard = 64;
  static constexpr uint8_t kFill = 0xCD;
  uint32_t base;
  uint32_t size;
  std::vector<uint8_t> storage;

  GuestBuffer(uint32_t base_, uint32_t size_)
      : base(base_), size(size_), storage(size_ + 2 * kGuard, kFill) {}
  uint8_t* host() { return storage.data() + kGuard; }
  m::GuestArena arena() { return m::GuestArena(base, host(), size); }
  const uint8_t* at(uint32_t guest) { return host() + (guest - base); }
  uint32_t u32(uint32_t guest) { return m::LoadBE<uint32_t>(at(guest)); }
  uint64_t u64(uint32_t guest) { return m::LoadBE<uint64_t>(at(guest)); }
  bool InRange(uint32_t guest, uint32_t len) const {
    return guest >= base && uint64_t(guest) + len <= uint64_t(base) + size;
  }
  bool GuardsIntact() const {
    for (uint32_t i = 0; i < kGuard; ++i) {
      if (storage[i] != kFill || storage[kGuard + size + i] != kFill) return false;
    }
    return true;
  }
};

static ac6net::XnAddr SampleXnAddr() {
  ac6net::XnAddr a;
  a.ina = 0x0A400007;
  a.ina_online = 0x0A400007;
  a.port_online = 1000;
  a.ab_enet = {0x00, 0x22, 0x48, 0x00, 0x00, 0x07};
  for (size_t i = 0; i < a.ab_online.size(); ++i) a.ab_online[i] = static_cast<uint8_t>(i + 1);
  return a;
}

static void TestBigEndian() {
  uint8_t b[8] = {};
  m::StoreBE<uint32_t>(b, 0x11223344u);
  CHECK(b[0] == 0x11 && b[3] == 0x44);
  CHECK(m::LoadBE<uint32_t>(b) == 0x11223344u);
  m::StoreBE<int64_t>(b, -2);
  CHECK(m::LoadBE<int64_t>(b) == -2);
  CHECK(b[0] == 0xFF && b[7] == 0xFE);
  m::StoreBE<double>(b, 1.5);
  CHECK(m::LoadBE<double>(b) == 1.5);
  CHECK(b[0] == 0x3F && b[1] == 0xF8);
  m::StoreBE<float>(b, -0.25f);
  CHECK(m::LoadBE<float>(b) == -0.25f);
  m::StoreBE<uint16_t>(b, 0xABCD);
  CHECK(b[0] == 0xAB && b[1] == 0xCD);
}

static void TestXnAddrAndSessionInfo() {
  uint8_t raw[m::kSessionInfoSize] = {};
  ac6net::SessionInfo info;
  info.session_id.b = {0x80, 1, 2, 3, 4, 5, 6, 7};
  info.host = SampleXnAddr();
  for (size_t i = 0; i < info.key.b.size(); ++i) info.key.b[i] = static_cast<uint8_t>(0xA0 + i);
  m::WriteSessionInfo(raw, info);
  // Guest byte order: XNKID raw at 0, ina network order at 8, wPortOnline BE at 16.
  CHECK(raw[0] == 0x80 && raw[7] == 7);
  CHECK(raw[8] == 10 && raw[9] == 64 && raw[10] == 0 && raw[11] == 7);
  CHECK(raw[16] == 0x03 && raw[17] == 0xE8);
  CHECK(raw[18] == 0x00 && raw[19] == 0x22 && raw[20] == 0x48);
  CHECK(raw[24] == 1 && raw[43] == 20);
  CHECK(raw[44] == 0xA0 && raw[59] == 0xAF);
  const ac6net::SessionInfo back = m::ReadSessionInfo(raw);
  CHECK(back.session_id == info.session_id);
  CHECK(back.host == info.host);
  CHECK(back.key == info.key);
}

static void TestArena() {
  GuestBuffer buf(0x40001003, 64);  // deliberately unaligned guest base
  auto arena = buf.arena();
  const uint32_t a = arena.Alloc(3, 1);
  CHECK(a == 0x40001003);
  const uint32_t b = arena.Alloc(8, 8);
  CHECK(b == 0x40001008);  // aligned guest address
  CHECK(arena.Alloc(0xFFFFFFFFu, 4) == 0);  // overflow-safe
  CHECK(arena.Alloc(8, 3) == 0);            // bad alignment
  const uint32_t c = arena.Alloc(arena.size() - arena.used(), 1);
  CHECK(c != 0 && arena.used() == arena.size());
  CHECK(arena.Alloc(1, 1) == 0);
  CHECK(buf.GuardsIntact());
  m::GuestArena empty;
  CHECK(empty.Alloc(1, 1) == 0 && empty.size() == 0);
  m::GuestArena null_host(0x40000000, nullptr, 100);
  CHECK(null_host.size() == 0 && null_host.Alloc(4) == 0);
}

static void TestUserData() {
  CHECK(m::DataTypeFromPropertyId(0x1000000C) == ac6net::DataType::kInt32);
  CHECK(m::DataTypeFromPropertyId(0x2000000B) == ac6net::DataType::kInt64);
  CHECK(m::DataTypeFromPropertyId(0x40008002) == ac6net::DataType::kUnicode);
  CHECK(m::DataTypeFromPropertyId(0xF0000000) == ac6net::DataType::kNull);
  CHECK(m::DataTypeFromByte(9) == ac6net::DataType::kNull);
  CHECK(m::FixedValueSize(ac6net::DataType::kDouble) == 8);
  CHECK(m::FixedValueSize(ac6net::DataType::kUnicode) == 0);

  GuestBuffer buf(0x50000000, 256);
  auto arena = buf.arena();
  ac6net::Property p32;
  p32.id = 0x1000000D;
  p32.value.type = ac6net::DataType::kInt32;
  p32.value.i = -5;
  const uint32_t d32 = arena.Alloc(m::kUserPropertySize, 8);
  CHECK(m::WriteUserProperty(arena, d32, p32));
  CHECK(buf.u32(d32) == 0x1000000D);
  CHECK(buf.at(d32)[8] == 1);  // XUSER_DATA.type at +8
  CHECK(m::LoadBE<int32_t>(buf.at(d32) + 16) == -5);  // union at +16
  ac6net::UserData decoded;
  m::DecodeFixedValue(ac6net::DataType::kInt32, buf.at(d32) + 16, &decoded);
  CHECK(decoded.i == -5);

  ac6net::Property uni;
  uni.id = 0x40008002;
  uni.value.type = ac6net::DataType::kUnicode;
  uni.value.bytes = {0x00, 'B', 0x00, 'i', 0x00, 0x00};
  const uint32_t du = arena.Alloc(m::kUserPropertySize, 8);
  CHECK(m::WriteUserProperty(arena, du, uni));
  CHECK(buf.u32(du + 16) == 6);
  const uint32_t ptr = buf.u32(du + 20);
  CHECK(buf.InRange(ptr, 6));
  CHECK(buf.at(ptr)[1] == 'B' && buf.at(ptr)[3] == 'i');

  ac6net::UserData dbl;
  dbl.type = ac6net::DataType::kDouble;
  dbl.d = 3.25;
  const uint32_t dd = arena.Alloc(m::kUserDataSize, 8);
  CHECK(m::WriteUserData(arena, dd, dbl));
  m::DecodeFixedValue(ac6net::DataType::kDouble, buf.at(dd) + 8, &decoded);
  CHECK(decoded.d == 3.25);

  // A payload that does not fit leaves an empty value and reports failure.
  GuestBuffer small(0x50001000, m::kUserDataSize + 4);
  auto small_arena = small.arena();
  ac6net::UserData big;
  big.type = ac6net::DataType::kBinary;
  big.bytes.assign(64, 0x5A);
  const uint32_t ds = small_arena.Alloc(m::kUserDataSize, 8);
  CHECK(!m::WriteUserData(small_arena, ds, big));
  CHECK(small.u32(ds + 8) == 0 && small.u32(ds + 12) == 0);
  CHECK(buf.GuardsIntact() && small.GuardsIntact());

  // Oversized payloads are capped at kMaxUserDataBytes.
  GuestBuffer large(0x50010000, 4096);
  auto large_arena = large.arena();
  ac6net::UserData huge;
  huge.type = ac6net::DataType::kBinary;
  huge.bytes.assign(5000, 1);
  const uint32_t dh = large_arena.Alloc(m::kUserDataSize, 8);
  CHECK(m::WriteUserData(large_arena, dh, huge));
  CHECK(large.u32(dh + 8) == m::kMaxUserDataBytes);
}

static xlive::SearchResult SampleResult(uint8_t tag, size_t n_props) {
  xlive::SearchResult r;
  r.info.session_id.b = {0x80, tag, 0, 0, 0, 0, 0, 1};
  r.info.host = SampleXnAddr();
  r.open_public = 15;
  r.filled_public = 1;
  for (size_t i = 0; i < n_props; ++i) {
    ac6net::Property p;
    p.id = 0x1000000C + static_cast<uint32_t>(i);
    p.value.type = ac6net::DataType::kInt32;
    p.value.i = tag;
    r.properties.push_back(p);
  }
  ac6net::Property name;
  name.id = 0x40008002;
  name.value.type = ac6net::DataType::kUnicode;
  name.value.bytes = {0, 'A', 0, 0};
  r.properties.push_back(name);
  r.contexts.push_back({ac6net::kContextGameType, 1});
  r.contexts.push_back({ac6net::kContextGameMode, tag});
  return r;
}

static void TestSearchLayout() {
  std::vector<xlive::SearchResult> results = {SampleResult(1, 2), SampleResult(2, 2)};
  // AC6 sizes the buffer as n * 1326 + 8 (rex_sub_821FDC88).
  GuestBuffer buf(0x60000000, 50 * 1326 + 8);
  auto arena = buf.arena();
  CHECK(m::LayoutSearchResultsFitting(arena, results, 50) == 2);
  CHECK(buf.u32(buf.base + 0) == 2);
  const uint32_t array = buf.u32(buf.base + 4);
  CHECK(array == buf.base + m::kSearchHeaderSize);
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t e = array + i * m::kSearchResultSize;
    CHECK(buf.at(e)[0] == 0x80 && buf.at(e)[1] == i + 1);
    CHECK(buf.u32(e + 60) == 15 && buf.u32(e + 68) == 1);
    CHECK(buf.u32(e + 76) == 3 && buf.u32(e + 80) == 2);
    const uint32_t props = buf.u32(e + 84);
    const uint32_t ctx = buf.u32(e + 88);
    // Arrays are packed after the whole results array, inside the buffer.
    CHECK(props >= array + 2 * m::kSearchResultSize && buf.InRange(props, 3 * m::kUserPropertySize));
    CHECK(props % 8 == 0);
    CHECK(buf.InRange(ctx, 2 * m::kUserContextSize));
    CHECK(buf.u32(props) == 0x1000000C && m::LoadBE<int32_t>(buf.at(props) + 16) == int32_t(i + 1));
    const uint32_t name = props + 2 * m::kUserPropertySize;
    CHECK(buf.at(name)[8] == 4 && buf.u32(name + 16) == 4 && buf.InRange(buf.u32(name + 20), 4));
    CHECK(buf.u32(ctx) == ac6net::kContextGameType && buf.u32(ctx + 4) == 1);
    CHECK(buf.u32(ctx + 8) == ac6net::kContextGameMode && buf.u32(ctx + 12) == i + 1);
  }
  CHECK(buf.GuardsIntact());

  // max_results caps the count.
  arena = buf.arena();
  CHECK(m::LayoutSearchResultsFitting(arena, results, 1) == 1);
  CHECK(buf.u32(buf.base) == 1);

  // A buffer that holds only one whole result keeps one.
  const uint32_t one = m::kSearchHeaderSize + m::kSearchResultSize + 3 * m::kUserPropertySize +
                       8 + 2 * m::kUserContextSize + 16;
  GuestBuffer tight(0x60100000, one + 60);
  auto tight_arena = tight.arena();
  CHECK(m::LayoutSearchResultsFitting(tight_arena, results, 50) == 1);
  CHECK(tight.u32(tight.base) == 1);
  CHECK(tight.GuardsIntact());

  // Header only: zero results, header still valid.
  GuestBuffer header_only(0x60200000, m::kSearchHeaderSize);
  auto header_arena = header_only.arena();
  CHECK(m::LayoutSearchResultsFitting(header_arena, results, 50) == 0);
  CHECK(header_only.u32(header_only.base) == 0);
  CHECK(header_only.GuardsIntact());

  // Smaller than the header: nothing fits.
  GuestBuffer too_small(0x60300000, 4);
  auto too_small_arena = too_small.arena();
  CHECK(m::LayoutSearchResultsFitting(too_small_arena, results, 50) == -1);
  CHECK(too_small.GuardsIntact());

  // No results at all.
  arena = buf.arena();
  CHECK(m::LayoutSearchResultsFitting(arena, {}, 50) == 0);
  CHECK(buf.u32(buf.base) == 0);
}

static void TestRegistrationLayout() {
  std::vector<xlive::Registrant> registrants(2);
  registrants[0].machine_id = 0xFA00002248000007ull;
  registrants[0].users = {0x0009000000000001ull};
  registrants[1].machine_id = 0xFA00002248000009ull;
  registrants[1].users = {0x0009000000000002ull, 0x0009000000000003ull};
  GuestBuffer buf(0x61000004, 3592);  // AC6's minimum (rex_sub_821FDA68)
  auto arena = buf.arena();
  CHECK(m::LayoutRegistrationResults(arena, registrants));
  CHECK(buf.u32(buf.base) == 2);
  const uint32_t array = buf.u32(buf.base + 4);
  CHECK(array % 8 == 0 && buf.InRange(array, 2 * m::kRegistrantSize));
  CHECK(buf.u64(array) == 0xFA00002248000007ull);
  CHECK(buf.u32(array + 8) == 1 && buf.u32(array + 12) == 1);
  const uint32_t users1 = buf.u32(array + m::kRegistrantSize + 16);
  CHECK(buf.u32(array + m::kRegistrantSize + 12) == 2 && buf.InRange(users1, 16));
  CHECK(buf.u64(users1) == 0x0009000000000002ull && buf.u64(users1 + 8) == 0x0009000000000003ull);
  CHECK(buf.GuardsIntact());

  GuestBuffer small(0x61100000, m::kRegistrationResultsSize + m::kRegistrantSize);
  auto small_arena = small.arena();
  CHECK(!m::LayoutRegistrationResults(small_arena, registrants));
  CHECK(small.GuardsIntact());

  GuestBuffer empty(0x61200000, m::kRegistrationResultsSize);
  auto empty_arena = empty.arena();
  CHECK(m::LayoutRegistrationResults(empty_arena, {}));
  CHECK(empty.u32(empty.base) == 0 && empty.u32(empty.base + 4) == 0);
}

static void TestStatsLayout() {
  std::vector<xlive::StatsSpec> specs(1);
  specs[0].view_id = 86;
  specs[0].column_ids = {2, 3, 1};
  const uint32_t size = m::StatsEnumeratorBufferSize(1, specs);
  CHECK(size == 1 * (m::kStatsRowSize * 1 + m::kStatsViewSize) + m::kStatsReadResultsSize +
                    (m::kStatsColumnSize + 4) * 1 * 3);

  std::vector<xlive::StatsView> views(1);
  views[0].view_id = 86;
  views[0].total_rows = 7;
  xlive::StatsRow row;
  row.xuid = 0x0009000000000042ull;
  row.rank = 3;
  row.rating = 1234;
  row.gamertag = "Talisman";
  for (uint16_t id : {2, 3, 1}) {
    xlive::StatsColumn c;
    c.column_id = id;
    c.value.type = ac6net::DataType::kInt64;
    c.value.i = id * 10;
    row.columns.push_back(c);
  }
  xlive::StatsColumn text;  // unicode columns are skipped, as in Xenia
  text.column_id = 9;
  text.value.type = ac6net::DataType::kUnicode;
  text.value.bytes = {0, 'x', 0, 0};
  row.columns.push_back(text);
  views[0].rows.push_back(row);

  GuestBuffer buf(0x62000000, size);
  auto arena = buf.arena();
  CHECK(m::LayoutStatsResults(arena, views));
  CHECK(buf.u32(buf.base) == 1);
  const uint32_t view = buf.u32(buf.base + 4);
  CHECK(buf.InRange(view, m::kStatsViewSize));
  CHECK(buf.u32(view) == 86 && buf.u32(view + 4) == 7 && buf.u32(view + 8) == 1);
  const uint32_t rows = buf.u32(view + 12);
  CHECK(buf.InRange(rows, m::kStatsRowSize));
  CHECK(buf.u64(rows) == 0x0009000000000042ull && buf.u32(rows + 8) == 3);
  CHECK(m::LoadBE<int64_t>(buf.at(rows + 16)) == 1234);
  CHECK(std::string(reinterpret_cast<const char*>(buf.at(rows + 24))) == "Talisman");
  CHECK(buf.u32(rows + 40) == 3);
  const uint32_t cols = buf.u32(rows + 44);
  CHECK(buf.InRange(cols, 3 * m::kStatsColumnSize));
  CHECK(m::LoadBE<uint16_t>(buf.at(cols)) == 2);
  CHECK(buf.at(cols + 8)[0] == 2);  // XUSER_DATA type int64
  CHECK(m::LoadBE<int64_t>(buf.at(cols + 16)) == 20);
  CHECK(m::LoadBE<uint16_t>(buf.at(cols + 2 * m::kStatsColumnSize)) == 1);
  CHECK(buf.GuardsIntact());

  // A short buffer drops rows but stays consistent.
  GuestBuffer small(0x62100000, m::kStatsReadResultsSize + m::kStatsViewSize + 8);
  auto small_arena = small.arena();
  CHECK(!m::LayoutStatsResults(small_arena, views));
  const uint32_t small_view = small.u32(small.base + 4);
  CHECK(small.u32(small_view + 8) == 0 && small.u32(small_view + 12) == 0);
  CHECK(small.GuardsIntact());

  // Zero views still produce the results header (one enumerator item).
  GuestBuffer header(0x62200000, m::kStatsReadResultsSize);
  auto header_arena = header.arena();
  CHECK(m::LayoutStatsResults(header_arena, {}));
  CHECK(header.u32(header.base) == 0 && header.u32(header.base + 4) == 0);
}

int main() {
  TestBigEndian();
  TestXnAddrAndSessionInfo();
  TestArena();
  TestUserData();
  TestSearchLayout();
  TestRegistrationLayout();
  TestStatsLayout();
  std::printf("netplay_xam_test: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
