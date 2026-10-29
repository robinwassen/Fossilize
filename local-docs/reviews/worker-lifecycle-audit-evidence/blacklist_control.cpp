#include <unistd.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <unordered_set>
namespace Global { std::unordered_set<uint64_t> faulty_spirv_modules; }
static bool write_all(int fd, const char *str)
{
	// write is async-signal safe, but not stdio.
	size_t len = strlen(str);
	while (len)
	{
		ssize_t wrote = write(fd, str, len);
		if (wrote <= 0)
			return false;

		str += wrote;
		len -= wrote;
	}

	return true;
}

static void send_faulty_modules_and_close(int fd)
{
	for (auto &m : Global::faulty_spirv_modules)
	{
		char buffer[18];
		sprintf(buffer, "%" PRIx64 "\n", m);
		write_all(fd, buffer);
	}

	close(fd);
}


int main(){
 int ready[2]; pipe(ready);
 pid_t controller=fork();
 if(controller==0){
  close(ready[0]);
  sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGTERM);sigprocmask(SIG_BLOCK,&mask,nullptr);
  int sigfd=signalfd(-1,&mask,0); (void)sigfd;
  int input_fds[2];pipe(input_fds);
  pid_t worker=fork();
  if(worker==0){close(input_fds[0]);close(input_fds[1]);_exit(0);}
  int status;waitpid(worker,&status,0);
  for(uint64_t i=0;i<4096;i++)Global::faulty_spirv_modules.insert(0x1000000000000000ull+i);
  write(ready[1],"R",1);
  // Checker negative control: close own reader before write and ignore SIGPIPE to receive EPIPE.
  close(input_fds[0]); signal(SIGPIPE,SIG_IGN);
  send_faulty_modules_and_close(input_fds[1]);
  close(input_fds[0]);write(ready[1],"F",1);_exit(0);
 }
 close(ready[1]);char c;read(ready[0],&c,1);
 pollfd p={ready[0],POLLIN,0};int before=poll(&p,1,200);
 kill(controller,SIGTERM);p.revents=0;int after=poll(&p,1,200);
 int status;pid_t exited=waitpid(controller,&status,WNOHANG);
 printf("returned_before_SIGTERM=%d returned_after_SIGTERM=%d controller_exited=%d\n",before,after,int(exited>0));
 if(!exited){kill(controller,SIGKILL);waitpid(controller,&status,0);}
 close(ready[0]);
 return before>0 ? 0:1;
}
