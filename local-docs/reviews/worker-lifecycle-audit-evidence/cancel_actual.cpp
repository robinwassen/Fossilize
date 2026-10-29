#define main fossilize_unused_cli_main
#include "cli/fossilize_replay.cpp"
#undef main
int main() {
 ProcessProgress p; p.start_graphics_index=0;p.end_graphics_index=20;p.start_compute_index=p.end_compute_index=0;p.start_raytracing_index=p.end_raytracing_index=0;
 p.graphics_progress=2;p.compute_progress=0;p.raytracing_progress=0;
 p.pid=fork(); if(p.pid==0)_exit(2);
 siginfo_t info={}; if(waitid(P_PID,p.pid,&info,WEXITED|WNOWAIT))return 3;
 Global::active_processes=1;std::vector<ProcessProgress> children;children.push_back(p);
 shutdown_processes(children);int status;if(waitpid(p.pid,&status,0)!=p.pid)return 4;
 bool restart=children[0].process_shutdown(status);
 printf("production_shutdown_called=1 child_already_exited=%d expect_kill=%d replacement_requested=%d\n",WEXITSTATUS(status),children[0].expect_kill,restart);
 return restart?0:1;
}
