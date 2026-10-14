#include <mgif/scene_editor/GltfSemanticCompare.h>

#include <mgif/scene_editor/EditorObjectId.h>
#include <mgif/scene_editor/GltfDocument.h>

#include <cstdint>

namespace demo::editor {
namespace {

[[nodiscard]] std::string digestBytes(const std::string& bytes)
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

} // namespace

bool GltfSemanticComparison::equivalent() const noexcept
{
	return m_equivalent;
}

const std::vector<GltfSemanticDifference>& GltfSemanticComparison::differences() const noexcept
{
	return m_differences;
}

const std::string& GltfSemanticComparison::leftDigest() const noexcept
{
	return m_leftDigest;
}

const std::string& GltfSemanticComparison::rightDigest() const noexcept
{
	return m_rightDigest;
}

GltfSemanticComparison GltfSemanticCompare::compare(
	const GltfDocument& left,
	const GltfDocument& right,
	const GltfSemanticCompareOptions& options)
{
	GltfSemanticComparison result;
	result.m_equivalent = true;
	const auto addDifference = [&result, &options](std::string path, std::string message)
	{
		result.m_equivalent = false;
		if(result.m_differences.size() < options.maxDifferences)
		{
			result.m_differences.push_back(GltfSemanticDifference{
				std::move(path),
				std::move(message),
			});
		}
	};

	for(std::uint8_t rawKind = static_cast<std::uint8_t>(EditorObjectKind::scene);
		rawKind < static_cast<std::uint8_t>(EditorObjectKind::count);
		++rawKind)
	{
		const auto kind = static_cast<EditorObjectKind>(rawKind);
		if(left.objectCount(kind) != right.objectCount(kind))
		{
			addDifference(editorObjectKindName(kind), "collection size differs");
		}
	}
	if(left.defaultSceneIndex() != right.defaultSceneIndex())
	{
		addDifference("scene", "default scene differs");
	}

	std::string leftError;
	std::string rightError;
	const std::string leftRepresentation = left.semanticRepresentation(leftError);
	const std::string rightRepresentation = right.semanticRepresentation(rightError);
	if(!leftError.empty())
	{
		addDifference("left", "canonicalization failed: " + leftError);
	}
	if(!rightError.empty())
	{
		addDifference("right", "canonicalization failed: " + rightError);
	}
	if(leftError.empty())
	{
		result.m_leftDigest = digestBytes(leftRepresentation);
	}
	if(rightError.empty())
	{
		result.m_rightDigest = digestBytes(rightRepresentation);
	}
	if(leftError.empty() && rightError.empty() && leftRepresentation != rightRepresentation)
	{
		addDifference("model",
		              "canonical glTF collections, references, values, payloads, extensions, or extras differ");
	}
	return result;
}

std::string GltfSemanticCompare::digest(const GltfDocument& document)
{
	return document.semanticDigest();
}

} // namespace demo::editor
