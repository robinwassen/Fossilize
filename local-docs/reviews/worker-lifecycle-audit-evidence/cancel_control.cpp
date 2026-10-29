#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <atomic>
#include <string>
#include <vector>
using namespace std;
#define LOGI(...) fprintf(stderr,__VA_ARGS__)
#define LOGE(...) fprintf(stderr,__VA_ARGS__)
struct Block {atomic<unsigned> dirty_process_deaths{0},clean_process_deaths{0};};
namespace Global {unsigned active_processes=1;Block *control_block=nullptr;}
static void close_and_remove_epoll_entry(int &fd){if(fd>=0)close(fd);fd=-1;}
struct ProcessProgress {int pid=-1,index=0,heartbeats=0,crash_fd=-1,timer_fd=-1,watchdog_timer_fd=-1; int graphics_progress=5,compute_progress=0,raytracing_progress=0; unsigned start_graphics_index=0,end_graphics_index=10,start_compute_index=0,end_compute_index=0,start_raytracing_index=0,end_raytracing_index=0; bool expect_kill=false,stopped=false; string parse_buffer; bool process_once(){return false;} bool process_shutdown(int);};
bool ProcessProgress::process_shutdown(int wstatus)
{
	// Flush out all messages we got.
	while (process_once());
	parse_buffer.clear();
	heartbeats = -1;

	close_and_remove_epoll_entry(crash_fd);
	// Close the timerfd.
	close_and_remove_epoll_entry(timer_fd);
	close_and_remove_epoll_entry(watchdog_timer_fd);

	// Reap child process.
	Global::active_processes--;
	auto wait_pid = pid;
	pid = -1;

	// If application exited in normal manner, we are done.
	if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
		return false;

	if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL && expect_kill)
	{
		LOGI("Parent process is shutting down. Expected SIGKILL.\n");
		return false;
	}

	if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL)
	{
		// We had to kill the process early. Log this for debugging.
		LOGE("Process index %u (PID: %d) failed and it had to be killed in timeout with SIGKILL.\n",
		     index, wait_pid);
	}

	// If the child did not exit in a normal manner, we failed to catch any crashing signal.
	// Do not try any further.
	if (!WIFEXITED(wstatus) && WIFSIGNALED(wstatus) &&
	    (WTERMSIG(wstatus) != SIGKILL && WTERMSIG(wstatus) != SIGSEGV))
	{
		LOGE("Process index %u (PID: %d) failed to terminate in a clean fashion (signal %d). We cannot continue replaying.\n",
		     index, wait_pid, WTERMSIG(wstatus));

		if (Global::control_block)
			Global::control_block->dirty_process_deaths.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	// We might have crashed, but we never saw any progress marker.
	// We do not know what to do from here, so we just terminate.
	if (graphics_progress < 0 || compute_progress < 0 || raytracing_progress < 0)
	{
		LOGE("Child process %d terminated before we could receive progress. Cannot continue.\n",
		     wait_pid);
		if (Global::control_block)
			Global::control_block->dirty_process_deaths.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	if (Global::control_block)
		Global::control_block->clean_process_deaths.fetch_add(1, std::memory_order_relaxed);

	start_graphics_index = uint32_t(graphics_progress);
	start_compute_index = uint32_t(compute_progress);
	start_raytracing_index = uint32_t(raytracing_progress);
	if (start_graphics_index >= end_graphics_index &&
	    start_compute_index >= end_compute_index &&
	    start_raytracing_index >= end_raytracing_index)
	{
		LOGE("Process index %u (PID: %d) crashed, but there is nothing more to replay.\n", index, wait_pid);
		return false;
	}
	else
	{
		LOGE("Process index %u (PID: %d) crashed, but will retry.\n", index, wait_pid);
		LOGE("  New graphics range (%u, %u)\n", start_graphics_index, end_graphics_index);
		LOGE("  New compute range (%u, %u)\n", start_compute_index, end_compute_index);
		LOGE("  New raytracing range (%u, %u)\n", start_raytracing_index, end_raytracing_index);
		return true;
	}
}

static void shutdown_processes(std::vector<ProcessProgress> &child_processes)
{
	for (auto &child_process : child_processes)
	{
		if (child_process.pid >= 0)
		{
			child_process.stopped = false;
			child_process.expect_kill = true;
			kill(child_process.pid, SIGKILL);
		}
	}
}

int main(){
 vector<ProcessProgress> slots(1);pid_t c=fork();if(c==0)_exit(2);slots[0].pid=c;
 siginfo_t info={};if(waitid(P_PID,c,&info,WEXITED|WNOWAIT))return 2;
 shutdown_processes(slots);int status=0;waitpid(c,&status,0);
 bool restart=slots[0].process_shutdown(status);
 printf("cancelled=%d actual_exit=%d restart_requested=%d active_after_reap=%u\n",int(slots[0].expect_kill),WEXITSTATUS(status),int(restart),Global::active_processes);
 return restart && WIFEXITED(status)&&WEXITSTATUS(status)==2?0:1;
}
