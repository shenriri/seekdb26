#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mvdb {
// Read-only view of an already committed heap file. The mapping is virtual;
// individual pages are faulted in by the OS and count toward cgroup memory.
struct HeapView {
  const unsigned char* bytes = nullptr;
  size_t size = 0;
  HeapView() = default;
  ~HeapView();
  HeapView(const HeapView&) = delete;
  HeapView& operator=(const HeapView&) = delete;
};

struct RowRef {
  uint64_t offset;
  uint32_t length;
};
struct DiskChange {
  std::string table;
  int64_t pk;
  std::optional<std::string> row;
};

// A persistent, copy-on-write B+Tree over append-only row blobs.
// Internal pages have 8 KiB fixed size. A single root+catalog manifest is
// atomically replaced only after the corresponding WAL has been synced.
class DiskStore {
public:
  explicit DiskStore(const std::string &directory);
  ~DiskStore();
  DiskStore(const DiskStore&) = delete;
  DiskStore& operator=(const DiskStore&) = delete;

  uint64_t wal_cursor() const noexcept;
  uint64_t wal_generation() const noexcept;
  uint64_t data_generation() const noexcept;
  // Explicit offline compaction: copy live rows and rebuild a dense B+Tree.
  // Atomic manifest generation switch keeps SIGKILL recovery safe.
  void compact();
  void checkpoint_wal(uint64_t new_generation);
  const std::string& catalog() const noexcept;
  std::optional<std::string> get(const std::string& table, int64_t pk) const;
  std::optional<RowRef> get_ref(const std::string& table, int64_t pk) const;
  // Scans only B+Tree leaf metadata. Row payload is fetched lazily, allowing
  // higher layers to use a separate, size-bounded decoded-row cache.
  void scan(const std::string& table,
            const std::function<void(int64_t, uint64_t, uint32_t)>& visitor) const;
  // Inclusive PK interval, optionally reversed, with early termination.
  // The visitor returns false to stop the traversal.
  void scan_range(const std::string& table,int64_t low,int64_t high,bool descending,
             const std::function<bool(int64_t,uint64_t,uint32_t)>& visitor) const;
  std::string read_row(uint64_t offset, uint32_t length) const;
  std::shared_ptr<HeapView> map_heap_readonly() const;
  void publish(uint64_t wal_cursor, const std::string& catalog,
               const std::vector<DiskChange>& changes);
  uint64_t page_count() const noexcept;
  uint64_t heap_bytes() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace mvdb
