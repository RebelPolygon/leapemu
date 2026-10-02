#pragma once

// Save-state serialization. Each component implements one
//   template <class Ar> void serialize(Ar& ar)
// that calls ar.io(field) for every piece of state; the same function is used
// for saving (StateWriter) and loading (StateReader), so the two can't drift.

#include <array>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

#include "core/common.h"

namespace leap {

class StateWriter {
 public:
  static constexpr bool kLoading = false;
  u32 version = 0;  // format version being written

  template <typename T>
    requires std::is_trivially_copyable_v<T>
  void io(T& v) { raw(&v, sizeof(T)); }

  template <typename T>
  void io(std::vector<T>& v) {
    u64 n = v.size();
    io(n);
    for (auto& e : v) io(e);
  }
  template <typename T>
  void io(std::deque<T>& v) {
    u64 n = v.size();
    io(n);
    for (auto& e : v) io(e);
  }
  void io(std::string& s) {
    u64 n = s.size();
    io(n);
    raw(s.data(), s.size());
  }
  // Large byte buffers of fixed size.
  void bytes(u8* p, size_t n) { raw(p, n); }

  void raw(const void* p, size_t n) {
    const u8* b = static_cast<const u8*>(p);
    data.insert(data.end(), b, b + n);
  }

  std::vector<u8> data;
};

class StateReader {
 public:
  static constexpr bool kLoading = true;
  u32 version = 0;  // format version being read (older formats are migrated)

  StateReader(const u8* p, size_t n) : p_(p), left_(n) {}

  template <typename T>
    requires std::is_trivially_copyable_v<T>
  void io(T& v) { raw(&v, sizeof(T)); }

  template <typename T>
  void io(std::vector<T>& v) {
    u64 n = 0;
    io(n);
    if (!check(n)) return;
    v.resize(size_t(n));
    for (auto& e : v) io(e);
  }
  template <typename T>
  void io(std::deque<T>& v) {
    u64 n = 0;
    io(n);
    if (!check(n)) return;
    v.resize(size_t(n));
    for (auto& e : v) io(e);
  }
  void io(std::string& s) {
    u64 n = 0;
    io(n);
    if (!check(n)) return;
    s.resize(size_t(n));
    raw(s.data(), size_t(n));
  }
  void bytes(u8* p, size_t n) { raw(p, n); }

  void raw(void* p, size_t n) {
    if (!ok || n > left_) { ok = false; std::memset(p, 0, n); return; }
    std::memcpy(p, p_, n);
    p_ += n;
    left_ -= n;
  }

  size_t remaining() const { return left_; }
  bool ok = true;

 private:
  bool check(u64 n) {
    if (n > left_) ok = false;  // every element is at least one byte
    return ok;
  }
  const u8* p_;
  size_t left_;
};

}  // namespace leap
