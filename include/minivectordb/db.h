#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace mvdb {
struct Result {
  bool ok = true;
  std::string error;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  bool is_query = false;
  uint64_t affected_rows = 0;
};
class Database {
 public:
  explicit Database(const std::string& data_dir, bool ann_mode=false);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Result execute(const std::string& sql);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
