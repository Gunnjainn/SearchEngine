#pragma once
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>

// ---------------------------------------------------------------------------
// Binary primitives for the on-disk index format.
//
// Every integer is written little-endian explicitly, byte by byte, so an index
// written on one machine loads on another regardless of host endianness.
//
// The reader functions all return false rather than throwing, and never trust
// a length read from the file: a truncated or corrupt index makes load() fail,
// not crash or allocate wildly.
//
// The format itself is documented in engine/README.md.
// ---------------------------------------------------------------------------
namespace search::io {

// Format identity, written at the head of meta.bin.
inline constexpr char        kMagic[4] = {'S', 'E', 'I', 'X'};
inline constexpr std::uint32_t kVersion = 1;

// File names inside an index directory.
inline constexpr const char* kMetaFile  = "meta.bin";
inline constexpr const char* kDocsFile  = "docs.bin";
inline constexpr const char* kTermsFile = "terms.bin";

// Refuse to allocate for an implausible string length. Contract 1 documents are
// text, so 64 MiB for a single field is already far past reasonable.
inline constexpr std::uint32_t kMaxStringBytes = 64u * 1024u * 1024u;

// ── Fixed-width little-endian scalars ───────────────────────────────────────
void write_u32(std::ostream& out, std::uint32_t v);
void write_u64(std::ostream& out, std::uint64_t v);
void write_i32(std::ostream& out, std::int32_t v);

bool read_u32(std::istream& in, std::uint32_t& v);
bool read_u64(std::istream& in, std::uint64_t& v);
bool read_i32(std::istream& in, std::int32_t& v);

// ── LEB128 varint, unsigned ─────────────────────────────────────────────────
// One to five bytes; seven payload bits each, low group first, high bit set on
// every byte but the last. Small values cost one byte, which is the point:
// postings store doc_id *gaps*, and gaps are usually small.
void write_varint(std::ostream& out, std::uint32_t v);
bool read_varint(std::istream& in, std::uint32_t& v);

// ── Length-prefixed string: u32 byte count, then raw bytes ───────────────────
void write_string(std::ostream& out, const std::string& s);
bool read_string(std::istream& in, std::string& s);

// ── Helpers ─────────────────────────────────────────────────────────────────

// Combined size of the index files in `dir`; 0 if they are not all present.
std::uint64_t index_size_bytes(const std::string& dir);

// True if `in` has no bytes left. Used to reject trailing garbage.
bool at_eof(std::istream& in);

}  // namespace search::io
