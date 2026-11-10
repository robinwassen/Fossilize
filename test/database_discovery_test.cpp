#include "fossilize_db.hpp"
#include "../cli/replay_selection.hpp"
#include "diagnostic_test.hpp"
#include <algorithm>
#include <memory>
#include <vector>
#include <cerrno>
#include <limits>
#include <atomic>
#include <set>
#include <tuple>
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
// Compare every scalar, including policy/filter bits, independently of serialization.
static auto input_facts(const DiscoveryInput &i) -> decltype(std::tie(i.path,i.index,i.required,i.filter,i.opened,i.state,i.reason,i.raw_error_known,i.raw_error,i.boundary_known,i.prefix_end,i.unread_offset,i.total_known,i.retained_count))
{ return std::tie(i.path,i.index,i.required,i.filter,i.opened,i.state,i.reason,i.raw_error_known,i.raw_error,i.boundary_known,i.prefix_end,i.unread_offset,i.total_known,i.retained_count); }
static DiscoveryInput expected_input(const char *path,unsigned index,bool required,bool opened,DiscoveryState state,DiscoveryReason reason,bool raw_known,int64_t raw,bool boundary,uint64_t end,bool total,uint64_t count)
{
 DiscoveryInput i;i.path=path;i.index=index;i.required=required;i.opened=opened;i.state=state;i.reason=reason;
 i.raw_error_known=raw_known;i.raw_error=raw;i.boundary_known=boundary;i.prefix_end=i.unread_offset=end;i.total_known=total;i.retained_count=count;return i;
}
static void exact_inputs(const std::vector<DiscoveryInput> &actual,const std::vector<DiscoveryInput> &expected,const std::string &id)
{
 c.require(actual.size()==expected.size(),id+"/count");
 for(size_t i=0;i<actual.size()&&i<expected.size();i++)c.require(input_facts(actual[i])==input_facts(expected[i]),id+"/row-"+std::to_string(i));
}
using ExcludedTuple=std::tuple<ResourceTag,Hash,DiscoveryExclusion>;
static void exact_excluded(const std::vector<DiscoveryExcludedKey> &actual,const std::set<ExcludedTuple> &expected,const std::string &id)
{
 std::set<ExcludedTuple> found;for(auto &e:actual)found.emplace(e.key.tag,e.key.hash,e.policy);
 c.require(found==expected&&found.size()==actual.size(),id);
}
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
// Only metadata is backed by bytes. Payload advancement is logical: no large
// file, allocation or payload read is involved, even at uint32_t size limits.
static struct LogicalText
{
 bool enabled = false, tell_unrepresentable = false;
 FILE *file = nullptr;
 std::vector<uint8_t> metadata;
 uint64_t position = 0, length = 0;
 unsigned reads = 0, fail_payload_seek = 0;
 int seek_errno = 0;
 std::vector<long> payload_seeks;
} logical_text;
extern "C" int __wrap_fseek(FILE *f,long n,int w)
{
 if(logical_text.enabled && (!logical_text.file || logical_text.file==f))
 {
  logical_text.file=f;
  if(w==SEEK_CUR)
  {
   logical_text.payload_seeks.push_back(n);
   if(logical_text.fail_payload_seek && !--logical_text.fail_payload_seek)
   {errno=logical_text.seek_errno;return 1;}
  }
  uint64_t base=w==SEEK_END?logical_text.length:w==SEEK_CUR?logical_text.position:0;
  if(n<0 || uint64_t(n)>UINT64_MAX-base){errno=0;return -1;}
  logical_text.position=base+uint64_t(n);
  return 0;
 }
 if(fail_seek&&!--fail_seek){errno=ESPIPE;return positive_seek_error?1:-1;}
 return __real_fseek(f,n,w);
}
extern "C" long __wrap_ftell(FILE *f)
{
 if(logical_text.enabled && logical_text.file==f)
 {
  if(logical_text.tell_unrepresentable || logical_text.position>uint64_t(std::numeric_limits<long>::max()))
  {errno=0;return -1;}
  return long(logical_text.position);
 }
 if(fail_tell&&!--fail_tell){errno=EIO;return -1;}
 return __real_ftell(f);
}
extern "C" size_t __wrap_fread(void *p,size_t n,size_t m,FILE *f)
{
 if(logical_text.enabled && logical_text.file==f)
 {
  logical_text.reads++;
  // Production scanning must only request the tiny magic/header regions.
  if(n!=1 || m>56 || logical_text.position>logical_text.metadata.size() ||
     m>logical_text.metadata.size()-size_t(logical_text.position))
  {errno=0;return 0;}
  memcpy(p,logical_text.metadata.data()+size_t(logical_text.position),m);
  logical_text.position+=m;
  errno=ERANGE; // A later failed seek must not inherit a previous operation's errno.
  return m;
 }
 if(fail_read&&!--fail_read){errno=injected_read_errno;return 0;}
 return __real_fread(p,n,m,f);
}
#endif
static void text_spans()
{
 // Keep a real small archive and payload readback as the healthy backend control.
 const char first[]="{}", second[]="hello";
 {DB writer(create_dumb_file_database("text.txt",DatabaseMode::OverWrite));
  c.require(writer->prepare()&&writer->write_entry(RESOURCE_COMPUTE_PIPELINE,0x20,first,2,0)&&
   writer->write_entry(RESOURCE_SHADER_MODULE,0x21,second,5,0),"D20/text-writer");}
 auto tiny=bytes("text.txt");
 c.require(tiny.size()==139,"D20/text-tiny-fixture");
 {DB db(create_database("text.txt",DatabaseMode::ReadOnly));c.require(db->prepare(),"D20/text-real-prepare");
  auto report=db->get_preparation_report();
  c.require(report.discovery_complete&&report.satisfies_required_scope()&&report.inputs[0].prefix_end==tiny.size()&&report.inputs[0].retained_count==2,"D20/text-real-complete");
  size_t n=0;c.require(db->read_entry(RESOURCE_SHADER_MODULE,0x21,&n,nullptr,0)&&n==5,"D20/text-real-query");
  char payload[5]={};c.require(db->read_entry(RESOURCE_SHADER_MODULE,0x21,&n,payload,0)&&memcmp(payload,second,5)==0,"D20/text-real-payload");}
#ifdef __linux__
 // The first entry ends at 76, the second metadata ends at 132. Only the
 // second advertised uncompressed size changes; the physical fixture stays tiny.
 for(uint32_t payload_size:{UINT32_MAX-2,UINT32_MAX-1,UINT32_MAX})
 for(unsigned mode=0;mode<5;mode++)
 {
  logical_text={};logical_text.metadata=tiny;
  for(unsigned byte=0;byte<4;byte++)logical_text.metadata[76+40+12+byte]=uint8_t(payload_size>>(8*byte));
  uint64_t span=uint64_t(payload_size)+2, end=132+span;
  logical_text.length=end+(mode==1?5:0);
  logical_text.fail_payload_seek=mode==2||mode==3?2:0;
  logical_text.seek_errno=mode==2?ESPIPE:0;
  logical_text.tell_unrepresentable=mode==4;
  logical_text.enabled=true;
  DB db(create_database("text.txt",DatabaseMode::ReadOnly));bool prepared=db->prepare();
  logical_text.enabled=false;
  auto report=db->get_preparation_report();auto &input=report.inputs[0];
  bool representable=logical_text.length<=uint64_t(std::numeric_limits<long>::max())&&
   logical_text.length<=uint64_t(std::numeric_limits<size_t>::max());
  if(!representable||mode==4)
  {
   c.require(!prepared&&input.state==DiscoveryState::Failed&&input.reason==DiscoveryReason::Tell&&
    !input.raw_error_known&&input.raw_error==0&&!input.total_known&&!report.unread_total_known&&
    input.retained_count==0&&report.discovered.empty()&&report.selected.empty()&&
    !report.satisfies_required_scope(true)&&logical_text.payload_seeks.empty(),"D20/text-unrepresentable-unknown");
   continue;
  }
  c.require(logical_text.reads==3&&logical_text.payload_seeks==std::vector<long>({4,long(span)}),"D20/text-exact-wide-seeks-bounded-reads");
  if(mode==2||mode==3)
  {
   c.require(!prepared&&input.state==DiscoveryState::Failed&&input.reason==DiscoveryReason::Seek&&
    input.raw_error_known==(mode==2)&&input.raw_error==(mode==2?ESPIPE:0)&&
    !input.total_known&&!report.discovery_complete&&!report.unread_total_known&&
    !report.satisfies_required_scope(true),"D20/text-seek-failed-unknown-honest-errno");
   c.require(input.boundary_known&&input.prefix_end==76&&input.unread_offset==76&&input.retained_count==1&&
    report.discovered==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x20}})&&report.selected==report.discovered,
    "D20/text-seek-failure-keeps-only-accepted-prefix");
  }
  else
  {
   c.require(prepared&&input.prefix_end==end&&input.unread_offset==end&&input.boundary_known&&input.retained_count==2&&
    report.discovered==std::vector<DiscoveryKey>({{RESOURCE_SHADER_MODULE,0x21},{RESOURCE_COMPUTE_PIPELINE,0x20}})&&
    report.selected==report.discovered&&logical_text.position==end,"D20/text-wide-prefix-exact-scope");
   c.require(input.state==(mode==1?DiscoveryState::RecoveredPrefix:DiscoveryState::Complete)&&
    input.reason==(mode==1?DiscoveryReason::TruncatedHeader:DiscoveryReason::None)&&
    input.total_known==(mode==0)&&report.discovery_complete==(mode==0)&&report.unread_total_known==(mode==0)&&
    report.satisfies_required_scope()==(mode==0)&&report.satisfies_required_scope(true)&&!input.raw_error_known,
    "D20/text-wide-complete-or-recovered-policy");
  }
 }
 logical_text={};
#endif
}
static void inputs()
{
 make("base.foz",2); make("extra.foz",1);
 {DB extra(create_stream_archive_database("extra.foz",DatabaseMode::Append));c.require(extra->prepare()&&extra->write_entry(RESOURCE_SHADER_MODULE,0x20,nullptr,0,0),"D20/same-hash-different-tag");} save("invalid.foz",{1,2,3}); remove("absent.foz");
 const char *paths[]={"extra.foz","absent.foz","invalid.foz","extra.foz"};
 const std::vector<DiscoveryKey> extras={{RESOURCE_SHADER_MODULE,0x20},{RESOURCE_COMPUTE_PIPELINE,0x20}};
 const std::vector<DiscoveryKey> with_base={{RESOURCE_SHADER_MODULE,0x20},{RESOURCE_COMPUTE_PIPELINE,0x20},{RESOURCE_COMPUTE_PIPELINE,0x21}};
 auto table=[&](bool primary,bool optional) {
  std::vector<DiscoveryInput> expected;
  if(primary)expected.push_back(expected_input("base.foz",0,true,true,DiscoveryState::Complete,DiscoveryReason::None,false,0,true,136,true,2));
  expected.push_back(expected_input("extra.foz",1,true,true,DiscoveryState::Complete,DiscoveryReason::None,false,0,true,132,true,2));
  expected.push_back(expected_input("absent.foz",2,!optional,false,DiscoveryState::Failed,DiscoveryReason::Open,true,ENOENT,false,0,false,0));
  expected.push_back(expected_input("invalid.foz",3,!optional,true,DiscoveryState::Failed,DiscoveryReason::InvalidMagic,false,0,true,0,false,0));
  expected.push_back(expected_input("extra.foz",4,!optional,true,DiscoveryState::Complete,DiscoveryReason::None,false,0,true,132,true,2));
  return expected;
 };
 DB defaults(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,4));c.require(defaults->prepare(),"D20/default-legacy-tolerant-bool");auto default_report=defaults->get_preparation_report();
 exact_inputs(default_report.inputs,table(false,false),"D20/default-complete-input-table");
 c.require(!default_report.satisfies_required_scope(),"D20/default-report-requires-requested-inputs");for(auto &input:default_report.inputs)c.require(input.required,"D20/default-report-explicit-required-policy");
 c.require(!default_report.satisfies_required_scope()&&default_report.discovered==extras&&default_report.selected==extras&&default_report.excluded.empty(),"D20/default-exact-logical-union-required-policy");
 DB db(create_concurrent_database("base",DatabaseMode::ReadOnly,paths,4));
 db->set_input_required(0,true); db->set_input_required(1,true); db->set_input_required(2,true);
 c.require(db->prepare(),"D20/legacy-composite-tolerant"); auto r=db->get_preparation_report();
 exact_inputs(r.inputs,table(true,false),"D20/primary-complete-input-table");
 c.require(r.inputs.size()==5&&r.discovered.size()==3&&r.selected.size()==3,"D20/input-observations-versus-logical-dedup");
 c.require(r.inputs[0].path=="base.foz"&&r.inputs[2].path=="absent.foz"&&r.inputs[2].index==2&&r.inputs[2].reason==DiscoveryReason::Open&&r.inputs[2].raw_error_known&&r.inputs[2].raw_error==ENOENT&&!r.inputs[2].opened,"D20/missing-origin-retained");
 c.require(r.inputs[3].opened&&r.inputs[3].reason==DiscoveryReason::InvalidMagic&&!r.inputs[3].raw_error_known&&!r.satisfies_required_scope(true),"D20/invalid-no-invented-errno");
 c.require(r.discovered==with_base&&r.selected==with_base&&r.excluded.empty()&&!r.satisfies_required_scope(true)&&!r.discovery_complete&&!r.unread_total_known,"D20/input-observations-versus-exact-logical-dedup");
 db.reset(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,4)); db->set_input_required(1,true);for(unsigned index:{2u,3u,4u})db->set_input_required(index,false);
 c.require(db->prepare(),"D20/no-primary-prepare"); r=db->get_preparation_report();
 exact_inputs(r.inputs,table(false,true),"D20/optional-complete-input-table");
 c.require(r.discovered==extras&&r.selected==extras&&r.excluded.empty()&&r.satisfies_required_scope()&&!r.discovery_complete&&!r.unread_total_known,"D20/optional-unknown-is-excluded-input");
#ifdef __linux__
 // The first real opened child fails after one accepted entry; ConcurrentDatabase
 // discards it. Both its observation and the healthy sibling must survive.
 {DB writer(create_stream_archive_database("io-child.foz",DatabaseMode::OverWrite));c.require(writer->prepare(),"D20/composite-io-writer");
  for(Hash h:{0x70u,0x71u,0x72u})c.require(writer->write_entry(RESOURCE_COMPUTE_PIPELINE,h,"data",4,0),"D20/composite-io-write");}
 const char *io_paths[]={"io-child.foz","base.foz"};
 for(unsigned mode=0;mode<4;mode++)for(bool required:{false,true})
 {
  DB composite(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,io_paths,2));composite->set_input_required(1,required);
  positive_seek_error=true;injected_read_errno=mode==2?0:EIO;
  fail_seek=mode==0?4:0;fail_read=(mode==1||mode==2)?3:0;
  bool prepared=composite->prepare();fail_seek=fail_read=0;positive_seek_error=false;injected_read_errno=EIO;
  auto report=composite->get_preparation_report();bool healthy=mode==3;
  c.require(prepared,"D20/composite-io-legacy-prepare");
  exact_inputs(report.inputs,{
   expected_input("io-child.foz",1,required,true,healthy?DiscoveryState::Complete:DiscoveryState::Failed,
    healthy?DiscoveryReason::None:mode==0?DiscoveryReason::Seek:DiscoveryReason::Read,
    !healthy&&mode!=2,healthy||mode==2?0:mode==0?ESPIPE:EIO,true,healthy?196:76,healthy,healthy?3:1),
   expected_input("base.foz",2,true,true,DiscoveryState::Complete,DiscoveryReason::None,false,0,true,136,true,2)},"D20/composite-io-exact-input-table");
  std::vector<DiscoveryKey> expected={{RESOURCE_COMPUTE_PIPELINE,0x20},{RESOURCE_COMPUTE_PIPELINE,0x21}};
  if(healthy)for(Hash h:{0x70u,0x71u,0x72u})expected.push_back({RESOURCE_COMPUTE_PIPELINE,h});
  c.require(report.discovered==expected&&report.selected==expected&&report.excluded.empty(),"D20/composite-io-exact-live-logical-union");
  c.require(bool(composite->get_sub_database(1))==healthy&&composite->get_sub_database(2)!=nullptr,"D20/composite-io-failed-child-actually-discarded");
  c.require(report.discovery_complete==healthy&&report.unread_total_known==healthy&&report.satisfies_required_scope()==(healthy||!required)&&report.satisfies_required_scope(true)==(healthy||!required),"D20/composite-io-required-optional-unknown-policy");
  unsigned starts[]={0,0,0},ends[]={~0u,~0u,~0u};auto scope=select_replay_scope(*composite,starts,ends,0,true);
  c.require(scope.scope_satisfied==(healthy||!required)&&scope.selected_total_known==(healthy||!required)&&scope.selected==expected&&scope.dispatch==expected&&scope.excluded.empty()&&scope.preparation.unread_total_known==healthy,"D20/composite-io-exact-selected-prefix-policy");
  exact_inputs(scope.optional_unavailable,healthy||required?std::vector<DiscoveryInput>{}:std::vector<DiscoveryInput>{report.inputs[0]},"D20/composite-io-named-optional-unknown-exclusion");
  read_prefix(*composite,2);
  c.require(!composite->has_entry(RESOURCE_COMPUTE_PIPELINE,0x70)||healthy,"D20/composite-io-discarded-prefix-is-not-live");
  exact_inputs(composite->get_preparation_report().inputs,report.inputs,"D20/composite-io-observations-stable-after-readback");
 }
#endif
 DB append(create_concurrent_database("absent-capture",DatabaseMode::Append,paths,4));
 c.require(append->prepare(),"D20/capture-append-tolerant");
}
static void errors()
{
 text_spans();
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
 c.require(r.discovered==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x20},{RESOURCE_COMPUTE_PIPELINE,0x21},{RESOURCE_COMPUTE_PIPELINE,0x22}})&&r.selected==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x20}})&&r.excluded.size()==2,"D20/filter-preserves-prefilter-universe");
 exact_excluded(r.excluded,{{RESOURCE_COMPUTE_PIPELINE,0x21,DiscoveryExclusion::Blacklist},{RESOURCE_COMPUTE_PIPELINE,0x22,DiscoveryExclusion::Whitelist}},"D20/filter-distinct-exact-policies");
 c.require(r.whitelist_sensitive&&r.blacklist_present&&!(r.whitelist_tag_mask&(1u<<RESOURCE_SHADER_MODULE)),"D20/filter-effective-mask-shader-empty-whitelist");
 {DB source(create_stream_archive_database("shader.foz",DatabaseMode::OverWrite));c.require(source->prepare()&&source->write_entry(RESOURCE_SHADER_MODULE,0x99,nullptr,0,0),"D20/shader-fixture");}
 auto shader=open("shader.foz");c.require(shader->load_whitelist_database("white.foz")&&shader->prepare()&&shader->get_preparation_report().selected==std::vector<DiscoveryKey>({{RESOURCE_SHADER_MODULE,0x99}}),"D20/shader-empty-whitelist-preserves-modules");
 auto w=bytes("white.foz");w.push_back(1);save("bad-white.foz",w);db=open("source.foz");c.require(db->load_whitelist_database("bad-white.foz")&&db->prepare()&&!db->get_preparation_report().satisfies_required_scope(true),"D20/filter-truncated-required");
 const char *paths[]={"source.foz","white.foz"}; DB composite(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,2)); composite->promote_sub_database_to_whitelist(2);
 c.require(composite->prepare(),"D20/implicit-prepare"); r=composite->get_preparation_report();c.require(r.discovered==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x20},{RESOURCE_COMPUTE_PIPELINE,0x21},{RESOURCE_COMPUTE_PIPELINE,0x22}})&&r.selected==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x20},{RESOURCE_COMPUTE_PIPELINE,0x21}}),"D20/implicit-whitelist-exact");exact_excluded(r.excluded,{{RESOURCE_COMPUTE_PIPELINE,0x22,DiscoveryExclusion::Whitelist}},"D20/implicit-whitelist-exact-excluded");
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
 for(unsigned mode=0;mode<7;mode++)
 {
  auto b=all;if(mode==1)b.resize(21);if(mode==2)b.resize(141);save("transfer.foz",b);
  const char *paths[]={"transfer.foz","absent.foz"}; remove("absent.foz");
  DB db(mode>=3?create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,2):create_stream_archive_database("transfer.foz",DatabaseMode::ReadOnly));
  db->set_input_required(mode>=3?1:0,true);if(mode>=3)db->set_input_required(2,mode==4);
  if(mode==5){make("transfer-white.foz",1);c.require(db->load_whitelist_database("transfer-white.foz"),"D20/metadata-white");}
  if(mode==6) {
   make("transfer-white.foz",2);
   {DB black(create_stream_archive_database("transfer-black.foz",DatabaseMode::OverWrite));c.require(black->prepare()&&black->write_entry(RESOURCE_COMPUTE_PIPELINE,0x21,nullptr,0,0),"D20/metadata-black-fixture");}
   c.require(db->load_whitelist_database("transfer-white.foz")&&db->load_blacklist_database("transfer-black.foz"),"D20/metadata-mixed-filters");
  }
  c.require(db->prepare(),"D20/metadata-source"); auto before=db->get_preparation_report(); char name[DatabaseInterface::OSHandleNameSize];DatabaseInterface::get_unique_os_export_name(name,sizeof(name));auto handle=db->export_metadata_to_os_handle(name);
  c.require(DatabaseInterface::metadata_handle_is_valid(handle),"D20/metadata-export");
  DB next(mode>=3?create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,2):create_stream_archive_database("transfer.foz",DatabaseMode::ReadOnly));
  c.require(next->import_metadata_from_os_handle(handle)&&next->prepare(),"D20/metadata-import");auto after=next->get_preparation_report();
  c.require(before.discovery_complete==after.discovery_complete&&before.unread_total_known==after.unread_total_known&&before.inputs.size()==after.inputs.size()&&before.discovered==after.discovered&&before.selected==after.selected&&before.satisfies_required_scope()==after.satisfies_required_scope()&&before.whitelist_tag_mask==after.whitelist_tag_mask&&before.whitelist_sensitive==after.whitelist_sensitive&&before.blacklist_present==after.blacklist_present,"D20/metadata-no-false-completeness");
  exact_inputs(after.inputs,before.inputs,"D20/metadata-exact-input-facts-including-filter");
  std::set<ExcludedTuple> expected_excluded;
  if(mode==5)expected_excluded={{RESOURCE_COMPUTE_PIPELINE,0x21,DiscoveryExclusion::Whitelist},{RESOURCE_COMPUTE_PIPELINE,0x22,DiscoveryExclusion::Whitelist}};
  if(mode==6)expected_excluded={{RESOURCE_COMPUTE_PIPELINE,0x21,DiscoveryExclusion::Blacklist},{RESOURCE_COMPUTE_PIPELINE,0x22,DiscoveryExclusion::Whitelist}};
  exact_excluded(before.excluded,expected_excluded,"D20/metadata-source-exact-exclusions-unique");
  exact_excluded(after.excluded,expected_excluded,"D20/metadata-exact-excluded-key-policy-roundtrip");
  std::vector<DiscoveryKey> expected_keys;for(unsigned i=0;i<(mode==1?0u:mode==2?2u:3u);i++)expected_keys.push_back({RESOURCE_COMPUTE_PIPELINE,0x20+i});
  c.require(before.discovered==expected_keys&&after.discovered==expected_keys&&before.selected==(mode>=5?std::vector<DiscoveryKey>{{RESOURCE_COMPUTE_PIPELINE,0x20}}:expected_keys),"D20/metadata-independent-exact-key-sets");
  if(mode==5){c.require(after.excluded.size()==2&&after.selected.size()==1&&after.discovered.size()==3,"D20/metadata-filter-exclusions");read_prefix(*next,1);}
  else if(mode==6) {
   const std::vector<DiscoveryKey> universe={{RESOURCE_COMPUTE_PIPELINE,0x20},{RESOURCE_COMPUTE_PIPELINE,0x21},{RESOURCE_COMPUTE_PIPELINE,0x22}};
   c.require(after.discovered==universe&&after.selected==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x20}})&&after.blacklist_present&&after.whitelist_sensitive,"D20/metadata-mixed-exact-universe-and-policy");
   exact_excluded(after.excluded,{{RESOURCE_COMPUTE_PIPELINE,0x21,DiscoveryExclusion::Blacklist},{RESOURCE_COMPUTE_PIPELINE,0x22,DiscoveryExclusion::Whitelist}},"D20/metadata-mixed-exact-exclusions");
   c.require(after.inputs.size()==4&&after.inputs[2].filter&&after.inputs[3].filter&&after.inputs[2].path=="transfer-white.foz"&&after.inputs[3].path=="transfer-black.foz","D20/metadata-two-real-named-filter-inputs");
   read_prefix(*next,1);
  }
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
