# Engine

C++17 core search engine with a hand-rolled inverted index, BM25 scoring, and
top-k retrieval. Exposed over HTTP via the [Crow](https://crowcpp.org/)
header-only framework.

## Status

🚧 Not yet implemented.

class Engine {
  void build_from_jsonl(const std::string& path);   // A implements
  void load(const std::string& index_dir);          // A implements
  void save(const std::string& index_dir);          // A implements
  std::vector<Result> search(const std::string& query, int k);  // B implements
};
struct Result { int doc_id; double score; std::string snippet; };

