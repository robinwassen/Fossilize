// Deliberately uses only the baseline file.hpp API, including exact pointer types.
#include "file.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static std::vector<char> read(const char *path)
{
	FILE *file = fopen(path, "rb"); CHECK(file); std::vector<char> result; int c;
	while ((c = fgetc(file)) != EOF) result.push_back(char(c));
	CHECK(!ferror(file)); CHECK(fclose(file) == 0); return result;
}
int main(int argc, char **argv)
{
	bool (*string_write)(const char *, const char *) = &Fossilize::write_string_to_file;
	bool (*buffer_write)(const char *, const void *, size_t) = &Fossilize::write_buffer_to_file;
	if (argc == 2)
	{
		// Separate modes establish that both historical helpers exhibit false success.
		if (strcmp(argv[1], "--dev-full-string") == 0) CHECK(!string_write("/dev/full", "buffered"));
		else if (strcmp(argv[1], "--dev-full-buffer") == 0) CHECK(!buffer_write("/dev/full", "buffered", 8));
		else CHECK(false);
		return 0;
	}
	CHECK(argc == 1);
	const char text[] = "A\nB\0C\n";
	CHECK(string_write("text", text));
#ifdef _WIN32
	const char expected[] = "A\r\nB";
#else
	const char expected[] = "A\nB";
#endif
	CHECK(read("text") == std::vector<char>(expected, expected + sizeof(expected) - 1));
	CHECK(buffer_write("binary", text, sizeof(text) - 1));
	CHECK(read("binary") == std::vector<char>(text, text + sizeof(text) - 1));
	CHECK(string_write("text", "")); CHECK(read("text").empty());
	CHECK(buffer_write("binary", "", 0)); CHECK(read("binary").empty());
	CHECK(!string_write("nonexistent-parent/output", "A"));
	CHECK(!buffer_write("nonexistent-parent/output", "A", 1));
	CHECK(remove("text") == 0); CHECK(remove("binary") == 0);
	return 0;
}
