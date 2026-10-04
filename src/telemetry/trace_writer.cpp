#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include "clache/trace.h"

#include "trace_internal.h"
namespace clache {
namespace {
class Writer final : public TraceWriter {
 public:
  Writer(const std::filesystem::path& p, TraceFormat f, TraceWriterOptions o)
      : path_(p), out_(p, std::ios::binary | std::ios::trunc), format_(f), options_(o) {
    if (!out_) trace_detail::fail(path_, 0, "cannot create trace");
    if (f == TraceFormat::Binary) {
      std::array<unsigned char, 64> h{};
      const std::string_view magic = "CLACHETR";
      for (std::size_t i = 0; i < 8; ++i) h[i] = static_cast<unsigned char>(magic[i]);
      trace_detail::encode(h.data() + 8, 1, 4);
      trace_detail::encode(h.data() + 12, 32, 4);
      out_.write(reinterpret_cast<const char*>(h.data()), 64);
    } else {
      out_ << "timestamp_ns,key_id,size_bytes,miss_cost_us,ttl_s";
      if (o.include_region) out_ << ",region";
      if (o.include_op) out_ << ",op";
      out_ << '\n';
    }
    check();
  }
  ~Writer() override {
    try {
      close();
    } catch (...) {
    }
  }
  void write(const TraceRecord& r) override {
    if (closed_) trace_detail::fail(path_, count_ + 1, "writer is closed");
    trace_detail::validate(r, previous_, path_, count_ + 1);
    if (count_ == std::numeric_limits<std::uint64_t>::max())
      trace_detail::fail(path_, count_, "too many records");
    if (format_ == TraceFormat::Binary) {
      std::array<unsigned char, 32> b{};
      trace_detail::encode(b.data(), r.timestamp_ns, 8);
      trace_detail::encode(b.data() + 8, r.key, 8);
      trace_detail::encode(b.data() + 16, r.size_bytes, 4);
      trace_detail::encode(b.data() + 20, r.miss_cost_us, 4);
      trace_detail::encode(b.data() + 24, r.ttl_s, 4);
      trace_detail::encode(b.data() + 28, r.region, 2);
      b[30] = static_cast<unsigned char>(r.op);
      out_.write(reinterpret_cast<const char*>(b.data()), 32);
    } else {
      if (!options_.include_region && r.region != 0)
        trace_detail::fail(path_, count_ + 1, "region column required");
      if (!options_.include_op && r.op != TraceOp::Get)
        trace_detail::fail(path_, count_ + 1, "op column required");
      csv_line_.clear();
      const auto append = [&](std::uint64_t v) {
        // Format fields independently so a failed conversion can never leave a
        // cursor at the end of the row buffer before a separator is written.
        std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> digits{};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), v);
        if (result.ec != std::errc{})
          trace_detail::fail(path_, count_ + 1, "integer formatting failure");
        csv_line_.append(digits.data(), static_cast<std::size_t>(result.ptr - digits.data()));
      };
      append(r.timestamp_ns);
      csv_line_ += ',';
      append(r.key);
      csv_line_ += ',';
      append(r.size_bytes);
      csv_line_ += ',';
      append(r.miss_cost_us);
      csv_line_ += ',';
      append(r.ttl_s);
      if (options_.include_region) {
        csv_line_ += ',';
        append(r.region);
      }
      if (options_.include_op) csv_line_ += r.op == TraceOp::Get ? ",get" : ",del";
      csv_line_ += '\n';
      out_.write(csv_line_.data(), static_cast<std::streamsize>(csv_line_.size()));
    }
    check();
    previous_ = r.timestamp_ns;
    ++count_;
  }
  void close() override {
    if (closed_) return;
    if (format_ == TraceFormat::Binary) {
      out_.seekp(16);
      std::array<unsigned char, 8> b{};
      trace_detail::encode(b.data(), count_, 8);
      out_.write(reinterpret_cast<const char*>(b.data()), 8);
    }
    out_.flush();
    check();
    out_.close();
    closed_ = true;
    check();
  }

 private:
  void check() {
    if (!out_) trace_detail::fail(path_, count_ + 1, "write failure");
  }
  std::filesystem::path path_;
  std::ofstream out_;
  std::string csv_line_;
  TraceFormat format_;
  TraceWriterOptions options_;
  std::uint64_t count_ = 0, previous_ = 0;
  bool closed_ = false;
};
}  // namespace
std::unique_ptr<TraceWriter> TraceWriter::create(const std::filesystem::path& p, TraceFormat f,
                                                 TraceWriterOptions o) {
  if (f != TraceFormat::Csv && f != TraceFormat::Binary) trace_detail::fail(p, 0, "invalid format");
  return std::make_unique<Writer>(p, f, o);
}
}  // namespace clache
