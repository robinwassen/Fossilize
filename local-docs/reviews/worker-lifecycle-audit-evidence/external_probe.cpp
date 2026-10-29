#include "fossilize_external_replayer.hpp"
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <fstream>
using namespace Fossilize;
static const char *self;
static int failpipe=0,pipecalls=0;
extern "C" int __real_pipe(int*);
extern "C" int __wrap_pipe(int *p) {if(failpipe && ++pipecalls==failpipe){errno=EMFILE;return -1;}return __real_pipe(p);}
static int countfds(){DIR*d=opendir("/proc/self/fd");int n=0;while(auto*e=readdir(d))if(e->d_name[0]!='.')n++;closedir(d);return n;}
static void start(ExternalReplayer&r,const char*mode){const char*args[]={self,mode};ExternalReplayer::Options o={};o.external_replayer_arguments=args;o.num_external_replayer_arguments=2;o.disable_rate_limiter=true;if(!r.start(o)){printf("START FAILED\n");exit(3);}}
static void zombie(pid_t p){char file[100];snprintf(file,sizeof file,"/proc/%d/stat",p);for(int i=0;i<200;i++){std::ifstream f(file);std::string s;std::getline(f,s);auto pos=s.rfind(')');if(pos!=std::string::npos&&s[pos+2]=='Z')return;usleep(10000);}printf("NOT ZOMBIE\n");exit(4);}
static void cleanup(pid_t p){kill(p,SIGKILL);int s;while(waitpid(p,&s,0)<0&&errno==EINTR){} }
static void sig(int){}
int main(int argc,char**argv){self=argv[0];const char*mode=argc>1?argv[1]:"";
 if(!strcmp(mode,"helper-exit"))return 37;
 if(!strcmp(mode,"helper-live")){sleep(30);return 0;}
 if(!strcmp(mode,"helper-control")){int fd=-1;for(int i=2;i+1<argc;i++)if(!strcmp(argv[i],"--control-fd"))fd=atoi(argv[i+1]);char c;while(read(fd,&c,1)>0){}return 0;}
 if(!strcmp(mode,"wait-steals-child")){ExternalReplayer r;start(r,"helper-exit");pid_t replay=(pid_t)r.get_process_handle();pid_t unrelated=fork();if(unrelated==0)_exit(42);siginfo_t info={};waitid(P_PID,replay,&info,WEXITED|WNOWAIT);waitid(P_PID,unrelated,&info,WEXITED|WNOWAIT);int got=r.wait();int st=0;errno=0;pid_t later=waitpid(unrelated,&st,WNOHANG);printf("wait_status=%d expected_replay=37 unrelated_waitpid=%d unrelated_errno=%d expected_unrelated=42\n",got,(int)later,errno);if(later==0)cleanup(unrelated);if(kill(replay,0)==0)cleanup(replay);return got==37&&later>0?0:1;}
 if(!strcmp(mode,"status")){ExternalReplayer r;start(r,"helper-exit");auto p=(pid_t)r.get_process_handle();zombie(p);int s=999;bool done=r.is_process_complete(&s);printf("done=%d status=%d expected=37\n",done,s);return s==37?0:1;}
 if(!strcmp(mode,"wait-interrupt")){ExternalReplayer r;start(r,"helper-live");auto p=(pid_t)r.get_process_handle();struct sigaction a={};a.sa_handler=sig;sigaction(SIGALRM,&a,nullptr);ualarm(100000,0);int s=r.wait();bool alive=kill(p,0)==0;printf("wait_status=%d child_alive=%d handle=%ld\n",s,alive,(long)r.get_process_handle());cleanup(p);return !alive?0:1;}
 if(!strcmp(mode,"thread")){ExternalReplayer r;std::thread t([&]{start(r,"helper-live");usleep(100000);});t.join();auto p=(pid_t)r.get_process_handle();zombie(p);int s;waitpid(p,&s,0);printf("host_alive=1 child_signaled=%d signal=%d\n",WIFSIGNALED(s),WIFSIGNALED(s)?WTERMSIG(s):0);return 0;}
 if(!strcmp(mode,"inherited-control")){auto*a=new ExternalReplayer;start(*a,"helper-control");pid_t pa=(pid_t)a->get_process_handle();usleep(100000);ExternalReplayer b;start(b,"helper-live");pid_t pb=(pid_t)b.get_process_handle();usleep(100000);delete a;usleep(200000);int st;pid_t ended=waitpid(pa,&st,WNOHANG);printf("after_A_destroyed_A_alive=%d ",ended==0);cleanup(pb);if(ended==0){zombie(pa);waitpid(pa,&st,0);}printf("after_B_killed_A_exited=%d\n",WIFEXITED(st));return 0;}
 if(!strcmp(mode,"pipe-failure")){int before=countfds();{ExternalReplayer r;failpipe=2;ExternalReplayer::Options o={};o.external_replayer_path=self;bool ok=r.start(o);printf("start=%d ",ok);}printf("fd_before=%d fd_after=%d leaked=%d\n",before,countfds(),countfds()-before);return 0;}
 return 2;
}
