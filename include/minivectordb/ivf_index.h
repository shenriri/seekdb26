#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mvdb {
// Derived, crash-rebuildable disk IVF-Flat index. Each posting carries the
// committed heap offset, so obsolete INSERT/UPDATE postings are never visible.
struct VectorPosting {
  int64_t pk = 0;
  uint64_t row_offset = 0;
  uint32_t row_length = 0;
  std::vector<float> vector;
};
class IVFIndex {
 public:
  using Visitor = std::function<void(const VectorPosting&)>;
  using Enumerator = std::function<void(const Visitor&)>;
  IVFIndex(std::string directory, std::string table, std::string index_name,
           std::string column, uint32_t dimension);
  bool load_if_current(uint64_t committed_cursor,uint64_t data_generation);
  void rebuild(uint64_t committed_cursor,uint64_t data_generation, const Enumerator& enumerate);
  void append(uint64_t committed_cursor, const std::vector<VectorPosting>& changed);
  void advance_cursor(uint64_t committed_cursor);
  void candidates(const std::vector<float>& query, size_t probes,
                  const Visitor& visitor) const;
  size_t list_count() const { return centroids_.size(); }
 private:
  std::string directory_;
  uint32_t dimension_;
  uint64_t cursor_ = 0;
  uint64_t data_generation_ = 0;
  std::vector<std::vector<float>> centroids_;
  uint32_t closest(const std::vector<float>& vector) const;
  std::string cluster_path(uint32_t cluster) const;
  void write_metadata(uint64_t cursor);
};
} // namespace mvdb
