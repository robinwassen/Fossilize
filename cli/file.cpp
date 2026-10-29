/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "file.hpp"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "file_operations.hpp"

namespace Fossilize
{
std::vector<uint8_t> load_buffer_from_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file)
		return {};

	fseek(file, 0, SEEK_END);
	long len = ftell(file);
	rewind(file);
	std::vector<uint8_t> ret(len);
	if (fread(ret.data(), 1, len, file) != size_t(len))
	{
		fclose(file);
		return {};
	}

	fclose(file);
	return ret;
}

namespace FileInternal
{
namespace
{
void *stdio_open(void *, const char *path, const char *mode) { return fopen(path, mode); }
size_t stdio_write(void *, const void *data, size_t size, void *file)
{
	return fwrite(data, 1, size, static_cast<FILE *>(file));
}
int stdio_flush(void *, void *file) { return fflush(static_cast<FILE *>(file)); }
int stdio_close(void *, void *file) { return fclose(static_cast<FILE *>(file)); }
const Operations stdio_ops = { nullptr, stdio_open, stdio_write, stdio_flush, stdio_close };

FileWriteResult write_file(const char *path, const char *mode, const void *data, size_t size,
                           const Operations &ops)
{
	FileWriteResult result = { true, FileWriteOperation::None, 0 };
	auto failure = [&](FileWriteOperation operation, int error) {
		if (result.success)
			result = { false, operation, error };
	};
	errno = 0;
	void *file = ops.open(ops.context, path, mode);
	if (!file)
		return { false, FileWriteOperation::Open, errno };

	// No early exit after open: explicit completion owns the only close attempt.
	// Operations are nonthrowing stdio-style callbacks; this path allocates nothing.
	if (size)
	{
		errno = 0;
		if (ops.write(ops.context, data, size, file) != size)
			failure(FileWriteOperation::Write, errno);
	}
	errno = 0;
	if (ops.flush(ops.context, file) != 0)
		failure(FileWriteOperation::Flush, errno);
	void *closing = file;
	file = nullptr; // A failed close still consumes the handle; never retry it.
	errno = 0;
	if (ops.close(ops.context, closing) != 0)
		failure(FileWriteOperation::Close, errno);
	return result;
}
}

FileWriteResult write_string(const char *path, const char *text, const Operations &ops)
{
	return write_file(path, "w", text, strlen(text), ops);
}
FileWriteResult write_buffer(const char *path, const void *data, size_t size, const Operations &ops)
{
	return write_file(path, "wb", data, size, ops);
}
bool legacy_result(FileWriteResult result)
{
	if (!result.success)
		errno = result.error;
	return result.success;
}
}

FileWriteResult write_string_to_file_result(const char *path, const char *text)
{
	return FileInternal::write_string(path, text, FileInternal::stdio_ops);
}
FileWriteResult write_buffer_to_file_result(const char *path, const void *data, size_t size)
{
	return FileInternal::write_buffer(path, data, size, FileInternal::stdio_ops);
}
bool write_string_to_file(const char *path, const char *text)
{
	return FileInternal::legacy_result(write_string_to_file_result(path, text));
}
bool write_buffer_to_file(const char *path, const void *data, size_t size)
{
	return FileInternal::legacy_result(write_buffer_to_file_result(path, data, size));
}
}
