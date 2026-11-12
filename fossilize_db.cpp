/* Copyright (c) 2019 Hans-Kristian Arntzen
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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#include "fossilize_db.hpp"
#include "fossilize_hasher.hpp"
#include "path.hpp"
#include "layer/utils.hpp"
#include "miniz.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <memory>
#include <mutex>
#include <atomic>
#include <limits>
#include <dirent.h>
#include <sys/stat.h>

#include "fossilize_inttypes.h"
#include "fossilize_errors.hpp"

using namespace std;

// So we can use SHA-1 for hashing blobs.
// Fossilize itself doesn't need this much.
// It only uses 18 hex characters.
// 2 for type and 16 for 64-bit Fossilize hash.
#define FOSSILIZE_BLOB_HASH_LENGTH 40
static_assert(FOSSILIZE_BLOB_HASH_LENGTH >= 32, "Blob hash length must be at least 32.");

namespace Fossilize
{
class ConditionalLockGuard
{
public:
	ConditionalLockGuard(std::mutex &lock_, bool enable_)
		: lock(lock_), enable(enable_)
	{
		if (enable)
			lock.lock();
	}

	~ConditionalLockGuard()
	{
		if (enable)
			lock.unlock();
	}

private:
	std::mutex &lock;
	bool enable;
};

struct PayloadHeader
{
	uint32_t payload_size;
	uint32_t format;
	uint32_t crc;
	uint32_t uncompressed_size;
};

struct ExportedMetadataBlock
{
	Hash hash;
	uint64_t file_offset;
	PayloadHeader payload;
};
static_assert(sizeof(ExportedMetadataBlock) % 8 == 0, "Alignment of ExportedMetadataBlock must be 8.");

// Encodes a unique list of hashes, so that we don't have to maintain per-process hashmaps
// when replaying concurrent databases.
using ExportedMetadataConcurrentPrimedBlock = Hash;
static_assert(sizeof(ExportedMetadataConcurrentPrimedBlock) % 8 == 0, "Alignment of ExportedMetadataPrimedBlock must be 8.");

struct ExportedMetadataList
{
	uint64_t offset;
	uint64_t count;
};
static_assert(sizeof(ExportedMetadataList) % 8 == 0, "Alignment of ExportedMetadataList must be 8.");

// Only for sanity checking when importing blobs, not a true file format.
static const uint64_t ExportedMetadataMagic = 0xb10bf05511153ull;
static const uint64_t ExportedMetadataMagicReport = 0xb10200005511153ull;
static const uint64_t ExportedMetadataMagicConcurrent = 0xb10b5f05511153ull;

struct ExportedMetadataHeader
{
	uint64_t magic;
	uint64_t size;
	ExportedMetadataList lists[RESOURCE_COUNT];
};
static_assert(sizeof(ExportedMetadataHeader) % 8 == 0, "Alignment of ExportedMetadataHeader must be 8.");

// Allow termination request if using the interface on a thread
std::atomic<bool> shutdown_requested;

struct DatabaseInterface::Impl
{
	std::unique_ptr<DatabaseInterface> whitelist;
	std::unique_ptr<DatabaseInterface> blacklist;
	std::vector<unsigned> sub_databases_in_whitelist;
	std::unordered_set<Hash> implicit_whitelisted[RESOURCE_COUNT];
	DatabaseMode mode;
	uint32_t whitelist_tag_mask = (1u << RESOURCE_SHADER_MODULE) |
	                              (1u << RESOURCE_GRAPHICS_PIPELINE) |
	                              (1u << RESOURCE_COMPUTE_PIPELINE) |
	                              (1u << RESOURCE_RAYTRACING_PIPELINE);

	const ExportedMetadataHeader *imported_concurrent_metadata = nullptr;

	std::vector<const ExportedMetadataHeader *> imported_metadata;
	const uint8_t *mapped_metadata = nullptr;
	size_t mapped_metadata_size = 0;
	bool overwrite_db_clear = false;
	DatabasePreparationReport report;
	std::vector<DiscoveryInput> failed_whitelist, failed_blacklist;
	std::unordered_map<unsigned, bool> required_inputs;
	bool report_imported = false;
	bool input_required(unsigned index) const
	{
		auto itr = required_inputs.find(index);
		return itr == required_inputs.end() || itr->second;
	}

	bool parse_imported_metadata(const void *data, size_t size);
};

DatabaseInterface::DatabaseInterface(DatabaseMode mode)
{
	impl = new Impl;
	impl->mode = mode;
}

bool DatabasePreparationReport::satisfies_required_scope(bool prefix) const
{
	if (inputs.empty())
		return false;
	for (auto &input : inputs)
	{
		if (input.required && input.state != DiscoveryState::Complete &&
		    !(prefix && !input.filter && input.opened && input.state == DiscoveryState::RecoveredPrefix))
			return false;
	}
	return true;
}

void DatabaseInterface::set_input_required(unsigned index, bool required)
{
	impl->required_inputs[index] = required;
}

DatabasePreparationReport DatabaseInterface::get_preparation_report() const
{
	auto report = impl->report;
	report.inputs.insert(report.inputs.end(), impl->failed_whitelist.begin(), impl->failed_whitelist.end());
	report.inputs.insert(report.inputs.end(), impl->failed_blacklist.begin(), impl->failed_blacklist.end());
	if (!impl->report_imported)
	{
		report.whitelist_tag_mask = impl->whitelist_tag_mask;
		report.whitelist_sensitive = impl->whitelist || !impl->sub_databases_in_whitelist.empty();
		report.blacklist_present = bool(impl->blacklist);
	}
	for (auto *filter : {impl->whitelist.get(), impl->blacklist.get()})
	{
		if (!filter)
			continue;
		for (auto input : filter->get_preparation_report().inputs)
		{
			input.filter = true;
			input.required = true;
			report.inputs.push_back(std::move(input));
		}
	}
	auto normalize = [](std::vector<DiscoveryKey> &keys) {
		std::sort(keys.begin(), keys.end());
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	};
	normalize(report.discovered);
	normalize(report.selected);
	std::sort(report.excluded.begin(), report.excluded.end(), [](const DiscoveryExcludedKey &a, const DiscoveryExcludedKey &b) {
		return a.key == b.key ? a.policy < b.policy : a.key < b.key;
	});
	report.excluded.erase(std::unique(report.excluded.begin(), report.excluded.end(),
	    [](const DiscoveryExcludedKey &a, const DiscoveryExcludedKey &b) {
		    return a.key == b.key && a.policy == b.policy;
	    }), report.excluded.end());
	report.discovery_complete = !report.inputs.empty();
	for (auto &input : report.inputs)
		report.discovery_complete &= input.state == DiscoveryState::Complete;
	report.unread_total_known = report.discovery_complete;
	return report;
}

void DatabaseInterface::observe_discovered(ResourceTag tag, Hash hash)
{
	DiscoveryKey key{tag, hash};
	impl->report.discovered.push_back(key);
	if (test_resource_filter(tag, hash))
		impl->report.selected.push_back(key);
	else
	{
		bool whitelist_rejected = (impl->whitelist_tag_mask & (1u << tag)) &&
		                          (impl->whitelist || !impl->sub_databases_in_whitelist.empty()) &&
		                          !(impl->whitelist && impl->whitelist->has_entry(tag, hash)) &&
		                          !impl->implicit_whitelisted[tag].count(hash);
		impl->report.excluded.push_back({key, whitelist_rejected ? DiscoveryExclusion::Whitelist : DiscoveryExclusion::Blacklist});
	}
}

void DatabaseInterface::set_overwrite_db_clear(bool value)
{
	impl->overwrite_db_clear = value;
}

bool DatabaseInterface::get_overwrite_db_clear() const
{
	return impl->overwrite_db_clear;
}

bool DatabaseInterface::has_sub_databases()
{
	return false;
}

DatabaseInterface *DatabaseInterface::get_sub_database(unsigned)
{
	return nullptr;
}

void DatabaseInterface::set_whitelist_tag_mask(uint32_t mask)
{
	impl->whitelist_tag_mask = mask;
}

bool DatabaseInterface::load_whitelist_database(const char *path)
{
	impl->failed_whitelist.clear();
	if (impl->mode != DatabaseMode::ReadOnly)
		return false;

	if (!impl->imported_metadata.empty())
	{
		LOGE_LEVEL("Cannot use imported metadata together with whitelists.\n");
		return false;
	}

	impl->whitelist.reset(create_stream_archive_database(path, DatabaseMode::ReadOnly));

	if (!impl->whitelist)
		return false;

	if (!impl->whitelist->prepare())
	{
		for (auto input : impl->whitelist->get_preparation_report().inputs)
		{
			input.required = true;
			input.filter = true;
			impl->failed_whitelist.push_back(std::move(input));
		}
		impl->whitelist.reset();
		return false;
	}

	// Ignore shader modules if whitelist doesn't contain any.
	size_t hash_count;
	if (impl->whitelist->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &hash_count, nullptr))
		if (hash_count == 0)
			set_whitelist_tag_mask(impl->whitelist_tag_mask & ~(1u << RESOURCE_SHADER_MODULE));

	return true;
}

bool DatabaseInterface::load_blacklist_database(const char *path)
{
	impl->failed_blacklist.clear();
	if (impl->mode != DatabaseMode::ReadOnly)
		return false;

	if (!impl->imported_metadata.empty())
	{
		LOGE_LEVEL("Cannot use imported metadata together with blacklists.\n");
		return false;
	}

	impl->blacklist.reset(create_stream_archive_database(path, DatabaseMode::ReadOnly));

	if (!impl->blacklist)
		return false;

	if (!impl->blacklist->prepare())
	{
		for (auto input : impl->blacklist->get_preparation_report().inputs)
		{
			input.required = true;
			input.filter = true;
			impl->failed_blacklist.push_back(std::move(input));
		}
		impl->blacklist.reset();
		return false;
	}

	return true;
}

void DatabaseInterface::promote_sub_database_to_whitelist(unsigned index)
{
	if (impl->mode != DatabaseMode::ReadOnly)
		return;

	impl->sub_databases_in_whitelist.push_back(index);
}

bool DatabaseInterface::add_to_implicit_whitelist(DatabaseInterface &iface)
{
	std::vector<Hash> hashes;
	size_t size = 0;

	const auto promote = [&](ResourceTag tag) -> bool {
		if (!iface.get_hash_list_for_resource_tag(tag, &size, nullptr))
			return false;
		hashes.resize(size);
		if (!iface.get_hash_list_for_resource_tag(tag, &size, hashes.data()))
			return false;
		for (auto &h : hashes)
			impl->implicit_whitelisted[tag].insert(h);
		return true;
	};

	if (!promote(RESOURCE_SHADER_MODULE))
		return false;
	if (!promote(RESOURCE_GRAPHICS_PIPELINE))
		return false;
	if (!promote(RESOURCE_COMPUTE_PIPELINE))
		return false;

	return true;
}

void DatabaseInterface::add_to_implicit_whitelist(ResourceTag tag, Hash hash)
{
	impl->implicit_whitelisted[tag].insert(hash);
}

DatabaseInterface::~DatabaseInterface()
{
#ifdef _WIN32
	if (impl->mapped_metadata)
		UnmapViewOfFile(impl->mapped_metadata);
#else
	if (impl->mapped_metadata)
		munmap(const_cast<uint8_t *>(impl->mapped_metadata), impl->mapped_metadata_size);
#endif
	delete impl;
}

bool DatabaseInterface::test_resource_filter(ResourceTag tag, Hash hash) const
{
	if ((impl->whitelist_tag_mask & (1u << tag)) != 0)
	{
		bool whitelist_sensitive = impl->whitelist || !impl->sub_databases_in_whitelist.empty();
		if (whitelist_sensitive)
		{
			bool whitelisted = (impl->whitelist && impl->whitelist->has_entry(tag, hash)) ||
			                   (impl->implicit_whitelisted[tag].count(hash) != 0);
			if (!whitelisted)
				return false;
		}
	}

	if (impl->blacklist && impl->blacklist->has_entry(tag, hash))
		return false;

	return true;
}

intptr_t DatabaseInterface::invalid_metadata_handle()
{
#ifdef _WIN32
	return 0;
#else
	return -1;
#endif
}

bool DatabaseInterface::metadata_handle_is_valid(intptr_t handle)
{
#ifdef _WIN32
	return handle != 0;
#else
	return handle >= 0;
#endif
}

static std::atomic<uint32_t> name_counter;
void DatabaseInterface::get_unique_os_export_name(char *buffer, size_t size)
{
	unsigned counter_value = name_counter.fetch_add(1);
#ifdef _WIN32
	snprintf(buffer, size, "fossilize-replayer-%lu-%u", GetCurrentProcessId(), counter_value);
#else
	snprintf(buffer, size, "/fossilize-replayer-%d-%u", getpid(), counter_value);
#endif
}

// Private exact-commit report record. Fixed-width scalars and bounded strings; no pointers.
static std::vector<uint8_t> encode_report(const DatabasePreparationReport &report)
{
	std::vector<uint8_t> bytes;
	auto put = [&](uint64_t value) {
		for (unsigned i = 0; i < 8; i++)
			bytes.push_back(uint8_t(value >> (i * 8)));
	};
	put(1);
	put(report.whitelist_tag_mask);
	put(report.whitelist_sensitive);
	put(report.blacklist_present);
	put(report.inputs.size());
	for (auto &input : report.inputs)
	{
		put(input.path.size());
		bytes.insert(bytes.end(), input.path.begin(), input.path.end());
		put(input.index);
		put(input.required);
		put(input.filter);
		put(input.opened);
		put(uint32_t(input.state));
		put(uint32_t(input.reason));
		put(input.raw_error_known);
		put(uint64_t(input.raw_error));
		put(input.boundary_known);
		put(input.prefix_end);
		put(input.unread_offset);
		put(input.total_known);
		put(input.retained_count);
	}
	for (auto *keys : {&report.discovered, &report.selected})
	{
		put(keys->size());
		for (auto &key : *keys)
		{
			put(key.tag);
			put(key.hash);
		}
	}
	put(report.excluded.size());
	for (auto &entry : report.excluded)
	{
		put(entry.key.tag);
		put(entry.key.hash);
		put(uint32_t(entry.policy));
	}
	while (bytes.size() % 8)
		bytes.push_back(0);
	return bytes;
}

static bool decode_report(const uint8_t *data, size_t size, DatabasePreparationReport &report)
{
	size_t offset = 0;
	bool valid = true;
	auto get = [&]() -> uint64_t {
		if (size - offset < 8)
		{
			valid = false;
			return 0;
		}
		uint64_t value = 0;
		for (unsigned i = 0; i < 8; i++)
			value |= uint64_t(data[offset++]) << (8 * i);
		return value;
	};
	auto boolean = [&]() -> bool {
		auto value = get();
		valid &= value <= 1;
		return value != 0;
	};
	if (get() != 1)
		return false;
	auto mask = get();
	if (mask > UINT32_MAX)
		return false;
	report.whitelist_tag_mask = uint32_t(mask);
	report.whitelist_sensitive = boolean();
	report.blacklist_present = boolean();
	auto count = get();
	if (count > size / 8)
		return false;
	for (uint64_t i = 0; i < count && valid; i++)
	{
		DiscoveryInput input;
		auto length = get();
		if (!valid || length > size - offset)
			return false;
		input.path.assign(reinterpret_cast<const char *>(data + offset), size_t(length));
		offset += size_t(length);
		auto index = get();
		if (index > UINT32_MAX)
			return false;
		input.index = unsigned(index);
		input.required = boolean();
		input.filter = boolean();
		input.opened = boolean();
		auto state = get();
		auto reason = get();
		if (state > uint32_t(DiscoveryState::Failed) || reason > uint32_t(DiscoveryReason::Cancelled))
			return false;
		input.state = DiscoveryState(state);
		input.reason = DiscoveryReason(reason);
		input.raw_error_known = boolean();
		input.raw_error = int64_t(get());
		input.boundary_known = boolean();
		input.prefix_end = get();
		input.unread_offset = get();
		input.total_known = boolean();
		input.retained_count = get();
		report.inputs.push_back(std::move(input));
	}
	for (auto *keys : {&report.discovered, &report.selected})
	{
		count = get();
		if (count > size / 16)
			return false;
		for (uint64_t i = 0; i < count && valid; i++)
		{
			auto tag = get();
			auto hash = get();
			if (tag >= RESOURCE_COUNT)
				return false;
			keys->push_back({ResourceTag(tag), hash});
		}
	}
	count = get();
	if (count > size / 24)
		return false;
	for (uint64_t i = 0; i < count && valid; i++)
	{
		auto tag = get();
		auto hash = get();
		auto policy = get();
		if (tag >= RESOURCE_COUNT || policy > uint32_t(DiscoveryExclusion::Blacklist))
			return false;
		report.excluded.push_back({{ResourceTag(tag), hash}, DiscoveryExclusion(policy)});
	}
	return valid && size - offset < 8;
}

intptr_t DatabaseInterface::export_metadata_to_os_handle(const char *name)
{
	if (impl->mode != DatabaseMode::ReadOnly)
		return invalid_metadata_handle();

	auto report_bytes = encode_report(get_preparation_report());
	size_t payload_size = compute_exported_metadata_size();
	if (!payload_size)
		return invalid_metadata_handle();
	if (report_bytes.size() > SIZE_MAX - sizeof(ExportedMetadataHeader) ||
		payload_size > SIZE_MAX - sizeof(ExportedMetadataHeader) - report_bytes.size())
		return invalid_metadata_handle();
	size_t size = payload_size + sizeof(ExportedMetadataHeader) + report_bytes.size();
	if (!size)
		return invalid_metadata_handle();

#ifdef _WIN32
	HANDLE mapping_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(uint64_t(size) >> 32), DWORD(size), name);
	if (!mapping_handle)
		return invalid_metadata_handle();

	void *mapped = MapViewOfFile(mapping_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
	if (!mapped)
	{
		CloseHandle(mapping_handle);
		return invalid_metadata_handle();
	}

	auto *report_header = reinterpret_cast<ExportedMetadataHeader *>(static_cast<uint8_t *>(mapped) + payload_size);
	*report_header = {};
	report_header->magic = ExportedMetadataMagicReport;
	report_header->size = sizeof(*report_header) + report_bytes.size();
	memcpy(report_header + 1, report_bytes.data(), report_bytes.size());
	if (!write_exported_metadata(mapped, payload_size))
	{
		LOGE_LEVEL("Failed to write metadata block.\n");
		UnmapViewOfFile(mapped);
		CloseHandle(mapping_handle);
		return invalid_metadata_handle();
	}

	UnmapViewOfFile(mapped);
	return reinterpret_cast<intptr_t>(mapping_handle);
#elif !defined(ANDROID)
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
	{
		LOGE_LEVEL("Failed to create shared memory.\n");
		return invalid_metadata_handle();
	}

	if (shm_unlink(name) < 0)
	{
		LOGE_LEVEL("Failed to unlink SHM block.\n");
		close(fd);
		return invalid_metadata_handle();
	}

	if (ftruncate(fd, size) < 0)
	{
		LOGE_LEVEL("Failed to allocate space for metadata block.\n");
		close(fd);
		return invalid_metadata_handle();
	}

	void *mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED)
	{
		LOGE_LEVEL("Failed to map metadata block.\n");
		close(fd);
		return invalid_metadata_handle();
	}

	auto *report_header = reinterpret_cast<ExportedMetadataHeader *>(static_cast<uint8_t *>(mapped) + payload_size);
	*report_header = {};
	report_header->magic = ExportedMetadataMagicReport;
	report_header->size = sizeof(*report_header) + report_bytes.size();
	memcpy(report_header + 1, report_bytes.data(), report_bytes.size());
	if (!write_exported_metadata(mapped, payload_size))
	{
		LOGE_LEVEL("Failed to write metadata block.\n");
		munmap(mapped, size);
		close(fd);
		return invalid_metadata_handle();
	}

	munmap(mapped, size);
	return intptr_t(fd);
#else
	return invalid_metadata_handle();
#endif
}

size_t DatabaseInterface::compute_exported_metadata_size() const
{
	return 0;
}

bool DatabaseInterface::write_exported_metadata(void *, size_t) const
{
	return false;
}

void DatabaseInterface::add_imported_metadata(const ExportedMetadataHeader *header)
{
	impl->imported_metadata.push_back(header);
}

bool DatabaseInterface::set_bucket_path(const char *, const char *)
{
	return false;
}

void DatabaseInterface::set_bucket_info(const char *)
{
}

static size_t deduce_imported_size(const void *mapped, size_t maximum_size)
{
	size_t total_size = 0;
	while (sizeof(ExportedMetadataHeader) <= maximum_size - total_size)
	{
		auto *header = reinterpret_cast<const ExportedMetadataHeader *>(static_cast<const uint8_t *>(mapped) + total_size);
		if (header->size < sizeof(*header) || header->size % 8 || header->size > maximum_size - total_size)
			break;
		if (header->magic != ExportedMetadataMagic && header->magic != ExportedMetadataMagicConcurrent && header->magic != ExportedMetadataMagicReport)
			break;
		total_size += header->size;
	}

	return total_size;
}

bool DatabaseInterface::Impl::parse_imported_metadata(const void *data_, size_t size_)
{
	std::vector<const ExportedMetadataHeader *> headers;
	DatabasePreparationReport report;
	bool got_report = false;
	auto *data = static_cast<const uint8_t *>(data_);

	// Imported size might be rounded up to page size, so find exact bound first.
	size_t size = deduce_imported_size(data_, size_);

	if (size < sizeof(ExportedMetadataHeader))
		return false;

	auto *concurrent_header = reinterpret_cast<const ExportedMetadataHeader *>(data);
	if (concurrent_header->magic == ExportedMetadataMagicConcurrent)
	{
		for (auto &list : concurrent_header->lists)
			if (list.offset % 8 || (list.count && list.offset < sizeof(*concurrent_header)) ||
				list.offset > concurrent_header->size || list.count > (concurrent_header->size-list.offset)/sizeof(Hash)) return false;
		data += concurrent_header->size;
		size -= concurrent_header->size;
	}
	else
		concurrent_header = nullptr;

	while (size != 0)
	{
		if (size < sizeof(ExportedMetadataHeader))
			return false;

		auto *header = reinterpret_cast<const ExportedMetadataHeader *>(data);
		if (header->magic == ExportedMetadataMagicReport)
		{
			if (got_report || header->size != size || !decode_report(data + sizeof(*header), size - sizeof(*header), report))
				return false;
			got_report = true;
			size = 0;
			break;
		}
		if (header->magic != ExportedMetadataMagic)
			return false;
		if (header->size > size)
			return false;

		for (auto &list : header->lists)
			if (list.offset % 8 || (list.count && list.offset < sizeof(*header)) || list.offset > header->size ||
			    list.count > (header->size - list.offset) / sizeof(ExportedMetadataBlock))
				return false;

		data += header->size;
		size -= header->size;
		headers.push_back(header);
	}

	if (!got_report)
		return false;
#ifdef _WIN32
	if (mapped_metadata)
		UnmapViewOfFile(mapped_metadata);
#else
	if (mapped_metadata)
		munmap(const_cast<uint8_t *>(mapped_metadata), mapped_metadata_size);
#endif
	mapped_metadata = static_cast<const uint8_t *>(data_);
	mapped_metadata_size = size_;
	this->report = std::move(report);
	report_imported = true;
	imported_metadata = std::move(headers);
	imported_concurrent_metadata = concurrent_header;
	return true;
}

bool DatabaseInterface::import_metadata_from_os_handle(intptr_t handle)
{
	if (impl->whitelist || impl->blacklist)
	{
		LOGE_LEVEL("Cannot use imported metadata along with white- or blacklists.\n");
		return false;
	}

#ifdef _WIN32
	HANDLE mapping_handle = reinterpret_cast<HANDLE>(handle);
	void *mapped = MapViewOfFile(mapping_handle, FILE_MAP_READ, 0, 0, 0);
	if (!mapped)
		return false;

	// There is no documented way to query size of a file mapping handle in Windows (?!?!), so rely on parsing the metadata.
	// As long as we find valid records within the bounds of the VirtualQuery, we will be fine.
	MEMORY_BASIC_INFORMATION info;
	if (!VirtualQuery(mapped, &info, sizeof(info)))
	{
		UnmapViewOfFile(mapped);
		return false;
	}

	bool ret = impl->parse_imported_metadata(mapped, info.RegionSize);
	if (ret)
		CloseHandle(mapping_handle);
	else
		UnmapViewOfFile(mapped);
	return ret;
#else
	int fd = int(handle);
	struct stat s = {};
	if (fstat(fd, &s) < 0)
		return false;

	if (s.st_size == 0)
		return false;

	void *mapped = mmap(nullptr, s.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED)
		return false;

	bool ret = impl->parse_imported_metadata(mapped, s.st_size);
	if (ret)
		close(fd);
	else
		munmap(mapped, s.st_size);

	return ret;
#endif
}

struct DumbDirectoryDatabase : DatabaseInterface
{
	DumbDirectoryDatabase(const string &base, DatabaseMode mode_)
		: DatabaseInterface(mode_), base_directory(base), mode(mode_)
	{
		if (mode == DatabaseMode::ExclusiveOverWrite)
			mode = DatabaseMode::OverWrite;
	}

	void flush() override
	{
	}

	bool overwrite_db_clear()
	{
		DIR *dp = opendir(base_directory.c_str());
		if (!dp)
			return false;

		dirent *next_file;

		while ((next_file = readdir(dp)) != nullptr)
		{
			if (strcmp(Path::ext(next_file->d_name).c_str(), "json") != 0)
				continue;

			if (remove(Path::join(base_directory, next_file->d_name).c_str()) != 0)
				return false;
		}

		closedir(dp);
		return true;
	}

	bool prepare() override
	{
		impl->report = {};
		DiscoveryInput input;
		input.path = base_directory;
		input.required = impl->input_required(0);
		impl->report.inputs.push_back(std::move(input));
		auto &observation = impl->report.inputs.front();
		observation.state = DiscoveryState::Failed;
		observation.reason = DiscoveryReason::Open;
		if (mode == DatabaseMode::OverWrite)
		{
			if (!Path::mkdir(base_directory) || (impl->overwrite_db_clear && !overwrite_db_clear()))
				return false;

			return true;
		}

		DIR *dp = opendir(base_directory.c_str());
		if (!dp)
		{
			observation.raw_error_known = true;
			observation.raw_error = errno;
			return false;
		}
		observation.opened = true;
		for (;;)
		{
			errno = 0;
			auto *pEntry = readdir(dp);
			if (!pEntry)
			{
				int error = errno;
				closedir(dp);
				if (error)
				{
					observation.reason = DiscoveryReason::Read;
					observation.raw_error_known = true;
					observation.raw_error = error;
					return false;
				}
				break;
			}
			if (shutdown_requested.load(std::memory_order_relaxed))
			{
				closedir(dp);
				observation.reason = DiscoveryReason::Cancelled;
				return false;
			}

			if (pEntry->d_type == DT_UNKNOWN)
			{
				struct stat status = {};
				auto path = Path::join(base_directory, pEntry->d_name);
#ifdef _WIN32
				// Classify the entry itself, rather than following a reparse point.
				DWORD attributes = GetFileAttributesA(path.c_str());
				if (attributes == INVALID_FILE_ATTRIBUTES)
				{
					DWORD error = GetLastError();
					closedir(dp);
					observation.reason = DiscoveryReason::Read;
					observation.raw_error_known = true;
					observation.raw_error = error;
					return false;
				}
				if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
					continue;
				if (::stat(path.c_str(), &status) != 0)
#else
				if (::lstat(path.c_str(), &status) != 0)
#endif
				{
					int error = errno;
					closedir(dp);
					observation.reason = DiscoveryReason::Read;
					observation.raw_error_known = true;
					observation.raw_error = error;
					return false;
				}
				if ((status.st_mode & S_IFMT) != S_IFREG)
					continue;
			}
			else if (pEntry->d_type != DT_REG)
				continue;

			unsigned tag;
			uint64_t value;
			if (sscanf(pEntry->d_name, "%x.%" SCNx64 ".json", &tag, &value) != 2)
				continue;

			if (tag >= RESOURCE_COUNT)
				continue;

			observe_discovered(static_cast<ResourceTag>(tag), value);
			observation.retained_count++;
			if (test_resource_filter(static_cast<ResourceTag>(tag), value))
				seen_blobs[tag].insert(value);
		}

		observation.state = DiscoveryState::Complete;
		observation.reason = DiscoveryReason::None;
		observation.total_known = true;
		return true;
	}

	bool has_entry(ResourceTag tag, Hash hash) override
	{
		if (!test_resource_filter(tag, hash))
			return false;
		return seen_blobs[tag].count(hash) != 0;
	}

	bool read_entry(ResourceTag tag, Hash hash, size_t *blob_size, void *blob, PayloadReadFlags flags) override
	{
		if ((flags & PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT) != 0)
			return false;

		if (mode != DatabaseMode::ReadOnly)
			return false;

		if (!has_entry(tag, hash))
			return false;

		if (!blob_size)
			return false;

		char filename[25]; // 2 digits + "." + 16 digits + ".json" + null
		sprintf(filename, "%02x.%016" PRIx64 ".json", static_cast<unsigned>(tag), hash);
		auto path = Path::join(base_directory, filename);

		FILE *file = fopen(path.c_str(), "rb");
		if (!file)
		{
			LOGE_LEVEL("Failed to open file: %s\n", path.c_str());
			return false;
		}

		if (fseek(file, 0, SEEK_END) < 0)
		{
			fclose(file);
			LOGE_LEVEL("Failed to seek in file: %s\n", path.c_str());
			return false;
		}

		size_t file_size = size_t(ftell(file));
		rewind(file);

		if (blob && *blob_size < file_size)
		{
			fclose(file);
			return false;
		}
		*blob_size = file_size;

		if (blob)
		{
			if (fread(blob, 1, file_size, file) != file_size)
			{
				fclose(file);
				return false;
			}
		}

		fclose(file);
		return true;
	}

	bool write_entry(ResourceTag tag, Hash hash, const void *blob, size_t size, PayloadWriteFlags flags) override
	{
		if ((flags & PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT) != 0)
			return false;

		if (mode == DatabaseMode::ReadOnly)
			return false;

		if (has_entry(tag, hash))
			return true;

		char filename[25]; // 2 digits + "." + 16 digits + ".json" + null
		sprintf(filename, "%02x.%016" PRIx64 ".json", static_cast<unsigned>(tag), hash);
		auto path = Path::join(base_directory, filename);

		FILE *file = fopen(path.c_str(), "wb");
		if (!file)
		{
			LOGE_LEVEL("Failed to write serialized state to disk (%s).\n", path.c_str());
			return false;
		}

		if (fwrite(blob, 1, size, file) != size)
		{
			LOGE_LEVEL("Failed to write serialized state to disk.\n");
			fclose(file);
			return false;
		}

		fclose(file);
		return true;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, size_t *hash_count, Hash *hashes) override
	{
		size_t size = seen_blobs[tag].size();
		if (hashes)
		{
			if (size != *hash_count)
				return false;
		}
		else
			*hash_count = size;

		if (hashes)
		{
			Hash *iter = hashes;
			for (auto &blob : seen_blobs[tag])
				*iter++ = blob;

			// Make replay more deterministic.
			sort(hashes, hashes + size);
		}
		return true;
	}

	const char *get_db_path_for_hash(ResourceTag tag, Hash hash) override
	{
		if (!has_entry(tag, hash))
			return nullptr;

		return base_directory.c_str();
	}

	string base_directory;
	DatabaseMode mode;
	unordered_set<Hash> seen_blobs[RESOURCE_COUNT];
};

DatabaseInterface *create_dumb_folder_database(const char *directory_path, DatabaseMode mode)
{
	auto *db = new DumbDirectoryDatabase(directory_path, mode);
	return db;
}

struct ZipDatabase : DatabaseInterface
{
	ZipDatabase(const string &path_, DatabaseMode mode_)
		: DatabaseInterface(mode_), path(path_), mode(mode_)
	{
		if (mode == DatabaseMode::ExclusiveOverWrite)
			mode = DatabaseMode::OverWrite;
		mz_zip_zero_struct(&mz);
	}

	~ZipDatabase()
	{
		if (alive)
		{
			if (mode != DatabaseMode::ReadOnly)
			{
				if (!mz_zip_writer_finalize_archive(&mz))
					LOGE_LEVEL("Failed to finalize archive.\n");
			}

			if (!mz_zip_end(&mz))
				LOGE_LEVEL("mz_zip_end failed!\n");
		}
	}

	void flush() override
	{
	}

	static bool string_is_hex(const char *str)
	{
		while (*str)
		{
			if (!isxdigit(uint8_t(*str)))
				return false;
			str++;
		}
		return true;
	}

	bool prepare() override
	{
		impl->report = {};
		DiscoveryInput input;
		input.path = path;
		input.required = impl->input_required(0);
		impl->report.inputs.push_back(std::move(input));
		auto &observation = impl->report.inputs.front();
		observation.state = DiscoveryState::Failed;
		observation.reason = DiscoveryReason::Open;
		if (mode != DatabaseMode::OverWrite && mz_zip_reader_init_file(&mz, path.c_str(), 0))
		{
			// We have an existing archive.
			observation.opened = true;
			observation.state = DiscoveryState::Complete;
			observation.reason = DiscoveryReason::None;
			observation.total_known = true;
			unsigned files = mz_zip_reader_get_num_files(&mz);
			char filename[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE] = {};

			for (unsigned i = 0; i < files; i++)
			{
				if (shutdown_requested.load(std::memory_order_relaxed))
				{
					observation.state = DiscoveryState::Failed;
					observation.reason = DiscoveryReason::Cancelled;
					observation.total_known = false;
					return false;
				}

				if (mz_zip_reader_is_file_a_directory(&mz, i))
					continue;

				mz_zip_reader_get_filename(&mz, i, filename, sizeof(filename));
				size_t len = strlen(filename);
				if (len != FOSSILIZE_BLOB_HASH_LENGTH)
					continue;

				if (!string_is_hex(filename))
					continue;

				mz_zip_archive_file_stat s;
				if (!mz_zip_reader_file_stat(&mz, i, &s))
				{
					observation.state = DiscoveryState::Failed;
					observation.reason = DiscoveryReason::Read;
					observation.total_known = false;
					continue;
				}

				char tag_str[16 + 1] = {};
				char value_str[16 + 1] = {};
				memcpy(tag_str, filename + FOSSILIZE_BLOB_HASH_LENGTH - 32, 16);
				memcpy(value_str, filename + FOSSILIZE_BLOB_HASH_LENGTH - 16, 16);

				auto tag = unsigned(strtoul(tag_str, nullptr, 16));
				if (tag >= RESOURCE_COUNT)
					continue;
				uint64_t value = strtoull(value_str, nullptr, 16);

				observe_discovered(static_cast<ResourceTag>(tag), value);
				observation.retained_count++;
				if (test_resource_filter(static_cast<ResourceTag>(tag), value))
					seen_blobs[tag].emplace(value, Entry{i, size_t(s.m_uncomp_size)});
			}

			// In-place update the archive. Should we consider emitting a new archive instead?
			if (!mz_zip_writer_init_from_reader(&mz, path.c_str()))
			{
				LOGE_LEVEL("Failed to initialize ZIP writer from reader.\n");
				mz_zip_end(&mz);
				return false;
			}

			alive = true;
		}
		else if (mode != DatabaseMode::ReadOnly)
		{
			if (!mz_zip_writer_init_file(&mz, path.c_str(), 0))
			{
				LOGE_LEVEL("Failed to open ZIP archive for writing. Cannot serialize anything to disk.\n");
				return false;
			}

			alive = true;

			for (auto &blob : seen_blobs)
				blob.clear();
		}

		return true;
	}

	bool read_entry(ResourceTag tag, Hash hash, size_t *blob_size, void *blob, PayloadReadFlags flags) override
	{
		if ((flags & PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT) != 0)
			return false;

		if (!alive || mode != DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr == end(seen_blobs[tag]))
			return false;

		if (!blob_size)
			return false;

		if (blob && *blob_size < itr->second.size)
			return false;
		*blob_size = itr->second.size;

		if (blob)
		{
			if (!mz_zip_reader_extract_to_mem(&mz, itr->second.index, blob, itr->second.size, 0))
			{
				LOGE_LEVEL("Failed to extract blob.\n");
				return false;
			}
		}

		return true;
	}

	bool write_entry(ResourceTag tag, Hash hash, const void *blob, size_t size, PayloadWriteFlags flags) override
	{
		if ((flags & PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT) != 0)
			return false;

		if (!alive || mode == DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr != end(seen_blobs[tag]))
			return true;

		char str[FOSSILIZE_BLOB_HASH_LENGTH + 1]; // 40 digits + null
		sprintf(str, "%0*x", FOSSILIZE_BLOB_HASH_LENGTH - 16, tag);
		sprintf(str + FOSSILIZE_BLOB_HASH_LENGTH - 16, "%016" PRIx64, hash);

		unsigned mz_flags;
		if ((flags & PAYLOAD_WRITE_COMPRESS_BIT) != 0)
		{
			if ((flags & PAYLOAD_WRITE_BEST_COMPRESSION_BIT) != 0)
				mz_flags = MZ_BEST_COMPRESSION;
			else
				mz_flags = MZ_BEST_SPEED;
		}
		else
			mz_flags = MZ_NO_COMPRESSION;

		if (!mz_zip_writer_add_mem(&mz, str, blob, size, mz_flags))
		{
			LOGE_LEVEL("Failed to add blob to cache.\n");
			return false;
		}

		// The index is irrelevant, we're not going to read from this archive any time soon.
		if (test_resource_filter(static_cast<ResourceTag>(tag), hash))
			seen_blobs[tag].emplace(hash, Entry{~0u, size});
		return true;
	}

	bool has_entry(ResourceTag tag, Hash hash) override
	{
		if (!test_resource_filter(tag, hash))
			return false;
		return seen_blobs[tag].count(hash) != 0;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, size_t *hash_count, Hash *hashes) override
	{
		size_t size = seen_blobs[tag].size();
		if (hashes)
		{
			if (size != *hash_count)
				return false;
		}
		else
			*hash_count = size;

		if (hashes)
		{
			Hash *iter = hashes;
			for (auto &blob : seen_blobs[tag])
				*iter++ = blob.first;

			// Make replay more deterministic.
			sort(hashes, hashes + size);
		}
		return true;
	}

	const char *get_db_path_for_hash(ResourceTag tag, Hash hash) override
	{
		if (!has_entry(tag, hash))
			return nullptr;

		return path.c_str();
	}

	string path;
	mz_zip_archive mz;

	struct Entry
	{
		unsigned index;
		size_t size;
	};

	unordered_map<Hash, Entry> seen_blobs[RESOURCE_COUNT];
	DatabaseMode mode;
	bool alive = false;
};

DatabaseInterface *create_zip_archive_database(const char *path, DatabaseMode mode)
{
	auto *db = new ZipDatabase(path, mode);
	return db;
}

/* Fossilize StreamArchive database format version 6:
 *
 * The file consists of a header, followed by an unlimited series of "entries".
 *
 * All multi-byte entities are little-endian.
 *
 * The file header is as follows:
 *
 * Field           Type           Description
 * -----           ----           -----------
 * magic_number    uint8_t[12]    Constant value: "\x81""FOSSILIZEDB"
 * unused1         uint8_t        Currently unused. Must be zero.
 * unused2         uint8_t        Currently unused. Must be zero.
 * unused3         uint8_t        Currently unused. Must be zero.
 * version         uint8_t        StreamArchive version: 6
 *
 *
 * Each entry follows this format:
 *
 * Field           Type                              Description
 * -----           ----                              -----------
 * tag             unsigned char[40 - hash_bytes]    Application-defined 'tag' which groups entry types. Stored as hexadecimal ASCII.
 * hash            unsigned char[hash_bytes]         Application-defined 'hash' to identify this entry. Stored as hexadecimal ASCII.
 * stored_size     uint32_t                          Size of the payload as stored in this file.
 * flags           uint32_t                          Flags for this entry (e.g. compression). See below.
 * crc32           uint32_t                          CRC32 of the payload as stored in this file. If zero, checksum is not checked when reading.
 * payload_size    uint32_t                          Size of this payload after decompression.
 * payload         uint8_t[stored_size]              Entry data.
 *
 * The flags field must contain one of:
 *     0x1: No compression.
 *     0x2: Deflate compression.
 *
 * Entries should have a unique tag and hash combination. Implementations may
 * ignore duplicated tag and hash combinations.
 *
 * It is acceptable for the last entry to be truncated. In this case, that
 * entry should be ignored.
 */

static const uint8_t stream_reference_magic_and_version[16] = {
	0x81, 'F', 'O', 'S',
	'S', 'I', 'L', 'I',
	'Z', 'E', 'D', 'B',
	0, 0, 0,
	FOSSILIZE_FORMAT_VERSION,
};

struct StreamArchive : DatabaseInterface
{
	enum { MagicSize = sizeof(stream_reference_magic_and_version) };
	enum { FOSSILIZE_COMPRESSION_NONE = 1, FOSSILIZE_COMPRESSION_DEFLATE = 2 };

	struct PayloadHeaderRaw
	{
		uint8_t data[4 * 4];
	};

	struct Entry
	{
		uint64_t offset;
		PayloadHeader header;
	};

	StreamArchive(const string &path_, DatabaseMode mode_)
		: DatabaseInterface(mode_), path(path_), mode(mode_)
	{
	}

	~StreamArchive()
	{
		free(zlib_buffer);
		if (file)
			fclose(file);
	}

	void flush() override
	{
		if (file && mode != DatabaseMode::ReadOnly)
			fflush(file);
	}

	bool prepare() override
	{
		if (!impl->report_imported)
			impl->report = {};
		if (impl->report.inputs.empty())
		{
			DiscoveryInput input;
			input.path = path;
			input.required = impl->input_required(0);
			impl->report.inputs.push_back(std::move(input));
		}
		auto &observation = impl->report.inputs.front();
		auto fail = [&](DiscoveryReason reason, bool raw = false) {
			observation.state = DiscoveryState::Failed;
			observation.reason = reason;
			observation.raw_error_known = raw;
			observation.raw_error = raw ? errno : 0;
			observation.total_known = false;
			return false;
		};
		if (!impl->imported_metadata.empty() && mode != DatabaseMode::ReadOnly)
			return false;
		if (impl->imported_metadata.size() > 1)
			return false;
		if (impl->report_imported && (impl->report.inputs.empty() || impl->report.inputs.front().path != path))
			return fail(DiscoveryReason::InvalidMagic);

		errno = 0;
		switch (mode)
		{
		case DatabaseMode::ReadOnly:
#if _WIN32
			{
				file = nullptr;
				int fd = _open(path.c_str(), _O_BINARY | _O_RDONLY |
				               (impl->imported_metadata.empty() ? _O_SEQUENTIAL : _O_RANDOM),
				               _S_IREAD);
				if (fd >= 0)
					file = _fdopen(fd, "rb");
			}
#else
			file = fopen(path.c_str(), "rb");
#endif
			break;

		case DatabaseMode::Append:
			file = fopen(path.c_str(), "r+b");
			// r+b on empty file does not seem to work on Windows, so just fall back to wb.
			if (!file)
				file = fopen(path.c_str(), "wb");
			break;

		case DatabaseMode::AppendWithReadOnlyAccess:
			return false;

		case DatabaseMode::OverWrite:
			file = fopen(path.c_str(), "wb");
			break;

		case DatabaseMode::ExclusiveOverWrite:
		{
#ifdef _WIN32
			file = nullptr;
			int fd = _open(path.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL | _O_TRUNC | _O_SEQUENTIAL, _S_IWRITE | _S_IREAD);
			if (fd >= 0)
				file = _fdopen(fd, "wb");
#else
			file = fopen(path.c_str(), "wbx");
#endif
			break;
		}
		}

		if (!file)
			return fail(DiscoveryReason::Open, errno != 0);
		observation.opened = true;
		if (!impl->report_imported)
			observation.boundary_known = true;

		if (!impl->imported_metadata.empty())
		{
			// Do nothing here. TODO: Set fadvise to RANDOM here.
			imported_metadata = impl->imported_metadata[0];
#ifdef __linux__
			// We're going to be doing scattered reads, which hopefully have been cached earlier.
			// However, if the archive has been paged out, RANDOM is the correct approach,
			// since prefetching data is only detrimental.
			if (posix_fadvise(fileno(file), 0, 0, POSIX_FADV_RANDOM) != 0)
				LOGW_LEVEL("Failed to advise of file usage. This is not fatal, but might compromise disk performance.\n");
#endif
		}
		else if (mode != DatabaseMode::OverWrite && mode != DatabaseMode::ExclusiveOverWrite)
		{
#if _WIN32
			if (mode == DatabaseMode::ReadOnly)
			{
				// Set the buffer size to reduce I/O cost of sparse freads
				setvbuf(file, nullptr, _IOFBF, FOSSILIZE_BLOB_HASH_LENGTH + sizeof(PayloadHeaderRaw));
			}
#endif

#ifdef __linux__
			// We're going to scan through the archive sequentially to discover metadata, so some prefetching is welcome.
			if (posix_fadvise(fileno(file), 0, 0, POSIX_FADV_SEQUENTIAL) != 0)
				LOGW_LEVEL("Failed to advise of file usage. This is not fatal, but might compromise disk performance.\n");
#endif

			// Scan through the archive and get the list of files.
			errno = 0;
			if (fseek(file, 0, SEEK_END) != 0)
				return fail(DiscoveryReason::Seek, errno != 0);
			errno = 0;
			auto length = ftell(file);
			if (length < 0)
				return fail(DiscoveryReason::Tell, errno != 0);
			if (uintmax_t(length) > numeric_limits<size_t>::max())
				return fail(DiscoveryReason::Tell);
			size_t len = size_t(length);
			errno = 0;
			if (fseek(file, 0, SEEK_SET) != 0)
				return fail(DiscoveryReason::Seek, errno != 0);
			if (shutdown_requested.load(std::memory_order_relaxed))
				return fail(DiscoveryReason::Cancelled);
			if (len == 0 && mode == DatabaseMode::ReadOnly)
				return fail(DiscoveryReason::InvalidMagic);

			if (len != 0 && !shutdown_requested.load(std::memory_order_relaxed))
			{
				uint8_t magic[MagicSize];
				errno = 0;
				if (fread(magic, 1, MagicSize, file) != MagicSize)
					return fail(ferror(file) ? DiscoveryReason::Read : DiscoveryReason::InvalidMagic, errno != 0);

				if (memcmp(magic, stream_reference_magic_and_version, MagicSize - 1))
					return fail(DiscoveryReason::InvalidMagic);
				int version = magic[MagicSize - 1];
				if (version > FOSSILIZE_FORMAT_VERSION || version < FOSSILIZE_FORMAT_MIN_COMPAT_VERSION)
					return fail(DiscoveryReason::UnsupportedVersion);

				observation.state = DiscoveryState::Complete;
				observation.total_known = true;
				observation.boundary_known = true;
				observation.prefix_end = MagicSize;
				size_t offset = MagicSize;
				size_t begin_append_offset = len;

				while (offset < len)
				{
					if (shutdown_requested.load(std::memory_order_relaxed))
						return fail(DiscoveryReason::Cancelled);

					begin_append_offset = offset;
					observation.unread_offset = offset;

					PayloadHeaderRaw *header_raw = nullptr;
					char bytes_to_read[FOSSILIZE_BLOB_HASH_LENGTH + sizeof(PayloadHeaderRaw)];
					PayloadHeader header = {};

					// Corrupt entry. Our process might have been killed before we could write all data.
					if (sizeof(bytes_to_read) > len - offset)
					{
						LOGW_LEVEL("Detected sliced file. Dropping entries from here.\n");
						observation.state = DiscoveryState::RecoveredPrefix;
						observation.reason = DiscoveryReason::TruncatedHeader;
						observation.total_known = false;
						observation.unread_offset = begin_append_offset;
						break;
					}

					// NAME + HEADER in one read. A short EOF need not carry an OS error.
					errno = 0;
					if (fread(bytes_to_read, 1, sizeof(bytes_to_read), file) != sizeof(bytes_to_read))
						return fail(DiscoveryReason::Read, errno != 0);
					offset += sizeof(bytes_to_read);
					header_raw = (PayloadHeaderRaw*)&bytes_to_read[FOSSILIZE_BLOB_HASH_LENGTH];
					header = get_converted_header(*header_raw);

					// Corrupt entry. Our process might have been killed before we could write all data.
					uint64_t payload_span = scan_payload_span(header);
					if (payload_span > len - offset)
					{
						LOGW_LEVEL("Detected sliced file. Dropping entries from here.\n");
						observation.state = DiscoveryState::RecoveredPrefix;
						observation.reason = DiscoveryReason::TruncatedPayload;
						observation.total_known = false;
						observation.unread_offset = begin_append_offset;
						break;
					}

					// Publish a key only once the same validated span has been advanced.
					Entry entry = fill_entry(header, offset);
					if (!move_offset_through_payload(payload_span, offset))
						return fail(DiscoveryReason::Seek, errno != 0);

					char tag_str[16 + 1] = {};
					char value_str[16 + 1] = {};
					memcpy(tag_str, bytes_to_read + FOSSILIZE_BLOB_HASH_LENGTH - 32, 16);
					memcpy(value_str, bytes_to_read + FOSSILIZE_BLOB_HASH_LENGTH - 16, 16);

					auto tag = unsigned(strtoul(tag_str, nullptr, 16));
					if (tag < RESOURCE_COUNT)
					{
						uint64_t value = strtoull(value_str, nullptr, 16);
						observe_discovered(static_cast<ResourceTag>(tag), value);
						observation.retained_count++;
						if (test_resource_filter(static_cast<ResourceTag>(tag), value))
							seen_blobs[tag].emplace(value, entry);
					}
					observation.prefix_end = offset;
					observation.unread_offset = offset;
				}

				if (mode == DatabaseMode::Append && offset != len)
				{
					errno = 0;
					if (fseek(file, long(begin_append_offset), SEEK_SET) != 0)
						return fail(DiscoveryReason::Seek, errno != 0);
				}
			}
			else
			{
				// Appending to a fresh file. Make sure we have the magic.
				if (fwrite(stream_reference_magic_and_version, 1,
				           sizeof(stream_reference_magic_and_version), file) != sizeof(stream_reference_magic_and_version))
					return false;
			}
		}
		else
		{
			if (fwrite(stream_reference_magic_and_version, 1, sizeof(stream_reference_magic_and_version), file) !=
			    sizeof(stream_reference_magic_and_version))
			{
				return false;
			}
		}

		alive = true;
		return true;
	}

	static bool find_entry_from_metadata(const ExportedMetadataHeader *header, ResourceTag tag, Hash hash, Entry *entry)
	{
		size_t count = header->lists[tag].count;
		if (!count)
			return false;

		// Binary search in-place.
		auto *blocks = reinterpret_cast<const ExportedMetadataBlock *>(
				reinterpret_cast<const uint8_t *>(header) + header->lists[tag].offset);

		auto itr = std::lower_bound(blocks, blocks + count, hash, [](const ExportedMetadataBlock &a, Hash hash_) {
			return a.hash < hash_;
		});

		if (itr != blocks + count && itr->hash == hash)
		{
			if (entry)
			{
				entry->offset = itr->file_offset;
				entry->header = itr->payload;
			}
			return true;
		}
		else
			return false;
	}

	bool find_entry(ResourceTag tag, Hash hash, Entry &entry) const
	{
		if (imported_metadata)
		{
			return find_entry_from_metadata(imported_metadata, tag, hash, &entry);
		}
		else
		{
			auto itr = seen_blobs[tag].find(hash);
			if (itr == end(seen_blobs[tag]))
				return false;

			entry = itr->second;
			return true;
		}
	}

	bool read_entry(ResourceTag tag, Hash hash, size_t *blob_size, void *blob, PayloadReadFlags flags) override
	{
		if (!alive || mode != DatabaseMode::ReadOnly)
			return false;

		Entry entry;
		if (!find_entry(tag, hash, entry))
			return false;

		if (!blob_size)
			return false;

		uint32_t out_size = (flags & PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT) != 0 ?
		                    (entry.header.payload_size + sizeof(PayloadHeaderRaw)) :
		                    entry.header.uncompressed_size;

		if (blob && *blob_size < out_size)
			return false;
		*blob_size = out_size;

		if (blob)
		{
			if ((flags & PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT) != 0)
			{
				// Include the header.
				ConditionalLockGuard holder(read_lock, (flags & PAYLOAD_READ_CONCURRENT_BIT) != 0);
				if (fseek(file, entry.offset - sizeof(PayloadHeaderRaw), SEEK_SET) < 0)
					return false;

				size_t read_size = entry.header.payload_size + sizeof(PayloadHeaderRaw);
				if (fread(blob, 1, read_size, file) != read_size)
					return false;
			}
			else
			{
				if (!decode_payload(blob, out_size, entry, (flags & PAYLOAD_READ_CONCURRENT_BIT) != 0))
					return false;
			}
		}

		return true;
	}

	static void convert_from_le(uint32_t *output, const uint8_t *le_input, unsigned word_count)
	{
		for (unsigned i = 0; i < word_count; i++)
		{
			uint32_t v =
					(uint32_t(le_input[0]) << 0) |
					(uint32_t(le_input[1]) << 8) |
					(uint32_t(le_input[2]) << 16) |
					(uint32_t(le_input[3]) << 24);

			*output++ = v;
			le_input += 4;
		}
	}

	static void convert_from_le(PayloadHeader &header, const PayloadHeaderRaw &raw)
	{
		convert_from_le(&header.payload_size, raw.data + 0, 1);
		convert_from_le(&header.format, raw.data + 4, 1);
		convert_from_le(&header.crc, raw.data + 8, 1);
		convert_from_le(&header.uncompressed_size, raw.data + 12, 1);
	}

	static void convert_to_le(uint8_t *le_output, const uint32_t *value_input, unsigned count)
	{
		for (unsigned i = 0; i < count; i++)
		{
			uint32_t v = value_input[i];
			*le_output++ = uint8_t((v >> 0) & 0xffu);
			*le_output++ = uint8_t((v >> 8) & 0xffu);
			*le_output++ = uint8_t((v >> 16) & 0xffu);
			*le_output++ = uint8_t((v >> 24) & 0xffu);
		}
	}

	static void convert_to_le(PayloadHeaderRaw &raw, const PayloadHeader &header)
	{
		uint8_t *le_output = raw.data;
		convert_to_le(le_output + 0, &header.payload_size, 1);
		convert_to_le(le_output + 4, &header.format, 1);
		convert_to_le(le_output + 8, &header.crc, 1);
		convert_to_le(le_output + 12, &header.uncompressed_size, 1);
	}

	bool write_entry(ResourceTag tag, Hash hash, const void *blob, size_t size, PayloadWriteFlags flags) override
	{
		if (!alive || mode == DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr != end(seen_blobs[tag]))
			return true;

		char str[FOSSILIZE_BLOB_HASH_LENGTH + 1]; // 40 digits + null
		sprintf(str, "%0*x", FOSSILIZE_BLOB_HASH_LENGTH - 16, tag);
		sprintf(str + FOSSILIZE_BLOB_HASH_LENGTH - 16, "%016" PRIx64, hash);

		if (fwrite(str, 1, FOSSILIZE_BLOB_HASH_LENGTH, file) != FOSSILIZE_BLOB_HASH_LENGTH)
			return false;

		if ((flags & PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT) != 0)
		{
			// The raw payload already contains the header, so just dump it straight to disk.
			if (size < sizeof(PayloadHeaderRaw))
				return false;
			if (fwrite(blob, 1, size, file) != size)
				return false;
		}
		else if ((flags & PAYLOAD_WRITE_COMPRESS_BIT) != 0)
		{
			auto compressed_bound = mz_compressBound(size);

			if (zlib_buffer_size < compressed_bound)
			{
				auto *new_zlib_buffer = static_cast<uint8_t *>(realloc(zlib_buffer, compressed_bound));
				if (new_zlib_buffer)
				{
					zlib_buffer = new_zlib_buffer;
					zlib_buffer_size = compressed_bound;
				}
				else
				{
					free(zlib_buffer);
					zlib_buffer = nullptr;
					zlib_buffer_size = 0;
				}
			}

			if (!zlib_buffer)
				return false;

			PayloadHeader header = {};
			PayloadHeaderRaw header_raw = {};
			header.uncompressed_size = uint32_t(size);
			header.format = FOSSILIZE_COMPRESSION_DEFLATE;

			mz_ulong zsize = zlib_buffer_size;
			if (mz_compress2(zlib_buffer, &zsize, static_cast<const unsigned char *>(blob), size,
			                 (flags & PAYLOAD_WRITE_BEST_COMPRESSION_BIT) != 0 ? MZ_BEST_COMPRESSION : MZ_BEST_SPEED) != MZ_OK)
				return false;

			header.payload_size = uint32_t(zsize);
			if ((flags & PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT) != 0)
				header.crc = uint32_t(mz_crc32(MZ_CRC32_INIT, zlib_buffer, zsize));

			convert_to_le(header_raw, header);
			if (fwrite(&header_raw, 1, sizeof(header_raw), file) != sizeof(header_raw))
				return false;

			if (fwrite(zlib_buffer, 1, header.payload_size, file) != header.payload_size)
				return false;
		}
		else
		{
			uint32_t crc = 0;
			if ((flags & PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT) != 0)
				crc = uint32_t(mz_crc32(MZ_CRC32_INIT, static_cast<const unsigned char *>(blob), size));

			PayloadHeader header = { uint32_t(size), FOSSILIZE_COMPRESSION_NONE, crc, uint32_t(size) };
			PayloadHeaderRaw raw = {};
			convert_to_le(raw, header);

			if (fwrite(&raw, 1, sizeof(raw), file) != sizeof(raw))
				return false;

			if (size != 0 && fwrite(blob, 1, size, file) != size)
				return false;
		}

		// The entry is irrelevant, we're not going to read from this archive any time soon.
		seen_blobs[tag].emplace(hash, Entry{});
		return true;
	}

	bool has_entry(ResourceTag tag, Hash hash) override
	{
		if (!test_resource_filter(tag, hash))
			return false;

		if (imported_metadata)
			return find_entry_from_metadata(imported_metadata, tag, hash, nullptr);
		else
			return seen_blobs[tag].count(hash) != 0;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, size_t *hash_count, Hash *hashes) override
	{
		if (imported_metadata)
		{
			size_t size = imported_metadata->lists[tag].count;
			if (hashes)
			{
				if (size != *hash_count)
					return false;
			}
			else
				*hash_count = size;

			if (hashes)
			{
				const auto *blocks = reinterpret_cast<const ExportedMetadataBlock *>(
						reinterpret_cast<const uint8_t *>(imported_metadata) + imported_metadata->lists[tag].offset);
				for (size_t i = 0; i < size; i++)
					hashes[i] = blocks[i].hash;
			}
		}
		else
		{
			size_t size = seen_blobs[tag].size();
			if (hashes)
			{
				if (size != *hash_count)
					return false;
			}
			else
				*hash_count = size;

			if (hashes)
			{
				Hash *iter = hashes;
				for (auto &blob : seen_blobs[tag])
					*iter++ = blob.first;

				// Make replay more deterministic.
				sort(hashes, hashes + size);
			}
		}

		return true;
	}

	bool decode_payload_uncompressed(void *blob, size_t blob_size, const Entry &entry, bool concurrent)
	{
		if (entry.header.uncompressed_size != blob_size || entry.header.payload_size != blob_size)
			return false;

		{
			ConditionalLockGuard holder(read_lock, concurrent);
			if (fseek(file, entry.offset, SEEK_SET) < 0)
				return false;

			size_t read_size = entry.header.payload_size;
			if (fread(blob, 1, read_size, file) != read_size)
				return false;
		}

		if (entry.header.crc != 0) // Verify checksum.
		{
			auto disk_crc = uint32_t(mz_crc32(MZ_CRC32_INIT, static_cast<unsigned char *>(blob), blob_size));
			if (disk_crc != entry.header.crc)
			{
				LOGE_LEVEL("CRC mismatch!\n");
				return false;
			}
		}

		return true;
	}

	bool decode_payload_deflate(void *blob, size_t blob_size, const Entry &entry, bool concurrent)
	{
		if (entry.header.uncompressed_size != blob_size)
			return false;

		uint8_t *dst_zlib_buffer = nullptr;
		std::unique_ptr<uint8_t[]> zlib_buffer_holder;

		{
			ConditionalLockGuard holder(read_lock, concurrent);
			if (concurrent)
			{
				dst_zlib_buffer = new uint8_t[entry.header.payload_size];
				zlib_buffer_holder.reset(dst_zlib_buffer);
			}
			else if (zlib_buffer_size < entry.header.payload_size)
			{
				auto *new_zlib_buffer = static_cast<uint8_t *>(realloc(zlib_buffer, entry.header.payload_size));
				if (new_zlib_buffer)
				{
					zlib_buffer = new_zlib_buffer;
					zlib_buffer_size = entry.header.payload_size;
				}
				else
				{
					free(zlib_buffer);
					zlib_buffer = nullptr;
					zlib_buffer_size = 0;
				}

				if (!zlib_buffer)
					return false;

				dst_zlib_buffer = zlib_buffer;
			}
			else
				dst_zlib_buffer = zlib_buffer;

			if (fseek(file, entry.offset, SEEK_SET) < 0)
				return false;
			if (fread(dst_zlib_buffer, 1, entry.header.payload_size, file) != entry.header.payload_size)
				return false;
		}

		if (entry.header.crc != 0) // Verify checksum.
		{
			auto disk_crc = uint32_t(mz_crc32(MZ_CRC32_INIT, dst_zlib_buffer, entry.header.payload_size));
			if (disk_crc != entry.header.crc)
			{
				LOGE_LEVEL("CRC mismatch!\n");
				return false;
			}
		}

		mz_ulong zsize = blob_size;
		if (mz_uncompress(static_cast<unsigned char *>(blob), &zsize, dst_zlib_buffer, entry.header.payload_size) != MZ_OK)
			return false;
		if (zsize != blob_size)
			return false;

		return true;
	}

	bool decode_payload(void *blob, size_t blob_size, const Entry &entry, bool concurrent)
	{
		if (entry.header.format == FOSSILIZE_COMPRESSION_NONE)
			return decode_payload_uncompressed(blob, blob_size, entry, concurrent);
		else if (entry.header.format == FOSSILIZE_COMPRESSION_DEFLATE)
			return decode_payload_deflate(blob, blob_size, entry, concurrent);
		else
			return false;
	}

	const char *get_db_path_for_hash(ResourceTag tag, Hash hash) override
	{
		if (!has_entry(tag, hash))
			return nullptr;

		return path.c_str();
	}

	size_t compute_exported_metadata_size() const override
	{
		size_t size = sizeof(ExportedMetadataHeader);
		for (auto &blobs : seen_blobs)
			size += blobs.size() * sizeof(ExportedMetadataBlock);
		return size;
	}

	bool write_exported_metadata(void *data_, size_t size) const override
	{
		auto *data = static_cast<uint8_t *>(data_);
		auto *header = static_cast<ExportedMetadataHeader *>(data_);
		if (size < sizeof(*header))
			return false;

		header->magic = ExportedMetadataMagic;
		header->size = size;

		size_t offset = sizeof(*header);
		for (unsigned i = 0; i < RESOURCE_COUNT; i++)
		{
			header->lists[i].offset = offset;
			header->lists[i].count = seen_blobs[i].size();
			offset += header->lists[i].count * sizeof(ExportedMetadataBlock);
		}

		if (offset != size)
			return false;

		for (unsigned i = 0; i < RESOURCE_COUNT; i++)
		{
			auto *blocks = reinterpret_cast<ExportedMetadataBlock *>(data + header->lists[i].offset);
			auto *pblocks = blocks;

			for (auto &blob : seen_blobs[i])
			{
				auto &block = *pblocks;
				block.hash = blob.first;
				block.file_offset = blob.second.offset;
				block.payload = blob.second.header;
				pblocks++;
			}

			// Sorting here is somewhat important, since we will be using binary search when looking up exported metadata.
			// Conserving memory is also somewhat important, so we should only have one copy of the data structures we need
			// in immutable shared memory.
			// For hashmaps we would either need to make a completely custom SHM compatible hashmap implementation, which
			// would likely consume more memory either way.
			std::sort(blocks, blocks + seen_blobs[i].size(), [](const ExportedMetadataBlock &a, const ExportedMetadataBlock &b) {
				return a.hash < b.hash;
			});
		}

		return true;
	}

	void resolve_path(const std::string &read_only_part)
	{
		constexpr char cmpstr[] = "$bucketdir";
		constexpr size_t offset = sizeof(cmpstr) - 1;
		if (path.compare(0, offset, cmpstr) == 0 &&
		    path.size() > offset &&
		    (path[offset] == '/' || path[offset] == '\\'))
		{
			path = Path::relpath(read_only_part, path.substr(offset + 1));
		}
	}

	const ExportedMetadataHeader *imported_metadata = nullptr;
	FILE *file = nullptr;
	string path;
	unordered_map<Hash, Entry> seen_blobs[RESOURCE_COUNT];
	DatabaseMode mode;
	uint8_t *zlib_buffer = nullptr;
	size_t zlib_buffer_size = 0;
	bool alive = false;
	std::mutex read_lock;

protected:
	virtual PayloadHeader get_converted_header(PayloadHeaderRaw header_raw)
	{
		PayloadHeader header;
		convert_from_le(header, header_raw);
		return header;
	}

	virtual uint64_t scan_payload_span(PayloadHeader header) const
	{
		return header.payload_size;
	}

	bool move_offset_through_payload(uint64_t span, size_t &offset)
	{
		// fseek takes long even on platforms with a wider size_t. A position
		// limit is not a native I/O error, so do not inherit a stale errno.
		errno = 0;
		if (span > uintmax_t(numeric_limits<long>::max()) ||
		    span > numeric_limits<size_t>::max() - offset ||
		    uintmax_t(offset) > uintmax_t(numeric_limits<long>::max()) - span)
			return false;
		if (fseek(file, long(span), SEEK_CUR) != 0)
			return false;

		offset += size_t(span);
		return true;
	}

	virtual Entry fill_entry(PayloadHeader header, size_t offset)
	{
		Entry entry = {};
		entry.header = header;
		entry.offset = offset;
		return entry;
	}
};

DatabaseInterface *create_stream_archive_database(const char *path, DatabaseMode mode)
{
	auto *db = new StreamArchive(path, mode);
	return db;
}

struct DumbFileDatabase : StreamArchive
{
	DumbFileDatabase(const string& path_, DatabaseMode mode_) : StreamArchive(path_, mode_)
	{}

	bool read_entry(ResourceTag tag, Hash hash, size_t *blob_size, void *blob, PayloadReadFlags flags) override
	{
		if (!alive || mode != DatabaseMode::ReadOnly)
			return false;

		Entry entry;
		if (!find_entry(tag, hash, entry))
			return false;

		if (!blob_size)
			return false;

		uint32_t out_size = entry.header.uncompressed_size;

		if (blob && *blob_size < out_size)
			return false;
		*blob_size = out_size;

		if (blob)
		{
			// Include the header.
			ConditionalLockGuard holder(read_lock, (flags & PAYLOAD_READ_CONCURRENT_BIT) != 0);
			if (fseek(file, entry.offset, SEEK_SET) < 0)
				return false;

			if (fread(blob, 1, out_size, file) != out_size)
				return false;
		}

		return true;
	}

	bool write_entry(ResourceTag tag, Hash hash, const void *blob, size_t size, PayloadWriteFlags flags) override
	{
		if (!alive || mode == DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr != end(seen_blobs[tag]))
			return true;

		char str[FOSSILIZE_BLOB_HASH_LENGTH + 1]; // 40 digits + null
		sprintf(str, "%0*x", FOSSILIZE_BLOB_HASH_LENGTH - 16, tag);
		sprintf(str + FOSSILIZE_BLOB_HASH_LENGTH - 16, "%016" PRIx64, hash);

		if (fwrite(str, 1, FOSSILIZE_BLOB_HASH_LENGTH, file) != FOSSILIZE_BLOB_HASH_LENGTH)
			return false;

		uint32_t crc = 0;
		if ((flags & PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT) != 0)
			crc = uint32_t(mz_crc32(MZ_CRC32_INIT, static_cast<const unsigned char*>(blob), size));

		PayloadHeader header = { uint32_t(size), FOSSILIZE_COMPRESSION_NONE, crc, uint32_t(size) };
		PayloadHeaderRaw raw = {};
		convert_to_le(raw, header);

		if (fwrite(&raw, 1, sizeof(raw), file) != sizeof(raw) || putc('\n', file) == EOF)
			return false;

		if (fwrite(blob, 1, size, file) != size || putc('\n', file) == EOF)
			return false;

		// The entry is irrelevant, we're not going to read from this archive any time soon.
		seen_blobs[tag].emplace(hash, Entry{});
		return true;
	}

protected:
	PayloadHeader get_converted_header(PayloadHeaderRaw header_raw) override
	{
		PayloadHeader header;
		convert_from_le(header, header_raw);
		header.payload_size = header.uncompressed_size;
		return header;
	}

	uint64_t scan_payload_span(PayloadHeader header) const override
	{
		return uint64_t(header.payload_size) + 2;
	}

	Entry fill_entry(PayloadHeader header, size_t offset) override
	{
		Entry entry = {};
		entry.header = header;
		entry.offset = offset + 1; // + newline symbol
		return entry;
	}
};

DatabaseInterface *create_dumb_file_database(const char *path, DatabaseMode mode)
{
	auto *db = new DumbFileDatabase(path, mode);
	return db;
}

DatabaseInterface *create_database(const char *path, DatabaseMode mode)
{
	auto ext = Path::ext(path);
	if (ext == "foz")
		return create_stream_archive_database(path, mode);
	else if (ext == "zip")
		return create_zip_archive_database(path, mode);
	else if (ext == "txt")
		return create_dumb_file_database(path, mode);
	else
		return create_dumb_folder_database(path, mode);
}

struct ConcurrentDatabase : DatabaseInterface
{
	explicit ConcurrentDatabase(const char *base_path_, DatabaseMode mode_,
	                            const char * const *extra_paths, size_t num_extra_paths)
		: DatabaseInterface(mode_), base_path(base_path_ ? base_path_ : ""), mode(mode_)
	{
		// Normalize this mode. The concurrent database is always "exclusive write".
		if (mode == DatabaseMode::ExclusiveOverWrite)
			mode = DatabaseMode::OverWrite;

		if (mode != DatabaseMode::OverWrite)
		{
			if (!base_path.empty())
			{
				std::string readonly_path = base_path + ".foz";
				readonly_interface.reset(create_stream_archive_database(readonly_path.c_str(), DatabaseMode::ReadOnly));
			}

			for (size_t i = 0; i < num_extra_paths; i++)
				extra_readonly.emplace_back(create_stream_archive_database(extra_paths[i], DatabaseMode::ReadOnly));
		}
	}

	void flush() override
	{
		if (writeonly_interface)
			writeonly_interface->flush();
	}

	void prime_read_only_hashes(DatabaseInterface &interface)
	{
		for (unsigned i = 0; i < RESOURCE_COUNT; i++)
		{
			auto tag = static_cast<ResourceTag>(i);
			size_t num_hashes;
			if (!interface.get_hash_list_for_resource_tag(tag, &num_hashes, nullptr))
				return;
			std::vector<Hash> hashes(num_hashes);
			if (!interface.get_hash_list_for_resource_tag(tag, &num_hashes, hashes.data()))
				return;

			for (auto &hash : hashes)
			{
				if (!impl->report_imported)
					observe_discovered(tag, hash);
				if (test_resource_filter(tag, hash))
					primed_hashes[i].insert(hash);
			}
		}
	}

	bool setup_bucket()
	{
		base_path += ".";
		base_path += bucket_dirname;

		if (!Path::mkdir(base_path))
		{
			LOGE("Failed to create directory %s.\n", base_path.c_str());
			return false;
		}

		base_path += "/";

		if (!bucket_info.empty())
		{
			auto info_path = base_path + "bucket.json";
			FILE *file = fopen(info_path.c_str(), "w");
			if (file)
			{
				fputs(bucket_info.c_str(), file);
				fclose(file);
			}
			else
			{
				LOGE("Failed to create file \"%s\".\n", info_path.c_str());
				return false;
			}
		}

		if (!Path::touch(base_path + "TOUCH"))
			LOGW("Failed to touch last access in %s.\n", base_path.c_str());
		base_path += "/" + bucket_basename;

		if (readonly_interface)
			static_cast<StreamArchive &>(*readonly_interface).path = base_path + ".foz";

		return true;
	}

	bool prepare() override
	{
		if (mode != DatabaseMode::Append &&
		    mode != DatabaseMode::ReadOnly &&
		    mode != DatabaseMode::AppendWithReadOnlyAccess &&
		    mode != DatabaseMode::OverWrite)
			return false;

		if (mode != DatabaseMode::ReadOnly && !impl->sub_databases_in_whitelist.empty())
			return false;

		if (mode == DatabaseMode::ReadOnly && !bucket_dirname.empty() && !bucket_basename.empty())
			return false;

		if (!bucket_dirname.empty() && !setup_bucket())
			return false;

		if (impl->report_imported)
		{
			if (impl->imported_metadata.size() != extra_readonly.size() + 1)
				return false;
			size_t inputs = 0;
			std::vector<bool> seen_inputs(extra_readonly.size() + 1);
			for (auto &input : impl->report.inputs)
			{
				if (input.filter)
					continue;
				if (input.index >= seen_inputs.size() || seen_inputs[input.index])
					return false;
				seen_inputs[input.index] = true;
				DatabaseInterface *iface = input.index == 0 ? readonly_interface.get() :
					input.index <= extra_readonly.size() ? extra_readonly[input.index - 1].get() : nullptr;
				if (!iface || static_cast<StreamArchive &>(*iface).path != input.path)
					return false;
				inputs++;
			}
			if (inputs != extra_readonly.size() + (readonly_interface ? 1 : 0))
				return false;
		}

		// Set inherited metadata in sub-databases before we prepare them.
		if (!impl->imported_metadata.empty())
		{
			if (readonly_interface)
				readonly_interface->add_imported_metadata(impl->imported_metadata.front());

			for (size_t i = 1; i < impl->imported_metadata.size(); i++)
			{
				size_t extra_index = i - 1;
				if (extra_index < extra_readonly.size() && extra_readonly[extra_index])
					extra_readonly[extra_index]->add_imported_metadata(impl->imported_metadata[i]);
			}
		}

		if (!has_prepared_readonly)
		{
			// Prepare everything.
			// It's okay if the database doesn't exist.
			auto prepare_input = [&](std::unique_ptr<DatabaseInterface> &input, unsigned index) {
				if (!input)
					return;
				input->set_input_required(0, impl->input_required(index));
				bool prepared = input->prepare();
				if (!impl->report_imported)
				{
					for (auto observation : input->get_preparation_report().inputs)
					{
						observation.index = index;
						impl->report.inputs.push_back(std::move(observation));
					}
				}
				if (!prepared)
					input.reset();
			};
			prepare_input(readonly_interface, 0);

			for (size_t i = 0; i < extra_readonly.size(); i++)
			{
				auto &extra = extra_readonly[i];
				if (extra)
					static_cast<StreamArchive &>(*extra).resolve_path(base_path);
				prepare_input(extra, unsigned(i + 1));
			}

			// Promote databases to whitelist.
			for (unsigned index : impl->sub_databases_in_whitelist)
			{
				DatabaseInterface *iface = nullptr;
				if (index == 0)
					iface = readonly_interface.get();
				else if (index <= extra_readonly.size())
					iface = extra_readonly[index - 1].get();

				// It's okay if the archive does not exist, we just ignore it.
				if (iface && !add_to_implicit_whitelist(*iface))
					return false;
			}

			// Prime the hashmaps, however, we'll rely on concurrent metadata if we have it to avoid memory bloat.
			if (!impl->imported_concurrent_metadata)
			{
				if (readonly_interface)
					prime_read_only_hashes(*readonly_interface);

				for (auto &extra : extra_readonly)
					if (extra)
						prime_read_only_hashes(*extra);
			}

			// We only need the database for priming purposes.
			if (mode == DatabaseMode::Append)
			{
				readonly_interface.reset();
				extra_readonly.clear();
			}
		}

		has_prepared_readonly = true;

		// To make harvesting easier, also store the bucket info as a separate entry in the DB.
		if (mode != DatabaseMode::ReadOnly && !bucket_basename.empty() && !bucket_info.empty())
		{
			Hasher h;
			h.string(bucket_info);
			if (!write_entry(RESOURCE_BUCKET_INFO, h.get(),
							 bucket_info.data(), bucket_info.size(), PAYLOAD_WRITE_NO_FLAGS))
			{
				return false;
			}
		}

		return true;
	}

	bool read_entry(ResourceTag tag, Hash hash, size_t *blob_size, void *blob, PayloadReadFlags flags) override
	{
		if (mode == DatabaseMode::Append || mode == DatabaseMode::OverWrite)
			return false;

		if (readonly_interface && readonly_interface->read_entry(tag, hash, blob_size, blob, flags))
			return true;

		// There shouldn't be that many read-only databases to the point where we need to use hashmaps
		// to map hash/tag to the readonly database.
		for (auto &extra : extra_readonly)
			if (extra && extra->read_entry(tag, hash, blob_size, blob, flags))
				return true;

		return false;
	}

	bool write_entry(ResourceTag tag, Hash hash, const void *blob, size_t blob_size, PayloadWriteFlags flags) override
	{
		if (mode != DatabaseMode::Append &&
		    mode != DatabaseMode::AppendWithReadOnlyAccess &&
		    mode != DatabaseMode::OverWrite)
			return false;

		if (primed_hashes[tag].count(hash))
			return true;

		// All threads must have called prepare and synchronized readonly_interface from that,
		// and from here on out readonly_interface is purely read-only, no need to lock just to check.
		if (readonly_interface && readonly_interface->has_entry(tag, hash))
			return true;

		if (writeonly_interface && writeonly_interface->has_entry(tag, hash))
			return true;

		if (need_writeonly_database)
		{
			// Lazily create a new database. Open the database file exclusively to work concurrently with other processes.
			// Don't try forever.
			for (unsigned index = 1; index < 256 && !writeonly_interface; index++)
			{
				std::string write_path = base_path + "." + std::to_string(index) + ".foz";
				writeonly_interface.reset(create_stream_archive_database(write_path.c_str(), DatabaseMode::ExclusiveOverWrite));
				if (!writeonly_interface->prepare())
					writeonly_interface.reset();
			}

			need_writeonly_database = false;
		}

		if (writeonly_interface)
			return writeonly_interface->write_entry(tag, hash, blob, blob_size, flags);
		else
			return false;
	}

	static bool find_entry_in_concurrent_metadata(const ExportedMetadataHeader *header, ResourceTag tag, Hash hash)
	{
		auto *begin_range = reinterpret_cast<const Hash *>(reinterpret_cast<const uint8_t *>(header) + header->lists[tag].offset);
		auto *end_range = begin_range + header->lists[tag].count;
		return std::binary_search(begin_range, end_range, hash);
	}

	// Checks if entry already exists in database, i.e. no need to serialize.
	bool has_entry(ResourceTag tag, Hash hash) override
	{
		if (impl->imported_concurrent_metadata)
			return find_entry_in_concurrent_metadata(impl->imported_concurrent_metadata, tag, hash);

		if (!test_resource_filter(tag, hash))
			return false;

		if (primed_hashes[tag].count(hash))
			return true;

		// All threads must have called prepare and synchronized readonly_interface from that,
		// and from here on out readonly_interface is purely read-only, no need to lock just to check.
		if (readonly_interface && readonly_interface->has_entry(tag, hash))
			return true;

		return writeonly_interface && writeonly_interface->has_entry(tag, hash);
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, size_t *num_hashes, Hash *hashes) override
	{
		if (impl->imported_concurrent_metadata)
		{
			size_t total_size = impl->imported_concurrent_metadata->lists[tag].count;

			if (hashes)
			{
				if (total_size != *num_hashes)
					return false;
			}
			else
				*num_hashes = total_size;

			if (hashes)
			{
				memcpy(hashes,
				       reinterpret_cast<const uint8_t *>(impl->imported_concurrent_metadata) +
				       impl->imported_concurrent_metadata->lists[tag].offset,
				       total_size * sizeof(*hashes));
			}
			return true;
		}

		size_t readonly_size = primed_hashes[tag].size();

		size_t writeonly_size = 0;
		if (!writeonly_interface || !writeonly_interface->get_hash_list_for_resource_tag(tag, &writeonly_size, nullptr))
			writeonly_size = 0;

		size_t total_size = readonly_size + writeonly_size;

		if (hashes)
		{
			if (total_size != *num_hashes)
				return false;
		}
		else
			*num_hashes = total_size;

		if (hashes)
		{
			Hash *iter = hashes;
			for (auto &blob : primed_hashes[tag])
				*iter++ = blob;

			if (writeonly_size != 0 && !writeonly_interface->get_hash_list_for_resource_tag(tag, &writeonly_size, iter))
				return false;

			// Make replay more deterministic.
			sort(hashes, hashes + total_size);
		}

		return true;
	}

	const char *get_db_path_for_hash(ResourceTag tag, Hash hash) override
	{
		if (readonly_interface && readonly_interface->has_entry(tag, hash))
			return readonly_interface->get_db_path_for_hash(tag, hash);

		for (auto &extra : extra_readonly)
		{
			if (extra && extra->has_entry(tag, hash))
				return extra->get_db_path_for_hash(tag, hash);
		}

		return nullptr;
	}

	DatabaseInterface *get_sub_database(unsigned index) override
	{
		if (mode != DatabaseMode::ReadOnly)
			return nullptr;

		if (index == 0)
			return readonly_interface.get();
		else if (index <= extra_readonly.size())
			return extra_readonly[index - 1].get();
		else
			return nullptr;
	}

	bool has_sub_databases() override
	{
		return true;
	}

	size_t get_total_num_hashes_for_tag(ResourceTag tag) const
	{
		size_t count = 0;

		// This is an upper bound, we might prune it later, although in general we don't expect duplicates.
		size_t hash_count = 0;
		if (readonly_interface &&
		    !readonly_interface->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
			return 0;
		count += hash_count;

		for (auto &e : extra_readonly)
		{
			hash_count = 0;
			if (e && !e->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
				return 0;
			count += hash_count;
		}

		return count;
	}

	size_t get_total_num_hashes() const
	{
		size_t count = 0;
		for (unsigned i = 0; i < RESOURCE_COUNT; i++)
		{
			auto tag = ResourceTag(i);
			count += get_total_num_hashes_for_tag(tag);
		}
		return count;
	}

	size_t compute_exported_metadata_size() const override
	{
		if (mode != DatabaseMode::ReadOnly)
			return 0;

		size_t size = 0;

		size += sizeof(ExportedMetadataHeader) + get_total_num_hashes() * sizeof(ExportedMetadataConcurrentPrimedBlock);

		if (readonly_interface)
			size += readonly_interface->compute_exported_metadata_size();
		else
			size += sizeof(ExportedMetadataHeader);

		for (auto &e : extra_readonly)
		{
			if (e)
				size += e->compute_exported_metadata_size();
			else
				size += sizeof(ExportedMetadataHeader);
		}

		return size;
	}

	bool write_exported_metadata(void *data_, size_t size) const override
	{
		auto *data = static_cast<uint8_t *>(data_);

		if (!write_exported_concurrent_metadata(data, size))
			return false;

		if (!write_exported_metadata_for_db(readonly_interface.get(), data, size))
			return false;

		for (auto &e : extra_readonly)
			if (!write_exported_metadata_for_db(e.get(), data, size))
				return false;

		return size == 0;
	}

	bool write_exported_concurrent_hashes(ResourceTag tag, ExportedMetadataConcurrentPrimedBlock *primed, uint64_t &total_count) const
	{
		auto *primed_begin = primed;
		size_t hash_count = 0;

		// This is an upper bound, we might prune it later, although in general we don't expect duplicates.
		if (readonly_interface &&
		    !readonly_interface->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
			return false;
		if (readonly_interface &&
		    !readonly_interface->get_hash_list_for_resource_tag(tag, &hash_count, primed))
			return false;
		primed += hash_count;

		for (auto &e : extra_readonly)
		{
			hash_count = 0;
			if (e && !e->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
				return false;
			if (e && !e->get_hash_list_for_resource_tag(tag, &hash_count, primed))
				return false;
			primed += hash_count;
		}

		std::sort(primed_begin, primed);
		primed = std::unique(primed_begin, primed);
		primed = std::remove_if(primed_begin, primed, [&](Hash h) { return !test_resource_filter(tag,h); });
		total_count = size_t(primed - primed_begin);
		return true;
	}

	bool write_exported_concurrent_metadata(uint8_t *&data, size_t &size) const
	{
		size_t total_hashes = get_total_num_hashes();
		size_t required = sizeof(ExportedMetadataHeader) + total_hashes * sizeof(ExportedMetadataConcurrentPrimedBlock);
		if (size < required)
			return false;

		auto *header = reinterpret_cast<ExportedMetadataHeader *>(data);
		header->magic = ExportedMetadataMagicConcurrent;
		header->size = required;

		size_t offset = sizeof(*header);
		for (unsigned i = 0; i < RESOURCE_COUNT; i++)
		{
			auto tag = ResourceTag(i);
			header->lists[i].offset = offset;
			if (!write_exported_concurrent_hashes(tag, reinterpret_cast<ExportedMetadataConcurrentPrimedBlock *>(data + offset),
			                                      header->lists[i].count))
			{
				return false;
			}
			offset += header->lists[i].count * sizeof(ExportedMetadataConcurrentPrimedBlock);
		}

		data += required;
		size -= required;
		return true;
	}

	static bool write_exported_metadata_for_db(const DatabaseInterface *iface, uint8_t *&data, size_t &size)
	{
		if (iface)
		{
			size_t required = iface->compute_exported_metadata_size();
			if (required > size)
				return false;

			if (!iface->write_exported_metadata(data, required))
				return false;

			data += required;
			size -= required;
		}
		else
		{
			if (size < sizeof(ExportedMetadataHeader))
				return false;

			auto *header = reinterpret_cast<ExportedMetadataHeader *>(data);
			header->magic = ExportedMetadataMagic;
			header->size = sizeof(*header);
			memset(header->lists, 0, sizeof(header->lists));
			data += sizeof(ExportedMetadataHeader);
			size -= sizeof(ExportedMetadataHeader);
		}

		return true;
	}

	bool set_bucket_path(const char *bucket_dirname_, const char *bucket_basename_) override
	{
		if (bucket_dirname_)
			bucket_dirname = bucket_dirname_;
		else
			bucket_dirname.clear();

		if (bucket_basename_)
			bucket_basename = bucket_basename_;
		else
			bucket_basename.clear();

		return true;
	}

	void set_bucket_info(const char *json) override
	{
		bucket_info = json;
	}

	std::string base_path;
	std::string bucket_dirname;
	std::string bucket_basename;
	std::string bucket_info;
	DatabaseMode mode;
	std::unique_ptr<DatabaseInterface> readonly_interface;
	std::unique_ptr<DatabaseInterface> writeonly_interface;
	std::vector<std::unique_ptr<DatabaseInterface>> extra_readonly;
	std::unordered_set<Hash> primed_hashes[RESOURCE_COUNT];
	bool has_prepared_readonly = false;
	bool need_writeonly_database = true;
};

DatabaseInterface *create_concurrent_database(const char *base_path, DatabaseMode mode,
											  const char * const *extra_read_only_database_paths,
											  size_t num_extra_read_only_database_paths)
{
	return new ConcurrentDatabase(base_path, mode, extra_read_only_database_paths, num_extra_read_only_database_paths);
}

DatabaseInterface *create_concurrent_database_with_encoded_extra_paths(const char *base_path, DatabaseMode mode,
																	   const char *encoded_extra_paths)
{
	if (!encoded_extra_paths)
		return create_concurrent_database(base_path, mode, nullptr, 0);

#ifdef _WIN32
	auto paths = Path::split_no_empty(encoded_extra_paths, ";");
#else
	auto paths = Path::split_no_empty(encoded_extra_paths, ";:");
#endif

	std::vector<const char *> char_paths;
	char_paths.reserve(paths.size());
	for (auto &path : paths)
		char_paths.push_back(path.c_str());

	return create_concurrent_database(base_path, mode, char_paths.data(), char_paths.size());
}

void DatabaseInterface::request_shutdown()
{
	shutdown_requested.store(true, std::memory_order_relaxed);
}

bool merge_concurrent_databases_last_use(const char *append_archive, const char * const *source_paths, size_t num_source_paths,
										 bool skip_missing_inputs)
{
	std::unordered_map<Hash, uint64_t> timestamps[RESOURCE_COUNT];

	for (size_t source = 0; source < num_source_paths + 1; source++)
	{
		const char *path = source ? source_paths[source - 1] : append_archive;
		auto source_db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(path, DatabaseMode::ReadOnly));
		if (!source_db->prepare())
		{
			if (source == 0)
				continue;
			else
			{
				if (!skip_missing_inputs)
					return false;

				LOGW("Archive %s could not be prepared, skipping.\n", path);
				continue;
			}
		}

		for (unsigned i = 0; i < RESOURCE_COUNT; i++)
		{
			auto tag = static_cast<ResourceTag>(i);

			size_t hash_count = 0;
			if (!source_db->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
				return false;
			std::vector<Hash> hashes(hash_count);
			if (!source_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
				return false;

			for (auto &hash : hashes)
			{
				size_t timestamp_size = sizeof(uint64_t);
				uint64_t timestamp = 0;
				if (!source_db->read_entry(tag, hash, &timestamp_size, &timestamp, PAYLOAD_READ_NO_FLAGS) ||
				    timestamp_size != sizeof(uint64_t))
					return false;

				auto &entry = timestamps[i][hash];
				entry = std::max<uint64_t>(entry, timestamp);
			}
		}
	}

	auto write_db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(append_archive, DatabaseMode::OverWrite));
	if (!write_db->prepare())
		return false;

	for (unsigned i = 0; i < RESOURCE_COUNT; i++)
	{
		auto tag = static_cast<ResourceTag>(i);
		auto &ts = timestamps[i];

		for (auto &t : ts)
			if (!write_db->write_entry(tag, t.first, &t.second, sizeof(t.second), PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT))
				return false;
	}

	return true;
}

bool merge_concurrent_databases(const char *append_archive, const char * const *source_paths, size_t num_source_paths,
								bool skip_missing_inputs)
{
	auto append_db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(append_archive, DatabaseMode::Append));
	if (!append_db->prepare())
		return false;

	for (size_t source = 0; source < num_source_paths; source++)
	{
		const char *path = source_paths[source];
		auto source_db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(path, DatabaseMode::ReadOnly));
		if (!source_db->prepare())
		{
			if (!skip_missing_inputs)
				return false;

			LOGW("Archive %s could not be prepared, skipping.\n", path);
			continue;
		}

		for (unsigned i = 0; i < RESOURCE_COUNT; i++)
		{
			auto tag = static_cast<ResourceTag>(i);

			size_t hash_count = 0;
			if (!source_db->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
				return false;
			std::vector<Hash> hashes(hash_count);
			if (!source_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
				return false;

			for (auto &hash : hashes)
			{
				size_t blob_size = 0;
				if (!source_db->read_entry(tag, hash, &blob_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
					return false;
				std::vector<uint8_t> blob(blob_size);
				if (!source_db->read_entry(tag, hash, &blob_size, blob.data(), PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
					return false;

				if (!append_db->write_entry(tag, hash, blob.data(), blob.size(), PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT))
					return false;
			}
		}
	}

	return true;
}

}
