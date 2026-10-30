// This caller uses only declarations present in the unchanged input headers.
#include "fossilize_external_replayer.hpp"
#include "diagnostic_test.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <errno.h>
#endif
int main(int argc, char **argv)
{
	if (argc != 4) return 2;
	DiagnosticChecks checks;
	for (unsigned failed = 0; failed < 2; failed++)
	for (unsigned mode = 0; mode < 3; mode++)
	{
		Fossilize::ExternalReplayer replay;
		Fossilize::ExternalReplayer::Options opts = {};
		const char *db[] = {argv[3]};
		opts.databases = db; opts.num_databases = 1;
		opts.num_threads = 1; opts.null_device = true; opts.quiet = true;
		const char *arguments[] = {argv[2], "--controller", "lost", argv[3]};
		if (failed) { opts.external_replayer_arguments = arguments; opts.num_external_replayer_arguments = 4; }
		else opts.external_replayer_path = argv[1];
		checks.require(replay.start(opts), "old-source/start");
		if (mode != 0)
		{
			// A kernel terminal-state barrier preserves the wait status for the
			// library and deterministically exercises each cached-status path.
#ifdef _WIN32
			checks.require(WaitForSingleObject(reinterpret_cast<HANDLE>(replay.get_process_handle()), 5000) == WAIT_OBJECT_0,
			               "old-source/terminal-state-barrier");
#else
			siginfo_t info = {};
			int waited;
			do { waited = waitid(P_PID, id_t(replay.get_process_handle()), &info, WEXITED | WNOWAIT); }
			while (waited < 0 && errno == EINTR);
			checks.require(waited == 0, "old-source/terminal-state-barrier");
#endif
		}
		int result = -99;
		if (mode == 2) checks.require(replay.is_process_complete(&result), "old-source/query-first-completion");
		else result = replay.wait();
		checks.require(result == (failed ? 1 : 0), "old-source/authoritative-status/mode-" + std::to_string(mode) +
		               "/expected-" + std::to_string(failed ? 1 : 0) + "/observed-" + std::to_string(result));
		int cached = -99;
		checks.require(replay.is_process_complete(&cached) && cached == result, "old-source/cached-completion");
		checks.require(replay.wait() == result, "old-source/repeated-wait");
	}
	printf("Executed R13/old-source-ExternalReplayer; failures=%d\n", checks.failures);
	return checks.failures ? 1 : 0;
}
