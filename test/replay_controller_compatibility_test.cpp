// This caller uses only declarations present in the unchanged input headers.
#include "fossilize_external_replayer.hpp"
#include "diagnostic_test.hpp"
int main(int argc, char **argv)
{
	if (argc != 4) return 2;
	DiagnosticChecks checks;
	for (unsigned failed = 0; failed < 2; failed++)
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
		int result = replay.wait();
		checks.require(result == (failed ? 1 : 0), "old-source/authoritative-wait-status");
		int cached = -99;
		checks.require(replay.is_process_complete(&cached) && cached == result, "old-source/cached-completion");
		checks.require(replay.wait() == result, "old-source/repeated-wait");
	}
	printf("Executed R13/old-source-ExternalReplayer; failures=%d\n", checks.failures);
	return checks.failures ? 1 : 0;
}
