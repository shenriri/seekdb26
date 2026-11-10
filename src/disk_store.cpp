#include "minivectordb/disk_store.h"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <sys/mman.h>
#include <limits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mvdb {
namespace {
namespace fs=std::filesystem;
constexpr size_t PAGE=8192;
constexpr uint32_t NODE_MAGIC=0x32444e4d; // MND2
constexpr uint32_t MAN_MAGIC=0x324d564d; // MVM2
constexpr size_t MAX_KEYS=320;

void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void failpoint(const char* name){const char* s=getenv("MVDB_TEST_FAILPOINT");if(s&&strcmp(s,name)==0)raise(SIGKILL);}
void put32(std::string& s,uint32_t n){for(int i=0;i<4;i++)s.push_back(char((n>>(i*8))&255));}
void put64(std::string& s,uint64_t n){for(int i=0;i<8;i++)s.push_back(char((n>>(i*8))&255));}
void putstr(std::string& s,const std::string& text){check(text.size()<=UINT32_MAX,"STORE_STRING_TOO_LONG");put32(s,uint32_t(text.size()));s+=text;}
struct BufRead {
 std::string_view s;size_t p=0;
 explicit BufRead(std::string_view str):s(str){}
 uint32_t u32(){check(p+4<=s.size(),"STORE_CORRUPT_RECORD");uint32_t x=0;for(int i=0;i<4;i++)x|=uint32_t(uint8_t(s[p++]))<<(i*8);return x;}
 uint64_t u64(){uint64_t x=0;for(int i=0;i<2;i++)x|=uint64_t(u32())<<(i*32);return x;}
 std::string str(){uint32_t n=u32();check(n<=s.size()-p,"STORE_CORRUPT_RECORD");auto z=std::string(s.substr(p,n));p+=n;return z;}
};
uint32_t crc(std::string_view bytes){uint32_t c=0xffffffff;for(unsigned char ch:bytes){c^=ch;for(int b=0;b<8;b++)c=(c>>1)^((c&1)?0xedb88320u:0u);}return ~c;}
void write_at(int fd,uint64_t at,std::string_view s){size_t n=0;while(n<s.size()){ssize_t r=pwrite(fd,s.data()+n,s.size()-n,off_t(at+n));if(r<0&&errno==EINTR)continue;check(r>0,"STORE_WRITE_FAILED");n+=size_t(r);}}
void read_at(int fd,uint64_t at,char* p,size_t len){size_t n=0;while(n<len){ssize_t r=pread(fd,p+n,len-n,off_t(at+n));if(r<0&&errno==EINTR)continue;check(r>0,"STORE_READ_FAILED");n+=size_t(r);}}
uint64_t filesize(int fd){struct stat st{};check(fstat(fd,&st)==0,"STORE_STAT_FAILED");return uint64_t(st.st_size);}
int open_rw(const fs::path& path){int fd=open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0644);check(fd>=0,"STORE_OPEN_FAILED");return fd;}
void sync(int fd){check(fdatasync(fd)==0,"STORE_SYNC_FAILED");}
struct Item {int64_t key;uint64_t offset;uint32_t length;};
struct Node {
 bool leaf=true;
 std::vector<int64_t> keys;
 std::vector<Item> vals;
 std::vector<uint64_t> children;
};
std::string serialize(const Node& n){
 check(n.keys.size()<=MAX_KEYS,"BTREE_NODE_OVERFLOW");
 std::string b;put32(b,NODE_MAGIC);put32(b,1);put32(b,n.leaf?1u:0u);put32(b,uint32_t(n.keys.size()));
 if(n.leaf){check(n.vals.size()==n.keys.size(),"BTREE_BAD_LEAF");
  for(const auto& e:n.vals){put64(b,uint64_t(e.key));put64(b,e.offset);put32(b,e.length);}
 } else {check(n.children.size()==n.keys.size()+1,"BTREE_BAD_INTERNAL");
  for(int64_t x:n.keys)put64(b,uint64_t(x));
  for(uint64_t x:n.children)put64(b,x);
 }
 check(b.size()<=PAGE-4,"BTREE_NODE_OVERFLOW");b.resize(PAGE-4,'\0');put32(b,crc(b));return b;
}
Node deserialize(std::string_view data){
 check(data.size()==PAGE,"BTREE_INVALID_PAGE");BufRead trailer(data.substr(PAGE-4));
 check(crc(data.substr(0,PAGE-4))==trailer.u32(),"BTREE_PAGE_CRC_MISMATCH");
 BufRead r(data.substr(0,PAGE-4));check(r.u32()==NODE_MAGIC&&r.u32()==1,"BTREE_BAD_HEADER");
 Node n;n.leaf=r.u32()==1;auto count=r.u32();check(count<=MAX_KEYS,"BTREE_INVALID_COUNT");
 if(n.leaf){for(uint32_t i=0;i<count;i++){int64_t key=int64_t(r.u64());uint64_t off=r.u64();uint32_t len=r.u32();n.keys.push_back(key);n.vals.push_back({key,off,len});}}
 else {for(uint32_t i=0;i<count;i++)n.keys.push_back(int64_t(r.u64()));for(uint32_t i=0;i<=count;i++)n.children.push_back(r.u64());}
 check(std::is_sorted(n.keys.begin(),n.keys.end()),"BTREE_KEYS_UNSORTED");
 return n;
}
} // namespace

struct DiskStore::Impl {
 fs::path path;
 int heap_fd=-1,page_fd=-1,dir_fd=-1;
 uint64_t applied_wal=0,heap_end=0,next_pid=1,wal_generation=0,data_generation=0;
 std::string catalog_blob;
 std::map<std::string,uint64_t> roots;
 struct Cached {Node node;std::list<uint64_t>::iterator it;};
 mutable std::list<uint64_t> lru;
 mutable std::unordered_map<uint64_t,Cached> cache;
 static constexpr size_t CACHE_FRAMES=64;
 struct CachedRow { std::string raw; std::list<uint64_t>::iterator it; };
 mutable std::list<uint64_t> row_lru;
 mutable std::unordered_map<uint64_t,CachedRow> row_cache;
 mutable size_t row_cache_bytes=0;
 static constexpr size_t ROW_CACHE_LIMIT=1u*1024u*1024u;
 // Within one transaction, keep new COW nodes transient until the whole batch
 // is assembled. Only reachable nodes are materialized on disk; otherwise
 // repeated updates to the same leaf cause O(batch_size) write amplification.
 bool staging=false;
 uint64_t staging_base=0;
 std::unordered_map<uint64_t,Node> staged;

 fs::path heap_path(uint64_t gen) const {
  return path/(gen ? ("heap."+std::to_string(gen)+".mvdb") : "heap.mvdb");
 }
 fs::path index_path(uint64_t gen) const {
  return path/(gen ? ("index."+std::to_string(gen)+".mvdb") : "index.mvdb");
 }
 explicit Impl(const std::string& directory):path(directory) {
  try {
   fs::create_directories(path);
   dir_fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
   check(dir_fd>=0,"STORE_DIRECTORY_OPEN_FAILED");
   auto manifest=path/"manifest.mvdb";
   if(fs::exists(manifest)){
    int fd=open(manifest.c_str(),O_RDONLY|O_CLOEXEC);check(fd>=0,"MANIFEST_OPEN_FAILED");
    auto len=filesize(fd);check(len>32&&len<=16u*1024u*1024u,"MANIFEST_INVALID_SIZE");
    std::string raw(len,'\0');
    try{read_at(fd,0,raw.data(),raw.size());}catch(...){close(fd);throw;}
    close(fd);
    BufRead tail(std::string_view(raw.data()+raw.size()-4,4));
    check(tail.u32()==crc(std::string_view(raw).substr(0,raw.size()-4)),"MANIFEST_CRC_MISMATCH");
    BufRead r(std::string_view(raw).substr(0,raw.size()-4));
    check(r.u32()==MAN_MAGIC,"MANIFEST_BAD_VERSION");
    auto version=r.u32();check(version==1||version==2||version==3,"MANIFEST_BAD_VERSION");
    applied_wal=r.u64();heap_end=r.u64();next_pid=r.u64();
    if(version>=2)wal_generation=r.u64();
    if(version>=3)data_generation=r.u64();
    catalog_blob=r.str();
    auto count=r.u32();check(count<=100000,"MANIFEST_TOO_MANY_TABLES");
    for(uint32_t i=0;i<count;i++){auto name=r.str();auto root=r.u64();check(root<next_pid,"MANIFEST_INVALID_ROOT");check(roots.emplace(name,root).second,"MANIFEST_DUP_TABLE");}
    check(r.p==raw.size()-4,"MANIFEST_TRAILING_BYTES");
    check(data_generation<UINT64_MAX,"MANIFEST_GENERATION_OVERFLOW");
    heap_fd=open_rw(heap_path(data_generation));page_fd=open_rw(index_path(data_generation));
    const uint64_t published_index_size=(next_pid==1?0:next_pid*PAGE);
    check(filesize(heap_fd)>=heap_end&&filesize(page_fd)>=published_index_size,"STORE_MISSING_COMMITTED_DATA");
    // Any data beyond the committed manifest is from an interrupted copy-on-write publish.
    check(ftruncate(heap_fd,off_t(heap_end))==0,"STORE_TRUNCATE_HEAP_FAILED");
    check(ftruncate(page_fd,off_t(published_index_size))==0,"STORE_TRUNCATE_INDEX_FAILED");
   } else {
    heap_fd=open_rw(heap_path(0));page_fd=open_rw(index_path(0));
    // A new store or a phase-1 WAL migration. Only a complete WAL commit is authoritative.
    check(ftruncate(heap_fd,0)==0,"STORE_RESET_HEAP_FAILED");
    check(ftruncate(page_fd,0)==0,"STORE_RESET_INDEX_FAILED");
   }
   // The manifest is the only data-generation authority. A SIGKILL during
   // compaction may leave files from either side of a generation switch;
   // reclaim them only after validating the published data files.
   bool removed=false;
   for(const auto& entry:fs::directory_iterator(path)){
     if(!entry.is_regular_file())continue;
     std::string name=entry.path().filename().string();
     uint64_t gen=0;bool recognized=false;
     for(const auto& prefix:{std::string("heap"),std::string("index")}){
       if(name==prefix+".mvdb"){recognized=true;gen=0;break;}
       const std::string head=prefix+".",suffix=".mvdb";
       if(name.rfind(head,0)==0&&name.size()>head.size()+suffix.size()&&
          name.substr(name.size()-suffix.size())==suffix){
         auto digits=name.substr(head.size(),name.size()-head.size()-suffix.size());
         if(digits.empty()||digits[0]=='0' ||
            !std::all_of(digits.begin(),digits.end(),[](unsigned char ch){return ch>='0'&&ch<='9';}))break;
         try{gen=std::stoull(digits);recognized=true;}catch(...){recognized=false;}
         break;
       }
     }
     if(recognized&&gen!=data_generation){
       std::error_code ec;fs::remove(entry.path(),ec);
       check(!ec,"STORE_ORPHAN_CLEANUP_FAILED");removed=true;
     }
   }
   if(removed)check(fsync(dir_fd)==0,"STORE_ORPHAN_SYNC_FAILED");
  } catch(...){if(heap_fd>=0)close(heap_fd);if(page_fd>=0)close(page_fd);if(dir_fd>=0)close(dir_fd);throw;}
 }
 ~Impl(){if(heap_fd>=0)close(heap_fd);if(page_fd>=0)close(page_fd);if(dir_fd>=0)close(dir_fd);}
 Node load(uint64_t pid) const {
  check(pid>0&&pid<next_pid,"BTREE_INVALID_PAGE_ID");
  if(staging && pid>=staging_base){
    auto it=staged.find(pid);check(it!=staged.end(),"BTREE_MISSING_STAGED_PAGE");return it->second;
  }
  auto it=cache.find(pid);
  if(it!=cache.end()){lru.splice(lru.begin(),lru,it->second.it);return it->second.node;}
  std::string bytes(PAGE,'\0');read_at(page_fd,pid*PAGE,bytes.data(),PAGE);
  Node n=deserialize(bytes);
  if(cache.size()==CACHE_FRAMES){cache.erase(lru.back());lru.pop_back();}
  lru.push_front(pid);cache.emplace(pid,Cached{n,lru.begin()});
  return n;
 }
 uint64_t append_node(const Node& n){
  const uint64_t pid=next_pid++;
  if(staging){staged.emplace(pid,n);return pid;}
  auto bytes=serialize(n);
  write_at(page_fd,pid*PAGE,bytes);
  if(cache.size()==CACHE_FRAMES){cache.erase(lru.back());lru.pop_back();}
  lru.push_front(pid);cache.emplace(pid,Cached{n,lru.begin()});
  return pid;
 }
 std::pair<uint64_t,uint32_t> append_row(const std::string& raw){
  check(raw.size()<=64u*1024u*1024u,"ROW_TOO_LARGE");
  const uint64_t offset=heap_end;
  std::string head;put32(head,uint32_t(raw.size()));put32(head,crc(raw));
  write_at(heap_fd,heap_end,head);heap_end+=head.size();
  write_at(heap_fd,heap_end,raw);heap_end+=raw.size();
  return {offset,uint32_t(raw.size())};
 }
 std::string read_row(const Item& item) const {
  check(item.length>0&&item.length<=64u*1024u*1024u,"STORE_INVALID_ROW_LENGTH");
  auto cached=row_cache.find(item.offset);
  if(cached!=row_cache.end()){
    row_lru.splice(row_lru.begin(),row_lru,cached->second.it);
    return cached->second.raw;
  }
  check(item.offset+8ull+item.length<=heap_end,"STORE_INVALID_ROW_OFFSET");
  char h[8];read_at(heap_fd,item.offset,h,8);BufRead r(std::string_view(h,8));
  auto n=r.u32();auto expected=r.u32();check(n==item.length,"STORE_ROW_LENGTH_MISMATCH");
  std::string raw(n,'\0');read_at(heap_fd,item.offset+8,raw.data(),n);
  check(crc(raw)==expected,"STORE_ROW_CRC_MISMATCH");
  if(raw.size()<=ROW_CACHE_LIMIT){
    while(!row_lru.empty()&&row_cache_bytes+raw.size()>ROW_CACHE_LIMIT){
      auto last=row_lru.back();auto it=row_cache.find(last);
      row_cache_bytes-=it->second.raw.size();row_cache.erase(it);row_lru.pop_back();
    }
    row_lru.push_front(item.offset);
    row_cache_bytes+=raw.size();
    row_cache.emplace(item.offset,CachedRow{raw,row_lru.begin()});
  }
  return raw;
 }
 std::optional<Item> find(uint64_t root,int64_t key) const {
  for(auto pid=root;pid;){Node node=load(pid);
   if(node.leaf){auto it=std::lower_bound(node.keys.begin(),node.keys.end(),key);if(it==node.keys.end()||*it!=key)return std::nullopt;return node.vals[size_t(it-node.keys.begin())];}
   size_t index=size_t(std::upper_bound(node.keys.begin(),node.keys.end(),key)-node.keys.begin());pid=node.children[index];
  }
  return std::nullopt;
 }
 struct UpResult {uint64_t left;std::optional<std::pair<int64_t,uint64_t>> split;};
 UpResult upsert(uint64_t pid,Item item){
  if(pid==0){Node node;node.keys.push_back(item.key);node.vals.push_back(item);return {append_node(node),std::nullopt};}
  Node n=load(pid);
  if(n.leaf){
   auto it=std::lower_bound(n.keys.begin(),n.keys.end(),item.key);
   size_t at=size_t(it-n.keys.begin());
   if(it!=n.keys.end()&&*it==item.key)n.vals[at]=item;
   else {n.keys.insert(it,item.key);n.vals.insert(n.vals.begin()+at,item);}
   if(n.keys.size()<=MAX_KEYS)return {append_node(n),std::nullopt};
   const size_t mid=n.keys.size()/2;
   Node right;right.keys.assign(n.keys.begin()+mid,n.keys.end());right.vals.assign(n.vals.begin()+mid,n.vals.end());
   n.keys.resize(mid);n.vals.resize(mid);
   auto left_pid=append_node(n);auto right_pid=append_node(right);
   return {left_pid,std::make_pair(right.keys.front(),right_pid)};
  }
  auto at=size_t(std::upper_bound(n.keys.begin(),n.keys.end(),item.key)-n.keys.begin());
  auto child=upsert(n.children[at],item);n.children[at]=child.left;
  if(child.split){n.keys.insert(n.keys.begin()+at,child.split->first);n.children.insert(n.children.begin()+at+1,child.split->second);}
  if(n.keys.size()<=MAX_KEYS)return {append_node(n),std::nullopt};
  const size_t mid=n.keys.size()/2;auto middle=n.keys[mid];
  Node right;right.leaf=false;
  right.keys.assign(n.keys.begin()+mid+1,n.keys.end());
  right.children.assign(n.children.begin()+mid+1,n.children.end());
  n.keys.resize(mid);n.children.resize(mid+1);
  auto left_pid=append_node(n);auto right_pid=append_node(right);
  return {left_pid,std::make_pair(middle,right_pid)};
 }
 uint64_t put(uint64_t root,Item item){
  auto result=upsert(root,item);
  if(!result.split)return result.left;
  Node n;n.leaf=false;n.keys.push_back(result.split->first);n.children={result.left,result.split->second};return append_node(n);
 }
 void scan_node(uint64_t pid,const std::function<void(int64_t,uint64_t,uint32_t)>& fn)const{
  if(!pid)return;
  auto n=load(pid);
  if(n.leaf){for(const auto& e:n.vals)if(e.length)fn(e.key,e.offset,e.length);}
  else for(auto child:n.children)scan_node(child,fn);
 }
 bool scan_node_range(uint64_t pid,int64_t low,int64_t high,bool descending,
       const std::function<bool(int64_t,uint64_t,uint32_t)>& fn)const {
  if(!pid)return true;
  const auto node=load(pid);
  if(node.leaf){
    if(descending){
      for(auto it=node.vals.rbegin();it!=node.vals.rend();++it){
        if(it->key>high)continue;
        if(it->key<low)break;
        if(it->length && !fn(it->key,it->offset,it->length))return false;
      }
    }else{
      for(const auto& v:node.vals){
        if(v.key<low)continue;
        if(v.key>high)break;
        if(v.length && !fn(v.key,v.offset,v.length))return false;
      }
    }
    return true;
  }
  // Children are in PK order, with the separator equal to the next child's
  // first key. Skip irrelevant subtrees and stop immediately at LIMIT.
  if(descending){
    for(size_t c=node.children.size();c-->0;){
      if(c>0 && node.keys[c-1]>high)continue;
      if(c<node.keys.size() && node.keys[c]<=low)continue;
      if(!scan_node_range(node.children[c],low,high,true,fn))return false;
    }
  } else {
    for(size_t c=0;c<node.children.size();++c){
      if(c>0 && node.keys[c-1]>high)break;
      if(c<node.keys.size() && node.keys[c]<=low)continue;
      if(!scan_node_range(node.children[c],low,high,false,fn))return false;
    }
  }
  return true;
 }
 std::string new_manifest(uint64_t cursor,const std::string& catalog,
                          const std::map<std::string,uint64_t>& newroots,
                          uint64_t new_heap_end,uint64_t new_next_pid,uint64_t new_generation)const{
  std::string b;put32(b,MAN_MAGIC);put32(b,3);put64(b,cursor);
  put64(b,new_heap_end);put64(b,new_next_pid);put64(b,wal_generation);
  put64(b,new_generation);putstr(b,catalog);
  put32(b,uint32_t(newroots.size()));for(auto& [name,root]:newroots){putstr(b,name);put64(b,root);}
  put32(b,crc(b));return b;
 }
 void checkpoint_wal(uint64_t generation){
  check(generation>wal_generation,"STORE_CHECKPOINT_GEN_REGRESSION");
  auto previous=wal_generation;
  wal_generation=generation;
  const std::string raw=new_manifest(0,catalog_blob,roots,heap_end,next_pid,data_generation);
  const auto tmp=path/"manifest.mvdb.tmp",dst=path/"manifest.mvdb";
  int fd=open(tmp.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0644);
  check(fd>=0,"MANIFEST_TEMP_OPEN_FAILED");
  try{write_at(fd,0,raw);sync(fd);}catch(...){close(fd);wal_generation=previous;throw;}
  close(fd);
  check(rename(tmp.c_str(),dst.c_str())==0,"MANIFEST_RENAME_FAILED");
  check(fsync(dir_fd)==0,"MANIFEST_DIR_SYNC_FAILED");
  applied_wal=0;
  failpoint("after_wal_checkpoint_manifest");
 }
 // Copy only visible data into a new file generation, using a streaming bulk
 // loader. Tombstones, obsolete rows, and unreachable COW pages are dropped.
 // The old generation remains authoritative until its manifest is replaced.
 void compact(){
  check(!staging,"STORE_COMPACT_DURING_PUBLISH");
  check(data_generation<UINT64_MAX,"STORE_GENERATION_OVERFLOW");
  const uint64_t next_gen=data_generation+1;
  const auto new_heap=heap_path(next_gen),new_index=index_path(next_gen);
  int h=open(new_heap.c_str(),O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
  check(h>=0,"COMPACT_HEAP_OPEN_FAILED");
  int ix=open(new_index.c_str(),O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
  if(ix<0){close(h);throw std::runtime_error("COMPACT_INDEX_OPEN_FAILED");}
  uint64_t h_end=0,pid_next=1;
  std::map<std::string,uint64_t> compact_roots;
  try{
   auto append=[&](const Node& n)->uint64_t {
     uint64_t id=pid_next++;
     write_at(ix,id*PAGE,serialize(n));return id;
   };
   for(const auto& [table,root]:roots){
     std::vector<std::pair<int64_t,uint64_t>> level;
     Node leaf;
     auto flush_leaf=[&](){
       if(leaf.keys.empty())return;
       auto first=leaf.keys.front();auto id=append(leaf);
       level.emplace_back(first,id);leaf=Node{};
     };
     scan_node(root,[&](int64_t pk,uint64_t old_off,uint32_t len){
       std::string raw=read_row(Item{pk,old_off,len});
       check(raw.size()==len,"COMPACT_LENGTH_MISMATCH");
       std::string header;put32(header,len);put32(header,crc(raw));
       write_at(h,h_end,header);uint64_t off=h_end;
       h_end+=header.size();write_at(h,h_end,raw);h_end+=raw.size();
       leaf.keys.push_back(pk);leaf.vals.push_back(Item{pk,off,len});
       if(leaf.keys.size()==MAX_KEYS)flush_leaf();
     });
     flush_leaf();
     if(level.empty()){compact_roots[table]=0;continue;}
     // Separator key is the first key of the right child. The existing B+Tree
     // uses upper_bound() and accepts this exact key convention.
     while(level.size()>1){
       std::vector<std::pair<int64_t,uint64_t>> parents;
       for(size_t first=0;first<level.size();first+=MAX_KEYS+1){
         size_t end=std::min(level.size(),first+MAX_KEYS+1);
         Node parent;parent.leaf=false;
         for(size_t i=first;i<end;i++){
           parent.children.push_back(level[i].second);
           if(i>first)parent.keys.push_back(level[i].first);
         }
         parents.emplace_back(level[first].first,append(parent));
       }
       level.swap(parents);
     }
     compact_roots[table]=level[0].second;
   }
   sync(h);sync(ix);
   // Data filenames must survive a crash before the manifest can reference them.
   check(fsync(dir_fd)==0,"COMPACT_DIR_SYNC_FAILED");
   failpoint("compact_after_data_sync");
   std::string manifest=new_manifest(applied_wal,catalog_blob,compact_roots,h_end,pid_next,next_gen);
   auto tmp=path/"manifest.mvdb.tmp",dst=path/"manifest.mvdb";
   int fd=open(tmp.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0644);
   check(fd>=0,"COMPACT_MANIFEST_OPEN_FAILED");
   try{write_at(fd,0,manifest);sync(fd);}catch(...){close(fd);throw;}
   close(fd);
   check(rename(tmp.c_str(),dst.c_str())==0,"COMPACT_MANIFEST_RENAME_FAILED");
   check(fsync(dir_fd)==0,"COMPACT_MANIFEST_SYNC_FAILED");
   failpoint("compact_after_manifest");
   const auto old_heap=heap_path(data_generation),old_index=index_path(data_generation);
   int oldh=heap_fd,oldix=page_fd;
   heap_fd=h;page_fd=ix;h=-1;ix=-1;
   heap_end=h_end;next_pid=pid_next;roots=std::move(compact_roots);
   data_generation=next_gen;
   cache.clear();lru.clear();row_cache.clear();row_lru.clear();row_cache_bytes=0;
   close(oldh);close(oldix);
   // Unlink strictly after durable manifest switch; interrupted deletion is safe.
   std::error_code ec;
   fs::remove(old_heap,ec);ec.clear();fs::remove(old_index,ec);
   check(fsync(dir_fd)==0,"COMPACT_CLEANUP_SYNC_FAILED");
   failpoint("compact_after_cleanup");
  }catch(...){if(h>=0)close(h);if(ix>=0)close(ix);throw;}
 }
 void publish(uint64_t cursor,const std::string& cat,const std::vector<DiskChange>& changes){
  check(cursor>=applied_wal,"STORE_WAL_CURSOR_REGRESSION");
  auto next_roots=roots;
  check(!staging,"BTREE_NESTED_STAGE");
  staging=true;staging_base=next_pid;
  // Coalesce sequential writes without changing WAL/fsync/manifest ordering.
  const char* setting=std::getenv("MVDB_BATCH_WRITES");
  const bool batch=!setting || std::strcmp(setting,"0")!=0;
  constexpr size_t WRITE_BATCH_LIMIT=1024u*1024u;
  std::string heap_batch;
  uint64_t heap_batch_start=heap_end;
  if(batch)heap_batch.reserve(65536);
  auto flush_heap=[&](){
   if(!heap_batch.empty()){write_at(heap_fd,heap_batch_start,heap_batch);heap_batch.clear();}
  };
  for(const auto& x:changes){
   auto& root=next_roots[x.table];
   Item item{x.pk,0,0};
   if(x.row){
    if(batch && x.row->size()+8<=WRITE_BATCH_LIMIT){
     if(heap_batch.size()+x.row->size()+8>WRITE_BATCH_LIMIT)flush_heap();
     if(heap_batch.empty())heap_batch_start=heap_end;
     item.offset=heap_end;item.length=uint32_t(x.row->size());
     put32(heap_batch,item.length);put32(heap_batch,crc(*x.row));heap_batch+=*x.row;
     heap_end+=8+x.row->size();
    }else{
     flush_heap();auto [offset,length]=append_row(*x.row);item.offset=offset;item.length=length;
    }
   }
   root=put(root,item);
  }
  flush_heap();
  // Crash before index publication; the durable WAL will reconstruct this batch.
  failpoint("after_heap_before_index");
  // Compact the transient COW tree to live nodes only, in child-first order.
  // The manifest will point at remapped page ids after all writes are synced.
  std::map<uint64_t,uint64_t> rewritten;
  std::vector<uint64_t> order;
  std::function<void(uint64_t)> visit=[&](uint64_t pid){
    if(pid<staging_base || rewritten.count(pid))return;
    auto it=staged.find(pid);check(it!=staged.end(),"BTREE_MISSING_STAGED_PAGE");
    if(!it->second.leaf)for(uint64_t child:it->second.children)visit(child);
    rewritten[pid]=staging_base+order.size();
    order.push_back(pid);
  };
  for(const auto& [_,root]:next_roots)if(root)visit(root);
  std::string page_batch;
  uint64_t page_batch_start=staging_base*PAGE;
  if(batch)page_batch.reserve(65536);
  for(uint64_t old_id:order){
    auto n=staged.at(old_id);
    if(!n.leaf)for(auto& child:n.children){if(child>=staging_base)child=rewritten.at(child);}
    const uint64_t offset=rewritten.at(old_id)*PAGE;
    auto bytes=serialize(n);
    if(batch){
     if(page_batch.empty())page_batch_start=offset;
     page_batch+=bytes;
     if(page_batch.size()>=WRITE_BATCH_LIMIT){write_at(page_fd,page_batch_start,page_batch);page_batch.clear();}
    }else write_at(page_fd,offset,bytes);
  }
  if(!page_batch.empty())write_at(page_fd,page_batch_start,page_batch);
  for(auto& [_,root]:next_roots)if(root>=staging_base)root=rewritten.at(root);
  next_pid=staging_base+order.size();
  staging=false;staged.clear();cache.clear();lru.clear();
  // A recovery interruption here leaves unreachable heap bytes and pages only.
  sync(heap_fd);sync(page_fd);
  failpoint("after_index_before_manifest");
  const std::string raw=new_manifest(cursor,cat,next_roots,heap_end,next_pid,data_generation);
  auto tmp=path/"manifest.mvdb.tmp";auto dst=path/"manifest.mvdb";
  int fd=open(tmp.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0644);check(fd>=0,"MANIFEST_TEMP_OPEN_FAILED");
  try{write_at(fd,0,raw);sync(fd);}catch(...){close(fd);throw;}close(fd);
  check(rename(tmp.c_str(),dst.c_str())==0,"MANIFEST_RENAME_FAILED");
  check(fsync(dir_fd)==0,"MANIFEST_DIR_SYNC_FAILED");
  roots=std::move(next_roots);catalog_blob=cat;applied_wal=cursor;
  failpoint("after_manifest_published");
 }
};

HeapView::~HeapView() {
  if(bytes && size) ::munmap(const_cast<unsigned char*>(bytes),size);
}
DiskStore::DiskStore(const std::string& d):impl_(std::make_unique<Impl>(d)){}
DiskStore::~DiskStore()=default;
uint64_t DiskStore::wal_cursor()const noexcept{return impl_->applied_wal;}
uint64_t DiskStore::wal_generation()const noexcept{return impl_->wal_generation;}
uint64_t DiskStore::data_generation()const noexcept{return impl_->data_generation;}
void DiskStore::compact(){impl_->compact();}
void DiskStore::checkpoint_wal(uint64_t next_generation){impl_->checkpoint_wal(next_generation);}
const std::string& DiskStore::catalog()const noexcept{return impl_->catalog_blob;}
uint64_t DiskStore::page_count()const noexcept{return impl_->next_pid;}
uint64_t DiskStore::heap_bytes()const noexcept{return impl_->heap_end;}
std::optional<std::string> DiskStore::get(const std::string& t,int64_t pk)const{
 auto root=impl_->roots.find(t);if(root==impl_->roots.end())return std::nullopt;
 auto hit=impl_->find(root->second,pk);if(!hit||hit->length==0)return std::nullopt;
 return impl_->read_row(*hit);
}
std::optional<RowRef> DiskStore::get_ref(const std::string& t,int64_t pk)const{
 auto root=impl_->roots.find(t);if(root==impl_->roots.end())return std::nullopt;
 auto item=impl_->find(root->second,pk);if(!item||item->length==0)return std::nullopt;
 return RowRef{item->offset,item->length};
}
void DiskStore::scan(const std::string& t,const std::function<void(int64_t,uint64_t,uint32_t)>& fn)const{
 auto r=impl_->roots.find(t);if(r!=impl_->roots.end())impl_->scan_node(r->second,fn);
}
void DiskStore::scan_range(const std::string& table,int64_t low,int64_t high,bool descending,
             const std::function<bool(int64_t,uint64_t,uint32_t)>& visitor)const{
  if(high<low)return;
  auto r=impl_->roots.find(table);
  if(r!=impl_->roots.end())impl_->scan_node_range(r->second,low,high,descending,visitor);
}
std::string DiskStore::read_row(uint64_t offset,uint32_t length)const{
 return impl_->read_row(Item{0,offset,length});
}
std::shared_ptr<HeapView> DiskStore::map_heap_readonly()const {
  if(impl_->heap_end==0 || impl_->heap_end > std::numeric_limits<size_t>::max()) return nullptr;
  void* p=::mmap(nullptr,static_cast<size_t>(impl_->heap_end),PROT_READ,MAP_SHARED,impl_->heap_fd,0);
  if(p==MAP_FAILED)return nullptr; // Safe fallback to existing row-by-row scanner.
  auto view=std::make_shared<HeapView>();
  view->bytes=static_cast<const unsigned char*>(p);
  view->size=static_cast<size_t>(impl_->heap_end);
  return view;
}
void DiskStore::publish(uint64_t pos,const std::string& cat,const std::vector<DiskChange>& changes){impl_->publish(pos,cat,changes);}
} // namespace mvdb
