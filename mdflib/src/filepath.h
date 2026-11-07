#ifndef MDF_FILEPATH_H
#define MDF_FILEPATH_H

// GCC before 12 instantiates std::filesystem::filesystem_error against
// _M_gen_what(), a symbol libstdc++ dropped in GCC 12. A Rocky 8 build then
// fails to load on a newer libstdc++. This stand-in uses stat/open only, so
// the same binary still runs on Red Hat 8 (glibc 2.28).

#if !defined(MDF_USE_FILEPATH_SHIM)
#  if defined(__clang__) || defined(_MSC_VER)
#    define MDF_USE_FILEPATH_SHIM 0
#  elif defined(__GNUC__) && (__GNUC__ < 12)
#    define MDF_USE_FILEPATH_SHIM 1
#  else
#    define MDF_USE_FILEPATH_SHIM 0
#  endif
#endif

#if MDF_USE_FILEPATH_SHIM

#include "cutf.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ios>
#include <ostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace fs {

class path {
 public:
  path() = default;
  path(const std::string& text) : utf8_(text) {}
  path(const char* text) : utf8_(text ? text : "") {}
  path(const std::wstring& text) { assign_wide(text); }

  path& operator=(const std::string& text) {
    utf8_ = text;
    return *this;
  }
  path& operator=(const char* text) {
    utf8_ = text ? text : "";
    return *this;
  }
  path& operator=(const std::wstring& text) {
    assign_wide(text);
    return *this;
  }

  bool empty() const { return utf8_.empty(); }
  const char* c_str() const { return utf8_.c_str(); }
  const std::string& string() const { return utf8_; }
  std::string u8string() const { return utf8_; }
  std::wstring wstring() const {
    if (utf8_.empty()) {
      return {};
    }
    return utf8towide(utf8_);
  }

  path filename() const {
    const auto slash = utf8_.find_last_of('/');
    if (slash == std::string::npos) {
      return *this;
    }
    return path(utf8_.substr(slash + 1));
  }

  path stem() const {
    const std::string name = filename().utf8_;
    if (name.empty() || name == "." || name == "..") {
      return path(name);
    }
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
      return path(name);
    }
    return path(name.substr(0, dot));
  }

  path extension() const {
    const std::string name = filename().utf8_;
    if (name.empty() || name == "." || name == "..") {
      return {};
    }
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
      return {};
    }
    return path(name.substr(dot));
  }

  path parent_path() const {
    if (utf8_.empty()) {
      return {};
    }
    std::size_t end = utf8_.size();
    while (end > 1 && utf8_[end - 1] == '/') {
      --end;
    }
    const auto slash = utf8_.find_last_of('/', end - 1);
    if (slash == std::string::npos) {
      return {};
    }
    if (slash == 0) {
      return path("/");
    }
    return path(utf8_.substr(0, slash));
  }

  path& operator/=(const path& extra) {
    if (extra.utf8_.empty()) {
      return *this;
    }
    if (!utf8_.empty() && extra.utf8_[0] == '/') {
      utf8_ = extra.utf8_;
      return *this;
    }
    if (!utf8_.empty() && utf8_.back() != '/') {
      utf8_.push_back('/');
    }
    utf8_ += extra.utf8_;
    return *this;
  }

  path& append(const std::string& extra) { return *this /= path(extra); }

  path& operator+=(const path& extra) {
    utf8_ += extra.utf8_;
    return *this;
  }
  path& operator+=(const std::string& extra) {
    utf8_ += extra;
    return *this;
  }
  path& operator+=(const char* extra) {
    if (extra) {
      utf8_ += extra;
    }
    return *this;
  }

  bool operator==(const path& other) const { return utf8_ == other.utf8_; }
  bool operator!=(const path& other) const { return utf8_ != other.utf8_; }

 private:
  void assign_wide(const std::wstring& text) {
    utf8_ = text.empty() ? std::string() : widetoutf8(text);
  }

  std::string utf8_;
};

inline path u8path(const std::string& text) { return path(text); }
inline path u8path(const char* text) { return path(text); }

inline bool exists(const path& file) {
  if (file.string().empty()) {
    return false;
  }
  struct stat info;
  if (::stat(file.c_str(), &info) == 0) {
    return true;
  }
  if (errno == ENOENT || errno == ENOTDIR) {
    return false;
  }
  throw std::runtime_error("exists failed");
}

inline std::uint64_t file_size(const path& file) {
  struct stat info;
  if (::stat(file.c_str(), &info) != 0) {
    throw std::runtime_error("file_size failed");
  }
  if (info.st_size < 0) {
    throw std::runtime_error("file_size failed");
  }
  return static_cast<std::uint64_t>(info.st_size);
}

enum class copy_options { none = 0, overwrite_existing = 1 };

inline bool is_regular_file(const path& file) {
  if (file.string().empty()) {
    return false;
  }
  struct stat info;
  if (::stat(file.c_str(), &info) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return false;
    }
    throw std::runtime_error("is_regular_file failed");
  }
  return S_ISREG(info.st_mode) != 0;
}

inline path temp_directory_path() {
  const char* env = std::getenv("TMPDIR");
  if (env && env[0]) {
    return path(env);
  }
  return path("/tmp");
}

inline bool create_directories(const path& directory) {
  const std::string& full = directory.string();
  if (full.empty() || full == ".") {
    return false;
  }
  std::string current;
  bool created = false;
  std::size_t index = 0;
  if (full[0] == '/') {
    current = "/";
    index = 1;
  }
  while (index <= full.size()) {
    const bool boundary = index == full.size() || full[index] == '/';
    if (boundary) {
      if (!current.empty() && current != "/") {
        if (::mkdir(current.c_str(), 0755) == 0) {
          created = true;
        } else if (errno != EEXIST) {
          throw std::runtime_error("create_directories failed");
        } else {
          struct stat info;
          if (::stat(current.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
            throw std::runtime_error("create_directories failed");
          }
        }
      }
      if (index < full.size() && (current.empty() || current.back() != '/')) {
        current.push_back('/');
      }
    } else {
      current.push_back(full[index]);
    }
    ++index;
  }
  return created;
}

inline bool remove(const path& file) {
  if (file.string().empty()) {
    return false;
  }
  if (::remove(file.c_str()) == 0) {
    return true;
  }
  if (errno == ENOENT) {
    return false;
  }
  throw std::runtime_error("remove failed");
}

inline void rename(const path& from, const path& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) {
    throw std::runtime_error("rename failed");
  }
}

inline void copy_file(const path& from, const path& to, copy_options); 

inline void copy_file(const path& from, const path& to) {
  if (exists(to)) {
    throw std::runtime_error("copy_file destination exists");
  }
  copy_file(from, to, copy_options::overwrite_existing);
}

inline void copy(const path& from, const path& to) {
  if (!is_regular_file(from)) {
    throw std::runtime_error("copy supports regular files");
  }
  copy_file(from, to);
}

inline void copy_file(const path& from, const path& to, copy_options) {
  if (from == to) {
    return;
  }
  std::FILE* input = std::fopen(from.c_str(), "rb");
  if (!input) {
    throw std::runtime_error("copy_file failed to open source");
  }
  std::FILE* output = std::fopen(to.c_str(), "wb");
  if (!output) {
    std::fclose(input);
    throw std::runtime_error("copy_file failed to open destination");
  }
  char buffer[8192];
  bool failed = false;
  while (true) {
    const std::size_t got = std::fread(buffer, 1, sizeof(buffer), input);
    if (got > 0 && std::fwrite(buffer, 1, got, output) != got) {
      failed = true;
      break;
    }
    if (got < sizeof(buffer)) {
      failed = std::ferror(input) != 0;
      break;
    }
  }
  if (std::ferror(output)) {
    failed = true;
  }
  std::fclose(input);
  std::fclose(output);
  if (failed) {
    throw std::runtime_error("copy_file failed");
  }
}

inline std::ostream& operator<<(std::ostream& os, const path& file) {
  return os << std::quoted(file.string());
}

}  // namespace fs

inline std::filebuf* MdfOpenFileBuf(std::filebuf& buffer, const fs::path& file,
                                    std::ios_base::openmode mode) {
  return buffer.open(file.c_str(), mode);
}

inline void MdfOpenOfstream(std::ofstream& file, const fs::path& name,
                            std::ios_base::openmode mode) {
  file.open(name.c_str(), mode);
}

#else

#include "platform.h"

#if INCLUDE_STD_FILESYSTEM_EXPERIMENTAL
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

#include <fstream>
#include <ios>

inline std::filebuf* MdfOpenFileBuf(std::filebuf& buffer, const fs::path& file,
                                    std::ios_base::openmode mode) {
  return buffer.open(file, mode);
}

inline void MdfOpenOfstream(std::ofstream& file, const fs::path& name,
                            std::ios_base::openmode mode) {
  file.open(name, mode);
}

#endif  // MDF_USE_FILEPATH_SHIM

#endif  // MDF_FILEPATH_H
