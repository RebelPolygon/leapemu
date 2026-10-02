#include "core/fileio.h"

#include <fstream>

namespace leap {

bool write_file_atomic(const std::filesystem::path& path, const void* data, size_t size, std::string* err) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  auto fail = [&](const std::string& why) {
    std::error_code ignored;  // (cleaning up must not hide the failure)
    std::filesystem::remove(tmp, ignored);
    if (err) *err = "cannot write " + path.string() + ": " + why;
    return false;
  };
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return fail("cannot create " + tmp.string());
    f.write(static_cast<const char*>(data), std::streamsize(size));
    f.close();
    if (!f) return fail("write error");
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) return fail(ec.message());
  return true;
}

}  // namespace leap
