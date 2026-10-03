#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "clache/types.h"

namespace clache {

enum class TraceOp : std::uint8_t { Get = 0, Del = 1 };
enum class TraceFormat : std::uint8_t { Csv, Binary };

struct TraceRecord {
  std::uint64_t timestamp_ns = 0;
  KeyId key = 0;
  std::uint32_t size_bytes = 0;
  std::uint32_t miss_cost_us = 0;
  std::uint32_t ttl_s = 0;  // 0 = no TTL
  std::uint16_t region = 0;
  TraceOp op = TraceOp::Get;
};

// what() = "<path>:<line>: <message>" (record index instead of line for binary).
class TraceError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Streaming reader; format auto-detected from the "CLACHETR" magic.
class TraceReader {
 public:
  static std::unique_ptr<TraceReader> open(const std::filesystem::path& path);
  virtual ~TraceReader() = default;
  virtual bool next(TraceRecord& out) = 0;  // false at EOF; throws TraceError when invalid
  virtual TraceFormat format() const = 0;
  virtual bool has_region_column() const = 0;  // CSV: header had `region`; binary: true
  virtual bool has_op_column() const = 0;
};

// Whole trace in memory, struct-of-arrays, shared read-only across simulator runs.
struct Trace {
  std::vector<std::uint64_t> timestamp_ns;
  std::vector<KeyId> key;
  std::vector<std::uint32_t> size_bytes;
  std::vector<std::uint32_t> miss_cost_us;
  std::vector<std::uint32_t> ttl_s;
  std::vector<std::uint16_t> region;
  std::vector<TraceOp> op;
  std::size_t size() const noexcept;
  TraceRecord at(std::size_t i) const;
};
Trace load_trace(const std::filesystem::path& path);

struct TraceWriterOptions {
  bool include_region = false;  // CSV only: emit the optional `region` column
  bool include_op = false;      // CSV only: emit the optional `op` column
};
class TraceWriter {
 public:
  static std::unique_ptr<TraceWriter> create(const std::filesystem::path& path, TraceFormat format,
                                             TraceWriterOptions options = {});
  virtual ~TraceWriter() = default;
  virtual void write(const TraceRecord& record) = 0;  // enforces the §13.1 rules too
  virtual void close() = 0;  // flushes; binary patches record_count; idempotent; dtor calls it
};

// Sidecar (Spec §13.3). `params` are sorted by key when written.
struct TraceMeta {
  std::string generator_version;
  std::string git_sha;
  std::string workload;
  std::vector<std::pair<std::string, std::string>> params;
  std::uint64_t seed = 0;
  std::uint64_t request_count = 0;
  std::uint64_t unique_keys = 0;
  std::uint64_t working_set_bytes = 0;
  std::uint64_t fingerprint = 0;  // fingerprint_file() of the trace
};
std::filesystem::path meta_path_for(
    const std::filesystem::path& trace_path);  // "<trace>.meta.json"
void write_trace_meta(const std::filesystem::path& path, const TraceMeta& meta);
TraceMeta read_trace_meta(const std::filesystem::path& path);
std::uint64_t fingerprint_file(const std::filesystem::path& path);  // XXH3-64 (Hasher64)

}  // namespace clache
