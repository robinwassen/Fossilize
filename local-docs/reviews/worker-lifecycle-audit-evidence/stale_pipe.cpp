#include <unistd.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <stdio.h>
struct ProcessProgress {int crash_fd=-1;bool process_once();void parse_raw(const char*){}};
bool ProcessProgress::process_once()
{
	if (crash_fd < 0)
		return false;

	// Important to use raw FD IO here since we are not guaranteed
	// to be able to read a newline here.
	// This can happen if the child process started writing to the file,
	// but received a SIGSTOP in the middle of writing for whatever reason.
	char buffer[65];
	ssize_t ret = ::read(crash_fd, buffer, sizeof(buffer) - 1);
	if (ret <= 0)
		return false;

	buffer[ret] = '\0';
	parse_raw(buffer);
	return true;
}


int main(){
 int result[2];pipe(result);pid_t ctl=fork();
 if(ctl==0){close(result[0]);int ep=epoll_create1(0),oldp[2];pipe(oldp);epoll_event ev={};ev.events=EPOLLIN|EPOLLHUP;ev.data.u32=0;epoll_ctl(ep,EPOLL_CTL_ADD,oldp[0],&ev);write(oldp[1],"x",1);close(oldp[1]);epoll_event batch[1];epoll_wait(ep,batch,1,-1);
 // Model SIGCHLD handler processed before this old slot event: old pipe retired, same slot restarted.
 close(oldp[0]);int replacement[2];pipe(replacement);ProcessProgress p;p.crash_fd=replacement[0];write(result[1],"R",1);
 // Exact pipe dispatch calls source-extracted process_once using CURRENT slot fd.
 p.process_once();write(result[1],"F",1);_exit(0);}
 close(result[1]);char c;read(result[0],&c,1);pollfd p={result[0],POLLIN,0};int returned=poll(&p,1,200);printf("stale_event_dispatch_returned=%d (0 means blocked on replacement pipe)\n",returned);kill(ctl,9);int status;waitpid(ctl,&status,0);close(result[0]);return returned==0?0:1;
}
