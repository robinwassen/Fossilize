#include "file_operations.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <string>
#include <vector>

using namespace Fossilize;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct Fake
{
	int opens = 0, writes = 0, flushes = 0, closes = 0;
	bool opened = false, consumed = false;
	bool open_fail = false, write_fail = false, flush_fail = false, close_fail = false;
	int open_error = ENOENT, write_error = ENOSPC, flush_error = EIO, close_error = EBADF;
	std::string mode, payload, order;
	static Fake &get(void *ctx) { return *static_cast<Fake *>(ctx); }
	static void *open(void *ctx, const char *, const char *mode)
	{
		auto &f = get(ctx); CHECK(errno == 0); f.opens++; f.order += 'o'; f.mode = mode;
		if (f.open_fail) { errno = f.open_error; return nullptr; }
		f.opened = true; errno = EINVAL; return &f;
	}
	static size_t write(void *ctx, const void *data, size_t size, void *handle)
	{
		auto &f = get(ctx); CHECK(errno == 0); CHECK(handle == &f && f.opened && !f.consumed);
		f.writes++; f.order += 'w'; f.payload.assign(static_cast<const char *>(data), size);
		errno = f.write_fail ? f.write_error : EINVAL; return f.write_fail ? size - 1 : size;
	}
	static int flush(void *ctx, void *handle)
	{
		auto &f = get(ctx); CHECK(errno == 0); CHECK(handle == &f && f.opened && !f.consumed);
		f.flushes++; f.order += 'f'; errno = f.flush_fail ? f.flush_error : EINVAL;
		return f.flush_fail ? EOF : 0;
	}
	static int close(void *ctx, void *handle)
	{
		auto &f = get(ctx); CHECK(errno == 0); CHECK(handle == &f && f.opened && !f.consumed);
		f.closes++; f.order += 'c'; f.consumed = true;
		errno = f.close_fail ? f.close_error : EINVAL; return f.close_fail ? EOF : 0;
	}
	FileInternal::Operations ops() { return { this, open, write, flush, close }; }
};

static FileWriteResult injected(bool buffer, Fake &f, bool empty = false)
{
	const char payload[] = "A\nB\0C";
	errno = ERANGE;
	return buffer ? FileInternal::write_buffer("ignored", payload, empty ? 0 : sizeof(payload) - 1, f.ops()) :
	                FileInternal::write_string("ignored", empty ? "" : payload, f.ops());
}
static void expect(FileWriteResult r, FileWriteOperation op, int error)
{
	CHECK(r.success == (op == FileWriteOperation::None)); CHECK(r.operation == op); CHECK(r.error == error);
	errno = ERANGE; CHECK(FileInternal::legacy_result(r) == r.success);
	if (!r.success) CHECK(errno == error);
}
static void completed(const Fake &f, bool empty = false)
{
	CHECK(f.opens == 1 && f.writes == (empty ? 0 : 1) && f.flushes == 1 && f.closes == 1);
	CHECK(f.consumed); CHECK(f.order == (empty ? "ofc" : "owfc"));
}
static std::vector<char> read(const char *path)
{
	FILE *file = fopen(path, "rb"); CHECK(file);
	std::vector<char> data; int c;
	while ((c = fgetc(file)) != EOF) data.push_back(char(c));
	CHECK(!ferror(file)); CHECK(fclose(file) == 0); return data;
}
static FileWriteResult real_result(bool buffer, const char *path, const char *data, size_t size)
{
	return buffer ? write_buffer_to_file_result(path, data, size) : write_string_to_file_result(path, data);
}
static bool real_bool(bool buffer, const char *path, const char *data, size_t size)
{
	return buffer ? write_buffer_to_file(path, data, size) : write_string_to_file(path, data);
}
int main(int argc, char **argv)
{
	CHECK(argc == 3); bool buffer = strcmp(argv[1], "buffer") == 0; std::string scenario = argv[2];
	Fake f;
	if (scenario == "open")
	{
		f.open_fail = true; expect(injected(buffer, f), FileWriteOperation::Open, ENOENT);
		CHECK(f.opens == 1 && !f.writes && !f.flushes && !f.closes && f.order == "o");
	}
	else if (scenario == "short-write")
	{
		f.write_fail = f.close_fail = true;
		expect(injected(buffer, f), FileWriteOperation::Write, ENOSPC); completed(f);
	}
	else if (scenario == "flush")
	{
		f.flush_fail = f.close_fail = true;
		expect(injected(buffer, f), FileWriteOperation::Flush, EIO); completed(f);
	}
	else if (scenario == "close")
	{
		f.close_fail = true; expect(injected(buffer, f), FileWriteOperation::Close, EBADF); completed(f);
	}
	else if (scenario == "first-error")
	{
		f.write_fail = f.flush_fail = f.close_fail = true;
		expect(injected(buffer, f), FileWriteOperation::Write, ENOSPC); completed(f);
		Fake g; g.flush_fail = g.close_fail = true;
		expect(injected(buffer, g), FileWriteOperation::Flush, EIO); completed(g);
		// Every failing stage can leave errno unspecified; do not attribute stale errors.
		for (int stage = 0; stage < 4; stage++)
		{
			Fake z; z.open_error = z.write_error = z.flush_error = z.close_error = 0;
			z.open_fail = stage == 0; z.write_fail = stage == 1; z.flush_fail = stage == 2; z.close_fail = stage == 3;
			expect(injected(buffer, z), static_cast<FileWriteOperation>(stage + 1), 0);
			if (stage) completed(z); else CHECK(z.closes == 0);
		}
	}
	else if (scenario == "success")
	{
		expect(injected(buffer, f), FileWriteOperation::None, 0); completed(f);
		CHECK(f.mode == (buffer ? "wb" : "w"));
		CHECK(f.payload == (buffer ? std::string("A\nB\0C", 5) : std::string("A\nB")));
		Fake e; expect(injected(buffer, e, true), FileWriteOperation::None, 0); completed(e, true);
	}
	else if (scenario == "filesystem")
	{
		const char data[] = "ASCII bytes\0binary";
		size_t size = buffer ? sizeof(data) - 1 : strlen(data);
		for (int api = 0; api < 2; api++)
		{
			if (api) expect(real_result(buffer, "output", data, size), FileWriteOperation::None, 0);
			else CHECK(real_bool(buffer, "output", data, size));
			CHECK(read("output") == std::vector<char>(data, data + size));
			if (api) expect(real_result(buffer, "output", "", 0), FileWriteOperation::None, 0);
			else CHECK(real_bool(buffer, "output", "", 0));
			CHECK(read("output").empty());
		}
		auto r = real_result(buffer, "nonexistent-parent/output", data, size);
		CHECK(!r.success && r.operation == FileWriteOperation::Open && r.error != 0);
		CHECK(!real_bool(buffer, "nonexistent-parent/output", data, size)); CHECK(errno != 0);
		CHECK(remove("output") == 0);
	}
	else if (scenario == "dev-full")
	{
		auto r = real_result(buffer, "/dev/full", "buffered", 8);
		CHECK(!r.success && (r.operation == FileWriteOperation::Flush || r.operation == FileWriteOperation::Close));
		CHECK(r.error == ENOSPC); CHECK(!real_bool(buffer, "/dev/full", "buffered", 8)); CHECK(errno == ENOSPC);
	}
	else CHECK(false);
	return 0;
}
