#include "itch/mapped_file.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace itch {

MappedFile::MappedFile(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const int err = errno;
    ::close(fd);
    throw std::runtime_error("cannot stat " + path + ": " + std::strerror(err));
  }
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ > 0) {
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
      const int err = errno;
      ::close(fd);
      throw std::runtime_error("cannot mmap " + path + ": " + std::strerror(err));
    }
    data_ = p;
    ::madvise(data_, size_, MADV_SEQUENTIAL);
  }
  ::close(fd);
}

MappedFile::~MappedFile() {
  if (data_ != nullptr) ::munmap(data_, size_);
}

}  // namespace itch
