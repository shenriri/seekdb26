#include "minivectordb/ivf_index.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <numeric>
#include <random>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace mvdb {
namespace {
namespace fs=std::filesystem;
void must(bool good, const char* why){ if(!good)throw std::runtime_error(why); }
void put32(std::string& s,uint32_t v){for(int i=0;i<4;++i)s+=char(v>>(8*i));}
void put64(std::string& s,uint64_t v){for(int i=0;i<8;++i)s+=char(v>>(8*i));}
uint32_t read32(const char* p){uint32_t x=0;for(int i=0;i<4;++i)x|=uint32_t(uint8_t(p[i]))<<(i*8);return x;}
uint64_t read64(const char* p){return uint64_t(read32(p))|(uint64_t(read32(p+4))<<32);}
uint32_t crc32(const std::string_view s){uint32_t c=0xffffffffu;for(unsigned char ch:s){c^=ch;for(int i=0;i<8;++i)c=(c>>1)^((c&1)?0xedb88320u:0u);}return ~c;}
void write_all(int fd, const std::string& b){size_t pos=0;while(pos<b.size()){ssize_t n=::write(fd,b.data()+pos,b.size()-pos);if(n<0&&errno==EINTR)continue;must(n>0,"ANN_WRITE_FAILED");pos+=size_t(n);}}
void read_all(int fd, char* p, size_t size){size_t pos=0;while(pos<size){ssize_t n=::read(fd,p+pos,size-pos);if(n<0&&errno==EINTR)continue;must(n>0,"ANN_READ_FAILED");pos+=size_t(n);}}
uint64_t file_size(int fd){struct stat s{};must(fstat(fd,&s)==0,"ANN_STAT_FAILED");return uint64_t(s.st_size);}
void sync_file(int fd){must(fdatasync(fd)==0,"ANN_SYNC_FAILED");}
void sync_dir(const fs::path& p){int fd=open(p.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);must(fd>=0,"ANN_DIR_OPEN_FAILED");int rc=fsync(fd);close(fd);must(rc==0,"ANN_DIR_SYNC_FAILED");}
double sqdist(const std::vector<float>& a,const std::vector<float>& b){double d=0;for(size_t k=0;k<a.size();++k){double x=double(a[k])-b[k];d+=x*x;}return d;}
struct ScopedFd {int fd=-1;explicit ScopedFd(int n):fd(n){}~ScopedFd(){if(fd>=0)close(fd);}ScopedFd(const ScopedFd&)=delete;ScopedFd& operator=(const ScopedFd&)=delete;};
}
IVFIndex::IVFIndex(std::string data, std::string table, std::string name,
                   std::string col,uint32_t dim):dimension_(dim){
  directory_=(fs::path(data)/("ann_"+table+"_"+name+"_"+col)).string();
  must(dim>0&&dim<=1000000,"ANN_INVALID_DIMENSION");
}
std::string IVFIndex::cluster_path(uint32_t c)const{return (fs::path(directory_)/("list_"+std::to_string(c)+".post")).string();}
uint32_t IVFIndex::closest(const std::vector<float>& x) const {
  must(x.size()==dimension_&&!centroids_.empty(),"ANN_INVALID_VECTOR");
  double best=INFINITY;uint32_t id=0;
  for(uint32_t i=0;i<centroids_.size();++i){double d=sqdist(x,centroids_[i]);if(d<best){best=d;id=i;}}
  return id;
}
bool IVFIndex::load_if_current(uint64_t cursor,uint64_t generation){
  centroids_.clear();
  auto meta=fs::path(directory_)/"meta.bin";
  if(!fs::exists(meta))return false;
  ScopedFd fd(open(meta.c_str(),O_RDONLY|O_CLOEXEC));if(fd.fd<0)return false;
  const auto len=file_size(fd.fd);
  if(len<36||len>32u*1024u*1024u)return false;
  std::string blob(len,'\0');read_all(fd.fd,blob.data(),blob.size());
  if(std::memcmp(blob.data(),"IVF3",4)!=0||read32(blob.data()+4)!=2)return false;
  uint32_t dim=read32(blob.data()+8),nlist=read32(blob.data()+12);
  uint64_t seen=read64(blob.data()+16),disk_gen=read64(blob.data()+24);
  if(dim!=dimension_||nlist<1||nlist>256||seen!=cursor||disk_gen!=generation)return false;
  if(len!=36ull+uint64_t(nlist)*dimension_*4)return false;
  if(crc32(std::string_view(blob).substr(0,len-4))!=read32(blob.data()+len-4))return false;
  const size_t recsz=24ull+size_t(dim)*4;
  for(uint32_t c=0;c<nlist;++c){
    auto file=cluster_path(c);if(!fs::exists(file))return false;
    auto n=fs::file_size(file);if(n%recsz!=0)return false;
  }
  size_t pos=32;
  for(uint32_t i=0;i<nlist;++i){std::vector<float> center(dim);
    for(uint32_t j=0;j<dim;++j){uint32_t b=read32(blob.data()+pos);std::memcpy(&center[j],&b,4);pos+=4;}
    centroids_.push_back(std::move(center));
  }
  cursor_=cursor;data_generation_=generation;return true;
}
void IVFIndex::write_metadata(uint64_t cursor){
  fs::create_directories(directory_);
  std::string blob="IVF3";put32(blob,2);put32(blob,dimension_);put32(blob,uint32_t(centroids_.size()));put64(blob,cursor);put64(blob,data_generation_);
  for(const auto& c:centroids_)for(float v:c){uint32_t bits;std::memcpy(&bits,&v,4);put32(blob,bits);}
  put32(blob,crc32(blob));
  const auto tmp=fs::path(directory_)/"meta.bin.tmp";
  ScopedFd fd(open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644));must(fd.fd>=0,"ANN_META_OPEN_FAILED");
  write_all(fd.fd,blob);sync_file(fd.fd);
  must(rename(tmp.c_str(),(fs::path(directory_)/"meta.bin").c_str())==0,"ANN_META_RENAME_FAILED");
  sync_dir(directory_);cursor_=cursor;
}
void IVFIndex::rebuild(uint64_t cursor,uint64_t generation,const Enumerator& enumerate){
  data_generation_=generation;
  // A missing/old meta header makes interrupted rebuilds distinguishable.
  fs::create_directories(directory_);
  std::error_code ec;fs::remove(fs::path(directory_)/"meta.bin",ec);
  // Reservoir sampling bounds centroid-training RAM even for oversized tables.
  constexpr size_t MAX_SAMPLE=1024;
  std::vector<std::vector<float>> sample;sample.reserve(MAX_SAMPLE);
  std::mt19937_64 rng(20261109);
  size_t seen=0;
  enumerate([&](const VectorPosting& p){
    must(p.vector.size()==dimension_,"ANN_BUILD_DIMENSION");
    ++seen;
    if(sample.size()<MAX_SAMPLE)sample.push_back(p.vector);
    else {std::uniform_int_distribution<size_t> pick(0,seen-1);auto i=pick(rng);if(i<MAX_SAMPLE)sample[i]=p.vector;}
  });
  size_t nlist=seen==0?1:std::min<size_t>(64,std::max<size_t>(1,size_t(std::sqrt(double(seen)))));
  centroids_.clear();centroids_.reserve(nlist);
  if(sample.empty())centroids_.push_back(std::vector<float>(dimension_,0));
  else {
    // K-means++ inspired deterministic farthest-first initialization.
    centroids_.push_back(sample.front());
    while(centroids_.size()<nlist){double far=-1;size_t best=0;
      for(size_t i=0;i<sample.size();++i){double d=INFINITY;for(auto& c:centroids_)d=std::min(d,sqdist(sample[i],c));
        if(d>far){far=d;best=i;}}
      centroids_.push_back(sample[best]);
    }
    for(int round=0;round<5;++round){
      std::vector<std::vector<double>> sums(nlist,std::vector<double>(dimension_,0));
      std::vector<size_t> counts(nlist,0);
      for(auto& x:sample){auto c=closest(x);counts[c]++;for(uint32_t j=0;j<dimension_;++j)sums[c][j]+=x[j];}
      for(size_t c=0;c<nlist;++c)if(counts[c])for(uint32_t j=0;j<dimension_;++j)centroids_[c][j]=float(sums[c][j]/counts[c]);
    }
  }
  std::vector<int> files;files.reserve(nlist);
  try{
    for(uint32_t c=0;c<nlist;++c){int fd=open(cluster_path(c).c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);must(fd>=0,"ANN_LIST_OPEN_FAILED");files.push_back(fd);}
    enumerate([&](const VectorPosting& p){
      std::string b; b.reserve(24+4*dimension_);
      put64(b,uint64_t(p.pk));put64(b,p.row_offset);put32(b,p.row_length);
      for(float v:p.vector){uint32_t bits;std::memcpy(&bits,&v,4);put32(b,bits);}
      put32(b,crc32(b));write_all(files[closest(p.vector)],b);
    });
    for(auto fd:files)sync_file(fd);
  }catch(...){for(auto fd:files)close(fd);throw;}
  for(auto fd:files)close(fd);
  write_metadata(cursor);
}
void IVFIndex::append(uint64_t cursor,const std::vector<VectorPosting>& changed){
  must(!centroids_.empty()&&cursor>=cursor_,"ANN_NOT_READY");
  std::vector<int> files(centroids_.size(),-1);
  try{
    for(const auto& p:changed){
      must(p.vector.size()==dimension_,"ANN_APPEND_DIMENSION");
      auto c=closest(p.vector);
      if(files[c]<0){files[c]=open(cluster_path(c).c_str(),O_WRONLY|O_APPEND|O_CLOEXEC);must(files[c]>=0,"ANN_APPEND_OPEN_FAILED");}
      std::string b;b.reserve(24+dimension_*4);put64(b,uint64_t(p.pk));put64(b,p.row_offset);put32(b,p.row_length);
      for(float v:p.vector){uint32_t bits;std::memcpy(&bits,&v,4);put32(b,bits);}
      put32(b,crc32(b));write_all(files[c],b);
    }
    for(int fd:files)if(fd>=0)sync_file(fd);
  }catch(...){for(int fd:files)if(fd>=0)close(fd);throw;}
  for(int fd:files)if(fd>=0)close(fd);
  // Publication order: committed DB manifest, synced new postings, then atomic
  // index cursor. If interrupted, startup discards/rebuilds the derived index.
  write_metadata(cursor);
}
void IVFIndex::advance_cursor(uint64_t cursor){write_metadata(cursor);}
void IVFIndex::candidates(const std::vector<float>& q,size_t probes,const Visitor& fn)const{
  must(q.size()==dimension_&&!centroids_.empty(),"ANN_NOT_READY");
  std::vector<uint32_t> ids(centroids_.size());std::iota(ids.begin(),ids.end(),0);
  const auto dist=[&](uint32_t i){return sqdist(q,centroids_[i]);};
  std::sort(ids.begin(),ids.end(),[&](uint32_t a,uint32_t b){double x=dist(a),y=dist(b);return x==y?a<b:x<y;});
  size_t recsz=24u+size_t(dimension_)*4;
  std::string chunk;chunk.resize(recsz*64);
  for(size_t z=0;z<std::min(probes,ids.size());++z){
    ScopedFd fd(open(cluster_path(ids[z]).c_str(),O_RDONLY|O_CLOEXEC));must(fd.fd>=0,"ANN_SCAN_OPEN_FAILED");
    uint64_t size=file_size(fd.fd);must(size%recsz==0,"ANN_BAD_POSTING_SIZE");
    for(uint64_t off=0;off<size;){
      size_t n=size_t(std::min<uint64_t>(chunk.size(),size-off));n-=n%recsz;
      read_all(fd.fd,chunk.data(),n);off+=n;
      for(size_t p=0;p<n;p+=recsz){const char* b=chunk.data()+p;
        must(crc32(std::string_view(b,recsz-4))==read32(b+recsz-4),"ANN_POSTING_CRC_MISMATCH");
        VectorPosting item;item.pk=int64_t(read64(b));item.row_offset=read64(b+8);item.row_length=read32(b+16);
        item.vector.resize(dimension_);
        for(uint32_t j=0;j<dimension_;++j){uint32_t bits=read32(b+20+4*j);std::memcpy(&item.vector[j],&bits,4);}
        fn(item);
      }
    }
  }
}
} // namespace mvdb
