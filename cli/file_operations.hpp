// Internal per-call stdio adapter for deterministic completion tests.
#pragma once
#include "file.hpp"

namespace Fossilize
{
namespace FileInternal
{
struct Operations
{
	void *context;
	void *(*open)(void *, const char *, const char *);
	size_t (*write)(void *, const void *, size_t, void *);
	int (*flush)(void *, void *);
	int (*close)(void *, void *);
};
FileWriteResult write_string(const char *path, const char *text, const Operations &ops);
FileWriteResult write_buffer(const char *path, const void *data, size_t size, const Operations &ops);
bool legacy_result(FileWriteResult result);
}
}
