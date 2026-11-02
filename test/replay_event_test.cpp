#include "diagnostic_test.hpp"
#include "replay_event.hpp"
#include "../cli/replay_worker_outcome.hpp"
#include <string>
#include <vector>
#include <algorithm>
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
 checks.require(!offer("abc\n",4,w,a,recognized)&&!recognized,"M18/legacy-first-hash-preserved");
}
static void malformed()
{
 const std::string final=wire("1 FINAL 0 0");
 for(size_t n=1;n<final.size();n++) { State s;s.begin(0,0);Stream p;feed(p,s,hello());feed(p,s,final.substr(0,n));p.finish(s);checks.require(!s.final&&s.lost&&!s.successful(),"M18/every-truncated-final-prefix"); }
 std::vector<std::string> bad={"FZ18 99999999999999999999999999:x\n","FZ18 1:abc\n","FZ18 0:\n",std::string(4096,'x')+"\n", "FZ18 11:1 HELLO 0 0\r\n", "FZ18 11:1 HELLO 0 0junk\n"};
 bad.push_back(wire("1 EVENT 18446744073709551616 2 0 0 0 0 0 0 0 0 0 0 0 0"));
 bad.push_back(wire("1 EVENT 1 2 2 1 9223372036854775808 0 0 0 0 0 0 0 0 0"));
 bad.push_back(wire("1 EVENT 1 2 2 1 -9223372036854775809 0 0 0 0 0 0 0 0 0"));
 bad.push_back(wire("1 EVENT 1 2 0 32 0 0 0 0 0 7 6 0 0 0"));
 bad.push_back(wire("1 EVENT 1 2 0 0 0 0 1 0 0 0 0 0 0 0"));
 auto nul=hello();nul[9]=0;bad.push_back(nul);auto high=hello();high[10]=char(255);bad.push_back(high);
 auto huge=sample();auto ewire=wire(huge);auto pos=ewire.find(" 3 "); if(pos!=std::string::npos) ewire.replace(pos,3," 18446744073709551616 "); bad.push_back(ewire);
 for(const auto &b:bad) {State s;s.begin(0,0);Stream p;feed(p,s,b);feed(p,s,hello()+wire(sample()));p.finish(s);checks.require((s.lost||s.unsupported)&&!s.successful()&&p.used==0,"M18/malformed-resynchronizes-with-explicit-loss");}
 Event invalid=sample(); invalid.known|=256;
 State s;s.begin(0,0);Stream p;feed(p,s,hello()+wire(invalid)+wire(sample()));checks.require(s.lost&&s.count==1,"M18/invalid-known-mask-rejected");
 for(unsigned which=0;which<3;which++) {State t;t.begin(0,0);Stream q;Event e=sample();if(which==0)e.phase=999;if(which==1)e.reason=999;if(which==2)e.domain=999;feed(q,t,hello()+wire(e)+final);q.finish(t);checks.require(t.unsupported&&t.count==1&&!t.successful(),"M18/opaque-unknown-never-known-success");}
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
 o.events.begin(0,2);p={};feed(p,o.events,wire("1 HELLO 0 2")+wire("1 FINAL 0 0"));p.finish(o.events);checks.require(o.events.successful()&&o.previous_events[0].events[0].reason==InvalidJSON,"M18/current-success-keeps-prior-evidence");
}
static void mutations()
{
 uint32_t seed=0x180018;uint64_t digest=1469598103934665603ull,accepted=0,rejected=0,lost=0;
 for(unsigned i=0;i<768;i++) {std::string b=hello()+wire(sample(i%3))+wire("1 FINAL 0 0");seed=seed*1664525u+1013904223u;if(i%4){size_t n=seed%b.size(); b[n]=char(seed>>24);if(i%3==0)b.resize(n);}
 for(unsigned char c:b){digest^=c;digest*=1099511628211ull;}State s;s.begin(0,0);Stream p;feed(p,s,b);feed(p,s,"\n"+wire(sample(1)));p.finish(s);accepted+=s.count; rejected+=!s.successful();lost+=s.lost;
 checks.require(p.used==0&&s.count<=MaxEvents&&!s.successful(),"M18/mutation-bounded-and-post-final-not-success");}
 printf("M18 corpus seed=0x180018 cases=768 max=4096 digest=%llx accepted=%llu rejected=%llu lost=%llu\n",(unsigned long long)digest,(unsigned long long)accepted,(unsigned long long)rejected,(unsigned long long)lost);
}
int main(int argc,char **argv)
{
 std::string c=argc>1?argv[1]:"all";
 if(c=="roundtrip")roundtrip();else if(c=="unknown")malformed();
 else if(c=="streaming")streaming();else if(c=="malformed")malformed();else if(c=="negotiation"||c=="attempts")negotiation();else if(c=="mutations")mutations();
 else if(c=="legacy") {ReplayAttemptEvidence e;checks.require(replay_parse_timeout(e,"TIMEOUT 1 2000000000 2500000000")&&e.triggers[0].present,"M18/legacy-timeout");checks.require(replay_parse_timeout(e,"TERMINATE 1 2 5")&&e.requests[0].native_error==5,"M18/legacy-native-error");}
 else return 2;
 printf("M18 unit %s failures=%d\n",c.c_str(),checks.failures);return checks.failures?1:0;
}
