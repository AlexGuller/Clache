#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string_view>
#include <system_error>

#include "clache/hash.h"
#include "clache/trace.h"

#include "trace_internal.h"
namespace clache {
namespace {
std::string quote(std::string_view s) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (char c : s) {
    const auto b = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (b < 32) {
      out += "\\u00";
      out += hex[b >> 4];
      out += hex[b & 15];
    } else
      out += c;
  }
  return out + '"';
}
// This small parser accepts the sidecar schema, with strict duplicate/type checks.
class Parser {
 public:
  Parser(std::string text, std::filesystem::path path)
      : text_(std::move(text)), path_(std::move(path)) {}
  TraceMeta parse() {
    TraceMeta m;
    std::set<std::string> seen;
    expect('{');
    if (!take('}')) do {
        const auto key = string();
        if (!seen.insert(key).second) error("duplicate key");
        expect(':');
        if (key == "generator_version")
          m.generator_version = string();
        else if (key == "git_sha")
          m.git_sha = string();
        else if (key == "workload")
          m.workload = string();
        else if (key == "seed")
          m.seed = number();
        else if (key == "request_count")
          m.request_count = number();
        else if (key == "unique_keys")
          m.unique_keys = number();
        else if (key == "working_set_bytes")
          m.working_set_bytes = number();
        else if (key == "fingerprint")
          m.fingerprint = number();
        else if (key == "params") {
          std::set<std::string> keys;
          expect('{');
          if (!take('}')) {
            do {
              auto k = string();
              if (!keys.insert(k).second) error("duplicate parameter");
              expect(':');
              m.params.emplace_back(std::move(k), string());
            } while (take(','));
            expect('}');
          }
        } else
          error("unknown key");
      } while (take(','));
    if (seen.size() != 9) error("missing sidecar fields");
    expect('}');
    space();
    if (pos_ != text_.size()) error("trailing JSON content");
    std::sort(m.params.begin(), m.params.end());
    return m;
  }

 private:
  [[noreturn]] void error(const std::string& message) const {
    std::uint64_t line = 1;
    for (std::size_t i = 0; i < pos_; ++i)
      if (text_[i] == '\n') ++line;
    trace_detail::fail(path_, line, message);
  }
  void space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\r' || text_[pos_] == '\n'))
      ++pos_;
  }
  bool take(char c) {
    space();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  void expect(char c) {
    if (!take(c)) error(std::string("expected ") + c);
  }
  unsigned hex() {
    if (pos_ == text_.size()) error("truncated escape");
    const char c = text_[pos_++];
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    error("invalid hex escape");
  }
  std::string string() {
    expect('"');
    std::string out;
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 32) error("unescaped control character");
      if (c == '\\') {
        if (pos_ == text_.size()) error("truncated escape");
        c = text_[pos_++];
        switch (c) {
          case '"':
          case '\\':
          case '/':
            break;
          case 'b':
            c = '\b';
            break;
          case 'f':
            c = '\f';
            break;
          case 'n':
            c = '\n';
            break;
          case 'r':
            c = '\r';
            break;
          case 't':
            c = '\t';
            break;
          case 'u': {
            unsigned v = 0;
            for (int i = 0; i < 4; ++i) v = v * 16 + hex();
            if (v > 127) error("only ASCII unicode escapes supported");
            c = static_cast<char>(v);
            break;
          }
          default:
            error("invalid escape");
        }
      }
      out += c;
    }
    error("unterminated string");
  }
  std::uint64_t number() {
    space();
    const auto start = pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    if (pos_ - start > 1 && text_[start] == '0') error("leading zero");
    std::uint64_t v = 0;
    const auto r = std::from_chars(text_.data() + start, text_.data() + pos_, v);
    if (r.ec != std::errc{} || r.ptr != text_.data() + pos_) error("invalid unsigned integer");
    return v;
  }
  std::string text_;
  std::filesystem::path path_;
  std::size_t pos_ = 0;
};
}  // namespace
std::filesystem::path meta_path_for(const std::filesystem::path& p) {
  auto result = p;
  result += ".meta.json";
  return result;
}
void write_trace_meta(const std::filesystem::path& p, const TraceMeta& m) {
  std::map<std::string, std::string> params;
  for (const auto& [k, v] : m.params)
    if (!params.emplace(k, v).second) trace_detail::fail(p, 0, "duplicate parameter");
  std::string text = "{\n\"fingerprint\":" + std::to_string(m.fingerprint) +
                     ",\n\"generator_version\":" + quote(m.generator_version) +
                     ",\n\"git_sha\":" + quote(m.git_sha) + ",\n\"params\":{";
  bool first = true;
  for (const auto& [k, v] : params) {
    if (!first) text += ',';
    first = false;
    text += quote(k) + ":" + quote(v);
  }
  text += "},\n\"request_count\":" + std::to_string(m.request_count) +
          ",\n\"seed\":" + std::to_string(m.seed) +
          ",\n\"unique_keys\":" + std::to_string(m.unique_keys) +
          ",\n\"working_set_bytes\":" + std::to_string(m.working_set_bytes) +
          ",\n\"workload\":" + quote(m.workload) + "\n}\n";
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  if (!out) trace_detail::fail(p, 0, "cannot write sidecar");
}
TraceMeta read_trace_meta(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) trace_detail::fail(p, 0, "cannot open sidecar");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) trace_detail::fail(p, 0, "cannot read sidecar");
  return Parser(std::move(text), p).parse();
}
std::uint64_t fingerprint_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) trace_detail::fail(p, 0, "cannot open file");
  Hasher64 hash;
  std::array<char, 65536> buffer{};
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    hash.update(buffer.data(), static_cast<std::size_t>(in.gcount()));
  }
  if (in.bad()) trace_detail::fail(p, 0, "cannot read file");
  return hash.digest();
}
}  // namespace clache
