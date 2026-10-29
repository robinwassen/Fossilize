#pragma once
// Explicit checks survive Release/NDEBUG. Each CTest owns an isolated work directory.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <io.h>
#define DIAG_DUP _dup
#define DIAG_DUP2 _dup2
#define DIAG_CLOSE _close
#define DIAG_FILENO _fileno
#else
#include <unistd.h>
#define DIAG_DUP dup
#define DIAG_DUP2 dup2
#define DIAG_CLOSE close
#define DIAG_FILENO fileno
#endif

struct DiagnosticChecks
{
	int failures = 0;
	void require(bool value, const std::string &id)
	{
		if (!value)
		{
			fprintf(stdout, "FAIL %s\n", id.c_str());
			failures++;
		}
	}
	void contains(const std::string &text, const std::string &part, const std::string &id)
	{
		require(text.find(part) != std::string::npos, id + " expected: " + part + " observed: " + text);
	}
};

struct DiagnosticStderr
{
	FILE *file = nullptr;
	int saved = -1;
	DiagnosticStderr()
	{
		fflush(stderr);
		file = tmpfile();
		if (!file || (saved = DIAG_DUP(DIAG_FILENO(stderr))) < 0 ||
		    DIAG_DUP2(DIAG_FILENO(file), DIAG_FILENO(stderr)) < 0)
			std::abort();
	}
	std::string finish()
	{
		fflush(stderr);
		DIAG_DUP2(saved, DIAG_FILENO(stderr));
		DIAG_CLOSE(saved);
		saved = -1;
		rewind(file);
		std::string result;
		char buffer[1024];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), file)) != 0)
			result.append(buffer, n);
		fclose(file);
		file = nullptr;
		return result;
	}
	~DiagnosticStderr()
	{
		if (saved >= 0)
			finish();
	}
};
