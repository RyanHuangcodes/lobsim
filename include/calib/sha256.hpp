#pragma once
// SHA-256 (FIPS 180-4), portable and dependency-free, for the input-file hashes of a calibration
// file. Lowercase hex output.
#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace calib {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t size);
  void update(std::string_view s) { update(s.data(), s.size()); }
  std::string hex();  // finishes the hash; the object must not be updated afterwards

 private:
  void block(const unsigned char* p);
  std::array<std::uint32_t, 8> h_;
  std::array<unsigned char, 64> buf_{};
  std::size_t buf_len_{0};
  std::uint64_t bytes_{0};
};

std::string sha256_hex(std::string_view data);
// Reads the stream to its end. Throws std::runtime_error on a read error.
std::string sha256_hex(std::istream& in);
// Throws std::runtime_error naming the path if it cannot be read.
std::string sha256_file(const std::string& path);

}  // namespace calib
