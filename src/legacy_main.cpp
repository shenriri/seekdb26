#include "minivectordb/db.h"
#include <iostream>
#include <string>
#include <exception>

static std::string escape(const std::string& v) {
  std::string out;
  for (char c : v) {
    if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out.push_back(c);
  }
  return out;
}
int legacy_main(int argc, char** argv) {
  std::string data_dir = "/data";
  bool ann_mode = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
    else if (std::string(argv[i]) == "--search-mode" && i + 1 < argc) {
      std::string mode=argv[++i];
      if(mode!="exact"&&mode!="ann") {std::cerr<<"search mode must be exact or ann\n";return 2;}
      ann_mode = (mode=="ann");
    }
    else { std::cerr << "usage: minivectordb [--data-dir DIR] [--search-mode exact|ann]\n"; return 2; }
  }
  try {
    mvdb::Database db(data_dir,ann_mode);
    // PROVISIONAL developer protocol: exactly one SQL statement per input line.
    // Replace only this adapter when the official test driver is published.
    std::string sql;
    while (std::getline(std::cin, sql)) {
      if (sql == "\\q" || sql == ".exit") break;
      if (sql.empty()) continue;
      auto r = db.execute(sql);
      if (!r.ok) std::cout << "ERR\t" << escape(r.error) << '\n';
      else if (!r.is_query) std::cout << "OK\n";
      else {
        std::cout << "ROWS\t" << r.rows.size() << '\n';
        for (const auto& row : r.rows) {
          std::cout << "ROW";
          for (const auto& field : row) std::cout << '\t' << escape(field);
          std::cout << '\n';
        }
        std::cout << "END\n";
      }
      std::cout.flush();
    }
  } catch (const std::exception& e) {
    std::cerr << "FATAL: " << e.what() << '\n';
    return 1;
  }
  return 0;
}

#ifdef MVDB_LEGACY_DEFAULT
int main(int argc, char** argv) { return legacy_main(argc, argv); }
#endif
