#include "index_io.h"

#include <cstring>
#include <filesystem>

namespace search::io {
namespace {

// Read exactly n bytes, or fail. istream::read sets failbit on a short read,
// but gcount() is the authoritative answer, so check that.
bool read_exact(std::istream& in, char* dst, std::streamsize n) {
    in.read(dst, n);
    return in.gcount() == n;
}

}  // namespace

// ── Fixed-width little-endian scalars ───────────────────────────────────────

void write_u32(std::ostream& out, std::uint32_t v) {
    char b[4];
    b[0] = static_cast<char>(v & 0xFFu);
    b[1] = static_cast<char>((v >> 8) & 0xFFu);
    b[2] = static_cast<char>((v >> 16) & 0xFFu);
    b[3] = static_cast<char>((v >> 24) & 0xFFu);
    out.write(b, 4);
}

void write_u64(std::ostream& out, std::uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<char>((v >> (8 * i)) & 0xFFull);
    }
    out.write(b, 8);
}

void write_i32(std::ostream& out, std::int32_t v) {
    // Copy the bit pattern rather than converting, so negative doc_ids survive
    // without relying on implementation-defined narrowing.
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u32(out, bits);
}

bool read_u32(std::istream& in, std::uint32_t& v) {
    unsigned char b[4];
    if (!read_exact(in, reinterpret_cast<char*>(b), 4)) return false;
    v = static_cast<std::uint32_t>(b[0])
      | (static_cast<std::uint32_t>(b[1]) << 8)
      | (static_cast<std::uint32_t>(b[2]) << 16)
      | (static_cast<std::uint32_t>(b[3]) << 24);
    return true;
}

bool read_u64(std::istream& in, std::uint64_t& v) {
    unsigned char b[8];
    if (!read_exact(in, reinterpret_cast<char*>(b), 8)) return false;
    v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint64_t>(b[i]);
    }
    return true;
}

bool read_i32(std::istream& in, std::int32_t& v) {
    std::uint32_t bits = 0;
    if (!read_u32(in, bits)) return false;
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

// ── LEB128 varint ───────────────────────────────────────────────────────────

void write_varint(std::ostream& out, std::uint32_t v) {
    while (v >= 0x80u) {
        out.put(static_cast<char>((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    out.put(static_cast<char>(v));
}

bool read_varint(std::istream& in, std::uint32_t& v) {
    std::uint32_t result = 0;
    for (int i = 0; i < 5; ++i) {
        const int c = in.get();
        if (c == std::istream::traits_type::eof()) return false;

        const std::uint32_t byte = static_cast<std::uint32_t>(c) & 0xFFu;

        // A 32-bit value needs at most 5 groups, and the 5th carries only 4
        // usable bits. Anything wider is malformed, not a big number.
        if (i == 4 && (byte & 0x7Fu) > 0x0Fu) return false;

        result |= (byte & 0x7Fu) << (7 * i);
        if ((byte & 0x80u) == 0) {
            v = result;
            return true;
        }
    }
    return false;  // no terminating byte within 5
}

// ── Length-prefixed strings ─────────────────────────────────────────────────

void write_string(std::ostream& out, const std::string& s) {
    write_u32(out, static_cast<std::uint32_t>(s.size()));
    if (!s.empty()) out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool read_string(std::istream& in, std::string& s) {
    std::uint32_t len = 0;
    if (!read_u32(in, len)) return false;
    if (len > kMaxStringBytes) return false;  // corrupt length; do not allocate

    s.assign(len, '\0');
    if (len == 0) return true;
    return read_exact(in, &s[0], static_cast<std::streamsize>(len));
}

// ── Helpers ─────────────────────────────────────────────────────────────────

std::uint64_t index_size_bytes(const std::string& dir) {
    namespace fs = std::filesystem;
    const fs::path base(dir);

    std::uint64_t total = 0;
    for (const char* name : {kMetaFile, kDocsIdxFile, kDocsFile, kTermsFile}) {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(base / name, ec);
        if (ec) return 0;  // incomplete index; report nothing rather than a partial sum
        total += static_cast<std::uint64_t>(size);
    }
    return total;
}

bool at_eof(std::istream& in) {
    return in.peek() == std::istream::traits_type::eof();
}

}  // namespace search::io
