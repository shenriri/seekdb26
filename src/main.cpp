#include "minivectordb/db.h"
#include "nlohmann/json.hpp"
#include <charconv>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Json = nlohmann::json;
int legacy_main(int argc, char** argv);

static void send(const Json& message) {
  std::cout << message.dump(-1, ' ', false, Json::error_handler_t::strict) << '\n' << std::flush;
}
static Json failure(const Json& id, const std::string& code, const std::string& message) {
  return Json{{"id", id}, {"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}
static Json success(const Json& id, const mvdb::Result& result) {
  if (!result.ok) return failure(id, result.error.empty() ? "SQL_ERROR" : result.error, result.error);
  Json response{{"id", id}, {"ok", true}};
  if (!result.is_query) { response["affected_rows"] = result.affected_rows; return response; }
  response["columns"] = result.columns;
  response["rows"] = Json::array();
  if (result.column_types.size() != result.columns.size()) throw std::runtime_error("RESULT_TYPE_METADATA");
  for (const auto& row : result.rows) {
    if (row.size() != result.columns.size()) throw std::runtime_error("RESULT_ROW_WIDTH");
    Json cells = Json::array();
    for (size_t i = 0; i < row.size(); ++i) {
      switch (result.column_types[i]) {
        case mvdb::ResultType::Text: cells.push_back(row[i]); break;
        case mvdb::ResultType::Bigint: {
          int64_t value = 0;
          const auto parsed = std::from_chars(row[i].data(), row[i].data()+row[i].size(), value);
          if (parsed.ec != std::errc{} || parsed.ptr != row[i].data()+row[i].size()) throw std::runtime_error("RESULT_BIGINT");
          cells.push_back(value); break;
        }
        case mvdb::ResultType::Real: {
          const double value = std::stod(row[i]);
          if (!std::isfinite(value)) throw std::runtime_error("RESULT_NONFINITE");
          cells.push_back(value); break;
        }
        case mvdb::ResultType::Vector: {
          auto vector = Json::parse(row[i]);
          if (!vector.is_array()) throw std::runtime_error("RESULT_VECTOR");
          for (const auto& value : vector)
            if (!value.is_number() || !std::isfinite(value.get<double>())) throw std::runtime_error("RESULT_NONFINITE_VECTOR");
          cells.push_back(std::move(vector)); break;
        }
      }
    }
    response["rows"].push_back(std::move(cells));
  }
  return response;
}
int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  std::string directory = "/data";
  bool default_ann = false, legacy = false;
  std::vector<char*> legacy_args{argv[0]};
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--protocol" && i+1 < argc) {
      const std::string protocol = argv[++i];
      if (protocol != "legacy" && protocol != "jsonl") { std::cerr << "protocol must be jsonl or legacy\n"; return 2; }
      legacy = protocol == "legacy";
    } else {
      legacy_args.push_back(argv[i]);
      if ((option == "--data-dir" || option == "--search-mode") && i+1 < argc) {
        const std::string value = argv[++i]; legacy_args.push_back(argv[i]);
        if (option == "--data-dir") directory = value;
        else if (value == "exact" || value == "ann") default_ann = value == "ann";
        else { std::cerr << "search mode must be exact or ann\n"; return 2; }
      } else { std::cerr << "unknown or incomplete option: " << option << '\n'; return 2; }
    }
  }
  if (legacy) return legacy_main(static_cast<int>(legacy_args.size()), legacy_args.data());
  try {
    auto database = std::make_unique<mvdb::Database>(directory, default_ann);
    send(Json{{"type", "ready"}});
    std::string line;
    while (std::getline(std::cin, line)) {
      Json id = nullptr;
      try {
        auto request = Json::parse(line);
        if (!request.is_object() || !request.contains("id")) throw std::invalid_argument("missing request id");
        id = request.at("id");
        if (!request.contains("op") || !request.at("op").is_string()) throw std::invalid_argument("missing string op");
        const auto op = request.at("op").get<std::string>();
        if (op == "shutdown") {
          database.reset();
          send(Json{{"id", id}, {"ok", true}});
          return 0;
        }
        if (op != "execute") throw std::invalid_argument("unsupported op");
        if (!request.contains("sql") || !request.at("sql").is_string()) throw std::invalid_argument("missing string sql");
        const auto mode = request.value("search_mode", std::string(default_ann ? "ann" : "exact"));
        if (mode != "exact" && mode != "ann") throw std::invalid_argument("invalid search_mode");
        database->set_search_mode(mode == "ann");
        send(success(id, database->execute(request.at("sql").get_ref<const std::string&>())));
      } catch (const Json::exception& error) { send(failure(id, "INVALID_REQUEST", error.what())); }
      catch (const std::invalid_argument& error) { send(failure(id, "INVALID_REQUEST", error.what())); }
      catch (const std::exception& error) { send(failure(id, "INTERNAL_ERROR", error.what())); }
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << "FATAL: " << error.what() << '\n'; return 1; }
}
