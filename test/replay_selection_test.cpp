#define FOSSILIZE_OUTCOME_NO_MAIN
#include "replay_outcome_test.cpp"
#ifdef __linux__
static unsigned optional_seek_failure;
extern "C" int __real_fseek(FILE *, long, int);
extern "C" int __wrap_fseek(FILE *file, long offset, int origin)
{
 if(optional_seek_failure && !--optional_seek_failure) { errno=ESPIPE;return 1; }
 return __real_fseek(file,offset,origin);
}
#endif

static void selection_input(const char *path)
{
 std::unique_ptr<DatabaseInterface> db(create_stream_archive_database(path,DatabaseMode::OverWrite));
 checks.require(db->prepare(),"D20/selection-writer");
 for (auto tag : replay_pipeline_tags) for (Hash hash : {0x30u,0x10u,0x20u})
  checks.require(db->write_entry(tag,hash,nullptr,0,0),"D20/selection-write");
}
static void ranges()
{
 selection_input("ranges.foz");std::unique_ptr<DatabaseInterface> db(create_stream_archive_database("ranges.foz",DatabaseMode::ReadOnly));db->set_input_required(0,true);checks.require(db->prepare(),"D20/ranges-prepare");
 struct Row {unsigned begin,end,actual_begin,actual_end;};
 for (auto row : {Row{0,~0u,0,3},Row{0,0,0,0},Row{0,3,0,3},Row{1,2,1,2},Row{2,9,2,3},Row{8,9,3,3},Row{2,1,1,1}})
 {
  unsigned starts[]={row.begin,row.begin,row.begin},ends[]={row.end,row.end,row.end};
  auto report=select_replay_scope(*db,starts,ends,0,false);
  checks.require(report.scope_satisfied&&report.preparation.discovered.size()==9,"D20/ranges-complete-universe");
  std::vector<DiscoveryKey> expected;
  for (unsigned i=0;i<3;i++)
  {
   auto &r=report.ranges[i];checks.require(r.requested_begin==row.begin&&r.requested_end==row.end&&r.effective_begin==row.actual_begin&&r.effective_end==row.actual_end,"D20/range-requested-effective");
   std::vector<Hash> hashes;
   for(unsigned j=row.actual_begin;j<row.actual_end;j++){hashes.push_back((j+1)*0x10);expected.push_back({replay_pipeline_tags[i],(j+1)*0x10});}
   checks.require(report.stable_hashes[i]==hashes,"D20/range-exact-stable-hashes");
  }
  checks.require(report.selected==expected&&report.dispatch==expected&&report.excluded.size()==9-expected.size(),"D20/range-exact-selection-and-exclusions");
  for(auto &e:report.excluded)checks.require(e.policy==ReplayExclusionPolicy::Range&&std::find(expected.begin(),expected.end(),e.key)==expected.end(),"D20/range-exclusions-are-policy");
 }
 unsigned starts[]={0,0,0},ends[]={~0u,~0u,~0u};
 auto hit=select_replay_scope(*db,starts,ends,0x20,false),miss=select_replay_scope(*db,starts,ends,0x99,false);
 checks.require(hit.scope_satisfied&&hit.selected.size()==3&&hit.excluded.size()==6&&miss.selected.empty()&&!miss.hash_found&&!miss.scope_satisfied,"D20/hash-exact-and-missing");
}
static void policy_compositions()
{
 checks.require(fixture("good","composition-complete.foz"),"D20/composition-fixture");
 auto content=cache_bytes("composition-complete.foz");
 {FILE *f=fopen("composition-prefix.foz","wb");checks.require(f!=nullptr,"D20/composition-prefix-open");if(f){fwrite(content.data(),1,content.size(),f);fputc('x',f);fclose(f);}}
 {FILE *f=fopen("composition-failed.foz","wb");checks.require(f!=nullptr,"D20/composition-failed-open");if(f){fputs("invalid",f);fclose(f);}}
 remove("composition-missing-filter.foz");
 const char *sources[]={"composition-complete.foz","composition-prefix.foz","composition-failed.foz"};
 unsigned starts[]={0,0,0},ends[]={~0u,~0u,~0u},rows=0;
 const std::vector<DiscoveryKey> pipelines={{RESOURCE_COMPUTE_PIPELINE,0x201},{RESOURCE_COMPUTE_PIPELINE,0x202}};
 for(unsigned state=0;state<3;state++)for(bool required:{false,true})for(bool filter_failed:{false,true})for(bool prefix:{false,true})
 {
  rows++;
  std::string row=std::to_string(state)+"/"+std::to_string(required)+"/"+std::to_string(filter_failed)+"/"+std::to_string(prefix);
  std::unique_ptr<DatabaseInterface> db(create_database(sources[state],DatabaseMode::ReadOnly));db->set_input_required(0,required);
  bool loaded=db->load_whitelist_database(filter_failed?"composition-missing-filter.foz":"composition-complete.foz");
  bool prepared=db->prepare();auto report=select_replay_scope(*db,starts,ends,0,prefix,nullptr,prepared);
  bool expected=!filter_failed&&(state==0||(state==1&&(!required||prefix))||(state==2&&!required));
  checks.require(loaded==!filter_failed&&prepared==(state!=2),"D20/composition-origin-bools/"+row);
  checks.require(report.scope_satisfied==expected&&report.selected_total_known==expected&&report.preparation_succeeded==prepared,"D20/composition-exact-policy/"+row);
  checks.require(report.preparation.discovery_complete==(state==0&&!filter_failed)&&report.preparation.unread_total_known==(state==0&&!filter_failed),"D20/composition-original-uncertainty/"+row);
  checks.require(report.preparation.inputs.size()==2,"D20/composition-exact-requested-inputs/"+row);
  if(report.preparation.inputs.size()==2)
  {
   const auto &source=report.preparation.inputs[0],&filter=report.preparation.inputs[1];
   checks.require(source.path==sources[state]&&source.required==required&&!source.filter&&source.state==(state==0?DiscoveryState::Complete:state==1?DiscoveryState::RecoveredPrefix:DiscoveryState::Failed),"D20/composition-source-facts/"+row);
   checks.require(filter.filter&&filter.required&&filter.state==(filter_failed?DiscoveryState::Failed:DiscoveryState::Complete),"D20/composition-required-filter-facts/"+row);
  }
  if(expected&&state!=2)checks.require(report.selected==pipelines&&report.dispatch==pipelines,"D20/composition-exact-known-selection/"+row);
  if(state==2)
  {
   checks.require(report.selected.empty()&&report.dispatch.empty()&&report.optional_inputs_excluded==!required,"D20/composition-failed-source-no-dispatch/"+row);
   if(!required)checks.require(report.optional_unavailable.size()==1&&report.optional_unavailable[0].path==sources[state]&&!report.optional_unavailable[0].filter,"D20/composition-filter-is-not-excluded-source/"+row);
  }
 }
 checks.require(rows==24,"D20/composition-all24-rows");
 printf("Executed D20 policy composition rows=%u\n",rows);
}

static void prefix_policy()
{
 checks.require(fixture("good","prefix.foz"),"D20/policy-recorder-fixture");
 {FILE *f=fopen("prefix.foz","ab");fputc(1,f);fclose(f);}
 unsigned starts[]={0,0,0},ends[]={0,0,0};
 std::unique_ptr<DatabaseInterface> db(create_stream_archive_database("prefix.foz",DatabaseMode::ReadOnly));db->set_input_required(0,true);checks.require(db->prepare(),"D20/policy-salvage");
 auto full=select_replay_scope(*db,starts,ends,0,false),prefix=select_replay_scope(*db,starts,ends,0,true);
 checks.require(!full.scope_satisfied&&full.selected.empty()&&!full.preparation.unread_total_known,"D20/zero-selection-cannot-certify-unknown");
 checks.require(prefix.scope_satisfied&&prefix.recovered_prefix_requested&&!prefix.preparation.discovery_complete&&!prefix.preparation.unread_total_known&&prefix.preparation.inputs[0].prefix_end>16,"D20/explicit-prefix-names-known-byte-scope");
 for (bool accept:{false,true})
 {
  auto opts=Replay::options(1,nullptr);opts.accept_recovered_prefix=accept; ThreadedReplayer r(device_options(),opts);
  marker_mode=true;results={VK_SUCCESS};calls=0;
  int status=run_normal_process(r,{"prefix.foz"},nullptr,0,-1);
  checks.require(status==(accept?0:1)&&r.selection_report.preparation_succeeded&&r.selection_report.scope_satisfied==accept,"D20/production-prefix-policy-status");
  if(accept)checks.require(r.selection_report.selected.size()==2&&r.selection_report.dispatch.size()==2&&calls==2,"D20/production-prefix-exact-dispatch");
  marker_mode=false;
 }
 const char *paths[]={"prefix.foz","absent.foz"};remove("absent.foz"); db.reset(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,2));configure_replay_inputs(*db,2,{});checks.require(db->prepare(),"D20/required-missing-tolerant-library");
 checks.require(!select_replay_scope(*db,starts,ends,0,true).scope_satisfied,"D20/prefix-cannot-accept-missing-required");
 db.reset(create_concurrent_database(nullptr,DatabaseMode::ReadOnly,paths,2));configure_replay_inputs(*db,2,{1});checks.require(db->prepare(),"D20/optional-prepare");auto optional=select_replay_scope(*db,starts,ends,0,true);
 checks.require(optional.scope_satisfied&&optional.optional_unavailable.size()==1&&optional.optional_unavailable[0].path=="absent.foz"&&!optional.preparation.unread_total_known,"D20/optional-input-exact-policy");
 for(const char *path:{"optional-missing.foz","optional-invalid.foz"})
 {
  remove(path);if(std::string(path)=="optional-invalid.foz"){FILE *f=fopen(path,"wb");fputs("invalid",f);fclose(f);}
  auto opts=Replay::options(1,nullptr);opts.optional_database_indices={0};ThreadedReplayer replay(device_options(),opts);calls=0;
  int status=run_normal_process(replay,{path},nullptr,0,-1);auto &report=replay.selection_report;
  checks.require(status==0&&calls==0&&report.scope_satisfied&&report.selected_total_known&&report.optional_inputs_excluded&&report.selected.empty()&&report.dispatch.empty()&&report.optional_unavailable.size()==1&&report.optional_unavailable[0].path==path&&!report.preparation.unread_total_known&&!report.preparation.discovery_complete&&replay.outcome.succeeded(),"D20/optional-only-unavailable-named-zero-selected-unknown-unread");
 }
#ifdef __linux__
 checks.require(fixture("good","optional-read-error.foz"),"D20/optional-partial-fixture");
 for(bool optional:{false,true})
 {
  auto opts=Replay::options(1,nullptr);if(optional)opts.optional_database_indices={0};ThreadedReplayer replay(device_options(),opts);calls=0;
  // End, rewind, first payload, then fail the second payload movement.
  optional_seek_failure=4;int status=run_normal_process(replay,{"optional-read-error.foz"},nullptr,0,-1);optional_seek_failure=0;auto &report=replay.selection_report;
  checks.require(status==(optional?0:1)&&calls==0&&report.selected.empty()&&report.dispatch.empty()&&!report.preparation.unread_total_known&&!report.preparation.discovery_complete,"D20/failed-prepare-never-dispatches-retained-keys");
  checks.require(!report.preparation_succeeded&&report.scope_satisfied==optional&&report.optional_inputs_excluded==optional&&report.selected_total_known==optional&&report.preparation.discovered.size()==1,"D20/optional-partial-exact-scope-policy");
  const auto &input=report.preparation.inputs[0];
  checks.require(input.state==DiscoveryState::Failed&&input.reason==DiscoveryReason::Seek&&input.raw_error_known&&input.raw_error==ESPIPE&&
   input.retained_count==1&&input.boundary_known&&input.prefix_end>16&&input.unread_offset==input.prefix_end&&!input.total_known,
   "D20/optional-partial-one-whole-entry-boundary");
  if(optional)checks.require(report.optional_unavailable.size()==1&&report.optional_unavailable[0].path=="optional-read-error.foz"&&report.excluded.size()==1&&report.excluded[0].key==report.preparation.discovered[0]&&report.excluded[0].policy==ReplayExclusionPolicy::OptionalInput&&replay.outcome.succeeded(),"D20/optional-partial-known-keys-excluded-not-succeeded-or-failed");
 }
#endif

 policy_compositions();
}
static void selection_filters()
{
 selection_input("filter.foz");
 {std::unique_ptr<DatabaseInterface> db(create_stream_archive_database("white.foz",DatabaseMode::OverWrite));checks.require(db->prepare(),"D20/selection-white");for(auto t:replay_pipeline_tags)for(Hash h:{0x20u,0x30u})db->write_entry(t,h,nullptr,0,0);}
 std::unique_ptr<DatabaseInterface> db(create_stream_archive_database("filter.foz",DatabaseMode::ReadOnly));db->set_input_required(0,true);checks.require(db->load_whitelist_database("white.foz")&&db->prepare(),"D20/selection-filter-prepare");
 unsigned starts[]={1,1,1},ends[]={2,2,2};auto r=select_replay_scope(*db,starts,ends,0,false);
 checks.require(r.preparation.discovered.size()==9&&r.preparation.selected.size()==6&&r.selected.size()==3&&r.excluded.size()==6,"D20/filter-before-range-exact-universe");
 for(unsigned i=0;i<3;i++)checks.require(r.stable_hashes[i]==std::vector<Hash>({0x30}),"D20/filter-before-range-exact-hashes");
 // Real production dispatch uses the same report and preserves excluded work as policy.
 checks.require(fixture("good","good.foz"),"D20/production-filter-fixture");seed_cache("one.foz",RESOURCE_COMPUTE_PIPELINE,0x202);
 auto opts=Replay::options(1,nullptr);ThreadedReplayer replay(device_options(),opts);marker_mode=true;results={VK_SUCCESS};calls=0;
 int status=run_normal_process(replay,{"good.foz"},"one.foz",ExternalReplayer::WHITELIST_MASK_ALL_BIT,-1);marker_mode=false;
 checks.require(status==0&&calls==1&&replay.selection_report.selected==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x202}})&&replay.selection_report.preparation.discovered.size()>replay.selection_report.preparation.selected.size(),"D20/production-filter-exact-dispatch-and-report");
}
static void hints()
{
 for(unsigned kind=0;kind<3;kind++)
 {
  std::string input="hint-"+std::to_string(kind)+".foz",cache="hints-"+std::to_string(kind),base=policy_base(cache);clear_cache(base);cache_input(input,kind);
  run_cache_archive(input,cache,kind,VK_SUCCESS,1);
  checks.require(harvest_cache(base,cache_tags[kind],cache_hash(kind))==1,"D20/hint-harvest-real-fragment");
  auto opts=Replay::options(1,nullptr);opts.replayer_cache_path=cache;
  ThreadedReplayer replay(device_options(),opts);marker_mode=true;results={VK_SUCCESS};calls=0;
  int status=run_normal_process(replay,{input.c_str()},nullptr,0,-1);marker_mode=false;auto &r=replay.selection_report;
  checks.require(status==0&&calls==0&&r.selected==std::vector<DiscoveryKey>({{cache_tags[kind],cache_hash(kind)}})&&r.dispatch.empty(),"D20/hints-exact-production-suppression");
  checks.require(r.stable_hashes[kind]==std::vector<Hash>({cache_hash(kind)})&&r.ranges[kind].effective_begin==0&&r.excluded.size()==1&&r.excluded[0].policy==ReplayExclusionPolicy::ReplayV1Hint&&r.excluded[0].key==r.selected[0],"D20/hints-preserve-stable-index-and-policy");
 }
 cache_legacy_provenance();cache_null_provenance();cache_whitelist();
}
static void healthy_selection()
{
 checks.require(fixture("good","healthy.foz"),"D20/healthy-fixture");
 auto opts=Replay::options(1,nullptr);ThreadedReplayer replay(device_options(),opts);marker_mode=true;results={VK_SUCCESS};calls=0;
 int status=run_normal_process(replay,{"healthy.foz"},nullptr,0,-1);marker_mode=false;
 checks.require(status==0&&calls==2&&replay.selection_report.scope_satisfied&&replay.selection_report.selected.size()==2&&replay.selection_report.dispatch.size()==2&&replay.selection_report.excluded.empty(),"D20/healthy-exact-production-work");
}

int main(int argc,char **argv)
{
 if(argc==4&&std::string(argv[1])=="--fixture")return fixture(argv[2],argv[3])?0:1;
 if(argc==4&&std::string(argv[1])=="--convert")
 {
  std::unique_ptr<DatabaseInterface> source(create_database(argv[2],DatabaseMode::ReadOnly));
  {std::unique_ptr<DatabaseInterface> dest(create_database(argv[3],DatabaseMode::OverWrite));
   checks.require(source->prepare()&&dest->prepare(),"D20/formats-open");
   for(unsigned t=0;t<RESOURCE_COUNT;t++)
   {size_t n=0;auto tag=ResourceTag(t);source->get_hash_list_for_resource_tag(tag,&n,nullptr);std::vector<Hash> hashes(n);source->get_hash_list_for_resource_tag(tag,&n,hashes.data());
    for(auto hash:hashes){size_t size=0;source->read_entry(tag,hash,&size,nullptr,0);std::vector<uint8_t> b(size);checks.require(source->read_entry(tag,hash,&size,b.data(),0)&&dest->write_entry(tag,hash,b.data(),size,0),"D20/formats-exact-copy");}}
  }
  std::unique_ptr<DatabaseInterface> readback(create_database(argv[3],DatabaseMode::ReadOnly));readback->set_input_required(0,true);
  checks.require(readback->prepare()&&readback->get_preparation_report().discovery_complete&&readback->get_preparation_report().satisfies_required_scope(),"D20/formats-complete-real-producer");
  for(unsigned t=0;t<RESOURCE_COUNT;t++){size_t n=0,m=0;source->get_hash_list_for_resource_tag(ResourceTag(t),&n,nullptr);readback->get_hash_list_for_resource_tag(ResourceTag(t),&m,nullptr);checks.require(n==m,"D20/formats-readback-count");}
  auto opts=Replay::options(1,nullptr);opts.optional_database_indices={0};ThreadedReplayer replay(device_options(),opts);marker_mode=true;results={VK_SUCCESS};calls=0;
  int status=run_normal_process(replay,{argv[3]},nullptr,0,-1);marker_mode=false;
  checks.require(status==0&&calls==2&&replay.selection_report.preparation_succeeded&&replay.selection_report.selected_total_known&&replay.selection_report.selected==std::vector<DiscoveryKey>({{RESOURCE_COMPUTE_PIPELINE,0x201},{RESOURCE_COMPUTE_PIPELINE,0x202}})&&replay.selection_report.dispatch==replay.selection_report.selected&&replay.selection_report.preparation.discovery_complete&&!replay.selection_report.optional_inputs_excluded&&replay.selection_report.optional_unavailable.empty(),"D20/optional-valid-backend-exact-production-dispatch");
  return checks.failures?1:0;
 }
 if(argc!=2)return 2;std::string mode=argv[1];
 if(mode=="healthy")healthy_selection();else if(mode=="ranges")ranges();else if(mode=="filters")selection_filters();else if(mode=="prefix-policy")prefix_policy();else if(mode=="hints")hints();else return 2;
 return checks.failures?1:0;
}
