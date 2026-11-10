#include "fossilize_db.hpp"
#include "diagnostic_test.hpp"
#include <algorithm>
#include <memory>
#include <vector>
#include <cerrno>
#include <limits>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#endif
using namespace Fossilize;
static DiagnosticChecks c;
#ifdef __linux__
static bool force_unknown_type;
extern "C" dirent *__real_readdir(DIR *);
extern "C" dirent *__wrap_readdir(DIR *dir)
{
 auto *entry=__real_readdir(dir);
 if(entry&&force_unknown_type)entry->d_type=DT_UNKNOWN;
 return entry;
}
#endif
using DB=std::unique_ptr<DatabaseInterface>;
static std::vector<uint8_t> bytes(const char *path)
{
 FILE *f=fopen(path,"rb"); c.require(f!=nullptr,"D20/file-open"); if(!f)return {};
 fseek(f,0,SEEK_END); auto n=ftell(f); rewind(f); std::vector<uint8_t> b(size_t(n),0);
 c.require(fread(b.data(),1,b.size(),f)==b.size(),"D20/file-read"); fclose(f); return b;
}
static void save(const char *path,const std::vector<uint8_t> &b)
{ FILE *f=fopen(path,"wb"); c.require(f!=nullptr,"D20/file-write-open"); if(f) { c.require(fwrite(b.data(),1,b.size(),f)==b.size(),"D20/file-write"); fclose(f); } }
static void make(const char *path,unsigned count=3)
{
 DB db(create_stream_archive_database(path,DatabaseMode::OverWrite)); c.require(db->prepare(),"D20/writer");
 for(unsigned i=0;i<count;i++) { uint8_t payload[]={uint8_t(i),0,0xff,uint8_t(42+i)};
  c.require(db->write_entry(RESOURCE_COMPUTE_PIPELINE,0x20+i,payload,sizeof(payload),PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT),"D20/write-checksum"); }
 db->flush();
}
static DB open(const char *path,bool required=true)
{ DB db(create_stream_archive_database(path,DatabaseMode::ReadOnly)); db->set_input_required(0,required); return db; }
static void read_prefix(DatabaseInterface &db,unsigned n)
{
 for(unsigned i=0;i<n;i++) { size_t size=0; c.require(db.read_entry(RESOURCE_COMPUTE_PIPELINE,0x20+i,&size,nullptr,0)&&size==4,"D20/prefix-query");
  std::vector<uint8_t>b(size); c.require(db.read_entry(RESOURCE_COMPUTE_PIPELINE,0x20+i,&size,b.data(),0)&&b==std::vector<uint8_t>({uint8_t(i),0,0xff,uint8_t(42+i)}),"D20/prefix-payload"); }
}
static void empty()
{
 make("empty.foz",0); c.require(bytes("empty.foz").size()==16,"D20/empty-valid-magic-size"); auto db=open("empty.foz");
 c.require(db->prepare(),"D20/empty-prepare"); auto r=db->get_preparation_report();
 c.require(r.discovery_complete&&r.unread_total_known&&r.inputs.size()==1&&r.inputs[0].total_known&&r.inputs[0].retained_count==0&&r.satisfies_required_scope(),"D20/empty-known-complete-zero");
 for(unsigned t=0;t<RESOURCE_COUNT;t++) {size_t n=99;c.require(db->get_hash_list_for_resource_tag(ResourceTag(t),&n,nullptr)&&n==0,"D20/empty-every-tag");}

#ifdef __linux__
 {DB writer(create_dumb_folder_database("unknown-directory",DatabaseMode::OverWrite));c.require(writer->prepare()&&writer->write_entry(RESOURCE_COMPUTE_PIPELINE,0x20,"{}",2,0),"D20/directory-writer");}
 DB directory(create_dumb_folder_database("unknown-directory",DatabaseMode::ReadOnly));directory->set_input_required(0,true);force_unknown_type=true;
 bool prepared=directory->prepare();force_unknown_type=false;auto report=directory->get_preparation_report();
 c.require(prepared&&report.discovery_complete&&report.satisfies_required_scope()&&report.selected==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x20}}),"D20/directory-unknown-type-resolved");
#endif
}
static void truncated(unsigned retained)
{
 make("full.foz"); auto all=bytes("full.foz");
 for(unsigned cut : {5u,58u})
 {
  auto b=all; b.resize(16+retained*60+cut); save("cut.foz",b); auto db=open("cut.foz");
  c.require(db->prepare(),"D20/legacy-salvage-true"); auto r=db->get_preparation_report();
  c.require(!r.discovery_complete&&!r.unread_total_known&&!r.satisfies_required_scope()&&r.satisfies_required_scope(true),"D20/truncation-never-full-coverage");
  auto &in=r.inputs[0];
  c.require(in.opened&&in.state==DiscoveryState::RecoveredPrefix&&!in.total_known&&in.retained_count==retained&&in.boundary_known&&in.prefix_end==16+retained*60&&in.unread_offset==in.prefix_end,"D20/truncation-boundary-unknown-tail");
  c.require(in.reason==(cut==5?DiscoveryReason::TruncatedHeader:DiscoveryReason::TruncatedPayload),"D20/truncation-origin-reason");
  c.require(r.discovered.size()==retained&&r.selected.size()==retained,"D20/prefix-exact-union");
  read_prefix(*db,retained); db->get_preparation_report(); read_prefix(*db,retained);
  c.require(bytes("cut.foz")==b,"D20/readonly-keeps-source");
 }
 auto db=open("full.foz"); c.require(db->prepare()&&db->get_preparation_report().satisfies_required_scope(),"D20/prefix-intact-control"); read_prefix(*db,3);
}
#ifdef __linux__
static unsigned fail_seek,fail_tell,fail_read;
static bool positive_seek_error;
static int injected_read_errno=EIO;
extern "C" int __real_fseek(FILE *,long,int);
extern "C" long __real_ftell(FILE *);
extern "C" size_t __real_fread(void *,size_t,size_t,FILE *);
extern "C" int __wrap_fseek(FILE *f,long n,int w) { if(fail_seek&&!--fail_seek){errno=ESPIPE;return positive_seek_error?1:-1;} return __real_fseek(f,n,w); }
extern "C" long __wrap_ftell(FILE *f) { if(fail_tell&&!--fail_tell){errno=EIO;return -1;} return __real_ftell(f); }
extern "C" size_t __wrap_fread(void *p,size_t n,size_t m,FILE *f) { if(fail_read&&!--fail_read){errno=injected_read_errno;return 0;} return __real_fread(p,n,m,f); }
#endif
static void inputs()
{
 make("base.foz",2); make("extra.foz",1);
 {DB extra(create_stream_archive_database("extra.foz",DatabaseMode::Append));c.require(extra->prepare()&&extra->write_entry(RESOURCE_SHADER_MODULE,0x20,nullptr,0,0),"D20/same-hash-different-tag");} save("invalid.foz",{1,2,3}); remove("absent.foz");
 const char *paths[]={"extra.foz","absent.foz","invalid.foz","extra.foz"};
 DB defaults(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,4));c.require(defaults->prepare(),"D20/default-legacy-tolerant-bool");auto default_report=defaults->get_preparation_report();
 c.require(!default_report.satisfies_required_scope(),"D20/default-report-requires-requested-inputs");for(auto &input:default_report.inputs)c.require(input.required,"D20/default-report-explicit-required-policy");
 DB db(create_concurrent_database("base",DatabaseMode::ReadOnly,paths,4));
 db->set_input_required(0,true); db->set_input_required(1,true); db->set_input_required(2,true);
 c.require(db->prepare(),"D20/legacy-composite-tolerant"); auto r=db->get_preparation_report();
 c.require(r.inputs.size()==5&&r.discovered.size()==3&&r.selected.size()==3,"D20/input-observations-versus-logical-dedup");
 c.require(r.inputs[0].path=="base.foz"&&r.inputs[2].path=="absent.foz"&&r.inputs[2].index==2&&r.inputs[2].reason==DiscoveryReason::Open&&r.inputs[2].raw_error_known&&r.inputs[2].raw_error==ENOENT&&!r.inputs[2].opened,"D20/missing-origin-retained");
 c.require(r.inputs[3].opened&&r.inputs[3].reason==DiscoveryReason::InvalidMagic&&!r.inputs[3].raw_error_known&&!r.satisfies_required_scope(true),"D20/invalid-no-invented-errno");
 db.reset(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,4)); db->set_input_required(1,true);for(unsigned index:{2u,3u,4u})db->set_input_required(index,false);
 c.require(db->prepare(),"D20/no-primary-prepare"); r=db->get_preparation_report();
 c.require(r.inputs.size()==4&&r.inputs[0].index==1&&r.satisfies_required_scope()&&!r.discovery_complete&&!r.unread_total_known,"D20/optional-unknown-is-excluded-input");
 DB append(create_concurrent_database("absent-capture",DatabaseMode::Append,paths,4));
 c.require(append->prepare(),"D20/capture-append-tolerant");
}
static void errors()
{
 make("valid.foz"); auto b=bytes("valid.foz");
 for(unsigned mode=0;mode<3;mode++) { auto x=b; if(mode==0)x.resize(8); if(mode==1)x[0]^=0xff; if(mode==2)x[15]=255; save("bad.foz",x);auto db=open("bad.foz");
  c.require(!db->prepare(),"D20/invalid-fails");auto r=db->get_preparation_report();c.require(!r.discovery_complete&&!r.unread_total_known&&!r.satisfies_required_scope(true)&&r.inputs[0].state==DiscoveryState::Failed,"D20/invalid-is-not-empty"); }
#ifdef __linux__
 for(unsigned mode=0;mode<5;mode++) {auto db=open("valid.foz");positive_seek_error=mode==3;injected_read_errno=mode==4?0:EIO;fail_seek=mode==0?1:mode==3?3:0;fail_tell=mode==1?1:0;fail_read=(mode==2||mode==4)?2:0;
  c.require(!db->prepare(),"D20/io-adapter-fails"); auto in=db->get_preparation_report().inputs[0];
  c.require(in.state==DiscoveryState::Failed&&in.raw_error_known==(mode!=4)&&in.raw_error==(mode==4?0:(mode==0||mode==3)?ESPIPE:EIO)&&in.reason==((mode==0||mode==3)?DiscoveryReason::Seek:mode==1?DiscoveryReason::Tell:DiscoveryReason::Read)&&!in.total_known,"D20/io-origin-exact"); }
#endif
 auto db=open("valid.foz"); c.require(db->prepare()&&db->get_preparation_report().discovery_complete,"D20/error-independent-valid-control");
 // An uninstrumented custom backend must never acquire discovery facts.
 struct Custom : DatabaseInterface { Custom():DatabaseInterface(DatabaseMode::ReadOnly){} bool prepare()override{return true;} bool read_entry(ResourceTag,Hash,size_t*,void*,PayloadReadFlags)override{return false;} bool write_entry(ResourceTag,Hash,const void*,size_t,PayloadWriteFlags)override{return false;} bool has_entry(ResourceTag,Hash)override{return false;} bool get_hash_list_for_resource_tag(ResourceTag,size_t*n,Hash*)override{*n=0;return true;} void flush()override{} const char *get_db_path_for_hash(ResourceTag,Hash)override{return nullptr;} } custom;
 c.require(custom.prepare()&&!custom.get_preparation_report().discovery_complete&&!custom.get_preparation_report().satisfies_required_scope(),"D20/custom-unknown");
 DatabaseInterface::request_shutdown(); db=open("valid.foz");c.require(!db->prepare()&&db->get_preparation_report().inputs[0].reason==DiscoveryReason::Cancelled,"D20/cancelled-unknown");
}
static void filters()
{
 make("source.foz"); make("white.foz",2);
 {DB black(create_stream_archive_database("black.foz",DatabaseMode::OverWrite)); c.require(black->prepare()&&black->write_entry(RESOURCE_COMPUTE_PIPELINE,0x21,nullptr,0,0),"D20/black-fixture");}
 auto db=open("source.foz");c.require(db->load_whitelist_database("white.foz")&&db->load_blacklist_database("black.foz")&&db->prepare(),"D20/filter-load");auto r=db->get_preparation_report();
 c.require(r.discovered.size()==3&&r.selected==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x20}})&&r.excluded.size()==2,"D20/filter-preserves-prefilter-universe");
 c.require(r.excluded[0].key.hash==0x21&&r.excluded[0].policy==DiscoveryExclusion::Blacklist&&r.excluded[1].key.hash==0x22&&r.excluded[1].policy==DiscoveryExclusion::Whitelist,"D20/filter-distinct-policies");
 c.require(r.whitelist_sensitive&&r.blacklist_present&&!(r.whitelist_tag_mask&(1u<<RESOURCE_SHADER_MODULE)),"D20/filter-effective-mask-shader-empty-whitelist");
 {DB source(create_stream_archive_database("shader.foz",DatabaseMode::OverWrite));c.require(source->prepare()&&source->write_entry(RESOURCE_SHADER_MODULE,0x99,nullptr,0,0),"D20/shader-fixture");}
 auto shader=open("shader.foz");c.require(shader->load_whitelist_database("white.foz")&&shader->prepare()&&shader->get_preparation_report().selected==std::vector<DiscoveryKey>({{RESOURCE_SHADER_MODULE,0x99}}),"D20/shader-empty-whitelist-preserves-modules");
 auto w=bytes("white.foz");w.push_back(1);save("bad-white.foz",w);db=open("source.foz");c.require(db->load_whitelist_database("bad-white.foz")&&db->prepare()&&!db->get_preparation_report().satisfies_required_scope(true),"D20/filter-truncated-required");
 const char *paths[]={"source.foz","white.foz"}; DB composite(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,2)); composite->promote_sub_database_to_whitelist(2);
 c.require(composite->prepare(),"D20/implicit-prepare"); r=composite->get_preparation_report();c.require(r.discovered.size()==3&&r.selected.size()==2&&r.selected[0].hash==0x20&&r.selected[1].hash==0x21,"D20/implicit-whitelist-exact");
 remove("absent-filter.foz");
 for(bool blacklist:{false,true})
 {
  auto source=open("source.foz");
  bool loaded=blacklist?source->load_blacklist_database("absent-filter.foz"):source->load_whitelist_database("absent-filter.foz");
  c.require(!loaded&&source->prepare(),"D20/failed-filter-bool-and-source-salvage");
  auto report=source->get_preparation_report();
  c.require(report.inputs.size()==2&&!report.discovery_complete&&!report.unread_total_known&&!report.satisfies_required_scope()&&!report.satisfies_required_scope(true),"D20/failed-filter-retains-incomplete-requested-scope");
  if(report.inputs.size()==2)
  {
   auto &input=report.inputs[1];
   c.require(input.path=="absent-filter.foz"&&input.filter&&input.required&&!input.opened&&input.state==DiscoveryState::Failed&&input.reason==DiscoveryReason::Open&&input.raw_error_known&&input.raw_error==ENOENT,"D20/failed-filter-exact-origin-after-reset");
  }
 }

}
static void metadata()
{
 make("full.foz");auto all=bytes("full.foz");
 for(unsigned mode=0;mode<6;mode++)
 {
  auto b=all;if(mode==1)b.resize(21);if(mode==2)b.resize(141);save("transfer.foz",b);
  const char *paths[]={"transfer.foz","absent.foz"}; remove("absent.foz");
  DB db(mode>=3?create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,2):create_stream_archive_database("transfer.foz",DatabaseMode::ReadOnly));
  db->set_input_required(mode>=3?1:0,true);if(mode>=3)db->set_input_required(2,mode==4);
  if(mode==5) {make("transfer-white.foz",1);c.require(db->load_whitelist_database("transfer-white.foz"),"D20/metadata-white");}
  c.require(db->prepare(),"D20/metadata-source"); auto before=db->get_preparation_report(); char name[DatabaseInterface::OSHandleNameSize];DatabaseInterface::get_unique_os_export_name(name,sizeof(name));auto handle=db->export_metadata_to_os_handle(name);
  c.require(DatabaseInterface::metadata_handle_is_valid(handle),"D20/metadata-export");
  DB next(mode>=3?create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,2):create_stream_archive_database("transfer.foz",DatabaseMode::ReadOnly));
  c.require(next->import_metadata_from_os_handle(handle)&&next->prepare(),"D20/metadata-import");auto after=next->get_preparation_report();
  c.require(before.discovery_complete==after.discovery_complete&&before.unread_total_known==after.unread_total_known&&before.inputs.size()==after.inputs.size()&&before.discovered==after.discovered&&before.selected==after.selected&&before.satisfies_required_scope()==after.satisfies_required_scope()&&before.whitelist_tag_mask==after.whitelist_tag_mask&&before.whitelist_sensitive==after.whitelist_sensitive&&before.blacklist_present==after.blacklist_present,"D20/metadata-no-false-completeness");
  for(size_t i=0;i<before.inputs.size()&&i<after.inputs.size();i++) { auto &a=before.inputs[i],&z=after.inputs[i];c.require(a.path==z.path&&a.index==z.index&&a.required==z.required&&a.opened==z.opened&&a.state==z.state&&a.reason==z.reason&&a.raw_error_known==z.raw_error_known&&a.raw_error==z.raw_error&&a.boundary_known==z.boundary_known&&a.prefix_end==z.prefix_end&&a.unread_offset==z.unread_offset&&a.total_known==z.total_known&&a.retained_count==z.retained_count,"D20/metadata-exact-input-facts"); }
  if(mode==5) {c.require(after.excluded.size()==2&&after.selected.size()==1&&after.discovered.size()==3,"D20/metadata-filter-exclusions");read_prefix(*next,1);}
  else read_prefix(*next,mode==1?0:mode==2?2:3);
  for(auto key:after.selected)c.require(next->has_entry(key.tag,key.hash),"D20/metadata-selected-membership");
 }
 // Corrupt actual exported mappings: impossible list counts, header size and schema.
 for(unsigned corruption=0;corruption<4;corruption++)
 {
  auto db=open("full.foz");c.require(db->prepare(),"D20/malformed-source");
  char name[DatabaseInterface::OSHandleNameSize];DatabaseInterface::get_unique_os_export_name(name,sizeof(name));auto handle=db->export_metadata_to_os_handle(name);
  void *mapped=nullptr;size_t length=0;
#ifdef _WIN32
  mapped=MapViewOfFile(reinterpret_cast<HANDLE>(handle),FILE_MAP_WRITE,0,0,0);
  MEMORY_BASIC_INFORMATION info={};if(mapped&&VirtualQuery(mapped,&info,sizeof(info)))length=info.RegionSize;
#else
  struct stat statbuf={};if(fstat(int(handle),&statbuf)==0)length=size_t(statbuf.st_size);
  mapped=mmap(nullptr,length,PROT_READ|PROT_WRITE,MAP_SHARED,int(handle),0);if(mapped==MAP_FAILED)mapped=nullptr;
#endif
  c.require(mapped&&length>=16,"D20/malformed-map");
  if(mapped)
  {
   auto *words=static_cast<uint64_t*>(mapped);
   if(corruption==0)words[1]=0;
   if(corruption==1)words[3]=UINT64_MAX;
   if(corruption>=2)
   {
    size_t header_size=16+RESOURCE_COUNT*16;
    size_t report_offset=size_t(words[1]);
    c.require(report_offset<=length&&length-report_offset>=header_size+40,"D20/malformed-report-offset");
    auto *payload=reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(mapped)+report_offset+header_size);
    payload[corruption==2?0:4]=UINT64_MAX;
   }
#ifdef _WIN32
   UnmapViewOfFile(mapped);
#else
   munmap(mapped,length);
#endif
  }
  auto rejected=open("full.foz");c.require(!rejected->import_metadata_from_os_handle(handle),"D20/malformed-metadata-rejected");
#ifdef _WIN32
  CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
  close(int(handle));
#endif
 }
}
int main(int argc,char **argv)
{
 if(argc!=2)return 2;std::string mode=argv[1];
 if(mode=="empty")empty();else if(mode=="truncated-zero")truncated(0);else if(mode=="prefix")truncated(2);else if(mode=="inputs")inputs();else if(mode=="errors")errors();else if(mode=="filters")filters();else if(mode=="metadata")metadata();else return 2;
 return c.failures?1:0;
}
