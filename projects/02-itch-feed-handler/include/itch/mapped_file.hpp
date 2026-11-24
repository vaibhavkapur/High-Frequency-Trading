// Read-only memory-mapped file (POSIX).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace itch {

class MappedFile {
 public:
  // Throws std::runtime_error if the file cannot be opened or mapped.
  explicit MappedFile(const std::string& path);
  ~MappedFile();

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
    return {static_cast<const std::uint8_t*>(data_), size_};
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  void* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace itch
