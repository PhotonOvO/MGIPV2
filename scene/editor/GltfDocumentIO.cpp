#include <mgif/scene_editor/GltfDocumentIO.h>

#include <mgif/scene_editor/GltfSemanticCompare.h>

#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace demo::editor {
namespace {

struct SecureReadState final
{
	std::filesystem::path root;
	EditorLimits limits;
	std::uint64_t aggregateBytes{0};
	GltfLoadError error{GltfLoadError::none};
	std::string message;
	std::unordered_map<std::string, std::uint64_t> readSizes;
};

struct StagedSaveState final
{
	std::filesystem::path root;
	bool embedImages{false};
	std::string error;
};

[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path)
{
	const auto value = path.generic_u8string();
	static_assert(sizeof(typename decltype(value)::value_type) == sizeof(char));
	std::string bytes(value.size(), '\0');
	if(!value.empty())
	{
		std::memcpy(bytes.data(), value.data(), value.size());
	}
	return bytes;
}

#ifdef _WIN32

[[nodiscard]] std::wstring windowsApiPath(const std::filesystem::path& path)
{
	std::filesystem::path preferred = path;
	preferred.make_preferred();
	std::wstring value = preferred.native();
	if(value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\.\\"))
	{
		return value;
	}
	if(value.starts_with(L"\\\\"))
	{
		return L"\\\\?\\UNC\\" + value.substr(2);
	}
	return L"\\\\?\\" + value;
}

[[nodiscard]] std::filesystem::path logicalWindowsPath(const std::filesystem::path& path)
{
	const std::wstring value = path.native();
	if(value.starts_with(L"\\\\?\\UNC\\"))
	{
		return std::filesystem::path(L"\\\\" + value.substr(8));
	}
	if(value.starts_with(L"\\\\?\\"))
	{
		return std::filesystem::path(value.substr(4));
	}
	return path;
}

#endif

[[nodiscard]] std::filesystem::path filesystemIoPath(const std::filesystem::path& path)
{
#ifdef _WIN32
	return std::filesystem::path(windowsApiPath(path));
#else
	return path;
#endif
}

[[nodiscard]] std::filesystem::path canonicalIoPath(
	const std::filesystem::path& path,
	std::error_code& error)
{
#ifdef _WIN32
	std::filesystem::path absolute = path;
	if(!absolute.is_absolute())
	{
		absolute = std::filesystem::absolute(path, error);
		if(error) return {};
	}
	const std::filesystem::path result =
		std::filesystem::canonical(filesystemIoPath(absolute), error);
	return error ? std::filesystem::path{} : logicalWindowsPath(result);
#else
	return std::filesystem::canonical(path, error);
#endif
}

[[nodiscard]] std::filesystem::path weaklyCanonicalIoPath(
	const std::filesystem::path& path,
	std::error_code& error)
{
#ifdef _WIN32
	std::filesystem::path absolute = path;
	if(!absolute.is_absolute())
	{
		absolute = std::filesystem::absolute(path, error);
		if(error) return {};
	}
	const std::filesystem::path result =
		std::filesystem::weakly_canonical(filesystemIoPath(absolute), error);
	return error ? std::filesystem::path{} : logicalWindowsPath(result);
#else
	return std::filesystem::weakly_canonical(path, error);
#endif
}

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view value);

[[nodiscard]] std::string lowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

[[nodiscard]] std::string imageExtension(const tinygltf::Image& image)
{
	const std::string mime = lowerAscii(image.mimeType);
	if(mime == "image/jpeg") return "jpg";
	if(mime == "image/png") return "png";
	if(mime == "image/bmp") return "bmp";
	if(mime == "image/gif") return "gif";
	if(mime == "image/webp") return "webp";
	if(mime == "image/ktx2") return "ktx2";

	std::string extension = lowerAscii(std::filesystem::path(image.uri).extension().string());
	if(!extension.empty() && extension.front() == '.')
	{
		extension.erase(extension.begin());
	}
	if(extension.empty()
		|| !std::all_of(extension.begin(), extension.end(), [](unsigned char character) { return std::isalnum(character); }))
	{
		return "bin";
	}
	return extension;
}

[[nodiscard]] std::string imageMime(const tinygltf::Image& image)
{
	if(!image.mimeType.empty()) return lowerAscii(image.mimeType);
	const std::string extension = imageExtension(image);
	if(extension == "jpg" || extension == "jpeg") return "image/jpeg";
	if(extension == "png") return "image/png";
	if(extension == "bmp") return "image/bmp";
	if(extension == "gif") return "image/gif";
	if(extension == "webp") return "image/webp";
	if(extension == "ktx2") return "image/ktx2";
	return "application/octet-stream";
}

[[nodiscard]] std::string encodeBase64(const std::vector<unsigned char>& bytes)
{
	static constexpr char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string encoded;
	encoded.reserve(((bytes.size() + 2u) / 3u) * 4u);
	for(std::size_t offset = 0; offset < bytes.size(); offset += 3u)
	{
		const std::uint32_t first = bytes[offset];
		const std::uint32_t second = offset + 1u < bytes.size() ? bytes[offset + 1u] : 0u;
		const std::uint32_t third = offset + 2u < bytes.size() ? bytes[offset + 2u] : 0u;
		const std::uint32_t value = (first << 16u) | (second << 8u) | third;
		encoded.push_back(alphabet[(value >> 18u) & 0x3fu]);
		encoded.push_back(alphabet[(value >> 12u) & 0x3fu]);
		encoded.push_back(offset + 1u < bytes.size() ? alphabet[(value >> 6u) & 0x3fu] : '=');
		encoded.push_back(offset + 2u < bytes.size() ? alphabet[value & 0x3fu] : '=');
	}
	return encoded;
}

[[nodiscard]] bool writeStagedBytes(
	const std::filesystem::path& path,
	const std::vector<unsigned char>& bytes,
	std::string_view kind,
	std::string& error)
{
	std::ofstream stream(filesystemIoPath(path), std::ios::binary | std::ios::trunc);
	if(!stream)
	{
		error = "cannot create staged " + std::string(kind) + ": " + pathToUtf8(path);
		return false;
	}
	if(!bytes.empty())
	{
		stream.write(reinterpret_cast<const char*>(bytes.data()),
		             static_cast<std::streamsize>(bytes.size()));
	}
	stream.flush();
	if(!stream)
	{
		error = "cannot flush staged " + std::string(kind) + ": " + pathToUtf8(path);
		return false;
	}
	stream.close();
	if(stream.fail())
	{
		error = "cannot close staged " + std::string(kind) + ": " + pathToUtf8(path);
		return false;
	}
	return true;
}

[[nodiscard]] bool stagedImageWriter(
	const std::string*,
	const std::string* filename,
	const tinygltf::Image* image,
	bool,
	const tinygltf::FsCallbacks*,
	const tinygltf::URICallbacks*,
	std::string* outputUri,
	void* userData)
{
	if(filename == nullptr || image == nullptr || outputUri == nullptr || userData == nullptr)
	{
		return false;
	}
	auto& state = *static_cast<StagedSaveState*>(userData);
	if(state.embedImages)
	{
		// loadCandidate keeps image.image encoded with SetImagesAsIs, including
		// TinyGLTF's unrecognized-format fallback where Image::as_is stays false.
		*outputUri = "data:" + imageMime(*image) + ";base64," + encodeBase64(image->image);
		return true;
	}

	const std::filesystem::path relative = pathFromUtf8(*filename);
	if(relative.empty() || relative.is_absolute() || relative.has_parent_path()
		|| relative.has_root_name() || relative.has_root_directory())
	{
		state.error = "TinyGLTF requested an unsafe staged image path";
		return false;
	}
	const std::filesystem::path path = state.root / relative;
	if(!writeStagedBytes(path, image->image, "image", state.error))
	{
		return false;
	}
	*outputUri = *filename;
	return true;
}

[[nodiscard]] std::string uniqueToken()
{
	static std::atomic<std::uint64_t> counter{0};
	const auto ticks = static_cast<std::uint64_t>(
		std::chrono::steady_clock::now().time_since_epoch().count());
	return std::to_string(ticks) + '-'
		+ std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] bool regularFileWithSize(
	const std::filesystem::path& path,
	std::uint64_t expectedBytes,
	std::string& error)
{
	std::error_code ec;
	if(!std::filesystem::is_regular_file(filesystemIoPath(path), ec) || ec)
	{
		error = "required dependency is not an accessible regular file: " + pathToUtf8(path);
		return false;
	}
	const std::uintmax_t size = std::filesystem::file_size(filesystemIoPath(path), ec);
	if(ec || size != expectedBytes)
	{
		error = "required dependency size changed since load: " + pathToUtf8(path);
		return false;
	}
	return true;
}

void removePathBestEffort(const std::filesystem::path& path) noexcept
{
	try
	{
		std::error_code ignored;
		std::filesystem::remove_all(filesystemIoPath(path), ignored);
	}
	catch(...)
	{
	}
}

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view value)
{
	// u8path accepts UTF-8 code units on both C++17 (char) and C++20
	// (char8_t) standard-library implementations.  Constructing path from the
	// result of generic_u8string directly is not portable across libc++ modes.
	return std::filesystem::u8path(value.begin(), value.end());
}

void rememberFailure(SecureReadState& state, GltfLoadError error, std::string message)
{
	if(state.error == GltfLoadError::none)
	{
		state.error = error;
		state.message = std::move(message);
	}
}

[[nodiscard]] bool isContainedPath(
	const std::filesystem::path& root,
	const std::filesystem::path& candidate) noexcept
{
	const std::filesystem::path relative = candidate.lexically_relative(root);
	if(relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
	{
		return false;
	}
	for(const std::filesystem::path& component : relative)
	{
		if(component == "..")
		{
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool validateDecodedUri(
	const std::string& raw,
	std::string& decoded,
	std::string& error)
{
	if(raw.empty())
	{
		error = "external URI is empty";
		return false;
	}
	if(raw.find('\\') != std::string::npos || raw.find('?') != std::string::npos || raw.find('#') != std::string::npos)
	{
		error = "external URI contains a backslash, query, or fragment";
		return false;
	}
	if(!tinygltf::URIDecode(raw, &decoded, nullptr))
	{
		error = "external URI percent decoding failed";
		return false;
	}
	if(decoded.empty()
		|| decoded.find('\0') != std::string::npos
		|| decoded.find('\\') != std::string::npos
		|| decoded.find('?') != std::string::npos
		|| decoded.find('#') != std::string::npos)
	{
		error = "decoded external URI is empty or contains an unsafe character";
		return false;
	}

	std::filesystem::path relative;
	try
	{
		relative = pathFromUtf8(decoded);
	}
	catch(const std::filesystem::filesystem_error&)
	{
		error = "decoded external URI is not a valid filesystem path";
		return false;
	}
	if(relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
	{
		error = "absolute external URIs are not allowed";
		return false;
	}
	for(const std::filesystem::path& component : relative)
	{
		if(component == "..")
		{
			error = "external URI escapes the document directory";
			return false;
		}
		if(pathToUtf8(component).find(':') != std::string::npos)
		{
			error = "external URI schemes, drive names, and alternate streams are not allowed";
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::uint64_t remainingAggregateBytes(const SecureReadState& state) noexcept
{
	return state.aggregateBytes >= state.limits.maxAggregateFileBytes
		? 0
		: state.limits.maxAggregateFileBytes - state.aggregateBytes;
}

[[nodiscard]] bool secureResolve(
	SecureReadState& state,
	const std::string& callbackPath,
	std::filesystem::path& resolved,
	bool requireExisting)
{
	std::filesystem::path candidate = pathFromUtf8(callbackPath);
	if(!candidate.is_absolute())
	{
		candidate = state.root / candidate;
	}

	std::error_code ec;
	resolved = requireExisting
		? canonicalIoPath(candidate, ec)
		: weaklyCanonicalIoPath(candidate, ec);
	if(ec)
	{
		rememberFailure(state,
		                GltfLoadError::dependencyUnavailable,
		                "cannot resolve external dependency: " + pathToUtf8(candidate));
		return false;
	}
	if(!isContainedPath(state.root, resolved))
	{
		rememberFailure(state,
		                GltfLoadError::unsafeDependency,
		                "external dependency resolves outside the document directory: " + pathToUtf8(resolved));
		return false;
	}
	if(requireExisting && (!std::filesystem::is_regular_file(filesystemIoPath(resolved), ec) || ec))
	{
		rememberFailure(state,
		                GltfLoadError::dependencyUnavailable,
		                "external dependency is not a regular file: " + pathToUtf8(resolved));
		return false;
	}
	return true;
}

[[nodiscard]] bool secureFileExists(const std::string& path, void* userData)
{
	auto& state = *static_cast<SecureReadState*>(userData);
	std::filesystem::path resolved;
	return secureResolve(state, path, resolved, true);
}

[[nodiscard]] std::string secureExpandPath(const std::string& path, void*)
{
	return path;
}

[[nodiscard]] bool secureGetFileSize(
	std::size_t* sizeOut,
	std::string* errorOut,
	const std::string& path,
	void* userData)
{
	auto& state = *static_cast<SecureReadState*>(userData);
	std::filesystem::path resolved;
	if(sizeOut == nullptr || !secureResolve(state, path, resolved, true))
	{
		if(errorOut != nullptr)
		{
			*errorOut = state.message;
		}
		return false;
	}
	std::error_code ec;
	const std::uintmax_t fileSize = std::filesystem::file_size(filesystemIoPath(resolved), ec);
	if(ec
		|| fileSize > state.limits.maxExternalFileBytes
		|| fileSize > remainingAggregateBytes(state)
		|| fileSize > std::numeric_limits<std::size_t>::max())
	{
		rememberFailure(state,
		                GltfLoadError::dependencyTooLarge,
		                "external dependency exceeds the configured file or aggregate byte limit: " + pathToUtf8(resolved));
		if(errorOut != nullptr)
		{
			*errorOut = state.message;
		}
		return false;
	}
	*sizeOut = static_cast<std::size_t>(fileSize);
	return true;
}

[[nodiscard]] bool readExactFile(
	const std::filesystem::path& path,
	std::uint64_t maximumBytes,
	std::vector<unsigned char>& output,
	std::string& error)
{
	std::error_code ec;
	const std::uintmax_t fileSize = std::filesystem::file_size(filesystemIoPath(path), ec);
	if(ec)
	{
		error = "cannot determine file size: " + pathToUtf8(path);
		return false;
	}
	if(fileSize == 0 || fileSize > maximumBytes
		|| fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())
		|| fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
	{
		error = "file is empty or exceeds its configured byte limit: " + pathToUtf8(path);
		return false;
	}

	output.resize(static_cast<std::size_t>(fileSize));
	std::ifstream stream(filesystemIoPath(path), std::ios::binary);
	if(!stream)
	{
		error = "cannot open file for reading: " + pathToUtf8(path);
		return false;
	}
	stream.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
	if(stream.gcount() != static_cast<std::streamsize>(output.size()) || stream.bad())
	{
		error = "short or failed read: " + pathToUtf8(path);
		output.clear();
		return false;
	}
	if(stream.peek() != std::char_traits<char>::eof())
	{
		error = "file changed while it was being read: " + pathToUtf8(path);
		output.clear();
		return false;
	}
	return true;
}

[[nodiscard]] bool secureReadWholeFile(
	std::vector<unsigned char>* output,
	std::string* errorOut,
	const std::string& path,
	void* userData)
{
	auto& state = *static_cast<SecureReadState*>(userData);
	std::filesystem::path resolved;
	if(output == nullptr || !secureResolve(state, path, resolved, true))
	{
		if(errorOut != nullptr)
		{
			*errorOut = state.message;
		}
		return false;
	}

	std::string readError;
	const std::uint64_t maximumBytes = std::min(
		state.limits.maxExternalFileBytes,
		remainingAggregateBytes(state));
	if(!readExactFile(resolved, maximumBytes, *output, readError))
	{
		std::error_code sizeError;
		const std::uintmax_t fileSize = std::filesystem::file_size(filesystemIoPath(resolved), sizeError);
		const GltfLoadError loadError = !sizeError && fileSize > maximumBytes
			? GltfLoadError::dependencyTooLarge
			: GltfLoadError::dependencyUnavailable;
		rememberFailure(state, loadError, std::move(readError));
		if(errorOut != nullptr)
		{
			*errorOut = state.message;
		}
		return false;
	}
	const std::uint64_t bytes = static_cast<std::uint64_t>(output->size());
	if(bytes > remainingAggregateBytes(state))
	{
		output->clear();
		rememberFailure(state, GltfLoadError::dependencyTooLarge, "aggregate external dependency bytes exceed the configured limit");
		if(errorOut != nullptr)
		{
			*errorOut = state.message;
		}
		return false;
	}
	state.aggregateBytes += bytes;
	state.readSizes[pathToUtf8(resolved)] = bytes;
	return true;
}

[[nodiscard]] bool rejectWrite(
	std::string* errorOut,
	const std::string&,
	const std::vector<unsigned char>&,
	void*)
{
	if(errorOut != nullptr)
	{
		*errorOut = "document loading does not permit filesystem writes";
	}
	return false;
}

[[nodiscard]] bool secureDecodeUri(
	const std::string& raw,
	std::string* decoded,
	void* userData)
{
	auto& state = *static_cast<SecureReadState*>(userData);
	std::string localDecoded;
	std::string error;
	if(decoded == nullptr || !validateDecodedUri(raw, localDecoded, error))
	{
		rememberFailure(state, GltfLoadError::unsafeDependency, std::move(error));
		return false;
	}
	*decoded = std::move(localDecoded);
	return true;
}

[[nodiscard]] bool isDataUri(std::string_view uri) noexcept
{
	return uri.starts_with("data:");
}

[[nodiscard]] std::string lowerExtension(const std::filesystem::path& path)
{
	std::string extension = pathToUtf8(path.extension());
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	return extension;
}

[[nodiscard]] bool addDependency(
	SecureReadState& state,
	GltfDependencyKind kind,
	std::size_t objectIndex,
	const std::string& uri,
	std::vector<GltfDependency>& output)
{
	if(uri.empty() || isDataUri(uri))
	{
		return true;
	}
	if(output.size() >= state.limits.maxDependencies)
	{
		rememberFailure(state, GltfLoadError::validationFailed, "dependency count exceeds configured limit");
		return false;
	}
	std::string decoded;
	std::string error;
	if(!validateDecodedUri(uri, decoded, error))
	{
		rememberFailure(state, GltfLoadError::unsafeDependency, std::move(error));
		return false;
	}
	std::filesystem::path resolved;
	if(!secureResolve(state, pathToUtf8(state.root / pathFromUtf8(decoded)), resolved, true))
	{
		return false;
	}
	const auto readIt = state.readSizes.find(pathToUtf8(resolved));
	if(readIt == state.readSizes.end())
	{
		rememberFailure(state,
		                GltfLoadError::dependencyUnavailable,
		                "TinyGLTF did not load external dependency: " + pathToUtf8(resolved));
		return false;
	}
	output.push_back(GltfDependency{kind, objectIndex, uri, std::move(resolved), readIt->second});
	return true;
}

[[nodiscard]] std::string payloadToken(const std::vector<unsigned char>& bytes)
{
	std::uint64_t hash = 14695981039346656037ull;
	for(const unsigned char byte : bytes)
	{
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	static constexpr char digits[] = "0123456789abcdef";
	std::string result(16, '0');
	for(std::size_t index = 0; index < result.size(); ++index)
	{
		const std::size_t shift = (result.size() - index - 1u) * 4u;
		result[index] = digits[(hash >> shift) & 0x0fu];
	}
	return result;
}

[[nodiscard]] std::string bufferResourceName(
	std::size_t index,
	const std::vector<unsigned char>& bytes)
{
	return "mgif-buffer-" + std::to_string(index) + '-' + std::to_string(bytes.size())
		+ '-' + payloadToken(bytes) + ".bin";
}

[[nodiscard]] std::string imageResourceName(std::size_t index, const tinygltf::Image& image)
{
	return "mgif-image-" + std::to_string(index) + '-' + std::to_string(image.image.size())
		+ '-' + payloadToken(image.image) + '.' + imageExtension(image);
}

class StagingDirectory final
{
public:
	explicit StagingDirectory(std::filesystem::path path) : m_path(std::move(path)) {}
	~StagingDirectory()
	{
		if(!m_path.empty())
		{
			removePathBestEffort(m_path);
		}
	}

	StagingDirectory(const StagingDirectory&) = delete;
	StagingDirectory& operator=(const StagingDirectory&) = delete;

	[[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

#ifdef _WIN32

class WindowsHandle final
{
public:
	explicit WindowsHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : m_value(value) {}
	~WindowsHandle()
	{
		if(m_value != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_value);
		}
	}

	WindowsHandle(const WindowsHandle&) = delete;
	WindowsHandle& operator=(const WindowsHandle&) = delete;

	[[nodiscard]] HANDLE get() const noexcept { return m_value; }
	[[nodiscard]] bool valid() const noexcept { return m_value != INVALID_HANDLE_VALUE; }

private:
	HANDLE m_value{INVALID_HANDLE_VALUE};
};

struct JsonStringRange final
{
	std::size_t begin{0};
	std::size_t end{0};
};

void skipJsonWhitespace(std::string_view json, std::size_t& position)
{
	while(position < json.size()
		&& (json[position] == ' ' || json[position] == '\t'
			|| json[position] == '\r' || json[position] == '\n'))
	{
		++position;
	}
}

[[nodiscard]] bool skipJsonString(std::string_view json, std::size_t& position)
{
	if(position >= json.size() || json[position] != '"') return false;
	++position;
	while(position < json.size())
	{
		const unsigned char character = static_cast<unsigned char>(json[position++]);
		if(character == '"') return true;
		if(character < 0x20u) return false;
		if(character != '\\') continue;
		if(position >= json.size()) return false;
		const char escape = json[position++];
		if(escape == 'u')
		{
			if(position + 4u > json.size()) return false;
			for(std::size_t digit = 0; digit < 4u; ++digit)
			{
				if(!std::isxdigit(static_cast<unsigned char>(json[position + digit]))) return false;
			}
			position += 4u;
		}
		else if(std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
		{
			return false;
		}
	}
	return false;
}

[[nodiscard]] bool skipJsonValue(std::string_view json, std::size_t& position)
{
	skipJsonWhitespace(json, position);
	if(position >= json.size()) return false;
	if(json[position] == '"') return skipJsonString(json, position);
	if(json[position] != '{' && json[position] != '[')
	{
		const std::size_t begin = position;
		while(position < json.size()
			&& json[position] != ',' && json[position] != ']'
			&& json[position] != '}' && json[position] != ' '
			&& json[position] != '\t' && json[position] != '\r' && json[position] != '\n')
		{
			++position;
		}
		return position != begin;
	}

	std::vector<char> closing;
	closing.push_back(json[position++] == '{' ? '}' : ']');
	while(position < json.size() && !closing.empty())
	{
		if(json[position] == '"')
		{
			if(!skipJsonString(json, position)) return false;
			continue;
		}
		if(json[position] == '{' || json[position] == '[')
		{
			closing.push_back(json[position++] == '{' ? '}' : ']');
			continue;
		}
		if(json[position] == '}' || json[position] == ']')
		{
			if(json[position] != closing.back()) return false;
			closing.pop_back();
		}
		++position;
	}
	return closing.empty();
}

[[nodiscard]] bool jsonKeyEquals(
	std::string_view json,
	const JsonStringRange& range,
	std::string_view key)
{
	return range.end == range.begin + key.size() + 2u
		&& json.substr(range.begin + 1u, key.size()) == key;
}

[[nodiscard]] bool collectBufferObjectUri(
	std::string_view json,
	std::size_t& position,
	JsonStringRange& uriRange)
{
	skipJsonWhitespace(json, position);
	if(position >= json.size() || json[position++] != '{') return false;
	bool foundUri = false;
	while(true)
	{
		skipJsonWhitespace(json, position);
		if(position >= json.size()) return false;
		if(json[position] == '}')
		{
			++position;
			return foundUri;
		}

		JsonStringRange key{position, 0};
		if(!skipJsonString(json, position)) return false;
		key.end = position;
		skipJsonWhitespace(json, position);
		if(position >= json.size() || json[position++] != ':') return false;
		skipJsonWhitespace(json, position);
		if(jsonKeyEquals(json, key, "uri"))
		{
			if(foundUri) return false;
			uriRange.begin = position;
			if(!skipJsonString(json, position)) return false;
			uriRange.end = position;
			foundUri = true;
		}
		else if(!skipJsonValue(json, position))
		{
			return false;
		}

		skipJsonWhitespace(json, position);
		if(position >= json.size()) return false;
		if(json[position] == ',')
		{
			++position;
			continue;
		}
		if(json[position] != '}') return false;
	}
}

[[nodiscard]] bool collectBufferUris(
	std::string_view json,
	std::size_t& position,
	std::vector<JsonStringRange>& uriRanges)
{
	skipJsonWhitespace(json, position);
	if(position >= json.size() || json[position++] != '[') return false;
	while(true)
	{
		skipJsonWhitespace(json, position);
		if(position >= json.size()) return false;
		if(json[position] == ']')
		{
			++position;
			return true;
		}
		JsonStringRange uri;
		if(!collectBufferObjectUri(json, position, uri)) return false;
		uriRanges.push_back(uri);
		skipJsonWhitespace(json, position);
		if(position >= json.size()) return false;
		if(json[position] == ',')
		{
			++position;
			continue;
		}
		if(json[position] != ']') return false;
	}
}

[[nodiscard]] bool findStagedBufferUris(
	std::string_view json,
	std::vector<JsonStringRange>& uriRanges)
{
	std::size_t position = 0;
	skipJsonWhitespace(json, position);
	if(position >= json.size() || json[position++] != '{') return false;
	bool foundBuffers = false;
	while(true)
	{
		skipJsonWhitespace(json, position);
		if(position >= json.size()) return false;
		if(json[position] == '}')
		{
			++position;
			skipJsonWhitespace(json, position);
			return foundBuffers && position == json.size();
		}

		JsonStringRange key{position, 0};
		if(!skipJsonString(json, position)) return false;
		key.end = position;
		skipJsonWhitespace(json, position);
		if(position >= json.size() || json[position++] != ':') return false;
		if(jsonKeyEquals(json, key, "buffers"))
		{
			if(foundBuffers || !collectBufferUris(json, position, uriRanges)) return false;
			foundBuffers = true;
		}
		else if(!skipJsonValue(json, position))
		{
			return false;
		}

		skipJsonWhitespace(json, position);
		if(position >= json.size()) return false;
		if(json[position] == ',')
		{
			++position;
			continue;
		}
		if(json[position] != '}') return false;
	}
}

[[nodiscard]] std::string jsonStringLiteral(std::string_view value)
{
	static constexpr char hex[] = "0123456789abcdef";
	std::string output(1, '"');
	for(const unsigned char character : value)
	{
		switch(character)
		{
		case '"': output += "\\\""; break;
		case '\\': output += "\\\\"; break;
		case '\b': output += "\\b"; break;
		case '\f': output += "\\f"; break;
		case '\n': output += "\\n"; break;
		case '\r': output += "\\r"; break;
		case '\t': output += "\\t"; break;
		default:
			if(character < 0x20u)
			{
				output += "\\u00";
				output.push_back(hex[(character >> 4u) & 0x0fu]);
				output.push_back(hex[character & 0x0fu]);
			}
			else
			{
				output.push_back(static_cast<char>(character));
			}
			break;
		}
	}
	output.push_back('"');
	return output;
}

[[nodiscard]] bool writeWindowsStagedScene(
	tinygltf::TinyGLTF& writer,
	const tinygltf::Model& model,
	const std::filesystem::path& stagedMain,
	bool binary,
	bool prettyPrint,
	StagedSaveState& state)
{
	if(binary)
	{
		std::ofstream stream(filesystemIoPath(stagedMain), std::ios::binary | std::ios::trunc);
		if(!stream)
		{
			state.error = "cannot create staged document: " + pathToUtf8(stagedMain);
			return false;
		}
		if(!writer.WriteGltfSceneToStream(&model, stream, prettyPrint, true))
		{
			return false;
		}
		stream.flush();
		if(!stream)
		{
			state.error = "cannot flush staged document: " + pathToUtf8(stagedMain);
			return false;
		}
		stream.close();
		if(stream.fail())
		{
			state.error = "cannot close staged document: " + pathToUtf8(stagedMain);
			return false;
		}
		return true;
	}

	std::ostringstream serialized;
	if(!writer.WriteGltfSceneToStream(&model, serialized, prettyPrint, false))
	{
		return false;
	}
	std::string json = serialized.str();
	std::vector<JsonStringRange> uriRanges;
	if(!model.buffers.empty()
		&& (!findStagedBufferUris(json, uriRanges) || uriRanges.size() != model.buffers.size()))
	{
		state.error = "TinyGLTF produced an inconsistent staged buffer list";
		return false;
	}

	for(std::size_t index = model.buffers.size(); index-- > 0;)
	{
		const tinygltf::Buffer& buffer = model.buffers[index];
		if(buffer.uri.empty())
		{
			state.error = "TinyGLTF produced an invalid staged buffer entry";
			return false;
		}
		const JsonStringRange range = uriRanges[index];
		json.replace(range.begin, range.end - range.begin, jsonStringLiteral(buffer.uri));
		if(!writeStagedBytes(
			state.root / pathFromUtf8(buffer.uri),
			buffer.data,
			"buffer",
			state.error))
		{
			return false;
		}
	}

	const std::vector<unsigned char> bytes(json.begin(), json.end());
	return writeStagedBytes(stagedMain, bytes, "document", state.error);
}

[[nodiscard]] std::string windowsErrorMessage(std::string_view operation, DWORD code)
{
	const std::error_code errorCode(static_cast<int>(code), std::system_category());
	return std::string(operation) + ": " + errorCode.message()
		+ " (Win32 error " + std::to_string(code) + ')';
}

[[nodiscard]] bool flushWindowsFile(
	const std::filesystem::path& path,
	DWORD* volumeSerial,
	std::string& error)
{
	const std::wstring nativePath = windowsApiPath(path);
	WindowsHandle handle(CreateFileW(
		nativePath.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
		nullptr));
	if(!handle.valid())
	{
		error = windowsErrorMessage("cannot open a Save As file for durable flush", GetLastError());
		return false;
	}
	if(volumeSerial != nullptr)
	{
		BY_HANDLE_FILE_INFORMATION information{};
		if(!GetFileInformationByHandle(handle.get(), &information))
		{
			error = windowsErrorMessage("cannot inspect the staged Save As volume", GetLastError());
			return false;
		}
		*volumeSerial = information.dwVolumeSerialNumber;
	}
	if(!FlushFileBuffers(handle.get()))
	{
		error = windowsErrorMessage("cannot durably flush a Save As file", GetLastError());
		return false;
	}
	return true;
}

[[nodiscard]] bool sameWindowsVolume(
	const std::filesystem::path& staged,
	const std::filesystem::path& target,
	std::string& error)
{
	DWORD stagedVolume = 0;
	if(!flushWindowsFile(staged, &stagedVolume, error))
	{
		return false;
	}

	const std::wstring targetPath = windowsApiPath(target);
	WindowsHandle targetHandle(CreateFileW(
		targetPath.c_str(),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr));
	if(!targetHandle.valid())
	{
		error = windowsErrorMessage("cannot open the existing Save As target", GetLastError());
		return false;
	}
	BY_HANDLE_FILE_INFORMATION targetInformation{};
	if(!GetFileInformationByHandle(targetHandle.get(), &targetInformation))
	{
		error = windowsErrorMessage("cannot inspect the existing Save As target volume", GetLastError());
		return false;
	}
	if(stagedVolume != targetInformation.dwVolumeSerialNumber)
	{
		error = "staged Save As file and existing target are not on the same volume";
		return false;
	}
	return true;
}

#endif

[[nodiscard]] bool flushStagedFileForPublication(
	const std::filesystem::path& path,
	std::string& error)
{
#ifdef _WIN32
	return flushWindowsFile(path, nullptr, error);
#else
	(void)path;
	(void)error;
	return true;
#endif
}

[[nodiscard]] bool createStagingDirectory(
	const std::filesystem::path& parent,
	std::filesystem::path& output,
	std::string& error)
{
	static std::atomic<std::uint64_t> stagingSequence{0};
	const std::string token = std::to_string(stagingSequence.fetch_add(1, std::memory_order_relaxed));
	for(std::size_t attempt = 0; attempt < 64; ++attempt)
	{
		output = parent / (".mgif-s-" + token + '-' + std::to_string(attempt));
		std::error_code ec;
		if(std::filesystem::create_directory(filesystemIoPath(output), ec) && !ec)
		{
			return true;
		}
		if(ec && ec != std::errc::file_exists)
		{
			error = "cannot create same-volume staging directory: " + ec.message();
			return false;
		}
	}
	error = "cannot allocate a unique same-volume staging directory";
	return false;
}

[[nodiscard]] bool filesEqual(
	const std::filesystem::path& left,
	const std::filesystem::path& right,
	std::string& error)
{
	std::error_code ec;
	const std::uintmax_t leftSize = std::filesystem::file_size(filesystemIoPath(left), ec);
	if(ec)
	{
		error = "cannot inspect staged resource: " + pathToUtf8(left);
		return false;
	}
	const std::uintmax_t rightSize = std::filesystem::file_size(filesystemIoPath(right), ec);
	if(ec)
	{
		error = "cannot inspect existing resource: " + pathToUtf8(right);
		return false;
	}
	if(leftSize != rightSize)
	{
		error = "content-addressed resource name collides with different byte size";
		return false;
	}
	std::ifstream leftStream(filesystemIoPath(left), std::ios::binary);
	std::ifstream rightStream(filesystemIoPath(right), std::ios::binary);
	if(!leftStream || !rightStream)
	{
		error = "cannot compare staged and existing resources";
		return false;
	}
	std::array<char, 64u * 1024u> leftBytes{};
	std::array<char, 64u * 1024u> rightBytes{};
	while(leftStream || rightStream)
	{
		leftStream.read(leftBytes.data(), static_cast<std::streamsize>(leftBytes.size()));
		rightStream.read(rightBytes.data(), static_cast<std::streamsize>(rightBytes.size()));
		if(leftStream.gcount() != rightStream.gcount()
			|| !std::equal(leftBytes.begin(), leftBytes.begin() + leftStream.gcount(), rightBytes.begin()))
		{
			error = "content-addressed resource name collides with different bytes";
			return false;
		}
	}
	if(leftStream.bad() || rightStream.bad())
	{
		error = "resource comparison failed while reading";
		return false;
	}
	return true;
}

[[nodiscard]] bool publishResource(
	const std::filesystem::path& staged,
	const std::filesystem::path& target,
	bool& created,
	std::string& error)
{
	created = false;
	std::error_code ec;
	const std::filesystem::file_status status = std::filesystem::symlink_status(filesystemIoPath(target), ec);
	if(!ec && status.type() != std::filesystem::file_type::not_found)
	{
		if(status.type() != std::filesystem::file_type::regular)
		{
			error = "resource target is not a regular file: " + pathToUtf8(target);
			return false;
		}
		if(!filesEqual(staged, target, error))
		{
			return false;
		}
		std::filesystem::remove(filesystemIoPath(staged), ec);
		return !ec;
	}
	if(ec && ec != std::errc::no_such_file_or_directory)
	{
		error = "cannot inspect resource target: " + ec.message();
		return false;
	}
	if(!flushStagedFileForPublication(staged, error))
	{
		return false;
	}
	std::filesystem::rename(filesystemIoPath(staged), filesystemIoPath(target), ec);
	if(ec)
	{
		error = "cannot publish staged resource: " + ec.message();
		return false;
	}
	created = true;
	return true;
}

struct MainPublishResult final
{
	bool succeeded{false};
	bool usedFallback{false};
	GltfSaveError errorCode{GltfSaveError::publishFailed};
	std::filesystem::path recoveryPath;
	std::string error;
};

[[nodiscard]] MainPublishResult publishMainFile(
	const std::filesystem::path& staged,
	const std::filesystem::path& target,
	EditorFaultInjector* injector)
{
	MainPublishResult result;
	std::error_code ec;
	const std::filesystem::file_status status = std::filesystem::symlink_status(filesystemIoPath(target), ec);
	const bool exists = !ec && status.type() != std::filesystem::file_type::not_found;
	if(exists && status.type() != std::filesystem::file_type::regular)
	{
		result.error = "Save As target is not a regular file";
		return result;
	}
	if(ec && ec != std::errc::no_such_file_or_directory)
	{
		result.error = "cannot inspect Save As target: " + ec.message();
		return result;
	}
	if(exists)
	{
		const auto writable = std::filesystem::perms::owner_write
			| std::filesystem::perms::group_write
			| std::filesystem::perms::others_write;
		if((status.permissions() & writable) == std::filesystem::perms::none)
		{
			result.error = "Save As target is read-only";
			return result;
		}
	}

	std::string flushError;
	if(!flushStagedFileForPublication(staged, flushError))
	{
		result.error = std::move(flushError);
		return result;
	}

#ifndef _WIN32
	if(injector != nullptr && injector->shouldFail(EditorFaultPoint::preAtomicReplace))
	{
		result.errorCode = GltfSaveError::injectedFailure;
		result.error = "Save As pre-replace fault injected at the atomic rename boundary";
		return result;
	}
	std::filesystem::rename(staged, target, ec);
	if(!ec)
	{
		result.succeeded = true;
		return result;
	}
	if(!exists)
	{
		result.error = "atomic Save As publish failed: " + ec.message();
		return result;
	}

	result.usedFallback = true;
	const std::filesystem::path backup = target.parent_path()
		/ (".mgif-b-" + uniqueToken());
	std::filesystem::rename(target, backup, ec);
	if(ec)
	{
		result.error = "cannot move the existing target to a recoverable backup: " + ec.message();
		return result;
	}
	std::filesystem::rename(staged, target, ec);
	if(!ec)
	{
		std::error_code removeError;
		std::filesystem::remove(backup, removeError);
		result.succeeded = true;
		if(removeError)
		{
			result.recoveryPath = backup;
		}
		return result;
	}

	const std::string publishError = ec.message();
	std::error_code restoreError;
	std::filesystem::rename(backup, target, restoreError);
	result.error = "recoverable Save As publish failed: " + publishError;
	if(restoreError)
	{
		result.error += "; restoring the original target also failed: " + restoreError.message();
		result.recoveryPath = backup;
	}
	return result;
#else
	const std::wstring stagedPath = windowsApiPath(staged);
	const std::wstring targetPath = windowsApiPath(target);
	if(!exists)
	{
		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::preAtomicReplace))
		{
			result.errorCode = GltfSaveError::injectedFailure;
			result.error = "Save As pre-replace fault injected at the atomic move boundary";
			return result;
		}
		if(MoveFileExW(stagedPath.c_str(), targetPath.c_str(), MOVEFILE_WRITE_THROUGH))
		{
			result.succeeded = true;
			return result;
		}
		result.error = windowsErrorMessage("atomic Save As publication failed", GetLastError());
		return result;
	}

	if(!sameWindowsVolume(staged, target, result.error))
	{
		return result;
	}
	if(injector != nullptr && injector->shouldFail(EditorFaultPoint::preAtomicReplace))
	{
		result.errorCode = GltfSaveError::injectedFailure;
		result.error = "Save As pre-replace fault injected at the atomic MoveFileExW boundary";
		return result;
	}
	if(!MoveFileExW(
		stagedPath.c_str(),
		targetPath.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD replaceError = GetLastError();
		result.error = windowsErrorMessage("atomic MoveFileExW publication failed", replaceError);
		return result;
	}
	result.succeeded = true;
	return result;
#endif
}

} // namespace

GltfLoadResult GltfDocumentIO::failure(
	GltfLoadError error,
	std::string message,
	std::string warnings,
	GltfValidationReport validationReport)
{
	return GltfLoadResult(
		error,
		std::move(message),
		std::move(warnings),
		std::move(validationReport),
		nullptr);
}

GltfLoadResult::GltfLoadResult(
	GltfLoadError error,
	std::string message,
	std::string warnings,
	GltfValidationReport validationReport,
	std::unique_ptr<GltfDocument> candidate) noexcept
	: m_error(error),
	  m_message(std::move(message)),
	  m_warnings(std::move(warnings)),
	  m_validationReport(std::move(validationReport)),
	  m_candidate(std::move(candidate))
{
}

GltfLoadResult::GltfLoadResult(GltfLoadResult&&) noexcept = default;
GltfLoadResult& GltfLoadResult::operator=(GltfLoadResult&&) noexcept = default;
GltfLoadResult::~GltfLoadResult() = default;

bool GltfLoadResult::succeeded() const noexcept
{
	return m_error == GltfLoadError::none && m_candidate != nullptr && m_validationReport.valid();
}

GltfLoadError GltfLoadResult::error() const noexcept
{
	return m_error;
}

const std::string& GltfLoadResult::message() const noexcept
{
	return m_message;
}

const std::string& GltfLoadResult::warnings() const noexcept
{
	return m_warnings;
}

const GltfValidationReport& GltfLoadResult::validationReport() const noexcept
{
	return m_validationReport;
}

GltfDocument* GltfLoadResult::candidate() noexcept
{
	return m_candidate.get();
}

const GltfDocument* GltfLoadResult::candidate() const noexcept
{
	return m_candidate.get();
}

std::unique_ptr<GltfDocument> GltfLoadResult::takeCandidate() && noexcept
{
	if(!succeeded()) return {};
	return std::move(m_candidate);
}

bool GltfLoadResult::publishTo(std::unique_ptr<GltfDocument>& active) && noexcept
{
	auto candidate = std::move(*this).takeCandidate();
	if(!candidate) return false;
	active = std::move(candidate);
	return true;
}

GltfSaveResult::GltfSaveResult(
	GltfSaveError error,
	std::string message,
	GltfValidationReport validationReport,
	bool usedRecoverableReplaceFallback,
	std::filesystem::path recoveryPath) noexcept
	: m_error(error),
	  m_message(std::move(message)),
	  m_validationReport(std::move(validationReport)),
	  m_usedRecoverableReplaceFallback(usedRecoverableReplaceFallback),
	  m_recoveryPath(std::move(recoveryPath))
{
}

bool GltfSaveResult::succeeded() const noexcept
{
	return m_error == GltfSaveError::none && m_validationReport.valid();
}

GltfSaveError GltfSaveResult::error() const noexcept
{
	return m_error;
}

const std::string& GltfSaveResult::message() const noexcept
{
	return m_message;
}

const GltfValidationReport& GltfSaveResult::validationReport() const noexcept
{
	return m_validationReport;
}

bool GltfSaveResult::usedRecoverableReplaceFallback() const noexcept
{
	return m_usedRecoverableReplaceFallback;
}

const std::filesystem::path& GltfSaveResult::recoveryPath() const noexcept
{
	return m_recoveryPath;
}

GltfSaveResult GltfDocumentIO::saveFailure(
	GltfSaveError error,
	std::string message,
	GltfValidationReport validationReport,
	bool usedRecoverableReplaceFallback,
	std::filesystem::path recoveryPath)
{
	return GltfSaveResult(
		error,
		std::move(message),
		std::move(validationReport),
		usedRecoverableReplaceFallback,
		std::move(recoveryPath));
}

GltfLoadResult GltfDocumentIO::loadCandidate(
	const std::filesystem::path& sourcePath,
	const GltfLoadOptions& options)
{
	try
	{
		std::error_code ec;
		const std::filesystem::path canonicalSource = canonicalIoPath(sourcePath, ec);
		if(ec || !std::filesystem::is_regular_file(filesystemIoPath(canonicalSource), ec) || ec)
		{
			return failure(GltfLoadError::sourceUnavailable, "source is not an accessible regular file");
		}

		const std::string extension = lowerExtension(canonicalSource);
		const bool binary = extension == ".glb";
		if(!binary && extension != ".gltf")
		{
			return failure(GltfLoadError::unsupportedFormat, "only .gltf and .glb sources are supported");
		}

		std::vector<unsigned char> sourceBytes;
		std::string sourceError;
		if(!readExactFile(canonicalSource, options.limits.maxSourceBytes, sourceBytes, sourceError))
		{
			return failure(GltfLoadError::sourceTooLarge, std::move(sourceError));
		}
		if(sourceBytes.size() > std::numeric_limits<unsigned int>::max())
		{
			return failure(GltfLoadError::sourceTooLarge, "source exceeds TinyGLTF's bounded memory parser range");
		}
		const bool hasGlbMagic = sourceBytes.size() >= 4
			&& sourceBytes[0] == 'g' && sourceBytes[1] == 'l'
			&& sourceBytes[2] == 'T' && sourceBytes[3] == 'F';
		if(binary != hasGlbMagic)
		{
			return failure(GltfLoadError::invalidContainer, "source suffix and glTF container magic disagree");
		}

		EditorFaultInjector* injector = options.faultInjector;
		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::parse))
		{
			return failure(GltfLoadError::injectedFailure, "parse fault injected");
		}
		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::allocation))
		{
			return failure(GltfLoadError::injectedFailure, "allocation fault injected");
		}

		auto model = std::make_unique<tinygltf::Model>();
		SecureReadState readState{
			canonicalSource.parent_path(),
			options.limits,
		};

		tinygltf::TinyGLTF loader;
		loader.SetParseStrictness(tinygltf::ParseStrictness::Strict);
		loader.SetImagesAsIs(true);
		loader.SetStoreOriginalJSONForExtrasAndExtensions(true);
		loader.SetMaxExternalFileSize(static_cast<std::size_t>(std::min<std::uint64_t>(
			options.limits.maxExternalFileBytes,
			std::numeric_limits<std::size_t>::max())));

		std::string callbackError;
		if(!loader.SetURICallbacks(tinygltf::URICallbacks{
			.encode = nullptr,
			.decode = &secureDecodeUri,
			.user_data = &readState,
		}, &callbackError)
			|| !loader.SetFsCallbacks(tinygltf::FsCallbacks{
				.FileExists = &secureFileExists,
				.ExpandFilePath = &secureExpandPath,
				.ReadWholeFile = &secureReadWholeFile,
				.WriteWholeFile = &rejectWrite,
				.GetFileSizeInBytes = &secureGetFileSize,
				.user_data = &readState,
			}, &callbackError))
		{
			return failure(GltfLoadError::internalError, "failed to install bounded TinyGLTF callbacks: " + callbackError);
		}

		std::string parseError;
		std::string warnings;
		const std::string baseDirectory = pathToUtf8(readState.root) + "/";
		const bool parsed = binary
			? loader.LoadBinaryFromMemory(model.get(),
			                              &parseError,
			                              &warnings,
			                              sourceBytes.data(),
			                              static_cast<unsigned int>(sourceBytes.size()),
			                              baseDirectory)
			: loader.LoadASCIIFromString(model.get(),
			                             &parseError,
			                             &warnings,
			                             reinterpret_cast<const char*>(sourceBytes.data()),
			                             static_cast<unsigned int>(sourceBytes.size()),
			                             baseDirectory);
		if(readState.error != GltfLoadError::none)
		{
			return failure(readState.error, readState.message, std::move(warnings));
		}
		if(!parsed || !parseError.empty())
		{
			return failure(GltfLoadError::parseFailed,
			               parseError.empty() ? "TinyGLTF rejected the source" : std::move(parseError),
			               std::move(warnings));
		}

		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::postParseValidation))
		{
			return failure(GltfLoadError::injectedFailure, "post-parse validation fault injected", std::move(warnings));
		}

		GltfValidationReport validation = GltfValidator::validateModel(*model, options.limits);
		if(!validation.valid())
		{
			return failure(GltfLoadError::validationFailed,
			               "parsed glTF failed bounded document validation",
			               std::move(warnings),
			               std::move(validation));
		}

		std::vector<GltfDependency> dependencies;
		std::uint64_t dependencyCapacity = 0;
		if(!editorCheckedAdd(model->buffers.size(), model->images.size(), dependencyCapacity))
		{
			return failure(GltfLoadError::validationFailed, "dependency collection count overflows", std::move(warnings));
		}
		dependencies.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
			dependencyCapacity,
			options.limits.maxDependencies)));
		for(std::size_t index = 0; index < model->buffers.size(); ++index)
		{
			if(!addDependency(readState, GltfDependencyKind::buffer, index, model->buffers[index].uri, dependencies))
			{
				return failure(readState.error, readState.message, std::move(warnings));
			}
		}
		for(std::size_t index = 0; index < model->images.size(); ++index)
		{
			if(!addDependency(readState, GltfDependencyKind::image, index, model->images[index].uri, dependencies))
			{
				return failure(readState.error, readState.message, std::move(warnings));
			}
		}
		if(dependencies.size() > options.limits.maxDependencies)
		{
			return failure(GltfLoadError::validationFailed, "dependency count exceeds configured limit", std::move(warnings));
		}

		auto candidate = std::unique_ptr<GltfDocument>(new GltfDocument(
			std::move(model),
			canonicalSource,
			binary ? GltfDocumentFormat::binary : GltfDocumentFormat::ascii,
			std::move(dependencies)));
		return GltfLoadResult(
			GltfLoadError::none,
			{},
			std::move(warnings),
			std::move(validation),
			std::move(candidate));
	}
	catch(const std::bad_alloc&)
	{
		return failure(GltfLoadError::allocationFailed, "allocation failed while preparing a candidate document");
	}
	catch(const std::exception& exception)
	{
		return failure(GltfLoadError::internalError, exception.what());
	}
	catch(...)
	{
		return failure(GltfLoadError::internalError, "unknown failure while preparing a candidate document");
	}
}

GltfLoadResult GltfDocumentIO::createEmptyCandidate(std::string_view sceneName)
{
	try
	{
		auto model = std::make_unique<tinygltf::Model>();
		model->asset.version = "2.0";
		model->asset.generator = "MGIF Scene Editor";
		tinygltf::Scene scene;
		scene.name = sceneName.empty() ? "Scene" : std::string(sceneName);
		model->scenes.push_back(std::move(scene));
		model->defaultScene = 0;

		GltfValidationReport validation = GltfValidator::validateModel(
			*model, kDefaultEditorLimits);
		if(!validation.valid())
		{
			return failure(GltfLoadError::validationFailed,
			               "new scene candidate failed bounded document validation",
			               {}, std::move(validation));
		}

		auto candidate = std::unique_ptr<GltfDocument>(new GltfDocument(
			std::move(model), {}, GltfDocumentFormat::ascii, {}));
		return GltfLoadResult(GltfLoadError::none, {}, {}, std::move(validation),
		                       std::move(candidate));
	}
	catch(const std::bad_alloc&)
	{
		return failure(GltfLoadError::allocationFailed,
		               "allocation failed while creating a new scene candidate");
	}
	catch(const std::exception& exception)
	{
		return failure(GltfLoadError::internalError, exception.what());
	}
	catch(...)
	{
		return failure(GltfLoadError::internalError,
		               "unknown failure while creating a new scene candidate");
	}
}

GltfSaveResult GltfDocumentIO::saveAs(
	GltfDocument& document,
	const std::filesystem::path& targetPath,
	const GltfSaveOptions& options)
{
	try
	{
		GltfValidationReport validation = GltfValidator::validate(document, options.limits);
		if(!validation.valid())
		{
			return saveFailure(GltfSaveError::invalidDocument,
			                   "document validation failed before Save As",
			                   std::move(validation));
		}

		for(const GltfDependency& dependency : document.dependencies())
		{
			std::string dependencyError;
			if(!regularFileWithSize(dependency.resolvedPath, dependency.byteSize, dependencyError))
			{
				return saveFailure(GltfSaveError::dependencyUnavailable, std::move(dependencyError));
			}
		}

		const std::string extension = lowerExtension(targetPath);
		const bool binary = extension == ".glb";
		if(!binary && extension != ".gltf")
		{
			return saveFailure(GltfSaveError::unsupportedFormat,
			                   "Save As supports only .gltf and .glb targets");
		}
		if(targetPath.filename().empty() || targetPath.filename() == "." || targetPath.filename() == "..")
		{
			return saveFailure(GltfSaveError::unsafeTarget, "Save As target filename is invalid");
		}

		std::error_code ec;
		std::filesystem::path requestedParent = targetPath.parent_path();
		if(requestedParent.empty())
		{
			requestedParent = std::filesystem::current_path(ec);
		}
		if(ec)
		{
			return saveFailure(GltfSaveError::unsafeTarget, "cannot resolve the current target directory");
		}
		const std::filesystem::path parent = canonicalIoPath(requestedParent, ec);
		if(ec || !std::filesystem::is_directory(filesystemIoPath(parent), ec) || ec)
		{
			return saveFailure(GltfSaveError::unsafeTarget,
			                   "Save As target directory is not accessible");
		}
		const std::filesystem::path target = parent / targetPath.filename();

		EditorFaultInjector* injector = options.faultInjector;
		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::allocation))
		{
			return saveFailure(GltfSaveError::injectedFailure, "Save As allocation fault injected");
		}

		auto stagedModel = std::make_unique<tinygltf::Model>(document.authorityModel());
		std::vector<std::string> resourceNames;
		if(binary)
		{
			for(tinygltf::Buffer& buffer : stagedModel->buffers)
			{
				buffer.uri.clear();
			}
		}
		else
		{
			resourceNames.reserve(stagedModel->buffers.size() + stagedModel->images.size());
			for(std::size_t index = 0; index < stagedModel->buffers.size(); ++index)
			{
				const std::string name = bufferResourceName(index, stagedModel->buffers[index].data);
				stagedModel->buffers[index].uri = name;
				resourceNames.push_back(name);
			}
		}
		for(std::size_t index = 0; index < stagedModel->images.size(); ++index)
		{
			tinygltf::Image& image = stagedModel->images[index];
			if(image.bufferView >= 0)
			{
				continue;
			}
			const std::string name = imageResourceName(index, image);
			image.uri = name;
			if(!binary)
			{
				resourceNames.push_back(name);
			}
		}

		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::stagingFileCreate))
		{
			return saveFailure(GltfSaveError::injectedFailure, "Save As staging creation fault injected");
		}
		std::filesystem::path stagePath;
		std::string stageError;
		if(!createStagingDirectory(parent, stagePath, stageError))
		{
			return saveFailure(GltfSaveError::stagingUnavailable, std::move(stageError));
		}
		StagingDirectory staging(stagePath);
		const std::filesystem::path stagedMain = stagePath / target.filename();

		StagedSaveState writeState{stagePath, binary};
		tinygltf::TinyGLTF writer;
		writer.SetImageWriter(&stagedImageWriter, &writeState);
		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::write))
		{
			return saveFailure(GltfSaveError::injectedFailure, "Save As write fault injected");
		}
#ifdef _WIN32
		const bool wrote = writeWindowsStagedScene(
			writer,
			*stagedModel,
			stagedMain,
			binary,
			options.prettyPrint,
			writeState);
#else
		const bool wrote = writer.WriteGltfSceneToFile(
			stagedModel.get(),
			pathToUtf8(stagedMain),
			binary,
			binary,
			options.prettyPrint,
			binary);
#endif
		if(!wrote)
		{
			return saveFailure(GltfSaveError::serializationFailed,
			                   writeState.error.empty()
				                   ? "TinyGLTF could not serialize the staged Save As document"
				                   : std::move(writeState.error));
		}
		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::flush))
		{
			return saveFailure(GltfSaveError::injectedFailure, "Save As flush fault injected");
		}

		std::error_code sizeError;
		const std::uintmax_t stagedMainBytes =
			std::filesystem::file_size(filesystemIoPath(stagedMain), sizeError);
		if(sizeError || stagedMainBytes == 0)
		{
			return saveFailure(GltfSaveError::serializationFailed,
			                   "staged Save As document is missing or empty");
		}
		for(const std::string& resource : resourceNames)
		{
			if(!std::filesystem::is_regular_file(
				filesystemIoPath(stagePath / pathFromUtf8(resource)), sizeError) || sizeError)
			{
				return saveFailure(GltfSaveError::serializationFailed,
				                   "staged Save As resource is missing: " + resource);
			}
		}

		GltfLoadOptions stagedLoadOptions;
		stagedLoadOptions.limits = options.limits;
		GltfLoadResult stagedLoad = loadCandidate(stagedMain, stagedLoadOptions);
		if(!stagedLoad.succeeded() || stagedLoad.candidate() == nullptr)
		{
			return saveFailure(GltfSaveError::stagedValidationFailed,
			                   "staged Save As document did not reload: " + stagedLoad.message(),
			                   stagedLoad.validationReport());
		}
		GltfSemanticComparison comparison = GltfSemanticCompare::compare(document, *stagedLoad.candidate());
		if(!comparison.equivalent())
		{
			return saveFailure(GltfSaveError::semanticMismatch,
			                   "staged Save As document is not semantically equivalent");
		}

		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::candidateUpload))
		{
			return saveFailure(GltfSaveError::injectedFailure, "Save As candidate handoff fault injected");
		}
		if(injector != nullptr && injector->shouldFail(EditorFaultPoint::preCandidatePublish))
		{
			return saveFailure(GltfSaveError::injectedFailure, "Save As candidate publish fault injected");
		}
		std::vector<GltfDependency> savedDependencies(
			stagedLoad.candidate()->dependencies().begin(),
			stagedLoad.candidate()->dependencies().end());
		for(GltfDependency& dependency : savedDependencies)
		{
			dependency.resolvedPath = parent / dependency.resolvedPath.filename();
		}
		std::unique_ptr<tinygltf::Model> savedModel = stagedLoad.candidate()->takeAuthorityModel();

		std::vector<std::filesystem::path> createdResources;
		for(const std::string& resource : resourceNames)
		{
			const std::filesystem::path relative = pathFromUtf8(resource);
			bool created = false;
			std::string publishError;
			if(!publishResource(stagePath / relative, parent / relative, created, publishError))
			{
				for(const std::filesystem::path& createdPath : createdResources)
				{
					removePathBestEffort(createdPath);
				}
				return saveFailure(GltfSaveError::publishFailed, std::move(publishError));
			}
			if(created)
			{
				createdResources.push_back(parent / relative);
			}
		}

		MainPublishResult publish = publishMainFile(stagedMain, target, injector);
		if(!publish.succeeded)
		{
			for(const std::filesystem::path& createdPath : createdResources)
			{
				removePathBestEffort(createdPath);
			}
			return saveFailure(publish.errorCode,
			                   std::move(publish.error),
			                   {},
			                   publish.usedFallback,
			                   std::move(publish.recoveryPath));
		}

		document.acceptSavedAuthority(
			std::move(savedModel),
			target,
			binary ? GltfDocumentFormat::binary : GltfDocumentFormat::ascii,
			std::move(savedDependencies));
		return GltfSaveResult(
			GltfSaveError::none,
			publish.recoveryPath.empty()
				? std::string{}
				: "Save As succeeded but the recoverable backup could not be removed",
			std::move(validation),
			publish.usedFallback,
			std::move(publish.recoveryPath));
	}
	catch(const std::bad_alloc&)
	{
		return saveFailure(GltfSaveError::allocationFailed, "allocation failed while staging Save As");
	}
	catch(const std::exception& exception)
	{
		return saveFailure(GltfSaveError::internalError, exception.what());
	}
	catch(...)
	{
		return saveFailure(GltfSaveError::internalError, "unknown Save As failure");
	}
}

} // namespace demo::editor
