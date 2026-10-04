#include <array>
#include <charconv>
#include <fstream>
#include <string_view>
#include <system_error>

#include "clache/trace.h"

#include "trace_internal.h"
namespace clache {
namespace {
class Reader final : public TraceReader {
 public:
  explicit Reader(const std::filesystem::path& path) : path_(path), in_(path, std::ios::binary) {
    if (!in_) trace_detail::fail(path_, 0, "cannot open trace");
    std::array<unsigned char, 64> h{};
    in_.read(reinterpret_cast<char*>(h.data()), 8);
    binary_ =
        in_.gcount() == 8 && std::string_view(reinterpret_cast<char*>(h.data()), 8) == "CLACHETR";
    in_.clear();
    in_.seekg(0);
    if (binary_) {
      in_.read(reinterpret_cast<char*>(h.data()), 64);
      if (in_.gcount() != 64) trace_detail::fail(path_, 0, "truncated binary header");
      if (trace_detail::decode(h.data() + 8, 4) != 1 ||
          trace_detail::decode(h.data() + 12, 4) != 32)
        trace_detail::fail(path_, 0, "unsupported binary version or record size");
      for (std::size_t i = 24; i < 64; ++i)
        if (h[i] != 0) trace_detail::fail(path_, 0, "unsupported flags or reserved bytes");
      count_ = trace_detail::decode(h.data() + 16, 8);
      in_.seekg(0, std::ios::end);
      const auto length = in_.tellg();
      if (length < 64 || static_cast<std::uint64_t>(length - std::streamoff{64}) / 32 != count_ ||
          (length - std::streamoff{64}) % 32 != 0)
        trace_detail::fail(path_, 0, "record_count does not match file length");
      in_.seekg(64);
      region_ = op_ = true;
    } else {
      if (!line()) trace_detail::fail(path_, line_, "missing CSV header");
      const std::string base = "timestamp_ns,key_id,size_bytes,miss_cost_us,ttl_s";
      if (text_ == base) {
      } else if (text_ == base + ",region")
        region_ = true;
      else if (text_ == base + ",op")
        op_ = true;
      else if (text_ == base + ",region,op")
        region_ = op_ = true;
      else
        trace_detail::fail(path_, line_, "invalid CSV header");
    }
  }
  bool next(TraceRecord& out) override {
    TraceRecord r;
    if (binary_) {
      if (index_ == count_) return false;
      ++index_;
      std::array<unsigned char, 32> b{};
      in_.read(reinterpret_cast<char*>(b.data()), 32);
      if (in_.gcount() != 32) trace_detail::fail(path_, index_, "truncated binary record");
      r.timestamp_ns = trace_detail::decode(b.data(), 8);
      r.key = trace_detail::decode(b.data() + 8, 8);
      r.size_bytes = static_cast<std::uint32_t>(trace_detail::decode(b.data() + 16, 4));
      r.miss_cost_us = static_cast<std::uint32_t>(trace_detail::decode(b.data() + 20, 4));
      r.ttl_s = static_cast<std::uint32_t>(trace_detail::decode(b.data() + 24, 4));
      r.region = static_cast<std::uint16_t>(trace_detail::decode(b.data() + 28, 2));
      r.op = static_cast<TraceOp>(b[30]);
      if (b[31] != 0) trace_detail::fail(path_, index_, "nonzero reserved byte");
    } else {
      if (!line()) return false;
      std::array<std::string_view, 7> fields{};
      std::size_t n = 0, start = 0;
      while (true) {
        const auto end = text_.find(',', start);
        if (n == fields.size()) trace_detail::fail(path_, line_, "too many CSV columns");
        fields[n++] =
            std::string_view(text_).substr(start, end == std::string::npos ? end : end - start);
        if (end == std::string::npos) break;
        start = end + 1;
      }
      if (n != 5 + static_cast<std::size_t>(region_) + static_cast<std::size_t>(op_))
        trace_detail::fail(path_, line_, "wrong CSV column count");
      number(fields[0], r.timestamp_ns);
      number(fields[1], r.key);
      number(fields[2], r.size_bytes);
      number(fields[3], r.miss_cost_us);
      number(fields[4], r.ttl_s);
      if (region_) number(fields[5], r.region);
      if (op_) {
        const auto s = fields[region_ ? 6 : 5];
        if (s == "del")
          r.op = TraceOp::Del;
        else if (s != "get")
          trace_detail::fail(path_, line_, "invalid op");
      }
    }
    trace_detail::validate(r, previous_, path_, binary_ ? index_ : line_);
    previous_ = r.timestamp_ns;
    out = r;
    return true;
  }
  TraceFormat format() const override { return binary_ ? TraceFormat::Binary : TraceFormat::Csv; }
  bool has_region_column() const override { return region_; }
  bool has_op_column() const override { return op_; }

 private:
  template <class T>
  void number(std::string_view s, T& value) {
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size())
      trace_detail::fail(path_, line_, "invalid unsigned integer: " + std::string(s));
  }
  bool line() {
    while (std::getline(in_, text_)) {
      ++line_;
      if (!text_.empty() && text_.back() == '\r') text_.pop_back();
      if (!text_.empty() && text_[0] == '#') continue;
      return true;
    }
    if (in_.bad()) trace_detail::fail(path_, line_, "read failure");
    return false;
  }
  std::filesystem::path path_;
  std::ifstream in_;
  std::string text_;
  bool binary_ = false, region_ = false, op_ = false;
  std::uint64_t line_ = 0, index_ = 0, count_ = 0, previous_ = 0;
};
}  // namespace
std::unique_ptr<TraceReader> TraceReader::open(const std::filesystem::path& p) {
  return std::make_unique<Reader>(p);
}
std::size_t Trace::size() const noexcept { return key.size(); }
TraceRecord Trace::at(std::size_t i) const {
  return {timestamp_ns.at(i), key.at(i),    size_bytes.at(i), miss_cost_us.at(i),
          ttl_s.at(i),        region.at(i), op.at(i)};
}
Trace load_trace(const std::filesystem::path& p) {
  auto reader = TraceReader::open(p);
  Trace trace;
  TraceRecord r;
  while (reader->next(r)) {
    trace.timestamp_ns.push_back(r.timestamp_ns);
    trace.key.push_back(r.key);
    trace.size_bytes.push_back(r.size_bytes);
    trace.miss_cost_us.push_back(r.miss_cost_us);
    trace.ttl_s.push_back(r.ttl_s);
    trace.region.push_back(r.region);
    trace.op.push_back(r.op);
  }
  return trace;
}
}  // namespace clache
