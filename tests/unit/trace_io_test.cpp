#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "clache/hash.h"
#include "clache/trace.h"

#include "gtest/gtest.h"
namespace clache {
namespace {
class TraceIoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> id{0};
    const auto base = std::filesystem::temp_directory_path();
    do {
      dir_ = base / ("clache-trace-test-" + std::to_string(id.fetch_add(1)));
    } while (!std::filesystem::create_directory(dir_));
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }
  std::filesystem::path path(std::string name = "trace") const { return dir_ / name; }
  void put(const std::string& s) {
    std::ofstream out(path(), std::ios::binary);
    out << s;
  }
  std::string contents(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
  void bad(const std::string& s, std::string line) {
    put(s);
    try {
      (void)load_trace(path());
      FAIL() << "accepted invalid trace";
    } catch (const TraceError& e) {
      EXPECT_NE(std::string(e.what()).find(path().string() + ":" + line + ":"), std::string::npos)
          << e.what();
    }
  }
  const std::string header_ = "timestamp_ns,key_id,size_bytes,miss_cost_us,ttl_s";
  std::filesystem::path dir_;
};
TEST_F(TraceIoTest, OptionalColumnsAndRoundTrips) {
  for (bool region : {false, true})
    for (bool op : {false, true}) {
      std::vector<TraceRecord> records = {
          {5, 42, 4096, 10, 60, static_cast<std::uint16_t>(region ? 3 : 0), TraceOp::Get},
          {5, std::numeric_limits<std::uint64_t>::max(), op ? 0U : 1U, 0, 0, 0,
           op ? TraceOp::Del : TraceOp::Get}};
      auto csv = TraceWriter::create(path(), TraceFormat::Csv, {region, op});
      for (const auto& r : records) csv->write(r);
      csv->close();
      csv->close();
      auto reader = TraceReader::open(path());
      EXPECT_EQ(reader->format(), TraceFormat::Csv);
      EXPECT_EQ(reader->has_region_column(), region);
      EXPECT_EQ(reader->has_op_column(), op);
      auto bin = TraceWriter::create(path("bin"), TraceFormat::Binary);
      TraceRecord r;
      while (reader->next(r)) bin->write(r);
      bin.reset();
      const auto loaded = load_trace(path("bin"));
      ASSERT_EQ(loaded.size(), records.size());
      EXPECT_EQ(TraceReader::open(path("bin"))->format(), TraceFormat::Binary);
      auto again = TraceWriter::create(path("again"), TraceFormat::Csv, {region, op});
      for (std::size_t i = 0; i < loaded.size(); ++i) {
        const auto v = loaded.at(i);
        EXPECT_EQ(v.key, records[i].key);
        EXPECT_EQ(v.op, records[i].op);
        again->write(v);
      }
      again->close();
      EXPECT_EQ(contents(path()), contents(path("again")));
      EXPECT_THROW(loaded.at(loaded.size()), std::out_of_range);
    }
}
TEST_F(TraceIoTest, CsvMaximumWidthRecord) {
  auto writer = TraceWriter::create(path(), TraceFormat::Csv, {true, true});
  const TraceRecord maximum{std::numeric_limits<std::uint64_t>::max(),
                            std::numeric_limits<KeyId>::max(),
                            std::numeric_limits<std::uint32_t>::max(),
                            std::numeric_limits<std::uint32_t>::max(),
                            std::numeric_limits<std::uint32_t>::max(),
                            std::numeric_limits<std::uint16_t>::max(),
                            TraceOp::Get};
  writer->write(maximum);
  auto deleted = maximum;
  deleted.op = TraceOp::Del;
  writer->write(deleted);
  writer->close();
  const std::string fields =
      "18446744073709551615,18446744073709551615,4294967295,4294967295,4294967295,65535";
  EXPECT_EQ(contents(path()), header_ + ",region,op\n" + fields + ",get\n" + fields + ",del\n");
  const auto loaded = load_trace(path());
  ASSERT_EQ(loaded.size(), 2U);
  EXPECT_EQ(loaded.at(0).timestamp_ns, maximum.timestamp_ns);
  EXPECT_EQ(loaded.at(0).key, maximum.key);
  EXPECT_EQ(loaded.at(0).region, maximum.region);
  EXPECT_EQ(loaded.at(1).op, TraceOp::Del);
}
TEST_F(TraceIoTest, CsvValidation) {
  bad("", "0");
  bad("wrong\n", "1");
  const std::vector<std::string> rows = {"1,2,0,0,0",
                                         "-1,2,1,0,0",
                                         "1,18446744073709551616,1,0,0",
                                         "1,2,4294967296,0,0",
                                         "1,2,1,4294967296,0",
                                         "1,2,1,0,4294967296",
                                         "1,2,1,0",
                                         "1,2,1,0,0,0",
                                         "1,2,1,0,0,",
                                         "1,2,1,0,",
                                         "1,2,1x,0,0",
                                         "1, 2,1,0,0",
                                         ""};
  for (const auto& row : rows) bad(header_ + "\n" + row + "\n", "2");
  bad(header_ + "\n2,2,1,0,0\n1,2,1,0,0\n", "3");
  bad(header_ + ",region\n1,2,1,0,0,65536\n", "2");
  bad(header_ + ",op\n1,2,1,0,0,SET\n", "2");
  put("# first\r\n" + header_ + ",op\r\n# comment\r\n0,0,0,0,0,del\r\n");
  EXPECT_EQ(load_trace(path()).size(), 1U);
}
TEST_F(TraceIoTest, BinaryLayoutAndCorruption) {
  auto w = TraceWriter::create(path(), TraceFormat::Binary);
  w->write({0x0102, 0x0304, 5, 6, 7, 8, TraceOp::Get});
  w->close();
  const auto data = contents(path());
  ASSERT_EQ(data.size(), 96U);
  EXPECT_EQ(data.substr(0, 8), "CLACHETR");
  EXPECT_EQ(static_cast<unsigned char>(data[16]), 1);
  EXPECT_EQ(static_cast<unsigned char>(data[64]), 2);
  EXPECT_EQ(static_cast<unsigned char>(data[65]), 1);
  for (std::size_t offset : {8U, 12U, 16U, 24U, 32U, 80U, 94U, 95U}) {
    auto changed = data;
    changed[offset] = offset == 80 ? 0 : static_cast<char>(99);
    bad(changed, offset < 64 ? "0" : "1");
  }
  for (std::size_t size : {8U, 63U, 64U, 95U}) bad(data.substr(0, size), "0");
  bad(data + "x", "0");
  auto changed = data;
  changed[0] = 'X';
  bad(changed, "1");
}
TEST_F(TraceIoTest, WriterValidationAndEmptyFiles) {
  auto w = TraceWriter::create(path(), TraceFormat::Csv);
  EXPECT_THROW(w->write({1, 1, 0}), TraceError);
  EXPECT_THROW(w->write({1, 1, 1, 0, 0, 1}), TraceError);
  EXPECT_THROW(w->write({1, 1, 1, 0, 0, 0, TraceOp::Del}), TraceError);
  EXPECT_THROW(w->write({1, 1, 1, 0, 0, 0, static_cast<TraceOp>(2)}), TraceError);
  w->write({2, 1, 1});
  EXPECT_THROW(w->write({1, 1, 1}), TraceError);
  w->close();
  EXPECT_THROW(w->write({3, 1, 1}), TraceError);
  for (auto format : {TraceFormat::Csv, TraceFormat::Binary}) {
    auto empty = TraceWriter::create(path("empty"), format);
    empty->close();
    EXPECT_EQ(load_trace(path("empty")).size(), 0U);
  }
  EXPECT_THROW(TraceReader::open(path("missing")), TraceError);
  EXPECT_THROW(TraceWriter::create(path("missing") / "child", TraceFormat::Csv), TraceError);
}
TEST_F(TraceIoTest, BinaryTimestampValidationAndNumericLimits) {
  auto writer = TraceWriter::create(path(), TraceFormat::Binary);
  const auto u32 = std::numeric_limits<std::uint32_t>::max();
  writer->write({100, 1, u32, u32, u32, std::numeric_limits<std::uint16_t>::max()});
  writer->write({101, 2, 1});
  writer->close();
  auto data = contents(path());
  EXPECT_EQ(load_trace(path()).at(0).ttl_s, u32);
  data[96] = 99;
  bad(data, "2");
}
TEST_F(TraceIoTest, SidecarRejectsMalformedJson) {
  TraceMeta meta;
  write_trace_meta(path(), meta);
  const auto valid = contents(path());
  for (const std::string suffix : {",", "x", "{}"}) {
    put(valid + suffix);
    EXPECT_THROW(read_trace_meta(path()), TraceError);
  }
  auto invalid = valid;
  invalid.replace(invalid.find("\"seed\":0"), 8, "\"seed\":-1");
  put(invalid);
  EXPECT_THROW(read_trace_meta(path()), TraceError);
  invalid = valid;
  invalid.insert(1, "\"seed\":0,");
  put(invalid);
  EXPECT_THROW(read_trace_meta(path()), TraceError);
  put(valid.substr(0, valid.size() - 3));
  EXPECT_THROW(read_trace_meta(path()), TraceError);
}
TEST_F(TraceIoTest, MetadataAndFingerprint) {
  TraceMeta m;
  m.generator_version = "v1";
  m.git_sha = "abc";
  m.workload = "test\n\"\\";
  m.params = {{"z", "\t\r\b\f\n"}, {"a", std::string("\0", 1)}};
  m.seed = std::numeric_limits<std::uint64_t>::max();
  m.request_count = 100;
  m.unique_keys = 12;
  m.working_set_bytes = 345;
  m.fingerprint = 987;
  const auto p = meta_path_for(path());
  EXPECT_EQ(p.string(), path().string() + ".meta.json");
  write_trace_meta(p, m);
  const auto read = read_trace_meta(p);
  EXPECT_EQ(read.workload, m.workload);
  EXPECT_EQ(read.seed, m.seed);
  EXPECT_EQ(read.generator_version, m.generator_version);
  EXPECT_EQ(read.git_sha, m.git_sha);
  EXPECT_EQ(read.request_count, m.request_count);
  EXPECT_EQ(read.unique_keys, m.unique_keys);
  EXPECT_EQ(read.working_set_bytes, m.working_set_bytes);
  EXPECT_EQ(read.fingerprint, m.fingerprint);
  EXPECT_EQ(read.params.front(), m.params.back());
  write_trace_meta(path("meta2"), read);
  EXPECT_EQ(contents(p), contents(path("meta2")));
  put("abcdef");
  EXPECT_EQ(fingerprint_file(path()), hash64("abcdef"));
  const auto before = fingerprint_file(path());
  put("abcdeg");
  EXPECT_NE(before, fingerprint_file(path()));
  m.params.push_back(m.params[0]);
  EXPECT_THROW(write_trace_meta(p, m), TraceError);
  put("{}");
  EXPECT_THROW(read_trace_meta(path()), TraceError);
  EXPECT_THROW(fingerprint_file(path("missing")), TraceError);
}
}  // namespace
}  // namespace clache
