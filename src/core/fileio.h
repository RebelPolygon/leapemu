// Writing a file whole or not at all.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace leap {

// Writes `size` bytes to `path` through a temporary file next to it, renamed
// over the old file once complete: a crash or a full disk never leaves a
// truncated file, and the old one stays until the new one is whole.
// (std::filesystem::rename replaces an existing file on Windows too, where
// std::rename fails.) Returns false, with a reason in *err, if the file was not
// replaced.
bool write_file_atomic(const std::filesystem::path& path, const void* data, size_t size, std::string* err = nullptr);
inline bool write_file_atomic(const std::filesystem::path& path, std::string_view text, std::string* err = nullptr) {
  return write_file_atomic(path, text.data(), text.size(), err);
}

}  // namespace leap
