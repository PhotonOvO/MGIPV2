#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace demo::editor {

enum class EditorObjectKind : std::uint8_t
{
	invalid = 0,
	scene,
	node,
	primitive,
	mesh,
	material,
	texture,
	image,
	sampler,
	camera,
	light,
	accessor,
	bufferView,
	buffer,
	skin,
	animation,
	count
};

[[nodiscard]] const char* editorObjectKindName(EditorObjectKind kind) noexcept;

struct EditorDocumentAuthority final
{
	std::uint64_t value{0};

	[[nodiscard]] constexpr bool valid() const noexcept
	{
		return value != 0;
	}

	auto operator<=>(const EditorDocumentAuthority&) const = default;
};

struct EditorObjectId final
{
	EditorObjectKind kind{EditorObjectKind::invalid};
	std::uint32_t slot{0};
	std::uint32_t generation{0};
	EditorDocumentAuthority authority;

	[[nodiscard]] constexpr bool valid() const noexcept
	{
		return kind > EditorObjectKind::invalid
			&& kind < EditorObjectKind::count
			&& generation != 0
			&& authority.valid();
	}

	[[nodiscard]] constexpr EditorObjectKind kindValue() const noexcept
	{
		return kind;
	}

	auto operator<=>(const EditorObjectId&) const = default;
};

template<EditorObjectKind Kind>
class TypedEditorObjectId final
{
public:
	static_assert(Kind > EditorObjectKind::invalid && Kind < EditorObjectKind::count);

	constexpr TypedEditorObjectId() noexcept = default;

	[[nodiscard]] static constexpr std::optional<TypedEditorObjectId> from(EditorObjectId id) noexcept
	{
		if(id.kind != Kind || !id.valid())
		{
			return std::nullopt;
		}
		return TypedEditorObjectId(id.slot, id.generation, id.authority);
	}

	[[nodiscard]] constexpr EditorObjectId untyped() const noexcept
	{
		return valid() ? EditorObjectId{Kind, m_slot, m_generation, m_authority} : EditorObjectId{};
	}

	[[nodiscard]] constexpr bool valid() const noexcept
	{
		return m_generation != 0 && m_authority.valid();
	}

	[[nodiscard]] constexpr std::uint32_t slot() const noexcept
	{
		return m_slot;
	}

	[[nodiscard]] constexpr std::uint32_t generation() const noexcept
	{
		return m_generation;
	}

	[[nodiscard]] constexpr EditorDocumentAuthority authority() const noexcept
	{
		return m_authority;
	}

	auto operator<=>(const TypedEditorObjectId&) const = default;

private:
	constexpr TypedEditorObjectId(std::uint32_t slot,
	                            std::uint32_t generation,
	                            EditorDocumentAuthority authority) noexcept
		: m_slot(slot), m_generation(generation), m_authority(authority)
	{
	}

	std::uint32_t m_slot{0};
	std::uint32_t m_generation{0};
	EditorDocumentAuthority m_authority;
};

struct EditorObjectIdHash final
{
	[[nodiscard]] std::size_t operator()(EditorObjectId id) const noexcept
	{
		std::size_t result = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(id.kind));
		result ^= std::hash<std::uint32_t>{}(id.slot) + static_cast<std::size_t>(0x9e3779b9u)
			+ (result << 6u) + (result >> 2u);
		result ^= std::hash<std::uint32_t>{}(id.generation) + static_cast<std::size_t>(0x9e3779b9u)
			+ (result << 6u) + (result >> 2u);
		result ^= std::hash<std::uint64_t>{}(id.authority.value) + static_cast<std::size_t>(0x9e3779b9u)
			+ (result << 6u) + (result >> 2u);
		return result;
	}

	template<EditorObjectKind Kind>
	[[nodiscard]] std::size_t operator()(TypedEditorObjectId<Kind> id) const noexcept
	{
		return (*this)(id.untyped());
	}
};

using SceneId = TypedEditorObjectId<EditorObjectKind::scene>;
using NodeId = TypedEditorObjectId<EditorObjectKind::node>;
using PrimitiveId = TypedEditorObjectId<EditorObjectKind::primitive>;
using MeshId = TypedEditorObjectId<EditorObjectKind::mesh>;
using MaterialId = TypedEditorObjectId<EditorObjectKind::material>;
using TextureId = TypedEditorObjectId<EditorObjectKind::texture>;
using ImageId = TypedEditorObjectId<EditorObjectKind::image>;
using SamplerId = TypedEditorObjectId<EditorObjectKind::sampler>;
using CameraId = TypedEditorObjectId<EditorObjectKind::camera>;
using LightId = TypedEditorObjectId<EditorObjectKind::light>;
using AccessorId = TypedEditorObjectId<EditorObjectKind::accessor>;
using BufferViewId = TypedEditorObjectId<EditorObjectKind::bufferView>;
using BufferId = TypedEditorObjectId<EditorObjectKind::buffer>;
using SkinId = TypedEditorObjectId<EditorObjectKind::skin>;
using AnimationId = TypedEditorObjectId<EditorObjectKind::animation>;

struct PrimitiveAddress final
{
	std::uint32_t meshIndex{0};
	std::uint32_t primitiveIndex{0};

	auto operator<=>(const PrimitiveAddress&) const = default;
};

} // namespace demo::editor
