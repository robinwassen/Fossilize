// Only declarations present in the unchanged base public header are used here.
#include "fossilize_external_replayer.hpp"
#include "diagnostic_test.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <cerrno>
#endif
int main(int argc, char **argv)
{
	if (argc != 3) return 2;
	DiagnosticChecks checks;
	for (unsigned failure = 0; failure < 2; failure++) for (unsigned mode = 0; mode < 3; mode++)
	{
		Fossilize::ExternalReplayer replay;
		Fossilize::ExternalReplayer::Options opts = {};
		const char *db[] = {argv[2]}; opts.databases = db; opts.num_databases = 1;
		opts.external_replayer_path = argv[1]; opts.num_threads = 2; opts.null_device = true; opts.quiet = true;
		// The fixed archive has a malformed record at index 2. Preserve old range
		// selection to exercise clean/controlled-failure with the same bytes.
		opts.use_pipeline_range = true;
		opts.start_compute_index = 0; opts.end_compute_index = failure ? 3 : 2;
		opts.start_graphics_index = opts.end_graphics_index = 0;
		opts.start_raytracing_index = opts.end_raytracing_index = 0;
		checks.require(replay.start(opts), "old-source/start");
		if (mode)
		{
#ifdef _WIN32
			checks.require(WaitForSingleObject(reinterpret_cast<HANDLE>(replay.get_process_handle()), 5000) == WAIT_OBJECT_0, "old-source/kernel-terminal-barrier");
#else
			siginfo_t info = {}; int status;
			do { status = waitid(P_PID,id_t(replay.get_process_handle()),&info,WEXITED|WNOWAIT); } while (status < 0 && errno == EINTR);
			checks.require(status == 0,"old-source/kernel-terminal-barrier");
#endif
		}
		int status = -99;
		if (mode == 2) checks.require(replay.is_process_complete(&status), "old-source/query-first"); else status = replay.wait();
		checks.require(status == int(failure), "old-source/numeric-aggregate-status");
		int cached = -99;
		checks.require(replay.is_process_complete(&cached) && cached == status && replay.wait() == status, "old-source/cached-query-and-repeat-wait");
		Fossilize::ExternalReplayer::Progress progress = {};
		checks.require(replay.poll_progress(progress) == Fossilize::ExternalReplayer::PollResult::Complete, "old-source/completion-reported");
		checks.require(progress.clean_crashes == 0 && progress.dirty_crashes == 0, "old-source/controlled-failure-is-not-crash");
		checks.require(progress.compute.completed == 2 && progress.compute.parsed_fail == failure, "old-source/all-valid-work-drained");
	}
	printf("Executed R14/old-source-ExternalReplayer; failures=%d\n", checks.failures);
	return checks.failures ? 1 : 0;
}
