// Little-endian binary serialisation used for every message payload.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "KUDA-Lite's wire format assumes a little-endian host");

namespace cl {

/// Thrown when a payload is truncated or otherwise malformed.
class ProtocolError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class ByteWriter {
 public:
  ByteWriter() = default;
  explicit ByteWriter(size_t reserve) { buf_.reserve(reserve); }

  template <typename T>
  ByteWriter& put(const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "put() needs a trivially copyable type");
    return putBytes(&value, sizeof(T));
  }

  ByteWriter& putBytes(const void* data, size_t n) {
    if (n != 0) std::memcpy(append(n), data, n);
    return *this;
  }

  /// u32 length followed by the characters.
  ByteWriter& putString(const std::string& s) {
    put<uint32_t>(static_cast<uint32_t>(s.size()));
    return putBytes(s.data(), s.size());
  }

  /// u32 length followed by the bytes.
  ByteWriter& putBlob(const void* data, size_t n) {
    put<uint32_t>(static_cast<uint32_t>(n));
    return putBytes(data, n);
  }

  /// Grows the buffer by n bytes and returns a pointer to them (for zero-extra-copy fills).
  uint8_t* append(size_t n) {
    const size_t off = buf_.size();
    buf_.resize(off + n);
    return buf_.data() + off;
  }

  size_t size() const { return buf_.size(); }
  std::vector<uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<uint8_t> buf_;
};

class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t n) : p_(data), n_(n) {}
  explicit ByteReader(const std::vector<uint8_t>& v) : ByteReader(v.data(), v.size()) {}

  template <typename T>
  T get() {
    static_assert(std::is_trivially_copyable<T>::value, "get() needs a trivially copyable type");
    T value;
    std::memcpy(&value, view(sizeof(T)), sizeof(T));
    return value;
  }

  /// Returns a pointer to the next n bytes inside the underlying buffer and skips them.
  const uint8_t* view(size_t n) {
    if (n > n_ - pos_) throw ProtocolError("truncated message");
    const uint8_t* at = p_ + pos_;
    pos_ += n;
    return at;
  }

  void getBytes(void* dst, size_t n) {
    if (n != 0) std::memcpy(dst, view(n), n);
  }

  std::string getString() {
    const uint32_t len = get<uint32_t>();
    const uint8_t* d = view(len);
    return std::string(reinterpret_cast<const char*>(d), len);
  }

  std::vector<uint8_t> getBlob() {
    const uint32_t len = get<uint32_t>();
    const uint8_t* d = view(len);
    return std::vector<uint8_t>(d, d + len);
  }

  size_t remaining() const { return n_ - pos_; }

 private:
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
};

}  // namespace cl
