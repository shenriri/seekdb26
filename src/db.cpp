#include "minivectordb/db.h"
#include "minivectordb/disk_store.h"
#include "minivectordb/ivf_index.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <list>
#include <unordered_map>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace mvdb {
namespace {
namespace fs = std::filesystem;
using Vec = std::vector<float>;
using Value = std::variant<int64_t, std::string, Vec>;
using Row = std::vector<Value>;

struct DbError : std::runtime_error { using runtime_error::runtime_error; };
void require(bool cond, const std::string& msg) { if (!cond) throw DbError(msg); }
std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return s;
}
size_t utf8_chars(const std::string& s) {
  size_t count = 0;
  for (size_t i = 0; i < s.size();) {
    const unsigned char a = static_cast<unsigned char>(s[i]);
    if (a < 0x80) { ++i; ++count; continue; }
    uint32_t cp = 0; size_t n = 0;
    if (a >= 0xC2 && a <= 0xDF) { cp = a & 0x1Fu; n = 2; }
    else if (a >= 0xE0 && a <= 0xEF) { cp = a & 0x0Fu; n = 3; }
    else if (a >= 0xF0 && a <= 0xF4) { cp = a & 0x07u; n = 4; }
    else throw DbError("INVALID_UTF8");
    require(i + n <= s.size(), "INVALID_UTF8");
    for (size_t j = 1; j < n; ++j) {
      const auto b = static_cast<unsigned char>(s[i+j]);
      require((b & 0xC0) == 0x80, "INVALID_UTF8");
      cp = (cp << 6) | (b & 0x3F);
    }
    require((n != 2 || cp >= 0x80) && (n != 3 || cp >= 0x800) &&
            (n != 4 || cp >= 0x10000) && !(cp >= 0xD800 && cp <= 0xDFFF) &&
            cp <= 0x10FFFF, "INVALID_UTF8");
    i += n; ++count;
  }
  return count;
}
bool utf8_less(const std::string& a, const std::string& b) {
  return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
    [](char x,char y){return static_cast<unsigned char>(x)<static_cast<unsigned char>(y);});
}
int compare_values(const Value& a, const Value& b) {
  require(a.index() == b.index(), "TYPE_MISMATCH");
  if (auto x = std::get_if<int64_t>(&a)) {
    const auto y = std::get<int64_t>(b);
    return *x < y ? -1 : (*x > y ? 1 : 0);
  }
  if (auto x = std::get_if<std::string>(&a)) {
    const auto& y = std::get<std::string>(b);
    return utf8_less(*x,y) ? -1 : (utf8_less(y,*x) ? 1 : 0);
  }
  throw DbError("UNSUPPORTED_VECTOR_COMPARISON");
}
std::string value_string(const Value& v) {
  if (auto i = std::get_if<int64_t>(&v)) return std::to_string(*i);
  if (auto s = std::get_if<std::string>(&v)) return *s;
  const auto& vec = std::get<Vec>(v);
  std::ostringstream os; os << '[' << std::setprecision(std::numeric_limits<float>::max_digits10);
  for (size_t j=0;j<vec.size();++j) { if(j) os<<','; os<<vec[j]; }
  return os.str()+"]";
}
std::string double_string(double x) {
  std::ostringstream os; os << std::setprecision(15) << x;
  return os.str();
}
double l2(const Vec& a, const Vec& b) {
  require(a.size() == b.size(),"VECTOR_DIM_MISMATCH");
  double d=0;
  for(size_t i=0;i<a.size();++i){const double delta=static_cast<double>(a[i])-b[i];d+=delta*delta;}
  return std::sqrt(d);
}
// The compact scan path preserves the precise FP32 -> FP64 arithmetic and
// distance ordering of the ordinary decoded-row path.  It does not use
// approximate arithmetic or SIMD reductions that might disturb tie-breaking.
double l2_contiguous(const float* data, const Vec& q) {
  double d=0;
  for(size_t i=0;i<q.size();++i){
    const double delta=static_cast<double>(data[i])-q[i];
    d+=delta*delta;
  }
  return std::sqrt(d);
}


// A per-query SIMD filter for the distance hot loop, without changing exact
// TopK semantics: SIMD only rejects candidates provably beyond the canonical
// scalar kth distance (with a conservative rounding margin). Every retained
// candidate is rescored using the original scalar L2 routine. The accuracy
// path and tie-breaking therefore remain unchanged.
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2"), noinline))
static double l2_squared_avx2(const float* x,const float* q,size_t n) {
  __m256d a=_mm256_setzero_pd(),b=_mm256_setzero_pd();
  size_t i=0;
  for(;i+8<=n;i+=8){
    const __m256 a8=_mm256_loadu_ps(x+i), b8=_mm256_loadu_ps(q+i);
    const __m128 xl=_mm256_castps256_ps128(a8),xh=_mm256_extractf128_ps(a8,1);
    const __m128 ql=_mm256_castps256_ps128(b8),qh=_mm256_extractf128_ps(b8,1);
    const __m256d dlo=_mm256_sub_pd(_mm256_cvtps_pd(xl),_mm256_cvtps_pd(ql));
    const __m256d dhi=_mm256_sub_pd(_mm256_cvtps_pd(xh),_mm256_cvtps_pd(qh));
    a=_mm256_add_pd(a,_mm256_mul_pd(dlo,dlo));
    b=_mm256_add_pd(b,_mm256_mul_pd(dhi,dhi));
  }
  const __m256d total=_mm256_add_pd(a,b);
  alignas(32) double lane[4];
  _mm256_store_pd(lane,total);
  double sum=(lane[0]+lane[1])+(lane[2]+lane[3]);
  for(;i<n;++i){const double d=static_cast<double>(x[i])-q[i];sum+=d*d;}
  return sum;
}
// Optional FMA implementation of the SAME conservative double-precision
// lower-bound rejection. The final candidate is always rescored by scalar L2.
__attribute__((target("avx2,fma"), noinline))
static double l2_squared_avx2_fma(const float* x,const float* q,size_t n) {
  __m256d s0=_mm256_setzero_pd(),s1=_mm256_setzero_pd();
  __m256d s2=_mm256_setzero_pd(),s3=_mm256_setzero_pd();
  size_t i=0;
  for(;i+16<=n;i+=16){
    const __m256 x0=_mm256_loadu_ps(x+i),q0=_mm256_loadu_ps(q+i);
    const __m256 x1=_mm256_loadu_ps(x+i+8),q1=_mm256_loadu_ps(q+i+8);
    const __m256d a=_mm256_sub_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x0)),
                                  _mm256_cvtps_pd(_mm256_castps256_ps128(q0)));
    const __m256d b=_mm256_sub_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x0,1)),
                                  _mm256_cvtps_pd(_mm256_extractf128_ps(q0,1)));
    const __m256d c=_mm256_sub_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x1)),
                                  _mm256_cvtps_pd(_mm256_castps256_ps128(q1)));
    const __m256d d=_mm256_sub_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x1,1)),
                                  _mm256_cvtps_pd(_mm256_extractf128_ps(q1,1)));
    s0=_mm256_fmadd_pd(a,a,s0);s1=_mm256_fmadd_pd(b,b,s1);
    s2=_mm256_fmadd_pd(c,c,s2);s3=_mm256_fmadd_pd(d,d,s3);
  }
  const __m256d total=_mm256_add_pd(_mm256_add_pd(s0,s1),_mm256_add_pd(s2,s3));
  alignas(32) double lane[4];_mm256_store_pd(lane,total);
  double sum=(lane[0]+lane[1])+(lane[2]+lane[3]);
  for(;i<n;++i){const double d=static_cast<double>(x[i])-q[i];sum+=d*d;}
  return sum;
}
// Hoist the float32->float64 conversion of a query vector outside the
// candidate loop. This is strictly a pruning estimate, never the final key.
__attribute__((target("avx2,fma"), noinline))
static double l2_squared_avx2_prepared(const float* x,const double* q,size_t n) {
  __m256d s0=_mm256_setzero_pd(),s1=_mm256_setzero_pd();
  __m256d s2=_mm256_setzero_pd(),s3=_mm256_setzero_pd();
  size_t i=0;
  for(;i+16<=n;i+=16){
    const __m256 x0=_mm256_loadu_ps(x+i),x1=_mm256_loadu_ps(x+i+8);
    const __m256d a=_mm256_sub_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x0)),_mm256_loadu_pd(q+i));
    const __m256d b=_mm256_sub_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x0,1)),_mm256_loadu_pd(q+i+4));
    const __m256d c=_mm256_sub_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x1)),_mm256_loadu_pd(q+i+8));
    const __m256d d=_mm256_sub_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x1,1)),_mm256_loadu_pd(q+i+12));
    s0=_mm256_fmadd_pd(a,a,s0);s1=_mm256_fmadd_pd(b,b,s1);
    s2=_mm256_fmadd_pd(c,c,s2);s3=_mm256_fmadd_pd(d,d,s3);
  }
  const __m256d total=_mm256_add_pd(_mm256_add_pd(s0,s1),_mm256_add_pd(s2,s3));
  alignas(32) double lane[4];_mm256_store_pd(lane,total);
  double sum=(lane[0]+lane[1])+(lane[2]+lane[3]);
  for(;i<n;++i){const double d=static_cast<double>(x[i])-q[i];sum+=d*d;}
  return sum;
}
// AVX-512 optional rejection path: still conservative and never supplies
// the final exact candidate ordering key. Runtime feature-gated.
__attribute__((target("avx512f,fma"), noinline))
static double l2_squared_avx512_prepared(const float* x,const double* q,size_t n){
  __m512d s0=_mm512_setzero_pd(),s1=_mm512_setzero_pd();
  size_t i=0;
  for(;i+16<=n;i+=16){
    const __m256 x0=_mm256_loadu_ps(x+i),x1=_mm256_loadu_ps(x+i+8);
    const __m512d a=_mm512_sub_pd(_mm512_cvtps_pd(x0),_mm512_loadu_pd(q+i));
    const __m512d b=_mm512_sub_pd(_mm512_cvtps_pd(x1),_mm512_loadu_pd(q+i+8));
    s0=_mm512_fmadd_pd(a,a,s0);s1=_mm512_fmadd_pd(b,b,s1);
  }
  double sum=_mm512_reduce_add_pd(_mm512_add_pd(s0,s1));
  for(;i<n;++i){const double d=static_cast<double>(x[i])-q[i];sum+=d*d;}
  return sum;
}
#endif
static bool simd_pruning_enabled() {
  if(const char* e=std::getenv("MVDB_SIMD_PRUNE");e&&std::strcmp(e,"0")==0)return false;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}
static bool avx512_pruning_enabled(){
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  const char* e=std::getenv("MVDB_AVX512");
  return (!e || std::strcmp(e,"0")!=0) &&
         __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}
static bool prepared_query_enabled(){
  const char* e=std::getenv("MVDB_PREPARED_QUERY");
  return !e || std::strcmp(e,"0")!=0;
}
static bool fma_pruning_enabled() {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  const char* e=std::getenv("MVDB_FMA_PRUNE");
  return (!e || std::strcmp(e,"0")!=0) && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}
// Our threshold includes the forward rounding error for both the SIMD sum and
// the scalar reference (IEEE double unit roundoff), plus sqrt->square rounding.
static bool definitely_worse_than_kth(double simd_sq,double kth_distance,size_t n){
  const double unit=std::numeric_limits<double>::epsilon();
  const double gamma=unit*static_cast<double>(n+8)*16.;
  if(gamma>=0.1)return false; // very large dimensions fall back to scalar
  const double upper=kth_distance*kth_distance*(1.+gamma);
  return simd_sq>upper;
}

struct Token {
  enum Kind { Ident, Number, String, Symbol, End } kind;
  std::string text;
};
std::vector<Token> lex(std::string_view sql) {
  std::vector<Token> out;
  for(size_t i=0;i<sql.size();) {
    char c=sql[i];
    if(c==' '||c=='\t'||c=='\n'||c=='\r'){++i;continue;}
    if(std::isalpha(static_cast<unsigned char>(c))||c=='_'){
      size_t j=i+1;
      while(j<sql.size()&&(std::isalnum(static_cast<unsigned char>(sql[j]))||sql[j]=='_'))++j;
      out.push_back({Token::Ident,std::string(sql.substr(i,j-i))});i=j;continue;
    }
    if(c=='\''){
      std::string val; ++i;bool terminated=false;
      while(i<sql.size()) {
        if(sql[i]=='\'') {
          if(i+1<sql.size()&&sql[i+1]=='\''){val+='\'';i+=2;}
          else {++i;terminated=true;break;}
        } else val+=sql[i++];
      }
      require(terminated,"UNTERMINATED_STRING");
      out.push_back({Token::String,std::move(val)});continue;
    }
    if(std::isdigit(static_cast<unsigned char>(c)) ||
       ((c=='-'||c=='+')&&i+1<sql.size()&&(std::isdigit(static_cast<unsigned char>(sql[i+1]))||sql[i+1]=='.')) ||
       (c=='.'&&i+1<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[i+1])))) {
      size_t j=i;
      if(sql[j]=='+'||sql[j]=='-')++j;
      bool digit=false;
      while(j<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[j]))){++j;digit=true;}
      if(j<sql.size()&&sql[j]=='.') {++j;while(j<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[j]))){++j;digit=true;}}
      require(digit,"INVALID_NUMBER");
      if(j<sql.size()&&(sql[j]=='e'||sql[j]=='E')) {
        ++j;if(j<sql.size()&&(sql[j]=='+'||sql[j]=='-'))++j;
        size_t old=j;while(j<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[j])))++j;
        require(j>old,"INVALID_NUMBER");
      }
      out.push_back({Token::Number,std::string(sql.substr(i,j-i))});i=j;continue;
    }
    if(c=='<'||c=='>'||c=='=') {
      if(i+1<sql.size()&&sql[i+1]=='=') {out.push_back({Token::Symbol,std::string(sql.substr(i,2))});i+=2;}
      else {out.push_back({Token::Symbol,std::string(1,c)});++i;}
      continue;
    }
    if(std::string(",();[]*").find(c)!=std::string::npos) {
      out.push_back({Token::Symbol,std::string(1,c)});++i;continue;
    }
    throw DbError("INVALID_CHARACTER");
  }
  out.push_back({Token::End,""});return out;
}
int64_t parse_i64(const std::string& s) {
  int64_t out=0;
  auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),out);
  require(e==std::errc()&&p==s.data()+s.size(),"INVALID_BIGINT");
  return out;
}
uint32_t parse_u32(const std::string& s) {
  int64_t x=parse_i64(s);
  require(x>0 && x<=1000000,"INVALID_DIMENSION_OR_LENGTH");
  return static_cast<uint32_t>(x);
}
float parse_float32(const std::string& s) {
  require(!s.empty(),"INVALID_VECTOR_VALUE");
  errno=0;char* end=nullptr;
  double d=0;
  const char* begin=s.data();
  if(*begin=='+')++begin;
  const auto parsed=std::from_chars(begin,s.data()+s.size(),d,std::chars_format::general);
  if(parsed.ec==std::errc{} && parsed.ptr==s.data()+s.size())end=const_cast<char*>(parsed.ptr);
  else d=std::strtod(s.c_str(),&end); // Preserve prior underflow and whitespace semantics.
  require(end==s.c_str()+s.size()&&std::isfinite(d)&&
          std::fabs(d)<=std::numeric_limits<float>::max(),"INVALID_VECTOR_VALUE");
  const float f=static_cast<float>(d);
  require(std::isfinite(f),"INVALID_VECTOR_VALUE");
  return f;
}
struct Raw {
  enum Kind { Number, Str, Array } kind = Number;
  std::string text;
  std::vector<std::string> components;
};
Vec parse_vector(const Raw& raw) {
  std::vector<std::string> parts;
  if(raw.kind==Raw::Array) parts=raw.components;
  else if(raw.kind==Raw::Str) {
    std::string s=raw.text;
    size_t a=s.find_first_not_of(" \t\r\n");size_t z=s.find_last_not_of(" \t\r\n");
    require(a!=std::string::npos&&s[a]=='['&&s[z]==']',"INVALID_VECTOR_LITERAL");
    s=s.substr(a+1,z-a-1);
    std::istringstream stream(s);std::string part;
    while(std::getline(stream,part,',')) {
      auto start=part.find_first_not_of(" \t\r\n");auto end=part.find_last_not_of(" \t\r\n");
      require(start!=std::string::npos,"INVALID_VECTOR_LITERAL");
      parts.push_back(part.substr(start,end-start+1));
    }
  } else throw DbError("INVALID_VECTOR_LITERAL");
  require(!parts.empty(),"INVALID_VECTOR_LITERAL");
  Vec v;v.reserve(parts.size());
  for(const auto& part:parts)v.push_back(parse_float32(part));
  return v;
}
enum class ColType : uint8_t {Bigint=1,Varchar=2,Vector=3};
struct Column {std::string name;ColType type;uint32_t size=0;bool pk=false;};
struct Schema {std::string name;std::vector<Column> cols;size_t pk_idx=0;};
struct IndexDef {std::string table;std::string name;std::string col;};
int col_index(const Schema& s,const std::string& name) {
  for(size_t i=0;i<s.cols.size();++i)if(s.cols[i].name==name)return static_cast<int>(i);
  throw DbError("UNKNOWN_COLUMN:"+name);
}
Value typed_value(const Raw& raw,const Column& c) {
  if(c.type==ColType::Bigint) {
    require(raw.kind==Raw::Number,"TYPE_MISMATCH");return parse_i64(raw.text);
  }
  if(c.type==ColType::Varchar) {
    require(raw.kind==Raw::Str,"TYPE_MISMATCH");
    require(utf8_chars(raw.text)<=c.size,"VARCHAR_LENGTH_EXCEEDED");
    return raw.text;
  }
  Vec v=parse_vector(raw);
  require(v.size()==c.size,"VECTOR_DIM_MISMATCH");
  return v;
}
struct Expr {enum Type{Column,Distance,Star} type=Column;std::string col;Raw vec;std::string alias;};
struct Pred {std::string col;std::string op;Raw rhs;};
struct Assign {std::string col;Raw val;};
struct Statement {
  enum Type {CreateTable,CreateVectorIndex,Insert,Select,Update,Delete,Begin,Commit,Rollback} type;
  std::string table,index_name,index_col;
  std::vector<Column> columns;
  std::vector<std::string> insert_columns;
  std::vector<std::vector<Raw>> insert_rows;
  std::vector<Expr> projections;
  std::vector<Pred> predicates;
  std::vector<Assign> assigns;
  std::optional<Expr> order;
  bool descending=false;
  std::optional<size_t> limit;
};
class Parser {
  std::vector<Token> t_; size_t at_=0;
  const Token& cur() const{return t_.at(at_);}
  bool accept(const std::string& s) {if(lower(cur().text)==lower(s)&&cur().kind!=Token::End){++at_;return true;}return false;}
  bool peek(const std::string& s)const{return cur().kind!=Token::End&&lower(cur().text)==lower(s);}
  void expect(const std::string& s){require(accept(s),"SYNTAX_EXPECTED:"+s);}
  std::string name(){require(cur().kind==Token::Ident,"SYNTAX_EXPECTED_IDENTIFIER");return lower(t_[at_++].text);}
  std::string number(){require(cur().kind==Token::Number,"SYNTAX_EXPECTED_NUMBER");return t_[at_++].text;}
  Raw val(){
    if(cur().kind==Token::Number)return {Raw::Number,t_[at_++].text,{}};
    if(cur().kind==Token::String)return {Raw::Str,t_[at_++].text,{}};
    if(accept("[")) {
      Raw r;r.kind=Raw::Array;
      r.components.push_back(number());
      while(accept(","))r.components.push_back(number());
      expect("]");return r;
    }
    throw DbError("SYNTAX_EXPECTED_LITERAL");
  }
  Expr expr(){
    if(accept("l2_distance")){
      expect("(");Expr e;e.type=Expr::Distance;e.col=name();expect(",");e.vec=val();expect(")");return e;
    }
    Expr e;e.type=Expr::Column;e.col=name();return e;
  }
  std::vector<Pred> conditions(){
    std::vector<Pred> v;
    if(!accept("where"))return v;
    do {
      Pred p;p.col=name();
      require(cur().kind==Token::Symbol&&
        (peek("=")||peek("<")||peek(">")||peek("<=")||peek(">=")),"SYNTAX_EXPECTED_COMPARATOR");
      p.op=t_[at_++].text;p.rhs=val();v.push_back(std::move(p));
    } while(accept("and"));
    return v;
  }
  void end(){while(accept(";")){}require(cur().kind==Token::End,"SYNTAX_TRAILING_TOKENS");}
 public:
  explicit Parser(const std::string& sql):t_(lex(sql)){}
  Statement parse(){
    Statement s;
    if(accept("begin")) {s.type=Statement::Begin;accept("transaction");end();return s;}
    if(accept("commit")) {s.type=Statement::Commit;end();return s;}
    if(accept("rollback")) {s.type=Statement::Rollback;end();return s;}
    if(accept("create")) {
      if(accept("table")) {
        s.type=Statement::CreateTable;s.table=name();expect("(");
        std::string table_pk;
        do {
          if(accept("primary")){expect("key");expect("(");table_pk=name();expect(")");}
          else {
            Column col;col.name=name();
            if(accept("bigint"))col.type=ColType::Bigint;
            else if(accept("varchar")){col.type=ColType::Varchar;expect("(");col.size=parse_u32(number());expect(")");}
            else if(accept("vector")){col.type=ColType::Vector;expect("(");col.size=parse_u32(number());expect(")");}
            else throw DbError("UNSUPPORTED_COLUMN_TYPE");
            if(accept("primary")){expect("key");col.pk=true;}
            s.columns.push_back(std::move(col));
          }
        }while(accept(","));
        expect(")");end();
        if(!table_pk.empty()){
          for(auto& c:s.columns)if(c.name==table_pk)c.pk=true;
        }
        return s;
      }
      if(accept("vector")) {
        expect("index");s.type=Statement::CreateVectorIndex;s.index_name=name();expect("on");s.table=name();expect("(");s.index_col=name();expect(")");end();return s;
      }
      throw DbError("UNSUPPORTED_SQL");
    }
    if(accept("insert")){
      s.type=Statement::Insert;expect("into");s.table=name();
      if(accept("(")) {s.insert_columns.push_back(name());while(accept(","))s.insert_columns.push_back(name());expect(")");}
      expect("values");
      do {
        expect("(");std::vector<Raw> row;row.push_back(val());while(accept(","))row.push_back(val());expect(")");
        s.insert_rows.push_back(std::move(row));
      }while(accept(","));end();return s;
    }
    if(accept("update")) {
      s.type=Statement::Update;s.table=name();expect("set");
      do{Assign a;a.col=name();expect("=");a.val=val();s.assigns.push_back(std::move(a));}while(accept(","));
      s.predicates=conditions();end();return s;
    }
    if(accept("delete")){
      s.type=Statement::Delete;expect("from");s.table=name();s.predicates=conditions();end();return s;
    }
    if(accept("select")){
      s.type=Statement::Select;
      if(accept("*")){Expr e;e.type=Expr::Star;s.projections.push_back(e);}
      else {
        do{
          auto e=expr();
          if(accept("as"))e.alias=name();
          else if(cur().kind==Token::Ident&&!peek("from")&&!peek("where")&&!peek("order")&&!peek("limit"))e.alias=name();
          s.projections.push_back(std::move(e));
        }while(accept(","));
      }
      expect("from");s.table=name();s.predicates=conditions();
      if(accept("order")){
        expect("by");s.order=expr();
        if(accept("desc"))s.descending=true;else accept("asc");
      }
      if(accept("limit")){
        auto text=number();
        int64_t v=parse_i64(text);require(v>=0&&v<=100000000,"INVALID_LIMIT");
        s.limit=static_cast<size_t>(v);
      }
      end();return s;
    }
    throw DbError("UNSUPPORTED_SQL");
  }
};

// Versioned WAL: each complete checksummed record represents ONE committed transaction.
// A torn/incomplete trailing record is discarded on startup. No uncommitted data pages
// are modified, so a committed REDO log is sufficient for this first stage.
struct Writer {
  std::string buf;
  void u8(uint8_t x){buf.push_back(static_cast<char>(x));}
  void u32(uint32_t x){for(int i=0;i<4;++i)u8(static_cast<uint8_t>(x>>(8*i)));}
  void u64(uint64_t x){for(int i=0;i<8;++i)u8(static_cast<uint8_t>(x>>(8*i)));}
  void str(const std::string& s){require(s.size()<=UINT32_MAX,"RECORD_TOO_LARGE");u32(static_cast<uint32_t>(s.size()));buf+=s;}
  void val(const Value& v){
    u8(static_cast<uint8_t>(v.index()+1));
    if(auto p=std::get_if<int64_t>(&v))u64(static_cast<uint64_t>(*p));
    else if(auto p=std::get_if<std::string>(&v))str(*p);
    else {
      const auto& a=std::get<Vec>(v);u32(static_cast<uint32_t>(a.size()));
      for(float f:a){uint32_t bits;std::memcpy(&bits,&f,4);u32(bits);}
    }
  }
};
struct Reader {
  std::string_view s;size_t p=0;
  explicit Reader(std::string_view ss):s(ss){}
  uint8_t u8(){require(p<s.size(),"WAL_INVALID_RECORD");return static_cast<uint8_t>(s[p++]);}
  uint32_t u32(){uint32_t v=0;for(int i=0;i<4;++i)v|=static_cast<uint32_t>(u8())<<(8*i);return v;}
  uint64_t u64(){uint64_t v=0;for(int i=0;i<8;++i)v|=static_cast<uint64_t>(u8())<<(8*i);return v;}
  std::string str(){const uint32_t n=u32();require(n<=s.size()-p,"WAL_INVALID_RECORD");std::string v(s.substr(p,n));p+=n;return v;}
  Value val(){
    auto kind=u8();
    if(kind==1)return static_cast<int64_t>(u64());
    if(kind==2)return str();
    if(kind==3){
      uint32_t n=u32();require(n<= (s.size()-p)/4,"WAL_INVALID_RECORD");
      Vec out;out.reserve(n);
      for(uint32_t i=0;i<n;++i){uint32_t bits=u32();float f;std::memcpy(&f,&bits,4);require(std::isfinite(f),"WAL_INVALID_RECORD");out.push_back(f);}
      return out;
    }
    throw DbError("WAL_INVALID_TYPE");
  }
  void done(){require(p==s.size(),"WAL_TRAILING_BYTES");}
};
// Test-only crash points, controlled exclusively by an explicitly set env var.
// They intentionally use SIGKILL, not normal shutdown, to exercise redo semantics.
void crash_if(const char* point) {
  const char* wanted=std::getenv("MVDB_TEST_FAILPOINT");
  if(wanted && std::strcmp(wanted,point)==0) std::raise(SIGKILL);
}
uint32_t crc32(std::string_view v){
  uint32_t crc=0xFFFFFFFFu;
  for(unsigned char c:v){crc^=c;for(int j=0;j<8;++j)crc=(crc>>1)^((crc&1)?0xEDB88320u:0u);}
  return ~crc;
}
void write_all(int fd,const std::string& s) {
  size_t off=0;
  while(off<s.size()){
    ssize_t n=::write(fd,s.data()+off,s.size()-off);
    if(n<0&&errno==EINTR)continue;
    if(n<=0)throw DbError("WAL_WRITE_FAILED:"+std::string(std::strerror(errno)));
    off+=static_cast<size_t>(n);
  }
}
bool read_all_at(int fd,off_t off,char* p,size_t n){
  size_t got=0;
  while(got<n){
    ssize_t r=::pread(fd,p+got,n-got,off+static_cast<off_t>(got));
    if(r<0&&errno==EINTR)continue;
    if(r<0)throw DbError("WAL_READ_FAILED");
    if(r==0)return false;
    got+=static_cast<size_t>(r);
  }
  return true;
}
struct Txn {
  std::map<std::string,Schema> new_tables;
  std::map<std::string,IndexDef> new_indexes;
  std::map<std::string,std::map<int64_t,std::optional<Row>>> changes;
};
void encode_schema(Writer& w,const Schema& s){
  w.str(s.name);w.u32(static_cast<uint32_t>(s.cols.size()));
  for(const auto& c:s.cols){w.str(c.name);w.u8(static_cast<uint8_t>(c.type));w.u32(c.size);w.u8(c.pk?1:0);}
}
Schema decode_schema(Reader& r){
  Schema s;s.name=r.str();uint32_t n=r.u32();require(n>0&&n<1024,"WAL_INVALID_SCHEMA");
  for(uint32_t i=0;i<n;++i){
    Column c;c.name=r.str();c.type=static_cast<ColType>(r.u8());c.size=r.u32();c.pk=r.u8()!=0;
    require(c.type==ColType::Bigint||c.type==ColType::Varchar||c.type==ColType::Vector,"WAL_INVALID_SCHEMA");
    if(c.pk)s.pk_idx=i;
    s.cols.push_back(std::move(c));
  }
  return s;
}
std::string encode_tx(const Txn& t){
  Writer w;w.u32(2); // phase-3 WAL format; reader also accepts version 1
  w.u32(static_cast<uint32_t>(t.new_tables.size()));
  for(const auto& [_,s]:t.new_tables)encode_schema(w,s);
  w.u32(static_cast<uint32_t>(t.changes.size()));
  for(const auto& [table,changes]:t.changes){
    w.str(table);w.u32(static_cast<uint32_t>(changes.size()));
    for(const auto& [pk,row]:changes){
      w.u64(static_cast<uint64_t>(pk));w.u8(row?1:0);
      if(row){w.u32(static_cast<uint32_t>(row->size()));for(const auto& val:*row)w.val(val);}
    }
  }
  w.u32(static_cast<uint32_t>(t.new_indexes.size()));
  for(const auto& [_, idx] : t.new_indexes){w.str(idx.table);w.str(idx.name);w.str(idx.col);}
  return w.buf;
}
Txn decode_tx(std::string_view raw){
  Reader r(raw);auto version=r.u32();require(version==1||version==2,"WAL_BAD_VERSION");Txn tx;
  uint32_t n=r.u32();require(n<=100000,"WAL_INVALID_RECORD");
  for(uint32_t i=0;i<n;++i){auto s=decode_schema(r);require(tx.new_tables.emplace(s.name,s).second,"WAL_DUP_SCHEMA");}
  n=r.u32();require(n<=100000,"WAL_INVALID_RECORD");
  for(uint32_t i=0;i<n;++i){
    auto name=r.str();require(!tx.changes.count(name),"WAL_DUP_TABLE");auto& changes=tx.changes[name];
    uint32_t m=r.u32();require(m<=10000000,"WAL_INVALID_RECORD");
    for(uint32_t j=0;j<m;++j){
      int64_t pk=static_cast<int64_t>(r.u64());bool present=r.u8()!=0;
      std::optional<Row> row;
      if(present){uint32_t k=r.u32();require(k<1024,"WAL_INVALID_RECORD");row.emplace();
        for(uint32_t z=0;z<k;++z)row->push_back(r.val());}
      require(changes.emplace(pk,std::move(row)).second,"WAL_DUP_PK");
    }
  }
  if(version==2){
    uint32_t count=r.u32();require(count<=100000,"WAL_INVALID_INDEX_COUNT");
    for(uint32_t i=0;i<count;++i){IndexDef d{r.str(),r.str(),r.str()};
      require(tx.new_indexes.emplace(d.table,d).second,"WAL_DUP_INDEX");}
  }
  r.done();return tx;
}

std::string encode_row(const Row& row){
  Writer w;w.u32(static_cast<uint32_t>(row.size()));
  for(const auto& v:row)w.val(v);
  return w.buf;
}
Row decode_row(std::string_view raw){
  Reader r(raw);auto size=r.u32();require(size<=1024,"STORE_INVALID_ROW");
  Row result;result.reserve(size);
  for(uint32_t i=0;i<size;++i)result.push_back(r.val());
  r.done();return result;
}
struct CatalogData {
  std::map<std::string,Schema> tables;
  std::map<std::string,IndexDef> indexes;
};
std::string encode_catalog(const CatalogData& cat){
  Writer w;w.u32(static_cast<uint32_t>(cat.tables.size()));
  for(const auto& [_,sch]:cat.tables)encode_schema(w,sch);
  // Optional extension after the phase-2 schema list, preserving old catalogs.
  w.u32(0x33584649); // IFX3
  w.u32(static_cast<uint32_t>(cat.indexes.size()));
  for(const auto& [_,idx]:cat.indexes){w.str(idx.table);w.str(idx.name);w.str(idx.col);}
  return w.buf;
}
CatalogData decode_catalog(std::string_view bytes){
  CatalogData out;
  if(bytes.empty())return out;
  Reader r(bytes);auto n=r.u32();require(n<=100000,"STORE_INVALID_CATALOG");
  for(uint32_t i=0;i<n;++i){auto sch=decode_schema(r);
    require(out.tables.emplace(sch.name,sch).second,"STORE_DUPLICATE_SCHEMA");}
  if(r.p<r.s.size()){
    require(r.u32()==0x33584649,"STORE_INVALID_INDEX_CATALOG");
    n=r.u32();require(n<=100000,"STORE_INVALID_INDEX_CATALOG");
    for(uint32_t i=0;i<n;++i){IndexDef d{r.str(),r.str(),r.str()};
      require(out.indexes.emplace(d.table,d).second,"STORE_DUPLICATE_INDEX");}
  }
  r.done();return out;
}

struct TypedPred {size_t col;std::string op;Value rhs;};
std::vector<TypedPred> prepare_preds(const Schema& s,const std::vector<Pred>& preds){
  std::vector<TypedPred> prepared;
  for(const auto& p:preds){auto c=static_cast<size_t>(col_index(s,p.col));
    require(s.cols[c].type!=ColType::Vector,"UNSUPPORTED_VECTOR_COMPARISON");
    prepared.push_back({c,p.op,typed_value(p.rhs,s.cols[c])});}
  return prepared;
}
bool matches(const Row& row,const std::vector<TypedPred>& pp){
  for(const auto& p:pp){
    int c=compare_values(row[p.col],p.rhs);
    bool ok=p.op=="="?c==0:p.op=="<"?c<0:p.op=="<="?c<=0:p.op==">"?c>0:c>=0;
    if(!ok)return false;
  }
  return true;
}
Schema validate_schema(std::string name,const std::vector<Column>& cols){
  require(!cols.empty()&&cols.size()<1024,"INVALID_SCHEMA");
  Schema schema;schema.name=std::move(name);schema.cols=cols;
  size_t pk_count=0,vec_count=0;
  for(size_t i=0;i<cols.size();++i){
    for(size_t j=0;j<i;++j)require(cols[i].name!=cols[j].name,"DUPLICATE_COLUMN");
    if(cols[i].pk){require(cols[i].type==ColType::Bigint,"PRIMARY_KEY_MUST_BE_BIGINT");schema.pk_idx=i;++pk_count;}
    if(cols[i].type==ColType::Vector)++vec_count;
  }
  require(pk_count==1,"REQUIRES_ONE_BIGINT_PRIMARY_KEY");
  require(vec_count<=1,"TOO_MANY_VECTOR_COLUMNS");
  return schema;
}
} // namespace

struct Database::Impl {
  std::map<std::string,Schema> tables;
  std::map<std::string,IndexDef> indexes;
  std::map<std::string,std::unique_ptr<IVFIndex>> ann;
  std::string directory;
  bool ann_mode=false;
  std::optional<Txn> active;
  int wal_fd=-1, lock_fd=-1;
  std::unique_ptr<DiskStore> store;
  bool io_poisoned=false;
  struct Decoded {Row row;size_t cost;std::list<uint64_t>::iterator lru_it;};
  mutable std::unordered_map<uint64_t,Decoded> decoded;
  mutable std::list<uint64_t> decoded_lru;
  mutable size_t decoded_bytes=0;
  static constexpr size_t DECODED_LIMIT=8u*1024u*1024u;
  // Query-only, one-table columnar scan cache. It is DERIVED state: never
  // persisted, and invalidated on every committed modification. Unlike the
  // decoded-row LRU, dense vectors are contiguous and B+Tree leaf scans are
  // not repeated for each TopK. Large tables always use the disk scan.
  struct VectorScanCache {
    std::string table;
    size_t vector_col=0;
    size_t dimension=0;
    std::vector<int64_t> pks;
    std::vector<float> vectors;
    // One column per non-vector attribute, in the same PK order as vectors.
    std::vector<std::vector<Value>> scalar_columns;
    // Lazy, bounded equality postings for low-cardinality BIGINT columns.
    // Building this auxiliary directory is optional and never changes results.
    std::vector<std::unordered_map<int64_t,std::vector<uint32_t>>> eq_int;
    std::vector<std::unordered_map<std::string,std::vector<uint32_t>>> eq_str;
    std::vector<bool> eq_ready;
    std::vector<bool> eq_enabled;
    std::vector<uint32_t> empty_postings;
    size_t budget_bytes=0;
  };
  mutable std::unique_ptr<VectorScanCache> vector_cache;
  // Large-table exact-search accelerator: never duplicates committed vectors.
  // Hold only bounded row descriptors and scalar BIGINT columns in memory,
  // and map the existing heap read-only (OS page cache is separately accounted).
  struct MappedVectorScan {
    struct Entry {int64_t pk; const unsigned char* vector;};
    std::string table;
    size_t vector_col=0, dimension=0;
    std::shared_ptr<HeapView> heap;
    std::vector<Entry> entries;
    std::vector<std::vector<int64_t>> numeric_columns;
    std::vector<std::vector<std::string_view>> string_columns;
    // Optional bounded low-cardinality posting lists, constructed on demand.
    std::vector<std::unordered_map<int64_t,std::vector<uint32_t>>> eq_numeric;
    std::vector<std::unordered_map<std::string_view,std::vector<uint32_t>>> eq_string;
    std::vector<bool> eq_ready;
    std::vector<bool> eq_enabled;
    std::vector<uint32_t> empty_postings;
    size_t meta_bytes=0;
  };
  mutable std::unique_ptr<MappedVectorScan> mapped_scan;
  mutable std::optional<std::pair<std::string,size_t>> mapped_unavailable;
  mutable std::optional<std::pair<std::string,size_t>> dense_unavailable;
  static constexpr size_t MAPPED_META_LIMIT=16u*1024u*1024u;
  static constexpr uint64_t MAPPED_HEAP_LIMIT=1ull*1024*1024*1024;
  static uint32_t le32(const unsigned char* p){
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  }
  static uint64_t le64(const unsigned char* p){
    return uint64_t(le32(p))|(uint64_t(le32(p+4))<<32);
  }
  MappedVectorScan* get_mapped_scan(const std::string& tbl,
                                       const Schema& sch,size_t vec_col)const {
    if(const char* e=std::getenv("MVDB_MAPPED_SCAN");e&&std::strcmp(e,"0")==0)return nullptr;
    const bool force=std::getenv("MVDB_TEST_FORCE_MAPPED") &&
      std::strcmp(std::getenv("MVDB_TEST_FORCE_MAPPED"),"1")==0;
    if((!force && store->heap_bytes()<VECTOR_CACHE_HEAP_CUTOFF) ||
       store->heap_bytes()>MAPPED_HEAP_LIMIT)return nullptr;
    for(size_t c=0;c<sch.cols.size();++c)
      if(c!=vec_col && sch.cols[c].type!=ColType::Bigint &&
         sch.cols[c].type!=ColType::Varchar)return nullptr;
    if(mapped_scan && mapped_scan->table==tbl && mapped_scan->vector_col==vec_col)
      return mapped_scan.get();
    if(mapped_unavailable && *mapped_unavailable==std::make_pair(tbl,vec_col))return nullptr;
    auto mapping=store->map_heap_readonly();
    if(!mapping)return nullptr;
    auto cache=std::make_unique<MappedVectorScan>();
    cache->table=tbl;cache->vector_col=vec_col;
    cache->dimension=sch.cols[vec_col].size;cache->heap=std::move(mapping);
    cache->numeric_columns.resize(sch.cols.size());
    cache->string_columns.resize(sch.cols.size());
    cache->eq_numeric.resize(sch.cols.size());
    cache->eq_string.resize(sch.cols.size());
    cache->eq_ready.resize(sch.cols.size(),false);
    cache->eq_enabled.resize(sch.cols.size(),false);
    size_t charge=sizeof(MappedVectorScan::Entry);
    for(size_t c=0;c<sch.cols.size();++c)if(c!=vec_col)
      charge+=(sch.cols[c].type==ColType::Varchar?sizeof(std::string_view):sizeof(int64_t));
    bool too_big=false;
    store->scan(tbl,[&](int64_t pk,uint64_t offset,uint32_t len){
      if(too_big)return;
      if(cache->entries.size()>=MAPPED_META_LIMIT/charge){too_big=true;return;}
      require(offset<=cache->heap->size && len<=cache->heap->size-offset &&
              cache->heap->size-offset-len>=8,"MAPPED_ROW_BOUNDS");
      const unsigned char* header=cache->heap->bytes+offset;
      const unsigned char* ptr=header+8;
      const unsigned char* end=ptr+len;
      require(le32(header)==len,"MAPPED_ROW_SIZE");
      require(crc32(std::string_view(reinterpret_cast<const char*>(ptr),len))==le32(header+4),
              "MAPPED_ROW_CRC");
      auto take=[&](size_t n)->const unsigned char*{
        require(size_t(end-ptr)>=n,"MAPPED_ROW_SHORT");
        const unsigned char* out=ptr;ptr+=n;return out;
      };
      require(le32(take(4))==sch.cols.size(),"MAPPED_ROW_SCHEMA");
      const unsigned char* vec=nullptr;
      for(size_t c=0;c<sch.cols.size();++c){
        const unsigned char kind=*take(1);
        if(c==vec_col){
          require(kind==3,"MAPPED_VECTOR_KIND");
          require(le32(take(4))==cache->dimension,"MAPPED_VECTOR_DIM");
          vec=take(cache->dimension*sizeof(float));
        } else if(sch.cols[c].type==ColType::Bigint) {
          require(kind==1,"MAPPED_INT_KIND");
          cache->numeric_columns[c].push_back(static_cast<int64_t>(le64(take(8))));
        } else {
          require(kind==2,"MAPPED_STRING_KIND");
          const uint32_t str_len=le32(take(4));
          const unsigned char* data=take(str_len);
          cache->string_columns[c].emplace_back(
            reinterpret_cast<const char*>(data),str_len);
        }
      }
      require(ptr==end && vec,"MAPPED_ROW_TRAILING");
      cache->entries.push_back({pk,vec});
    });
    if(too_big){mapped_unavailable=std::make_pair(tbl,vec_col);return nullptr;}
    cache->meta_bytes=charge*cache->entries.size();
    mapped_scan=std::move(cache);
    return mapped_scan.get();
  }
  static constexpr size_t VECTOR_CACHE_LIMIT=8u*1024u*1024u;
  static constexpr uint64_t VECTOR_CACHE_HEAP_CUTOFF=12ull*1024*1024;
  size_t row_cost(const Row& row)const{
    size_t sz=sizeof(Row)+row.size()*sizeof(Value);
    for(const auto& v:row){
      if(const auto* str=std::get_if<std::string>(&v))sz+=str->capacity();
      if(const auto* vec=std::get_if<Vec>(&v))sz+=vec->capacity()*sizeof(float);
    }
    return sz;
  }

  static std::string wal_path(const std::string& dir,uint64_t gen){
    return (fs::path(dir)/(gen==0?"wal.mvdb":("wal."+std::to_string(gen)+".mvdb"))).string();
  }
  explicit Impl(const std::string& directory_,bool use_ann):directory(directory_),ann_mode(use_ann){
    fs::create_directories(directory);
    const auto lock=(fs::path(directory)/"db.lock").string();
    lock_fd=::open(lock.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0644);
    if(lock_fd<0)throw DbError("DB_LOCK_OPEN_FAILED");
    if(::flock(lock_fd,LOCK_EX|LOCK_NB)!=0){::close(lock_fd);lock_fd=-1;throw DbError("DATABASE_ALREADY_OPEN");}
    try {
      store=std::make_unique<DiskStore>(directory);
      wal_fd=::open(wal_path(directory,store->wal_generation()).c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0644);
      if(wal_fd<0)throw DbError("WAL_OPEN_FAILED");
      auto catalog=decode_catalog(store->catalog());
      tables=std::move(catalog.tables);
      indexes=std::move(catalog.indexes);
      recover();
      // Explicit maintenance mode; not run during the timed query benchmark.
      // A full copy must finish before readiness is reported.
      if(const char* e=std::getenv("MVDB_COMPACT_ON_START");e&&std::strcmp(e,"1")==0){
        store->compact();decoded.clear();decoded_lru.clear();decoded_bytes=0;
      }
      initialize_ann();
      rotate_wal_if_needed();
    }catch(...){if(wal_fd>=0)::close(wal_fd);wal_fd=-1;if(lock_fd>=0)::close(lock_fd);lock_fd=-1;throw;}
  }
  ~Impl(){if(wal_fd>=0)::close(wal_fd);if(lock_fd>=0)::close(lock_fd);}
  const Schema& schema(const std::string& name,const Txn* tx)const{
    if(tx){auto it=tx->new_tables.find(name);if(it!=tx->new_tables.end())return it->second;}
    auto it=tables.find(name);require(it!=tables.end(),"UNKNOWN_TABLE:"+name);return it->second;
  }
  std::optional<Row> visible_row(const std::string& tbl,int64_t pk,const Txn* tx)const {
    if(tx){auto it=tx->changes.find(tbl);if(it!=tx->changes.end()){
      auto m=it->second.find(pk);if(m!=it->second.end())return m->second;}}
    auto raw=store->get(tbl,pk);
    return raw?std::make_optional(decode_row(*raw)):std::nullopt;
  }
  template<typename Callback>
  void each_visible(const std::string& tbl,const Txn* tx,Callback fn)const {
    const std::map<int64_t,std::optional<Row>>* delta=nullptr;
    if(tx){auto it=tx->changes.find(tbl);if(it!=tx->changes.end())delta=&it->second;}
    store->scan(tbl,[&](int64_t pk,uint64_t offset,uint32_t length){
      if(delta&&delta->count(pk))return;
      auto cached=decoded.find(offset);
      if(cached!=decoded.end()){
        decoded_lru.splice(decoded_lru.begin(),decoded_lru,cached->second.lru_it);
        fn(pk,cached->second.row);
        return;
      }
      Row row=decode_row(store->read_row(offset,length));
      const size_t bytes=row_cost(row);
      if(bytes>DECODED_LIMIT){fn(pk,row);return;}
      while(decoded_bytes+bytes>DECODED_LIMIT && !decoded_lru.empty()){
        auto last=decoded_lru.back();auto old=decoded.find(last);
        decoded_bytes-=old->second.cost;
        decoded.erase(old);
        decoded_lru.pop_back();
      }
      decoded_lru.push_front(offset);
      decoded_bytes+=bytes;
      auto item=decoded.emplace(offset,Decoded{std::move(row),bytes,decoded_lru.begin()});
      fn(pk,item.first->second.row);
    });
    if(delta)for(const auto& [pk,row]:*delta)if(row)fn(pk,*row);
  }
  VectorScanCache* get_vector_cache(const std::string& tbl,
                                          const Schema& sch,size_t vec_col)const {
    if(const char* e=std::getenv("MVDB_TEST_FORCE_MAPPED");e&&std::strcmp(e,"1")==0)
      return nullptr;
    // Disabled for A/B validation and resource-constrained deployments.
    if(const char* env=std::getenv("MVDB_VECTOR_CACHE");env&&std::strcmp(env,"0")==0)
      return nullptr;
    // Cheap size gate prevents repeatedly decoding oversized tables just to
    // discover that their vectors will not fit. The budget is checked again
    // during construction; non-vector VARCHAR columns also consume memory.
    if(store->heap_bytes()>VECTOR_CACHE_HEAP_CUTOFF)return nullptr;
    if(vector_cache && vector_cache->table==tbl && vector_cache->vector_col==vec_col)
      return vector_cache.get();
    if(dense_unavailable && *dense_unavailable==std::make_pair(tbl,vec_col))return nullptr;
    auto building=std::make_unique<VectorScanCache>();
    building->table=tbl;
    building->vector_col=vec_col;
    building->dimension=sch.cols.at(vec_col).size;
    building->scalar_columns.resize(sch.cols.size());
    building->eq_int.resize(sch.cols.size());
    building->eq_str.resize(sch.cols.size());
    building->eq_ready.resize(sch.cols.size(),false);
    building->eq_enabled.resize(sch.cols.size(),false);
    size_t approx_bytes=0;
    bool too_large=false;
    each_visible(tbl,nullptr,[&](int64_t pk,const Row& row){
      if(too_large)return;
      const Vec& v=std::get<Vec>(row.at(vec_col));
      require(v.size()==building->dimension,"VECTOR_DIM_MISMATCH");
      size_t row_bytes=sizeof(int64_t)+v.size()*sizeof(float);
      for(size_t j=0;j<sch.cols.size();++j)if(j!=vec_col){
        row_bytes+=sizeof(Value);
        if(auto str=std::get_if<std::string>(&row[j]))row_bytes+=str->size();
      }
      if(row_bytes>VECTOR_CACHE_LIMIT-approx_bytes){too_large=true;return;}
      approx_bytes+=row_bytes;
      building->pks.push_back(pk);
      building->vectors.insert(building->vectors.end(),v.begin(),v.end());
      for(size_t j=0;j<sch.cols.size();++j)
        if(j!=vec_col)building->scalar_columns[j].push_back(row[j]);
    });
    if(too_large){dense_unavailable=std::make_pair(tbl,vec_col);return nullptr;}
    building->budget_bytes=approx_bytes;
    vector_cache=std::move(building);
    return vector_cache.get();
  }
  const std::vector<uint32_t>* mapped_equal_postings(
      MappedVectorScan& m,const Schema& sch,size_t col,const Value& target)const {
    if(const char* e=std::getenv("MVDB_MMAP_FILTER_INDEX");e&&std::strcmp(e,"0")==0)
      return nullptr;
    if(!m.eq_ready[col]){
      m.eq_ready[col]=true;
      constexpr size_t GROUP_LIMIT=128;
      const size_t charge=m.entries.size()*sizeof(uint32_t)+GROUP_LIMIT*64;
      if(m.entries.size()>UINT32_MAX || charge>MAPPED_META_LIMIT-m.meta_bytes)
        return nullptr;
      bool valid=true;
      if(sch.cols[col].type==ColType::Bigint){
        auto& groups=m.eq_numeric[col];groups.reserve(GROUP_LIMIT);
        for(size_t i=0;i<m.entries.size();++i){
          auto key=m.numeric_columns[col][i];
          auto it=groups.find(key);
          if(it==groups.end()){
            if(groups.size()>=GROUP_LIMIT){valid=false;break;}
            it=groups.emplace(key,std::vector<uint32_t>{}).first;
          }
          it->second.push_back(uint32_t(i));
        }
        if(!valid)std::unordered_map<int64_t,std::vector<uint32_t>>{}.swap(groups);
      } else {
        auto& groups=m.eq_string[col];groups.reserve(GROUP_LIMIT);
        for(size_t i=0;i<m.entries.size();++i){
          const auto key=m.string_columns[col][i];
          auto it=groups.find(key);
          if(it==groups.end()){
            if(groups.size()>=GROUP_LIMIT){valid=false;break;}
            it=groups.emplace(key,std::vector<uint32_t>{}).first;
          }
          it->second.push_back(uint32_t(i));
        }
        if(!valid)std::unordered_map<std::string_view,std::vector<uint32_t>>{}.swap(groups);
      }
      if(!valid)return nullptr;
      m.eq_enabled[col]=true;m.meta_bytes+=charge;
    }
    if(!m.eq_enabled[col])return nullptr;
    if(sch.cols[col].type==ColType::Bigint){
      auto it=m.eq_numeric[col].find(std::get<int64_t>(target));
      return it==m.eq_numeric[col].end()?&m.empty_postings:&it->second;
    }
    auto it=m.eq_string[col].find(std::string_view(std::get<std::string>(target)));
    return it==m.eq_string[col].end()?&m.empty_postings:&it->second;
  }
  const std::vector<uint32_t>* cached_equal_bigint(VectorScanCache& cache,
                                                    size_t col,int64_t value)const {
    if(const char* env=std::getenv("MVDB_VECTOR_FILTER_INDEX");env&&std::strcmp(env,"0")==0)
      return nullptr;
    if(!cache.eq_ready[col]){
      cache.eq_ready[col]=true;
      const size_t n=cache.pks.size();
      // Never manufacture a large index for high-cardinality columns or go
      // over the shared 8 MiB query-cache budget.
      constexpr size_t MAX_GROUPS=128;
      const size_t charge=n*sizeof(uint32_t)+MAX_GROUPS*64;
      if(n>std::numeric_limits<uint32_t>::max() ||
         charge>VECTOR_CACHE_LIMIT-cache.budget_bytes)return nullptr;
      auto& groups=cache.eq_int[col];
      groups.reserve(MAX_GROUPS);
      bool low_cardinality=true;
      for(size_t i=0;i<n;++i){
        const int64_t k=std::get<int64_t>(cache.scalar_columns[col][i]);
        auto it=groups.find(k);
        if(it==groups.end()){
          if(groups.size()>=MAX_GROUPS){low_cardinality=false;break;}
          it=groups.emplace(k,std::vector<uint32_t>{}).first;
        }
        it->second.push_back(uint32_t(i));
      }
      if(!low_cardinality){std::unordered_map<int64_t,std::vector<uint32_t>>{}.swap(groups);return nullptr;}
      cache.budget_bytes+=charge;
      cache.eq_enabled[col]=true;
    }
    if(!cache.eq_enabled[col])return nullptr;
    auto it=cache.eq_int[col].find(value);
    return it==cache.eq_int[col].end()?&cache.empty_postings:&it->second;
  }
  const std::vector<uint32_t>* cached_equal_string(VectorScanCache& cache,
                                                   size_t col,const std::string& value)const{
    if(const char* env=std::getenv("MVDB_VECTOR_FILTER_INDEX");env&&std::strcmp(env,"0")==0)
      return nullptr;
    if(!cache.eq_ready[col]){
      cache.eq_ready[col]=true;
      const size_t n=cache.pks.size();
      constexpr size_t MAX_GROUPS=128;
      const size_t charge=n*sizeof(uint32_t)+MAX_GROUPS*128;
      if(n>std::numeric_limits<uint32_t>::max() ||
         charge>VECTOR_CACHE_LIMIT-cache.budget_bytes)return nullptr;
      auto& groups=cache.eq_str[col];groups.reserve(MAX_GROUPS);
      bool low_cardinality=true;
      for(size_t i=0;i<n;++i){
        const auto& k=std::get<std::string>(cache.scalar_columns[col][i]);
        auto it=groups.find(k);
        if(it==groups.end()){
          if(groups.size()>=MAX_GROUPS){low_cardinality=false;break;}
          it=groups.emplace(k,std::vector<uint32_t>{}).first;
        }
        it->second.push_back(uint32_t(i));
      }
      if(!low_cardinality){std::unordered_map<std::string,std::vector<uint32_t>>{}.swap(groups);return nullptr;}
      cache.budget_bytes+=charge;cache.eq_enabled[col]=true;
    }
    if(!cache.eq_enabled[col])return nullptr;
    auto it=cache.eq_str[col].find(value);
    return it==cache.eq_str[col].end()?&cache.empty_postings:&it->second;
  }
  static bool pk_fastpath_enabled(){
    const char* e=std::getenv("MVDB_PK_FASTPATH");
    return !e || std::strcmp(e,"0")!=0;
  }
  struct PkBounds {
    int64_t low=std::numeric_limits<int64_t>::min();
    int64_t high=std::numeric_limits<int64_t>::max();
    bool constrained=false,empty=false;
  };
  static PkBounds pk_bounds(const Schema& sch,const std::vector<TypedPred>& preds){
    PkBounds b;
    for(const auto& pred:preds){
      if(pred.col!=sch.pk_idx)continue;
      b.constrained=true;
      const int64_t v=std::get<int64_t>(pred.rhs);
      if(pred.op=="="){b.low=std::max(b.low,v);b.high=std::min(b.high,v);}
      else if(pred.op==">=")b.low=std::max(b.low,v);
      else if(pred.op=="<=")b.high=std::min(b.high,v);
      else if(pred.op==">"){
        if(v==std::numeric_limits<int64_t>::max())b.empty=true;
        else b.low=std::max(b.low,int64_t(v+1));
      }else if(pred.op=="<"){
        if(v==std::numeric_limits<int64_t>::min())b.empty=true;
        else b.high=std::min(b.high,int64_t(v-1));
      }
    }
    if(b.low>b.high)b.empty=true;
    return b;
  }
  // Primary-key equality is a single B+Tree lookup, not a whole-table scan.
  // Predicate checks still run on the visible (including txn-overlay) row.
  template<typename Callback>
  void each_matching(const std::string& table,const Schema& sch,const Txn* tx,
                     const std::vector<TypedPred>& preds,Callback fn) const {
    for(const auto& pred:preds){
      if(pred.col==sch.pk_idx&&pred.op=="="){
        const int64_t pk=std::get<int64_t>(pred.rhs);
        auto row=visible_row(table,pk,tx);
        if(row&&matches(*row,preds))fn(pk,*row);
        return;
      }
    }
    if(!tx && pk_fastpath_enabled()){
      const auto bounds=pk_bounds(sch,preds);
      if(bounds.empty)return;
      if(bounds.constrained){
        store->scan_range(table,bounds.low,bounds.high,false,
          [&](int64_t pk,uint64_t off,uint32_t len){
            Row row=decode_row(store->read_row(off,len));
            if(matches(row,preds))fn(pk,row);
            return true;
          });
        return;
      }
    }
    each_visible(table,tx,[&](int64_t pk,const Row& row){
      if(matches(row,preds))fn(pk,row);
    });
  }
  void apply(const Txn& tx,uint64_t next_wal_cursor){
    auto new_tables=tables;
    auto new_indexes=indexes;
    for(const auto& [name,sch]:tx.new_tables){
      require(!new_tables.count(name),"WAL_DUP_SCHEMA");
      new_tables.emplace(name,sch);
    }
    for(const auto& [table,index]:tx.new_indexes){
      require(new_tables.count(table)!=0,"WAL_UNKNOWN_INDEX_TABLE");
      require(new_indexes.emplace(table,index).second,"WAL_DUP_INDEX");
    }
    std::vector<DiskChange> changes;
    for(const auto& [table,by_pk]:tx.changes){
      require(new_tables.count(table)!=0,"WAL_UNKNOWN_TABLE");
      for(const auto& [pk,row]:by_pk){
        changes.push_back({table,pk,row?std::make_optional(encode_row(*row)):std::nullopt});
      }
    }
    store->publish(next_wal_cursor,encode_catalog(CatalogData{new_tables,new_indexes}),changes);
    // The cache refers to the prior committed snapshot. No query may reuse
    // it after a successful publication, including during WAL replay.
    vector_cache.reset();dense_unavailable.reset();
    mapped_scan.reset();mapped_unavailable.reset();
    tables=std::move(new_tables);
    indexes=std::move(new_indexes);
  }
  void recover(){
    struct stat st{};require(::fstat(wal_fd,&st)==0,"WAL_STAT_FAILED");
    constexpr uint32_t MAGIC=0x3142444du;
    constexpr size_t HDR=12;constexpr uint32_t MAX_PAYLOAD=64u*1024u*1024u;
    off_t pos=static_cast<off_t>(store->wal_cursor());
    require(pos<=st.st_size,"WAL_CHECKPOINT_BEYOND_LOG");
    while(pos<st.st_size){
      if(st.st_size-pos<static_cast<off_t>(HDR))break;
      char h[HDR];require(read_all_at(wal_fd,pos,h,HDR),"WAL_READ_FAILED");
      Reader hdr(std::string_view(h,HDR));
      uint32_t magic=hdr.u32(),n=hdr.u32(),crc=hdr.u32();
      require(magic==MAGIC&&n<=MAX_PAYLOAD,"WAL_INVALID_HEADER");
      if(static_cast<uint64_t>(st.st_size-pos-HDR)<n)break;
      std::string payload(n,'\0');
      require(read_all_at(wal_fd,pos+HDR,payload.data(),n),"WAL_READ_FAILED");
      require(crc32(payload)==crc,"WAL_CRC_MISMATCH");
      auto tx=decode_tx(payload);
      pos+=HDR+n;
      apply(tx,static_cast<uint64_t>(pos));
      crash_if("recovery_after_replay");
    }
    if(pos<st.st_size){
      require(::ftruncate(wal_fd,pos)==0,"WAL_TRUNCATE_FAILED");
      require(::fdatasync(wal_fd)==0,"WAL_SYNC_FAILED");
    }
    require(::lseek(wal_fd,0,SEEK_END)>=0,"WAL_SEEK_FAILED");
  }
  void commit_tx(const Txn& tx){
    if(tx.new_tables.empty()&&tx.new_indexes.empty()&&tx.changes.empty())return;
    constexpr uint32_t MAGIC=0x3142444du;
    std::string payload=encode_tx(tx);
    require(payload.size()<=64u*1024u*1024u,"TRANSACTION_TOO_LARGE");
    Writer w;w.u32(MAGIC);w.u32(static_cast<uint32_t>(payload.size()));w.u32(crc32(payload));w.buf+=payload;
    if(std::getenv("MVDB_TEST_FAILPOINT") &&
       std::strcmp(std::getenv("MVDB_TEST_FAILPOINT"),"mid_wal_write")==0){
      auto half=w.buf.substr(0,std::max<size_t>(12,w.buf.size()/2));
      write_all(wal_fd,half);
      require(::fdatasync(wal_fd)==0,"WAL_SYNC_FAILED");
      crash_if("mid_wal_write");
    }
    try {
      const off_t begin=::lseek(wal_fd,0,SEEK_END);
      require(begin>=0,"WAL_SEEK_FAILED");
      write_all(wal_fd,w.buf);
      crash_if("after_write_before_sync");
      require(::fdatasync(wal_fd)==0,"WAL_SYNC_FAILED");
      crash_if("after_sync_before_apply");
      apply(tx,static_cast<uint64_t>(begin)+w.buf.size());
      sync_ann(tx);
      rotate_wal_if_needed();
    } catch (...) {
      io_poisoned=true;
      throw;
    }
  }
  void rotate_wal_if_needed(){
    // Durable, versioned WAL switch. No dangerous truncate-in-place: a crash
    // sees either the old manifest+log or the new manifest+log, never a mix.
    uint64_t threshold=64ull*1024*1024;
    if(const char* env=std::getenv("MVDB_CHECKPOINT_BYTES")){
      try{threshold=std::max<uint64_t>(1024,std::stoull(env));}catch(...){}
    }
    struct stat st{};require(fstat(wal_fd,&st)==0,"WAL_STAT_FAILED");
    if(uint64_t(st.st_size)<threshold)return;
    const uint64_t old_generation=store->wal_generation(),next=old_generation+1;
    const auto next_path=wal_path(directory,next);
    const int fd=::open(next_path.c_str(),O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
    require(fd>=0,"WAL_ROTATION_OPEN_FAILED");
    try {
      require(::fdatasync(fd)==0,"WAL_ROTATION_SYNC_FAILED");
      const int dirfd=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
      require(dirfd>=0,"WAL_ROTATION_DIRECTORY_FAILED");
      int rc=::fsync(dirfd);::close(dirfd);require(rc==0,"WAL_ROTATION_DIRECTORY_SYNC_FAILED");
      crash_if("before_wal_checkpoint_manifest");
      store->checkpoint_wal(next);
      for(auto& [_,idx]:ann)idx->advance_cursor(0);
      auto old_path=wal_path(directory,old_generation);
      ::close(wal_fd);wal_fd=fd;
      if(::unlink(old_path.c_str())==0){
        const int dfd=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(dfd>=0){::fsync(dfd);::close(dfd);}
      }
    }catch(...){::close(fd);throw;}
  }
  void create_vector_index(const Statement& s, Txn& tx){
    const auto& sch=schema(s.table,&tx);
    require(!indexes.count(s.table)&&!tx.new_indexes.count(s.table),"VECTOR_INDEX_ALREADY_EXISTS");
    const auto col=static_cast<size_t>(col_index(sch,s.index_col));
    require(sch.cols[col].type==ColType::Vector,"INDEX_REQUIRES_VECTOR_COLUMN");
    for(const auto& [_,d]:indexes)require(d.name!=s.index_name,"DUPLICATE_INDEX_NAME");
    for(const auto& [_,d]:tx.new_indexes)require(d.name!=s.index_name,"DUPLICATE_INDEX_NAME");
    tx.new_indexes.emplace(s.table,IndexDef{s.table,s.index_name,s.index_col});
  }
  void enumerate_index(const IndexDef& d, const IVFIndex::Visitor& fn) const{
    const Schema& sch=tables.at(d.table);
    const auto col=static_cast<size_t>(col_index(sch,d.col));
    store->scan(d.table,[&](int64_t pk,uint64_t offset,uint32_t length){
      Row row=decode_row(store->read_row(offset,length));
      fn(VectorPosting{pk,offset,length,std::get<Vec>(row.at(col))});
    });
  }
  std::unique_ptr<IVFIndex> make_index(const IndexDef& d) const{
    auto dim=tables.at(d.table).cols.at(static_cast<size_t>(col_index(tables.at(d.table),d.col))).size;
    return std::make_unique<IVFIndex>(directory,d.table,d.name,d.col,dim);
  }
  void initialize_ann(){
    for(const auto& [table,d]:indexes){
      auto idx=make_index(d);
      if(!idx->load_if_current(store->wal_cursor(),store->data_generation()))
        idx->rebuild(store->wal_cursor(),store->data_generation(),[&](const IVFIndex::Visitor& fn){enumerate_index(d,fn);});
      ann.emplace(table,std::move(idx));
    }
  }
  void sync_ann(const Txn& tx){
    for(const auto& [table,d]:indexes){
      if(!ann.count(table)){
        auto idx=make_index(d);
        idx->rebuild(store->wal_cursor(),store->data_generation(),[&](const IVFIndex::Visitor& fn){enumerate_index(d,fn);});
        ann.emplace(table,std::move(idx));
        continue;
      }
      std::vector<VectorPosting> changed;
      auto it=tx.changes.find(table);
      if(it!=tx.changes.end()){
        const auto col=static_cast<size_t>(col_index(tables.at(table),d.col));
        for(const auto& [pk,row]:it->second)if(row){
          auto ref=store->get_ref(table,pk);
          require(ref.has_value(),"ANN_COMMITTED_ROW_NOT_FOUND");
          changed.push_back(VectorPosting{pk,ref->offset,ref->length,std::get<Vec>((*row)[col])});
        }
      }
      ann.at(table)->append(store->wal_cursor(),changed);
    }
  }
  void create_table(const Statement& s,Txn& tx){
    require(!tables.count(s.table)&&!tx.new_tables.count(s.table),"TABLE_ALREADY_EXISTS");
    tx.new_tables.emplace(s.table,validate_schema(s.table,s.columns));
  }
  uint64_t insert(const Statement& s,Txn& tx){
    const auto& sch=schema(s.table,&tx);
    std::vector<size_t> indexes;
    if(s.insert_columns.empty()){for(size_t i=0;i<sch.cols.size();++i)indexes.push_back(i);}
    else {
      require(s.insert_columns.size()==sch.cols.size(),"COLUMN_COUNT_MISMATCH");
      for(const auto& col:s.insert_columns){size_t i=static_cast<size_t>(col_index(sch,col));
        require(std::find(indexes.begin(),indexes.end(),i)==indexes.end(),"DUPLICATE_COLUMN");indexes.push_back(i);}
    }
    std::map<int64_t,std::optional<Row>> staged;
    for(const auto& literals:s.insert_rows){
      require(literals.size()==indexes.size(),"COLUMN_COUNT_MISMATCH");
      Row row(sch.cols.size());
      for(size_t i=0;i<indexes.size();++i)row[indexes[i]]=typed_value(literals[i],sch.cols[indexes[i]]);
      int64_t pk=std::get<int64_t>(row[sch.pk_idx]);
      require(!staged.count(pk)&&!visible_row(s.table,pk,&tx).has_value(),"DUPLICATE_PRIMARY_KEY");
      staged.emplace(pk,std::move(row));
    }
    auto& changes=tx.changes[s.table];
    for(auto& [pk,row]:staged)changes[pk]=std::move(row);
    return static_cast<uint64_t>(staged.size());
  }
  uint64_t update_or_delete(const Statement& s,Txn& tx,bool del){
    const auto& sch=schema(s.table,&tx);
    const auto predicates=prepare_preds(sch,s.predicates);
    std::vector<std::pair<size_t,Value>> changes;
    if(!del)for(const auto& a:s.assigns){
      const size_t c=static_cast<size_t>(col_index(sch,a.col));
      require(c!=sch.pk_idx,"PRIMARY_KEY_UPDATE_NOT_SUPPORTED");
      require(std::none_of(changes.begin(),changes.end(),[&](const auto& ch){return ch.first==c;}),"DUPLICATE_COLUMN");
      changes.emplace_back(c,typed_value(a.val,sch.cols[c]));
    }
    std::map<int64_t,std::optional<Row>> staged;
    each_matching(s.table,sch,&tx,predicates,[&](int64_t pk,const Row& row){
      if(del)staged[pk]=std::nullopt;
      else {
        Row newer=row;for(const auto& [col,val]:changes)newer[col]=val;
        staged[pk]=std::move(newer);
      }
    });
    auto& overlay=tx.changes[s.table];
    for(auto& [pk,row]:staged)overlay[pk]=std::move(row);
    return static_cast<uint64_t>(staged.size());
  }
  Result select(const Statement& s,const Txn* tx)const{
    const auto& sch=schema(s.table,tx);
    auto predicates=prepare_preds(sch,s.predicates);
    std::vector<Expr> projections;
    for(const auto& e:s.projections){
      if(e.type==Expr::Star){for(const auto& c:sch.cols){Expr x;x.col=c.name;projections.push_back(x);}}
      else projections.push_back(e);
    }
    // Compile distance queries only once, rather than once per row.
    struct PreparedExpr{Expr e;size_t col=0;Vec q;};
    auto compile=[&](Expr e)->PreparedExpr{
      PreparedExpr p;p.e=e;
      p.col=static_cast<size_t>(col_index(sch,e.col));
      if(e.type==Expr::Distance){
        require(sch.cols[p.col].type==ColType::Vector,"L2_REQUIRES_VECTOR");
        p.q=parse_vector(e.vec);require(p.q.size()==sch.cols[p.col].size,"VECTOR_DIM_MISMATCH");
      }
      return p;
    };
    std::vector<PreparedExpr> pp;
    Result res;res.is_query=true;
    for(const auto& e:projections){auto p=compile(e);pp.push_back(std::move(p));
      res.columns.push_back(e.alias.empty()?(e.type==Expr::Distance?"l2_distance":e.col):e.alias);
      res.column_types.push_back(e.type==Expr::Distance ? ResultType::Real :
        (sch.cols[pp.back().col].type==ColType::Bigint ? ResultType::Bigint :
         (sch.cols[pp.back().col].type==ColType::Vector ? ResultType::Vector : ResultType::Text)));
    }
    std::optional<PreparedExpr> order;
    if(s.order){
      Expr e=*s.order;
      if(e.type==Expr::Column){
        for(const auto& x:projections)if(!x.alias.empty()&&x.alias==e.col){e=x;break;}
      }
      order=compile(e);
    }
    // LIMIT 0 is an empty result, not a request to scan the whole table.
    if(s.limit && *s.limit==0)return res;
    // Late materialization: TopK candidates keep only PK and ordering key.
    // Rows are re-fetched from disk only for the final K output records.
    struct Pick{int64_t pk;Value key=int64_t{0};double distance=0;};
    auto better=[&](const Pick& a,const Pick& b){
      int cmp=0;
      if(order){
        if(order->e.type==Expr::Distance)cmp=a.distance<b.distance?-1:(a.distance>b.distance?1:0);
        else cmp=compare_values(a.key,b.key);
        if(s.descending)cmp=-cmp;
      }
      return cmp!=0?cmp<0:a.pk<b.pk;
    };
    using Heap=std::priority_queue<Pick,std::vector<Pick>,decltype(better)>;
    Heap heap(better);
    std::vector<Pick> all;
    auto offer=[&](Pick p){
      if(s.limit){
        if(*s.limit==0)return;
        if(heap.size()<*s.limit)heap.push(std::move(p));
        else if(better(p,heap.top())){heap.pop();heap.push(std::move(p));}
      } else all.push_back(std::move(p));
    };
    auto exact_scan=[&](){
      // A transaction sees its pending changes layered on a committed
      // immutable scan snapshot. Skip overwritten base PKs, then add the
      // transaction's new/updated rows; deletions contribute nothing.
      const std::map<int64_t,std::optional<Row>>* delta=nullptr;
      if(tx){auto it=tx->changes.find(s.table);if(it!=tx->changes.end())delta=&it->second;}
      auto add_delta=[&](){
        if(!delta)return;
        for(const auto& [pk,row]:*delta){
          if(!row || !matches(*row,predicates))continue;
          Pick p;p.pk=pk;
          p.distance=l2(std::get<Vec>((*row)[order->col]),order->q);
          offer(std::move(p));
        }
      };
      bool pk_equality=false;
      for(const auto& pred:predicates)
        if(pred.col==sch.pk_idx && pred.op=="=")pk_equality=true;
      if(order && order->e.type==Expr::Distance && s.limit && *s.limit>0 &&
         !pk_equality && (!tx || !tx->new_tables.count(s.table))) {
        if(auto* dense=get_vector_cache(s.table,sch,order->col)){
          const size_t dim=dense->dimension;
          const std::vector<uint32_t>* candidate_ids=nullptr;
          for(const auto& pred:predicates){
            if(pred.op!="=" || pred.col==sch.pk_idx)continue;
            const std::vector<uint32_t>* ids=nullptr;
            if(sch.cols[pred.col].type==ColType::Bigint)
              ids=cached_equal_bigint(*dense,pred.col,std::get<int64_t>(pred.rhs));
            else if(sch.cols[pred.col].type==ColType::Varchar)
              ids=cached_equal_string(*dense,pred.col,std::get<std::string>(pred.rhs));
            if(ids && (!candidate_ids || ids->size()<candidate_ids->size()))candidate_ids=ids;
          }
          const bool use_simd=simd_pruning_enabled() && !s.descending && dim>=8;
          const bool use_fma=use_simd && fma_pruning_enabled();
          const bool prepared=use_fma && prepared_query_enabled();
          const bool use_avx512=prepared && avx512_pruning_enabled();
          const std::vector<double> qdouble=prepared ?
            std::vector<double>(order->q.begin(),order->q.end()):std::vector<double>{};
          const auto visit=[&](size_t i){
            if(delta && delta->count(dense->pks[i]))return;
            for(const auto& pred:predicates){
              const int cmp=compare_values(dense->scalar_columns[pred.col][i],pred.rhs);
              const bool yes=pred.op=="="?cmp==0:pred.op=="<"?cmp<0:
                             pred.op=="<="?cmp<=0:pred.op==">"?cmp>0:cmp>=0;
              if(!yes)return;
            }
            Pick pick;
            pick.pk=dense->pks[i];
            const float* vec=dense->vectors.data()+i*dim;
            if(use_simd && heap.size()==*s.limit){
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
              const double fast_sq=use_avx512?
                l2_squared_avx512_prepared(vec,qdouble.data(),dim):
                (prepared?l2_squared_avx2_prepared(vec,qdouble.data(),dim):
                (use_fma?l2_squared_avx2_fma(vec,order->q.data(),dim):
                         l2_squared_avx2(vec,order->q.data(),dim)));
              if(definitely_worse_than_kth(fast_sq,heap.top().distance,dim))return;
#endif
            }
            pick.distance=l2_contiguous(vec,order->q);
            offer(std::move(pick));
          };
          if(candidate_ids)for(uint32_t i:*candidate_ids)visit(i);
          else for(size_t i=0;i<dense->pks.size();++i)visit(i);
          add_delta();
          return;
        }
        // Large-table path: rows are still read from the committed heap
        // mapping, but the B+Tree and variant row decoder are bypassed.
        // Mapping is invalidated after every committed publish.
        if(auto* mapped=get_mapped_scan(s.table,sch,order->col)){
          const size_t dim=mapped->dimension;
          const bool use_simd=simd_pruning_enabled() && !s.descending && dim>=8;
          const bool use_fma=use_simd && fma_pruning_enabled();
          const bool prepared=use_fma && prepared_query_enabled();
          const bool use_avx512=prepared && avx512_pruning_enabled();
          const std::vector<double> qdouble=prepared ?
            std::vector<double>(order->q.begin(),order->q.end()):std::vector<double>{};
          std::vector<float> aligned(dim); // one reusable, correctly aligned buffer
          const std::vector<uint32_t>* candidates=nullptr;
          for(const auto& pred:predicates){
            if(pred.op!="=" || pred.col==sch.pk_idx)continue;
            if(const auto* posting=mapped_equal_postings(*mapped,sch,pred.col,pred.rhs))
              if(!candidates || posting->size()<candidates->size())candidates=posting;
          }
          const auto visit=[&](size_t i){
            if(delta && delta->count(mapped->entries[i].pk))return;
            for(const auto& pred:predicates){
              int cmp=0;
              if(sch.cols[pred.col].type==ColType::Bigint){
                const int64_t v=mapped->numeric_columns[pred.col][i];
                const int64_t wanted=std::get<int64_t>(pred.rhs);
                cmp=v<wanted?-1:v>wanted?1:0;
              } else {
                cmp=mapped->string_columns[pred.col][i].compare(
                    std::string_view(std::get<std::string>(pred.rhs)));
              }
              const bool ok=pred.op=="="?cmp==0:pred.op=="<"?cmp<0:
                pred.op=="<="?cmp<=0:pred.op==">"?cmp>0:cmp>=0;
              if(!ok)return;
            }
            std::memcpy(aligned.data(),mapped->entries[i].vector,dim*sizeof(float));
            if(use_simd && heap.size()==*s.limit){
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
              if(definitely_worse_than_kth(
                   (use_avx512?l2_squared_avx512_prepared(aligned.data(),qdouble.data(),dim):
                   (prepared?l2_squared_avx2_prepared(aligned.data(),qdouble.data(),dim):
                   (use_fma?l2_squared_avx2_fma(aligned.data(),order->q.data(),dim):
                            l2_squared_avx2(aligned.data(),order->q.data(),dim)))),
                   heap.top().distance,dim))return;
#endif
            }
            Pick p;p.pk=mapped->entries[i].pk;
            p.distance=l2_contiguous(aligned.data(),order->q);
            offer(std::move(p));
          };
          if(candidates)for(uint32_t i:*candidates)visit(i);
          else for(size_t i=0;i<mapped->entries.size();++i)visit(i);
          add_delta();
          return;
        }
      }
      each_matching(s.table,sch,tx,predicates,[&](int64_t pk,const Row& row){
        Pick p;p.pk=pk;
        if(order){
          if(order->e.type==Expr::Distance)p.distance=l2(std::get<Vec>(row[order->col]),order->q);
          else p.key=row[order->col];
        }
        offer(std::move(p));
      });
    };
    // A committed PK-ordered LIMIT query can stop after K matching rows.
    // Unlike materializing+sorting an entire table, this visits only the
    // necessary B+Tree range; transaction overlays retain the old safe path.
    bool pk_limit_shortcut=false;
    if(!tx && pk_fastpath_enabled() && order && order->e.type==Expr::Column &&
       order->col==sch.pk_idx && s.limit){
      pk_limit_shortcut=true;
      const auto bounds=pk_bounds(sch,predicates);
      if(!bounds.empty){
        size_t accepted=0;
        store->scan_range(s.table,bounds.low,bounds.high,s.descending,
          [&](int64_t pk,uint64_t off,uint32_t len){
            Row row=decode_row(store->read_row(off,len));
            if(matches(row,predicates)){
              Pick pick;pick.pk=pk;pick.key=row[order->col];
              offer(std::move(pick));
              ++accepted;
            }
            return accepted<*s.limit;
          });
      }
    }
    const bool use_ann=ann_mode&&order&&order->e.type==Expr::Distance&&
      s.limit&&*s.limit>0&&!s.descending&&ann.count(s.table)&&
      indexes.at(s.table).col==order->e.col;
    if(pk_limit_shortcut){ /* already streamed the PK-ordered range */ }
    else if(!use_ann)exact_scan();
    else {
      const auto& idx=*ann.at(s.table);
      size_t probes=std::max<size_t>(1,(idx.list_count()*98+99)/100);
      if(const char* env=std::getenv("MVDB_ANN_PROBES")){
        try{probes=std::min(idx.list_count(),std::max<size_t>(1,std::stoul(env)));}
        catch(...){} // Invalid development-only configuration keeps conservative default.
      }
      // IVF visits random B+Tree pages to check versioned postings. The
      // exact scanner is faster on small/medium datasets or high nprobe;
      // avoid paying ANN I/O for near-exhaustive searches. An explicit
      // development flag permits measurements of the actual IVF path.
      const bool force_index=std::getenv("MVDB_ANN_FORCE_INDEX") &&
        std::strcmp(std::getenv("MVDB_ANN_FORCE_INDEX"),"1")==0;
      if(!force_index && (store->heap_bytes()<64ull*1024*1024 ||
                          probes>=std::max<size_t>(1,idx.list_count()/4))){
        exact_scan();
      } else {
      const auto* overlay=[&]()->const std::map<int64_t,std::optional<Row>>*{
        if(!tx)return nullptr;
        auto it=tx->changes.find(s.table);
        return it==tx->changes.end()?nullptr:&it->second;
      }();
      idx.candidates(order->q,probes,[&](const VectorPosting& candidate){
        if(overlay&&overlay->count(candidate.pk))return;
        auto current=store->get_ref(s.table,candidate.pk);
        if(!current||current->offset!=candidate.row_offset||current->length!=candidate.row_length)return;
        if(!predicates.empty()){
          auto row=visible_row(s.table,candidate.pk,tx);
          if(!row||!matches(*row,predicates))return;
        }
        Pick p;p.pk=candidate.pk;p.distance=l2(candidate.vector,order->q);
        offer(std::move(p));
      });
      // Include uncommitted writes and suppress overwritten/deleted index entries.
      if(overlay)for(const auto& [pk,row]:*overlay){
        if(!row||!matches(*row,predicates))continue;
        Pick p;p.pk=pk;p.distance=l2(std::get<Vec>((*row)[order->col]),order->q);
        offer(std::move(p));
      }
      // Underfull candidate sets must return all matching rows; don't silently
      // report fewer than K when non-probed clusters contain eligible records.
      if(heap.size()<*s.limit){
        while(!heap.empty())heap.pop();
        exact_scan();
      }
      }
    }
    while(!heap.empty()){all.push_back(heap.top());heap.pop();}
    std::sort(all.begin(),all.end(),better);
    for(const auto& p:all){
      auto row=visible_row(s.table,p.pk,tx);
      require(row.has_value(),"STORE_LOST_SELECTED_ROW");
      std::vector<std::string> cells;
      cells.reserve(pp.size());
      for(const auto& e:pp){
        if(e.e.type==Expr::Distance)
          cells.push_back(double_string(l2(std::get<Vec>((*row)[e.col]),e.q)));
        else cells.push_back(value_string((*row)[e.col]));
      }
      res.rows.push_back(std::move(cells));
    }
    return res;
  }
  Result execute(const std::string& sql){
    require(!io_poisoned,"DATABASE_IO_POISONED_RESTART_REQUIRED");
    Statement stmt=Parser(sql).parse();
    if(stmt.type==Statement::Begin){require(!active,"NESTED_TRANSACTION_NOT_SUPPORTED");active.emplace();return {};}
    if(stmt.type==Statement::Rollback){require(active.has_value(),"NO_ACTIVE_TRANSACTION");active.reset();return {};}
    if(stmt.type==Statement::Commit){require(active.has_value(),"NO_ACTIVE_TRANSACTION");
      commit_tx(*active);active.reset();return {};}
    if(stmt.type==Statement::Select)return select(stmt,active?&*active:nullptr);

    Result result;
    if(active){
      if(stmt.type==Statement::CreateVectorIndex)create_vector_index(stmt,*active);
      else if(stmt.type==Statement::CreateTable)create_table(stmt,*active);
      else if(stmt.type==Statement::Insert)result.affected_rows=insert(stmt,*active);
      else if(stmt.type==Statement::Update)result.affected_rows=update_or_delete(stmt,*active,false);
      else if(stmt.type==Statement::Delete)result.affected_rows=update_or_delete(stmt,*active,true);
    } else {
      Txn tx;
      if(stmt.type==Statement::CreateVectorIndex)create_vector_index(stmt,tx);
      else if(stmt.type==Statement::CreateTable)create_table(stmt,tx);
      else if(stmt.type==Statement::Insert)result.affected_rows=insert(stmt,tx);
      else if(stmt.type==Statement::Update)result.affected_rows=update_or_delete(stmt,tx,false);
      else if(stmt.type==Statement::Delete)result.affected_rows=update_or_delete(stmt,tx,true);
      commit_tx(tx);
    }
    return result;
  }
};

Database::Database(const std::string& data_dir,bool ann_mode):impl_(std::make_unique<Impl>(data_dir,ann_mode)){}
Database::~Database()=default;
void Database::set_search_mode(bool ann_mode) { impl_->ann_mode=ann_mode; }
Result Database::execute(const std::string& sql) {
  try{return impl_->execute(sql);}
  catch(const DbError& e){Result r;r.ok=false;r.error=e.what();return r;}
  catch(const std::exception& e){Result r;r.ok=false;r.error="INTERNAL_ERROR";return r;}
}
} // namespace mvdb
