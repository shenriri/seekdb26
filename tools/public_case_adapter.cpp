#include "minivectordb/db.h"
#include <iostream>
#include <iomanip>
#include <string>

static void json_str(std::ostream& os, const std::string& s) {
  os << '"';
  const char* hex = "0123456789abcdef";
  for (unsigned char c : s) {
    if (c == '"') os << "\\\"";
    else if (c == '\\') os << "\\\\";
    else if (c == '\n') os << "\\n";
    else if (c == '\r') os << "\\r";
    else if (c == '\t') os << "\\t";
    else if (c < 32) { os << "\\u00" << hex[c >> 4] << hex[c & 15]; }
    else os << static_cast<char>(c);
  }
  os << '"';
}
int main(int argc,char** argv) {
  if (argc != 2) { std::cerr << "usage: public_case_adapter DATA_DIR\n"; return 2; }
  try {
    mvdb::Database db(argv[1]);
    std::string sql;
    while (std::getline(std::cin,sql)) {
      if (sql.empty()) continue;
      mvdb::Result r=db.execute(sql);
      std::cout << "{\"ok\":" << (r.ok?"true":"false");
      if (!r.ok) { std::cout << ",\"error\":"; json_str(std::cout,r.error); }
      else {
        std::cout << ",\"affected_rows\":" << r.affected_rows;
        if (r.is_query) {
          std::cout << ",\"columns\":[";
          for (size_t i=0;i<r.columns.size();++i) {if (i)std::cout<<',';json_str(std::cout,r.columns[i]);}
          std::cout << "],\"rows\":[";
          for (size_t i=0;i<r.rows.size();++i) {
            if (i)std::cout<<',';std::cout << '[';
            for (size_t j=0;j<r.rows[i].size();++j) {if (j)std::cout<<',';json_str(std::cout,r.rows[i][j]);}
            std::cout << ']';
          }
          std::cout << ']';
        }
      }
      std::cout << "}\n" << std::flush;
    }
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
