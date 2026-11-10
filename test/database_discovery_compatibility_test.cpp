// These declarations and overrides predate issue #20; compile actual input headers first.
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "diagnostic_test.hpp"
#include <memory>
using namespace Fossilize;
struct OldCustom : DatabaseInterface
{
 OldCustom():DatabaseInterface(DatabaseMode::ReadOnly){}
 bool prepare() override {return true;}
 bool read_entry(ResourceTag,Hash,size_t*,void*,PayloadReadFlags) override {return false;}
 bool write_entry(ResourceTag,Hash,const void*,size_t,PayloadWriteFlags) override {return false;}
 bool has_entry(ResourceTag,Hash) override {return false;}
 bool get_hash_list_for_resource_tag(ResourceTag,size_t *n,Hash*) override {*n=0;return true;}
 void flush() override {}
 const char *get_db_path_for_hash(ResourceTag,Hash) override {return nullptr;}
};
int main()
{
 DiagnosticChecks c;OldCustom custom;c.require(custom.prepare(),"D20/old-custom-no-new-override");
 {
  std::unique_ptr<DatabaseInterface> db(create_stream_archive_database("old.foz",DatabaseMode::OverWrite));c.require(db->prepare(),"D20/old-writer");
  StateRecorder recorder;recorder.init_recording_synchronized(db.get());VkPipelineLayoutCreateInfo layout={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  c.require(recorder.record_pipeline_layout((VkPipelineLayout)1,layout,0x123),"D20/old-recorder");recorder.tear_down_recording_thread();
 }
 std::unique_ptr<DatabaseInterface> db(create_stream_archive_database("old.foz",DatabaseMode::ReadOnly));size_t n=0;
 c.require(db->prepare()&&db->get_hash_list_for_resource_tag(RESOURCE_PIPELINE_LAYOUT,&n,nullptr)&&n==1,"D20/old-readback-count");Hash hash=0;c.require(db->get_hash_list_for_resource_tag(RESOURCE_PIPELINE_LAYOUT,&n,&hash)&&hash==0x123,"D20/old-readback-identity");size_t size=0;c.require(db->read_entry(RESOURCE_PIPELINE_LAYOUT,hash,&size,nullptr,0)&&size>0,"D20/old-readback-payload-size");
 return c.failures?1:0;
}
