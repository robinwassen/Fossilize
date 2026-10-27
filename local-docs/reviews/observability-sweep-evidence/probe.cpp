#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "fossilize_errors.hpp"
#include "cli/file.hpp"
#include <cstdio>
#include <memory>
using namespace Fossilize;
struct RejectDatabase : DatabaseInterface {
 bool prepare_ok; unsigned writes=0;
 explicit RejectDatabase(bool p):DatabaseInterface(DatabaseMode::OverWrite),prepare_ok(p){}
 bool prepare() override {return prepare_ok;}
 bool read_entry(ResourceTag,Hash,size_t*,void*,PayloadReadFlags) override {return false;}
 bool write_entry(ResourceTag,Hash,const void*,size_t,PayloadWriteFlags) override {writes++;return false;}
 bool has_entry(ResourceTag,Hash) override {return false;}
 bool get_hash_list_for_resource_tag(ResourceTag,size_t*n,Hash*) override {*n=0;return true;}
 void flush() override {}
 const char *get_db_path_for_hash(ResourceTag,Hash) override {return "reject";}
};
static unsigned diagnostics=0;
static void sink(LogLevel,const char*,void*) {diagnostics++;}
int main(int argc,char**argv) {
 set_thread_log_callback(sink,nullptr);
 for(bool prepare_ok : {false,true}) {
  RejectDatabase db(prepare_ok); diagnostics=0;
  StateRecorder recorder; recorder.init_recording_thread(&db);
  VkSamplerCreateInfo info={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  bool accepted=recorder.record_sampler((VkSampler)1,info);
  recorder.tear_down_recording_thread();
  printf("prepare=%d accepted=%d rejected_writes=%u diagnostics=%u\n",prepare_ok,accepted,db.writes,diagnostics);
 }
 printf("write_string_to_dev_full=%d\n",write_string_to_file("/dev/full","hello"));
 printf("write_buffer_to_dev_full=%d\n",write_buffer_to_file("/dev/full","hello",5));
 if(argc>1) {
  auto db=std::unique_ptr<DatabaseInterface>(create_stream_archive_database(argv[1],DatabaseMode::OverWrite));
  bool p=db->prepare(); const char payload[]="{}";
  bool w=db->write_entry(RESOURCE_COMPUTE_PIPELINE,1,payload,2,0);
  db->flush(); printf("fixture prepare=%d write=%d\n",p,w);
 }
}
