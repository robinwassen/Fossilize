#include "fossilize_external_replayer.hpp"
#include "fossilize_external_replayer_control_block.hpp"
#include "platform/futex_wrapper_linux.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
using namespace Fossilize;
int main(int argc,char **argv) {
 if(argc>1 && !strcmp(argv[1],"helper")) {
  int fd=-1; for(int i=2;i+1<argc;i++) if(!strcmp(argv[i],"--shmem-fd")) fd=atoi(argv[i+1]);
  struct stat st; if(fd<0 || fstat(fd,&st)) return 3;
  auto *s=(SharedControlBlock*)mmap(nullptr,st.st_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(s==MAP_FAILED) return 4;
  s->progress_started.store(1); futex_wrapper_lock(&s->futex_lock); _exit(37);
 }
 int out[2]; pipe(out); pid_t subject=fork();
 if(subject==0) {
  close(out[0]); ExternalReplayer r; ExternalReplayer::Options o={}; const char *args[]={argv[0],"helper"};
  o.external_replayer_arguments=args;o.num_external_replayer_arguments=2;o.disable_rate_limiter=true;
  if(!r.start(o)) _exit(5); pid_t c=(pid_t)r.get_process_handle();
  // Observe exit without collecting it, to retain the API's normal exit ownership.
  siginfo_t info={}; if(waitid(P_PID,c,&info,WEXITED|WNOWAIT)) _exit(6);
  write(out[1],"R",1); ExternalReplayer::Progress p={};r.poll_progress(p);write(out[1],"D",1); _exit(0);
 }
 close(out[1]);char b; if(read(out[0],&b,1)!=1)return 7;
 usleep(200000);int st=0;pid_t done=waitpid(subject,&st,WNOHANG);
 char path[100],channel[100]={};snprintf(path,sizeof path,"/proc/%d/wchan",subject);FILE *f=fopen(path,"r");if(f){fgets(channel,sizeof channel,f);fclose(f);}
 printf("child_exit_observed_before_poll=1 poll_returned=%d subject_alive=%d wait_channel=%s\n",done!=0,done==0,channel);
 if(done==0){kill(subject,SIGKILL);waitpid(subject,&st,0);}close(out[0]);return done==0?0:1;
}
