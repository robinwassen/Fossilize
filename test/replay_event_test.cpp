#include "diagnostic_test.hpp"
#include "replay_event.hpp"
#include "../cli/replay_worker_outcome.hpp"
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
using namespace Fossilize::ReplayEvent;
static DiagnosticChecks checks;
static std::string wire(const std::string &p) { char b[MaxFrame]; checks.require(frame(b,sizeof(b),p.c_str()) != 0,"M18/frame-fits"); return b; }
static std::string wire(const Event &e) { char b[MaxFrame]; checks.require(event_frame(b,sizeof(b),e)!=0,"M18/event-fits"); return b; }
static void feed(Stream &p, State &s, const std::string &str) { p.feed(s,str.data(),str.size(),[](const char *){}); }
static std::string hello() { return wire("1 HELLO 0 0"); }
static Event sample(unsigned kind = 0)
{
 Event e; e.phase=kind==0?Parse:kind==1?Create:Timeout; e.reason=kind==0?InvalidJSON:kind==1?CreationFailed:NoProgress;
 e.known=Worker|Attempt|HasTag|HasHash|Range; e.worker=e.attempt=e.hash=0; e.tag=4; e.begin=e.end=0;
 if(kind<2) {e.known|=Raw; e.domain=kind?Vulkan:RapidJSON; e.raw=kind?-4:3;}
 if(!kind) {e.known|=Offset;e.offset=27;}
 if(kind==2) {e.known|=Duration;e.threshold=2000000000;e.elapsed=2500000001;}
 return e;
}
static void roundtrip()
{
 for(unsigned k=0;k<3;k++) { State s; s.begin(0,0);Stream p;auto e=sample(k);feed(p,s,hello()+wire(e)+wire("1 FINAL 1 0"));p.finish(s);
 checks.require(s.count==1 && s.final && s.failed && !s.lost,"M18/exact-roundtrip-count");auto &v=s.events[0];
 checks.require(v.phase==e.phase&&v.reason==e.reason&&v.known==e.known&&v.raw==e.raw&&v.domain==e.domain&&v.offset==e.offset&&v.threshold==e.threshold&&v.elapsed==e.elapsed&&v.hash==0&&v.worker==0&&v.attempt==0,"M18/exact-fields-zero-is-known"); }
 State s;s.begin(0,0);Stream p;Event e;e.phase=Parse;e.reason=GenericParse;feed(p,s,hello()+wire(e));checks.require(s.count==1&&s.events[0].known==0,"M18/absent-fields-remain-absent");
 Event zero=sample(1);zero.raw=0;State zs;zs.begin(0,0);Stream zp;feed(zp,zs,hello()+wire(zero));checks.require(zs.count==1&&(zs.events[0].known&Raw)&&zs.events[0].raw==0&&zs.events[0].domain==Vulkan,"M18/known-zero-raw-distinct-from-absence");
 for(auto value:{int64_t(5),INT64_MIN,INT64_MAX}){Event native=sample(2);native.known|=Raw;native.domain=Native;native.raw=value;State state;state.begin(0,0);Stream stream;feed(stream,state,hello()+wire(native));checks.require(state.count==1&&state.events[0].domain==Native&&state.events[0].raw==value&&(state.events[0].known&Raw),"M18/native-domain-and-signed-boundaries");}
 checks.require(wire("1 HELLO 0 0")=="FZ18 11:1 HELLO 0 0\n","M18/golden-stable-hello");
 checks.require(Parse==1&&Create==2&&Timeout==3&&InvalidJSON==2&&CreationFailed==16&&NoProgress==32&&Raw==1&&Duration==128&&Vulkan==1&&RapidJSON==2&&Native==3,"M18/stable-identifiers");
}
static void streaming()
{
 const std::string all=hello()+wire(sample())+wire(sample(1))+wire("1 FINAL 1 0");
 for(size_t split=0;split<=all.size();split++) { State s;s.begin(0,0);Stream p;feed(p,s,all.substr(0,split));feed(p,s,all.substr(split));p.finish(s);checks.require(s.count==2&&s.events[0].phase==Parse&&s.events[0].offset==27&&s.events[1].phase==Create&&s.events[1].raw==-4&&s.final&&!s.lost,"M18/every-two-part-split"); }
 State s;s.begin(0,0);Stream p;for(char c:all)feed(p,s,std::string(1,c));p.finish(s);checks.require(s.count==2&&s.final&&!s.lost,"M18/every-byte-split");
 // OFFER is consumed by the startup decoder and never steals a first legacy hash.
 const auto offerwire=wire("1 OFFER 2 0 0"); uint64_t w=9,a=9;bool recognized=false;
 checks.require(offer(offerwire.data(),offerwire.size(),w,a,recognized)&&recognized&&w==0&&a==0,"M18/actual-offer-selection");
 for(size_t split=0;split<=offerwire.size();split++){State state;Stream stream;unsigned selected=0;auto on_offer=[&](const char *p,size_t n){uint64_t w=9,a=9;if(select_offer(p,n,w,a)&&w==0&&a==0)selected++;};stream.feed_frames(state,offerwire.data(),split,[](const char*){},on_offer);stream.feed_frames(state,offerwire.data()+split,offerwire.size()-split,[](const char*){},on_offer);checks.require(selected==1&&!state.lost,"M18/every-offer-frame-split");}
 State os;Stream op;unsigned selected=0;for(char c:offerwire)op.feed_frames(os,&c,1,[](const char*){},[&](const char *text,size_t n){uint64_t worker=9,attempt=9;if(select_offer(text,n,worker,attempt)&&worker==0&&attempt==0)selected++;});checks.require(selected==1&&!os.lost,"M18/every-byte-offer-fragmentation");
 checks.require(!offer("abc\n",4,w,a,recognized)&&!recognized,"M18/legacy-first-hash-preserved");
}
static void malformed()
{
 const std::string final=wire("1 FINAL 0 0");
 for(size_t n=1;n<final.size();n++) { State s;s.begin(0,0);Stream p;feed(p,s,hello());feed(p,s,final.substr(0,n));p.finish(s);checks.require(!s.final&&s.lost&&!s.successful(),"M18/every-truncated-final-prefix"); }
 const std::vector<std::string> fragments={hello(),wire(sample()),final};
 for(unsigned kind=0;kind<fragments.size();kind++)for(size_t cut=1;cut<fragments[kind].size();cut++){State state;state.begin(0,0);Stream stream;if(kind)feed(stream,state,hello());feed(stream,state,fragments[kind].substr(0,cut));stream.finish(state);checks.require(state.lost&&!state.count&&!state.final&&(kind||!state.hello),"M18/every-truncated-hello-event-final-prefix");}
 std::vector<std::string> bad={wire("1 FUTURE 7"),"FZ18 99999999999999999999999999:x\n","FZ18 1:abc\n","FZ18 0:\n",std::string(4096,'x')+"\n", "FZ18 11:1 HELLO 0 0\r\n", "FZ18 11:1 HELLO 0 0junk\n"};
 bad.push_back(wire("1 EVENT 18446744073709551616 2 0 0 0 0 0 0 0 0 0 0 0 0"));
 bad.push_back(wire("1 EVENT 1 2 2 1 9223372036854775808 0 0 0 0 0 0 0 0 0"));
 bad.push_back(wire("1 EVENT 1 2 2 1 -9223372036854775809 0 0 0 0 0 0 0 0 0"));
 bad.push_back(wire("1 EVENT 1 2 0 32 0 0 0 0 0 7 6 0 0 0"));
 bad.push_back(wire("1 EVENT 1 2 0 0 0 0 1 0 0 0 0 0 0 0"));
 auto nul=hello();nul[9]=0;bad.push_back(nul);auto high=hello();high[10]=char(255);bad.push_back(high);
 auto huge=sample();auto ewire=wire(huge);auto pos=ewire.find(" 3 "); if(pos!=std::string::npos) ewire.replace(pos,3," 18446744073709551616 "); bad.push_back(ewire);
 for(const auto &b:bad) {State s;s.begin(0,0);Stream p;feed(p,s,b);feed(p,s,hello()+wire(sample()));p.finish(s);checks.require((s.lost||s.unsupported)&&!s.successful()&&p.used==0&&s.count==1&&s.events[0].reason==InvalidJSON,"M18/malformed-resynchronizes-with-explicit-loss");}
 Event invalid=sample(); invalid.known|=256;
 State s;s.begin(0,0);Stream p;feed(p,s,hello()+wire(invalid)+wire(sample()));checks.require(s.lost&&s.count==1,"M18/invalid-known-mask-rejected");
 for(unsigned which=0;which<3;which++) {State t;t.begin(0,0);Stream q;Event e=sample();if(which==0)e.phase=999;if(which==1)e.reason=999;if(which==2)e.domain=999;feed(q,t,hello()+wire(e)+wire(sample(1))+final);q.finish(t);checks.require(t.unsupported&&t.count==2&&t.events[0].phase==e.phase&&t.events[0].reason==e.reason&&t.events[0].domain==e.domain&&t.events[1].reason==CreationFailed&&!t.successful(),"M18/opaque-unknown-never-known-success");}
 State bounded;bounded.begin(0,0);Stream q;feed(q,bounded,hello());for(unsigned i=0;i<1000;i++)feed(q,bounded,wire(sample()));checks.require(bounded.count==MaxEvents&&bounded.omitted==1000-MaxEvents&&bounded.lost,"M18/bounded-storage-loss-accounted");
}
static void negotiation()
{
 uint64_t w,a;bool recognized;
 auto no=wire("1 OFFER 4 0 0"); checks.require(!offer(no.data(),no.size(),w,a,recognized)&&recognized,"M18/no-overlap");
 std::vector<std::string> bad={wire("2 HELLO 0 0"),wire("1 HELLO 0 1"),hello()+hello(),wire(sample())+hello(),wire("1 FINAL 0 0")};
 for(auto b:bad){ State s;s.begin(0,0);Stream p;feed(p,s,b+wire("1 FINAL 0 0"));p.finish(s);checks.require(!s.successful(),"M18/invalid-selection-not-certified"); }
 ReplayWorkerOutcome o;o.attempts=1;o.begin_events(0);Stream p;Event prior=sample();prior.attempt=1;feed(p,o.events,wire("1 HELLO 0 1")+wire(prior));p.finish(o.events);o.attempts=2;o.begin_events(0);p={};feed(p,o.events,hello()+wire("1 FINAL 0 0"));p.finish(o.events);
 checks.require(o.previous_event_count==1&&o.previous_events[0].count==1&&!o.events.hello&&o.events.lost,"M18/attempt-reset-and-stale-selection");
 for(unsigned mismatch=0;mismatch<2;mismatch++){State stale;stale.begin(0,2);Stream stream;Event event=sample();event.attempt=mismatch?2:1;event.worker=mismatch?1:0;feed(stream,stale,wire("1 HELLO 0 2")+wire(event)+wire("1 FINAL 0 0"));stream.finish(stale);checks.require(stale.lost&&!stale.count&&!stale.successful(),"M18/stale-worker-attempt-event-rejected");}
 State duplicate;duplicate.begin(0,0);Stream dp;feed(dp,duplicate,hello()+wire("1 FINAL 0 0")+wire("1 FINAL 1 0"));checks.require(duplicate.final&&duplicate.lost&&!duplicate.failed&&!duplicate.successful(),"M18/conflicting-final-cannot-overwrite-first");
 o.events.begin(0,2);p={};feed(p,o.events,wire("1 HELLO 0 2")+wire("1 FINAL 0 0"));p.finish(o.events);checks.require(o.events.successful()&&o.previous_events[0].events[0].reason==InvalidJSON,"M18/current-success-keeps-prior-evidence");
}
struct StartupCase
{
 std::string name, input;
 bool accepted = false, rich = false;
 std::vector<uint64_t> hashes;
};
static std::vector<StartupCase> startup_cases()
{
 const auto complete = wire("1 OFFER 2 7 9");
 std::vector<StartupCase> cases;
 StartupCase valid; valid.name="valid";valid.input=complete;valid.accepted=valid.rich=true;cases.push_back(valid);
 valid.name="valid-hashes";valid.input=complete+"abcdef\n0123456789abcdef\n";valid.hashes={0xabcdef,0x123456789abcdef};cases.push_back(valid);
 StartupCase legacy;legacy.name="legacy";legacy.input="abcdef\n0123456789abcdef\n";legacy.accepted=true;legacy.hashes=valid.hashes;cases.push_back(legacy);
 legacy.name="empty";legacy.input="";legacy.hashes.clear();cases.push_back(legacy);
 for(size_t n=1;n<complete.size();n++){StartupCase c;c.name="reject-prefix-"+std::to_string(n);c.input=complete.substr(0,n);cases.push_back(c);}
 std::vector<std::string> invalid={complete.substr(0,complete.size()-1)+"junk\n",complete.substr(0,complete.size()-1)+"\r\n", "FZ18 999999999999999999999999999999:x\n", "FZ18 "+std::string(4096,'x')+"\n",wire("1 OFFER 4 7 9"),wire("2 OFFER 2 7 9"),"FZ18 1:1 OFFER 2 7 9\n",complete+"abcjunk\n", "abcjunk\n"};
 auto nul=complete.substr(0,complete.size()-1);nul.push_back(0);nul+="junk\n";invalid.push_back(nul);
 nul=complete;nul[2]=0;invalid.push_back(nul);
 auto high=complete;high[10]=char(255);invalid.push_back(high);
 nul=complete+"abc";nul.push_back(0);nul+="junk\n";invalid.push_back(nul);
 for(size_t i=0;i<invalid.size();i++){StartupCase c;c.name="reject-invalid-"+std::to_string(i);c.input=invalid[i];cases.push_back(c);}
 return cases;
}
static void startup_tests(const char *directory = nullptr)
{
 const auto complete=wire("1 OFFER 2 7 9");
 for(size_t n=0;n<complete.size();n++){uint64_t w=99,a=99;bool recognized=false;checks.require(!offer(complete.data(),n,w,a,recognized),"M18/actual-offer-every-truncated-prefix-rejected");}
 for(const auto &c:startup_cases())
 {
  FILE *input=std::tmpfile();checks.require(input!=nullptr,"M18/startup-input-file");if(!input)continue;
  checks.require(std::fwrite(c.input.data(),1,c.input.size(),input)==c.input.size(),"M18/startup-input-exact-bytes");std::rewind(input);
  bool rich=false;uint64_t worker=99,attempt=99;std::vector<uint64_t> hashes;
  bool accepted=startup(input,rich,worker,attempt,[&](uint64_t hash){hashes.push_back(hash);});std::fclose(input);
  checks.require(accepted==c.accepted,"M18/actual-startup-complete-valid-bytes-only");
  if(c.accepted)checks.require(rich==c.rich&&hashes==c.hashes&&(!rich||(worker==7&&attempt==9)),"M18/actual-startup-identity-and-first-legacy-hash");
  if(directory){std::ofstream out(std::string(directory)+"/"+c.name+".input",std::ios::binary);out.write(c.input.data(),std::streamsize(c.input.size()));checks.require(bool(out),"M18/startup-corpus-written");}
 }
}
static void mutations()
{
 uint32_t seed=0x180018;uint64_t digest=1469598103934665603ull,accepted=0,rejected=0,lost=0;
 for(unsigned i=0;i<768;i++)
 {
  Event event=sample(i%3);if(i%8==1)event.reason=999;
  std::string b=hello()+wire(event);
  seed=seed*1664525u+1013904223u;
  if(i%8==2)b=hello()+wire("1 EVENT 18446744073709551616 2 0 0 0 0 0 0 0 0 0 0 0 0");
  else if(i%8>=3){size_t n=seed%b.size();b[n]=char(seed>>24);if(i%3==0)b.resize(n);}
  for(unsigned char c:b){digest^=c;digest*=1099511628211ull;}
  State state;state.begin(0,0);Stream stream;feed(stream,state,b);feed(stream,state,"\n");
  if(!state.hello)feed(stream,state,hello());
  feed(stream,state,wire(sample(1))+wire("1 FINAL 1 0"));stream.finish(state);
  accepted+=state.count;rejected+=state.lost||state.unsupported;lost+=state.lost;
  checks.require(stream.used==0&&state.count&&state.count<=MaxEvents&&state.events[state.count-1].reason==CreationFailed&&state.events[state.count-1].raw==-4&&state.final&&state.failed&&!state.successful(),"M18/mutation-bounded-and-valid-continuation");
 }
 printf("M18 corpus seed=0x180018 cases=768 max=4096 digest=%llx accepted=%llu rejected=%llu lost=%llu\n",(unsigned long long)digest,(unsigned long long)accepted,(unsigned long long)rejected,(unsigned long long)lost);
}
int main(int argc,char **argv)
{
 std::string c=argc>1?argv[1]:"all";
 if(c=="startup")startup_tests();else if(c=="startup-files"&&argc==3)startup_tests(argv[2]);else if(c=="roundtrip")roundtrip();else if(c=="unknown")malformed();
 else if(c=="streaming")streaming();else if(c=="malformed")malformed();else if(c=="negotiation"||c=="attempts")negotiation();else if(c=="mutations")mutations();
 else if(c=="legacy") {ReplayAttemptEvidence e;checks.require(replay_parse_timeout(e,"TIMEOUT 1 2000000000 2500000000")&&e.triggers[0].present,"M18/legacy-timeout");checks.require(replay_parse_timeout(e,"TERMINATE 1 2 5")&&e.requests[0].native_error==5,"M18/legacy-native-error");}
 else return 2;
 printf("M18 unit %s failures=%d\n",c.c_str(),checks.failures);return checks.failures?1:0;
}
