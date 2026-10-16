#include "modern_resources.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ZipHelpers.h"
#include "modern_resources_ids.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include <array>
#include <cstdio>
#include <string>

namespace
{
	constexpr const char* RESOURCE_MARKER = ".lrps2-modern-resources-2.8.1";

	bool IsSafeArchivePath(const std::string_view path)
	{
		if (path.empty() || path.front() == '/' || path.front() == '\\')
			return false;
		for (size_t start = 0; start < path.size();)
		{
			const size_t end = path.find_first_of("/\\", start);
			const std::string_view part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
			if (part == ".." || part == ".")
				return false;
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		return true;
	}
}

bool ModernHostEnsureEmbeddedResources(const std::string_view app_root)
{
	const std::string marker_path = Path::Combine(app_root, RESOURCE_MARKER);
	const std::string font_path = Path::Combine(app_root,
		"resources" FS_OSPATH_SEPARATOR_STR "fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
	if (FileSystem::FileExists(marker_path.c_str()) && FileSystem::FileExists(font_path.c_str()))
		return true;

#ifndef _WIN32
	return FileSystem::FileExists(font_path.c_str());
#else
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(&ModernHostEnsureEmbeddedResources)), &module))
	{
		return false;
	}
	const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_PCSX2_RESOURCES), RT_RCDATA);
	if (!resource)
		return false;
	const HGLOBAL loaded = LoadResource(module, resource);
	const void* const bytes = loaded ? LockResource(loaded) : nullptr;
	const DWORD byte_count = SizeofResource(module, resource);
	if (!bytes || byte_count == 0)
		return false;

	zip_error_t error;
	zip_error_init(&error);
	auto archive = zip_open_buffer_managed(bytes, byte_count, ZIP_RDONLY, 0, &error);
	zip_error_fini(&error);
	if (!archive)
		return false;

	const zip_int64_t count = zip_get_num_entries(archive.get(), 0);
	std::array<u8, 64 * 1024> buffer;
	for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); index++)
	{
		zip_stat_t stat;
		zip_stat_init(&stat);
		if (zip_stat_index(archive.get(), index, 0, &stat) != 0 || !stat.name)
			return false;
		const std::string_view archive_path(stat.name);
		if (!IsSafeArchivePath(archive_path))
			return false;

		std::string native_path(archive_path);
		for (char& character : native_path)
		{
			if (character == '/' || character == '\\')
				character = FS_OSPATH_SEPARATOR_CHARACTER;
		}
		const std::string output_path = Path::Combine(app_root, native_path);
		if (archive_path.back() == '/' || archive_path.back() == '\\')
		{
			if (!FileSystem::CreateDirectoryPath(output_path.c_str(), true))
				return false;
			continue;
		}

		const std::string directory(Path::GetDirectory(output_path));
		if (!directory.empty() && !FileSystem::CreateDirectoryPath(directory.c_str(), true))
			return false;
		auto input = zip_fopen_index_managed(archive.get(), index, 0);
		auto output = FileSystem::OpenManagedCFile(output_path.c_str(), "wb");
		if (!input || !output)
			return false;
		for (;;)
		{
			const zip_int64_t read = zip_fread(input.get(), buffer.data(), buffer.size());
			if (read < 0 || (read > 0 && std::fwrite(buffer.data(), 1, static_cast<size_t>(read), output.get()) != static_cast<size_t>(read)))
				return false;
			if (read == 0)
				break;
		}
	}

	auto marker = FileSystem::OpenManagedCFile(marker_path.c_str(), "wb");
	if (!marker)
		return false;
	static constexpr char marker_contents[] = "PCSX2 2.8.1 embedded resources\n";
	return std::fwrite(marker_contents, 1, sizeof(marker_contents) - 1, marker.get()) == sizeof(marker_contents) - 1;
#endif
}
