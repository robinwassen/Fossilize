#define FOSSILIZE_REPLAY_OUTCOME_NO_MAIN
#include "replay_outcome_test.cpp"
#undef main
using namespace Fossilize::ReplayEvent;
static void connect(Replay &r, State &s, Stream &p)
{
 s.begin(0,1);char b[MaxFrame];frame(b,sizeof(b),"1 HELLO 0 1");p.feed(s,b,strlen(b),[](const char*){});
 r.event_attempt=1;r.event_writer=[&s,&p](const char *b){p.feed(s,b,strlen(b),[](const char*){});return true;};
}
static void event_parse()
{
 const std::vector<std::pair<std::string,uint64_t>> cases={{"{",InvalidJSON},{"{\"version\":999}",IncompatibleVersion},{"{}",MalformedEnvelope},{"{\"version\":6,\"shaderModules\":{\"101\":{\"flags\":0,\"codeSize\":3,\"code\":\"AAAA\"}}}",InvalidShader}};
 for(auto &c:cases){Replay r;State s;Stream p;connect(r,s,p);BlobDatabase db;r.global_database=&db;db.blobs[{RESOURCE_SHADER_MODULE,0x101}]=c.first;StateReplayer parser;std::vector<uint8_t>buffer;
 set_thread_log_level(LOG_NONE);
 checks.require(!r.run_parse_work_item(parser,buffer,parse_item(0x101)),"M18/actual-parse-fails");checks.require(s.count==1,"M18/one-origin-event");const auto &e=s.events[0];
 checks.require(e.phase==Parse&&e.reason==c.second&&e.tag==RESOURCE_SHADER_MODULE&&e.hash==0x101&&e.worker==0&&e.attempt==1&&(e.known&(HasTag|HasHash|Worker|Attempt))==(HasTag|HasHash|Worker|Attempt),"M18/parse-origin-fields");
 checks.require(c.second==InvalidJSON?((e.known&(Raw|Offset))==(Raw|Offset)&&e.domain==RapidJSON&&e.raw==rapidjson::kParseErrorObjectMissName&&e.offset==1):(!(e.known&(Raw|Offset))&&e.domain==0),"M18/parse-raw-known-only-at-origin");
 db.blobs[{RESOURCE_SHADER_MODULE,0x101}]=shader_json();checks.require(r.run_parse_work_item(parser,buffer,parse_item(0x101)),"M18/valid-after-rejection");checks.require(s.count==1,"M18/valid-does-not-repeat-stale-error");
 ParseCapture capture;auto good=shader_json();checks.require(parser.parse(r,nullptr,good.data(),good.size())&&!capture.result.reason,"M18/capture-reset-after-failure"); }
 // TLS capture isolation with actual parser calls and no shared StateReplayer.
 std::atomic<unsigned> correct{0};std::vector<std::unique_ptr<Replay>> owners;
 for(unsigned i=0;i<8;i++)owners.emplace_back(new Replay);
 std::vector<std::thread> threads;for(unsigned i=0;i<8;i++)threads.emplace_back([&,i](){set_thread_log_level(LOG_NONE);StateReplayer parser;ParseCapture scope;const char *b="{";if(!parser.parse(*owners[i],nullptr,b,1)&&scope.result.reason==InvalidJSON)correct++;});for(auto &t:threads)t.join();checks.require(correct==8,"M18/concurrent-parse-capture-isolated");
 struct Recursive : Replay
 {
  unsigned nested=0;
  bool enqueue_create_shader_module(Fossilize::Hash h,const VkShaderModuleCreateInfo *i,VkShaderModule *m) override
  {
   StateReplayer inner;ParseCapture scope;
   checks.require(!inner.parse(*this,nullptr,"{",1)&&scope.result.reason==InvalidJSON,"M18/recursive-reentrant-origin");nested++;
   return Replay::enqueue_create_shader_module(h,i,m);
  }
 } recursive;
 recursive.get_per_thread_data().expected_tag=RESOURCE_SHADER_MODULE;recursive.get_per_thread_data().expected_hash=0x101;
 StateReplayer outer;auto good=shader_json();ParseCapture capture;
 checks.require(outer.parse(recursive,nullptr,good.data(),good.size())&&recursive.nested==1&&!capture.result.reason,"M18/successful-outer-clears-reentrant-failure");
}
static void event_creation()
{
 for(unsigned k=0;k<3;k++)for(unsigned scenario=0;scenario<3;scenario++){Replay r;State s;Stream p;connect(r,s,p);Pipeline item(k);results=scenario==0?std::vector<VkResult>{VK_ERROR_DEVICE_LOST}:scenario==1?std::vector<VkResult>{VK_SUCCESS}:std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED,VK_SUCCESS};if(scenario==2)item.flags(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);item.run(r,k);
 checks.require(s.count==(scenario==0?1u:0u),"M18/create-origin-before-final-success-has-no-failure");if(!scenario){const auto&e=s.events[0];checks.require(e.phase==Create&&e.reason==CreationFailed&&e.domain==Vulkan&&e.raw==VK_ERROR_DEVICE_LOST&&e.tag==item.item.tag&&e.hash==item.item.hash&&e.worker==0&&e.attempt==1&&(e.known&(Raw|HasTag|HasHash|Worker|Attempt))==(Raw|HasTag|HasHash|Worker|Attempt),"M18/create-exact-signed-result-and-identity");}}
 for(unsigned k=0;k<3;k++){Replay r;State s;Stream p;connect(r,s,p);Pipeline item(k);item.flags(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);results={VK_PIPELINE_COMPILE_REQUIRED,VK_ERROR_OUT_OF_HOST_MEMORY};item.run(r,k);checks.require(s.count==1&&s.events[0].raw==VK_ERROR_OUT_OF_HOST_MEMORY&&s.events[0].reason==CreationFailed,"M18/failed-fallback-actual-origin");}
 {Replay r;State s;Stream p;connect(r,s,p);results={VK_ERROR_OUT_OF_HOST_MEMORY};VkShaderModuleCreateInfo info={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.pCode=valid_code;info.codeSize=sizeof(valid_code);VkShaderModule m;r.get_per_thread_data().expected_tag=RESOURCE_SHADER_MODULE;r.get_per_thread_data().expected_hash=0x101;r.enqueue_create_shader_module(0x101,&info,&m);checks.require(s.count==1&&s.events[0].raw==VK_ERROR_OUT_OF_HOST_MEMORY&&s.events[0].tag==RESOURCE_SHADER_MODULE&&s.events[0].hash==0x101&&s.events[0].worker==0&&s.events[0].attempt==1&&(s.events[0].known&(Raw|HasTag|HasHash|Worker|Attempt))==(Raw|HasTag|HasHash|Worker|Attempt),"M18/shader-create-origin");}
 {Replay r(8);State s;Stream p;connect(r,s,p);std::vector<std::thread> producers;for(unsigned n=0;n<8;n++)producers.emplace_back([&r,n](){Global::worker_thread_index=n+1;for(unsigned j=0;j<8;j++)r.record_failure(ReplayFailure::Create,RESOURCE_COMPUTE_PIPELINE,n*8+j,VK_ERROR_DEVICE_LOST);});for(auto&t:producers)t.join();r.finish_events(1);p.finish(s);checks.require(s.count==MaxEvents&&s.omitted==64-MaxEvents&&s.final&&s.failed&&s.lost,"M18/concurrent-complete-frames-and-explicit-omissions");}
}
int main(int argc,char**argv){if(argc!=2)return 2;std::string c=argv[1];if(c=="parse")event_parse();else if(c=="creation")event_creation();else return 2;printf("M18 producer %s failures=%d\n",c.c_str(),checks.failures);return checks.failures?1:0;}
