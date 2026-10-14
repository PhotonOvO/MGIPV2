#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace demo::editor {

class GltfDocument;

struct GltfSemanticDifference final
{
	std::string path;
	std::string message;
};

struct GltfSemanticCompareOptions final
{
	std::size_t maxDifferences{64};
};

class GltfSemanticComparison final
{
public:
	[[nodiscard]] bool equivalent() const noexcept;
	[[nodiscard]] const std::vector<GltfSemanticDifference>& differences() const noexcept;
	[[nodiscard]] const std::string& leftDigest() const noexcept;
	[[nodiscard]] const std::string& rightDigest() const noexcept;

private:
	bool m_equivalent{false};
	std::vector<GltfSemanticDifference> m_differences;
	std::string m_leftDigest;
	std::string m_rightDigest;

	friend class GltfSemanticCompare;
};

class GltfSemanticCompare final
{
public:
	// Resource URI spelling and JSON object formatting/order are packaging, not
	// document semantics. Collection order, references, values, payload bytes,
	// extensions, and extras remain part of the canonical comparison.
	[[nodiscard]] static GltfSemanticComparison compare(
		const GltfDocument& left,
		const GltfDocument& right,
		const GltfSemanticCompareOptions& options = {});

	[[nodiscard]] static std::string digest(const GltfDocument& document);
};

} // namespace demo::editor
