#include "EditorUiOsReplayRuntime.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace demo::editor::testing::osreplay {
namespace {

[[nodiscard]] bool lowercaseHex(std::string_view value,
                                std::size_t size) noexcept {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool graphKeyValid(const ui::GraphRowKey &key) noexcept {
  return key.scene.valid() && key.object.valid() &&
         key.scene.authority() == key.object.authority &&
         (!key.directParent ||
          key.directParent->authority() == key.scene.authority());
}

[[nodiscard]] bool sameSelection(const AuthorityProbe &left,
                                 const AuthorityProbe &right) noexcept {
  return left.selectionKind == right.selectionKind &&
         left.selectedObject == right.selectedObject &&
         left.selectedGraphRowKey == right.selectedGraphRowKey;
}

[[nodiscard]] bool nativeGizmoTarget(TargetSemantic semantic) noexcept {
  return semantic == TargetSemantic::gizmoTranslateY ||
         semantic == TargetSemantic::viewportOutside;
}

[[nodiscard]] bool containsPoint(const ui::EditorUiProbeRect &rect,
                                  const ui::EditorUiProbePoint &point) noexcept {
  return rect.valid() && std::isfinite(point.x) && std::isfinite(point.y) &&
         point.x >= rect.minimumX && point.x < rect.maximumX &&
         point.y >= rect.minimumY && point.y < rect.maximumY;
}

[[nodiscard]] bool containsRect(const ui::EditorUiProbeRect &outer,
                                 const ui::EditorUiProbeRect &inner) noexcept {
  return outer.valid() && inner.valid() &&
         inner.minimumX >= outer.minimumX &&
         inner.minimumY >= outer.minimumY &&
         inner.maximumX <= outer.maximumX &&
         inner.maximumY <= outer.maximumY;
}

[[nodiscard]] bool disjoint(const ui::EditorUiProbeRect &left,
                             const ui::EditorUiProbeRect &right) noexcept {
  return left.maximumX <= right.minimumX ||
         right.maximumX <= left.minimumX ||
         left.maximumY <= right.minimumY ||
         right.maximumY <= left.minimumY;
}

[[nodiscard]] bool sameNonYTransform(const GizmoProbe &before,
                                     const GizmoProbe &after) noexcept {
  return before.translation[0] == after.translation[0] &&
         before.translation[2] == after.translation[2] &&
         before.rotation == after.rotation && before.scale == after.scale;
}

[[nodiscard]] bool sameTransform(const GizmoProbe &before,
                                  const GizmoProbe &after) noexcept {
  return before.translation == after.translation &&
         before.rotation == after.rotation && before.scale == after.scale;
}

[[nodiscard]] bool sameTerminalJournal(const GizmoProbe &before,
                                        const GizmoProbe &after) noexcept {
  return before.lastTerminalFrame == after.lastTerminalFrame &&
         before.lastTerminalSequence == after.lastTerminalSequence &&
         before.lastTerminalEventsOverflowed == after.lastTerminalEventsOverflowed &&
         before.lastTerminalEvents.size() == after.lastTerminalEvents.size() &&
         std::equal(before.lastTerminalEvents.begin(), before.lastTerminalEvents.end(),
                    after.lastTerminalEvents.begin(),
                    [](const GizmoTerminalProbe &left,
                       const GizmoTerminalProbe &right) {
                      return left.kind == right.kind && left.sequence == right.sequence &&
                             left.callbackSequence == right.callbackSequence;
                    });
}

[[nodiscard]] bool noSelection(const AuthorityProbe &authority) noexcept {
  return authority.selectionKind == "None" && !authority.selectedObject &&
         !authority.selectedGraphRowKey;
}

[[nodiscard]] std::optional<std::size_t>
utf16CodeUnitCount(std::string_view text) noexcept {
  std::size_t result = 0u;
  for (std::size_t index = 0u; index < text.size();) {
    const unsigned char first = static_cast<unsigned char>(text[index]);
    std::uint32_t codePoint = 0u;
    std::size_t count = 0u;
    if (first < 0x80u) {
      codePoint = first;
      count = 1u;
    } else if ((first & 0xe0u) == 0xc0u) {
      codePoint = first & 0x1fu;
      count = 2u;
    } else if ((first & 0xf0u) == 0xe0u) {
      codePoint = first & 0x0fu;
      count = 3u;
    } else if ((first & 0xf8u) == 0xf0u) {
      codePoint = first & 0x07u;
      count = 4u;
    } else {
      return std::nullopt;
    }
    if (index + count > text.size())
      return std::nullopt;
    for (std::size_t offset = 1u; offset < count; ++offset) {
      const unsigned char continuation =
          static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xc0u) != 0x80u)
        return std::nullopt;
      codePoint = (codePoint << 6u) | (continuation & 0x3fu);
    }
    const std::uint32_t minimum =
        count == 1u ? 0u : count == 2u ? 0x80u : count == 3u ? 0x800u
                                                              : 0x10000u;
    if (codePoint < minimum || codePoint > 0x10ffffu ||
        (codePoint >= 0xd800u && codePoint <= 0xdfffu))
      return std::nullopt;
    result += codePoint > 0xffffu ? 2u : 1u;
    index += count;
  }
  return result;
}

[[nodiscard]] std::optional<std::uint32_t>
expectedInputCount(const InputSpec &input) noexcept {
  switch (input.kind) {
  case InputKind::none:
    return 0u;
  case InputKind::movePointer:
  case InputKind::rightDown:
  case InputKind::rightUp:
  case InputKind::leftDown:
  case InputKind::leftUp:
  case InputKind::keyDown:
  case InputKind::keyUp:
    return 1u;
  case InputKind::focusLoss:
  case InputKind::focusRestore:
    return 0u;
  case InputKind::leftUpFocusLoss:
    return 1u;
  case InputKind::leftClick:
  case InputKind::rightClick:
  case InputKind::pressKey:
  case InputKind::escapeReleaseBatch:
    return 2u;
  case InputKind::keyChord:
    return 4u;
  case InputKind::replaceText: {
    const std::optional<std::size_t> units = utf16CodeUnitCount(input.value);
    if (!units || *units >
                      (std::numeric_limits<std::uint32_t>::max() - 4u) / 2u)
      return std::nullopt;
    return 4u + static_cast<std::uint32_t>(*units * 2u);
  }
  }
  return std::nullopt;
}

[[nodiscard]] std::string_view invokeOperation(ui::CommandId id) noexcept {
  switch (id) {
  case ui::CommandId::undo:
    return "undo";
  case ui::CommandId::redo:
    return "redo";
  default:
    return "invalid";
  }
}

[[nodiscard]] std::string_view
hierarchyOperation(ui::HierarchyOperation operation) noexcept {
  switch (operation) {
  case ui::HierarchyOperation::addRootNode:
    return "add-root";
  case ui::HierarchyOperation::addChildNode:
    return "add-child";
  case ui::HierarchyOperation::duplicateNode:
    return "duplicate";
  case ui::HierarchyOperation::requestDeleteNode:
  case ui::HierarchyOperation::deleteNode:
    return "delete";
  case ui::HierarchyOperation::addPointLight:
    return "add-point-light";
  case ui::HierarchyOperation::addDirectionalLight:
    return "add-directional-light";
  case ui::HierarchyOperation::addSpotLight:
    return "add-spot-light";
  }
  return "invalid";
}

[[nodiscard]] std::string propertyFieldName(ui::FieldId field) {
  switch (field) {
  case ui::FieldId::translation:
    return "translation";
  case ui::FieldId::rotationDegrees:
    return "rotation";
  case ui::FieldId::scale:
    return "scale";
  case ui::FieldId::visibility:
    return "visibility";
  default:
    return "field-" + std::to_string(static_cast<unsigned>(field));
  }
}

[[nodiscard]] std::string propertyValueText(const ui::Value &value) {
  return std::visit(
      [](const auto &item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          return item ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
          return item;
        } else if constexpr (std::is_same_v<T, double>) {
          char buffer[64];
          const auto converted = std::to_chars(
              std::begin(buffer), std::end(buffer), item,
              std::chars_format::general);
          if (converted.ec != std::errc{})
            return {};
          std::string result(buffer, converted.ptr);
          if (result.find_first_of(".eE") == std::string::npos)
            result += ".0";
          return result;
        } else if constexpr (std::is_arithmetic_v<T>) {
          return std::to_string(item);
        } else if constexpr (std::is_same_v<T, EditorObjectId>) {
          return std::to_string(static_cast<unsigned>(item.kind)) + ":" +
                 std::to_string(item.slot) + ":" +
                 std::to_string(item.generation) + ":" +
                 std::to_string(item.authority.value);
        } else {
          std::string result;
          for (std::size_t index = 0; index < item.size(); ++index) {
            if (index != 0)
              result.push_back(',');
            result += propertyValueText(ui::Value{item[index]});
          }
          return result;
        }
      },
      value);
}

[[nodiscard]] std::string editPhaseName(ui::EditPhase phase) {
  switch (phase) {
  case ui::EditPhase::begin:
    return "begin";
  case ui::EditPhase::preview:
    return "preview";
  case ui::EditPhase::commit:
    return "commit";
  }
  return "invalid";
}

} // namespace

struct EditorUiOsReplayRuntime::Impl final {
  struct OwnedItem final {
    ui::EditorUiProbeItemKey key;
    std::string label;
    std::uint64_t order{0};
    std::uint32_t itemId{0};
    std::uint32_t windowToken{0};
    std::uint32_t popupToken{0};
    std::uint32_t modalToken{0};
    ui::EditorUiProbeCoordinateSpace coordinateSpace{
        ui::EditorUiProbeCoordinateSpace::imguiScreenLogical};
    ui::EditorUiProbeRect rect;
    bool visible{false};
    bool hovered{false};
    bool active{false};
    bool focused{false};
    bool enabled{false};
    bool dropAccepted{false};
  };

  struct RoleState final {
    GraphRoleSpec spec;
    std::optional<EditorObjectId> object;
    std::optional<ui::GraphRowKey> current;
  };

  RuntimeConfig config;
  RuntimeCallbacks callbacks;
  RuntimeStatus runtimeStatus{RuntimeStatus::running};
  std::string failure;
  std::uint32_t step{0};
  std::size_t phase{0};
  bool frameOpen{false};
  bool frameSealed{false};
  bool ignoreFrame{false};
  ui::EditorUiProbeFrameBinding frame;
  std::uint64_t lastFrameSerial{0};
  std::vector<OwnedItem> items;
  std::vector<RoleState> roles;
  std::vector<ObservedDispatch> dispatches;
  std::uint64_t lastDispatchSequence{0};
  std::optional<AuthorityProbe> stepBefore;
  std::optional<GizmoProbe> stepBeforeGizmo;
  std::optional<GizmoProbe> lastPublishedGizmo;
  std::optional<AuthorityProbe> beforeDrop;
  std::optional<AuthorityProbe> previousStepAfter;
  std::optional<ReadyProbe> ready;
  std::string readySha256;
  std::string stableCandidateIdentity;
  std::uint64_t stableCandidateFrameSerial{0};
  std::uint32_t stableCandidateFrames{0};
  std::string pendingReason;

  Impl(RuntimeConfig value, RuntimeCallbacks values)
      : config(std::move(value)), callbacks(std::move(values)),
        step(config.startStep) {
    const PlanValidation validation = config.plan.validate();
    if (!validation.valid) {
      fail(validation.error.empty() ? "runtime plan is invalid"
                                    : validation.error);
      return;
    }
    if (config.subcase.empty() || config.startStep > config.stopStep ||
        config.stopStep >= config.plan.steps.size() ||
        !lowercaseHex(config.runNonce, 32u) ||
        !lowercaseHex(config.planSha256, 64u) ||
        !lowercaseHex(config.executableSha256, 64u) ||
        !lowercaseHex(config.sourceSha256, 64u) ||
        config.requiredStableFrames == 0u ||
        config.requiredStableFrames > 8u || !callbacks.snapshot ||
        !callbacks.publishReady) {
      fail("runtime configuration is invalid");
      return;
    }
    for (std::uint32_t index = config.startStep; index <= config.stopStep;
         ++index) {
      if (config.plan.steps[index].subcase != config.subcase) {
        fail("runtime range crosses a replay subcase");
        return;
      }
    }
    roles.reserve(config.plan.graphRoles.size());
    for (const GraphRoleSpec &role : config.plan.graphRoles) {
      if (role.subcase == config.subcase)
        roles.push_back(RoleState{role});
    }
  }

  void fail(std::string message) {
    if (runtimeStatus == RuntimeStatus::failed)
      return;
    failure = message.empty() ? "OS replay runtime failed" : std::move(message);
    runtimeStatus = RuntimeStatus::failed;
    frameOpen = false;
    frameSealed = false;
    ignoreFrame = false;
    stableCandidateIdentity.clear();
    stableCandidateFrameSerial = 0u;
    stableCandidateFrames = 0u;
    pendingReason.clear();
  }

  [[nodiscard]] const StepSpec *currentStep() const noexcept {
    return step < config.plan.steps.size() ? &config.plan.steps[step] : nullptr;
  }

  [[nodiscard]] const PhaseSpec *currentPhase() const noexcept {
    const StepSpec *value = currentStep();
    return value != nullptr && phase < value->phases.size()
               ? &value->phases[phase]
               : nullptr;
  }

  [[nodiscard]] std::optional<std::uint32_t>
  introductionIndex(const GraphRoleSpec &role) const noexcept {
    const auto found = std::find_if(
        config.plan.steps.begin(), config.plan.steps.end(),
        [&](const StepSpec &value) { return value.id == role.introducedByStep; });
    return found == config.plan.steps.end()
               ? std::nullopt
               : std::optional<std::uint32_t>{found->index};
  }

  [[nodiscard]] RoleState *findRole(std::string_view name) noexcept {
    const auto found = std::find_if(roles.begin(), roles.end(),
                                    [&](const RoleState &role) {
                                      return role.spec.role == name;
                                    });
    return found == roles.end() ? nullptr : &*found;
  }

  [[nodiscard]] const RoleState *
  findRole(std::string_view name) const noexcept {
    const auto found = std::find_if(roles.begin(), roles.end(),
                                    [&](const RoleState &role) {
                                      return role.spec.role == name;
                                    });
    return found == roles.end() ? nullptr : &*found;
  }

  [[nodiscard]] bool roleCandidate(const OwnedItem &item,
                                   const RoleState &role,
                                   bool requireBoundObject) const noexcept {
    if (item.key.semantic != ui::EditorUiProbeSemantic::graphRow ||
        !graphKeyValid(item.key.row) ||
        item.key.row.object.kind != role.spec.objectKind ||
        (requireBoundObject && item.key.row.object != *role.object) ||
        (!role.spec.labelContains.empty() &&
         item.label.find(role.spec.labelContains) == std::string::npos) ||
        (role.spec.requireRoot && item.key.row.directParent))
      return false;
    if (!role.spec.sameObjectAsRole.empty()) {
      const RoleState *same = findRole(role.spec.sameObjectAsRole);
      if (same == nullptr || !same->object ||
          item.key.row.object != *same->object)
        return false;
    }
    if (!role.spec.distinctFromRole.empty()) {
      const RoleState *distinct = findRole(role.spec.distinctFromRole);
      if (distinct == nullptr || !distinct->object ||
          item.key.row.object == *distinct->object)
        return false;
    }
    if (!role.spec.directParentRole.empty()) {
      const RoleState *parent = findRole(role.spec.directParentRole);
      const std::optional<NodeId> parentNode =
          parent != nullptr && parent->object
              ? NodeId::from(*parent->object)
              : std::optional<NodeId>{};
      if (!parentNode || item.key.row.directParent != parentNode)
        return false;
    }
    return true;
  }

  [[nodiscard]] bool resolveRoles() {
    for (RoleState &role : roles)
      role.current.reset();
    bool progress = true;
    while (progress) {
      progress = false;
      for (RoleState &role : roles) {
        const std::optional<std::uint32_t> introduced =
            introductionIndex(role.spec);
        if (!introduced || step < *introduced)
          continue;
        std::vector<const OwnedItem *> candidates;
        for (const OwnedItem &item : items) {
          if (roleCandidate(item, role, role.object.has_value()))
            candidates.push_back(&item);
        }
        if (candidates.size() > 1u) {
          fail("graph role has duplicate matching rows: " + role.spec.role);
          return false;
        }
        if (candidates.size() == 1u) {
          const EditorObjectId object = candidates.front()->key.row.object;
          if (!role.object) {
            role.object = object;
            progress = true;
          }
          role.current = candidates.front()->key.row;
        }
      }
    }
    return runtimeStatus != RuntimeStatus::failed;
  }

  [[nodiscard]] std::optional<ScreenRect>
  physicalRect(const ui::EditorUiProbeRect &rect,
               const RuntimeHostSnapshot &host) const noexcept {
    const auto finite = [](float value) { return std::isfinite(value); };
    const auto &binding = frame;
    if (!rect.valid() ||
        !finite(binding.viewportPosition.x) ||
        !finite(binding.viewportPosition.y) ||
        !finite(binding.viewportSize.x) ||
        !finite(binding.viewportSize.y) ||
        !finite(binding.displaySize.x) ||
        !finite(binding.displaySize.y) ||
        !finite(binding.framebufferScale.x) ||
        !finite(binding.framebufferScale.y) ||
        binding.viewportSize.x <= 0.0F || binding.viewportSize.y <= 0.0F ||
        binding.displaySize.x <= 0.0F || binding.displaySize.y <= 0.0F ||
        binding.framebufferScale.x <= 0.0F ||
        binding.framebufferScale.y <= 0.0F || !host.client.screenRect.valid() ||
        host.client.width != static_cast<std::uint32_t>(
                                 host.client.screenRect.right -
                                 host.client.screenRect.left) ||
        host.client.height != static_cast<std::uint32_t>(
                                  host.client.screenRect.bottom -
                                  host.client.screenRect.top))
      return std::nullopt;
    constexpr double tolerance = 1.01;
    const double expectedWidth = static_cast<double>(binding.viewportSize.x) *
                                 binding.framebufferScale.x;
    const double expectedHeight = static_cast<double>(binding.viewportSize.y) *
                                  binding.framebufferScale.y;
    if (std::abs(expectedWidth - host.client.width) > tolerance ||
        std::abs(expectedHeight - host.client.height) > tolerance ||
        std::abs(binding.displaySize.x - binding.viewportSize.x) > 0.01F ||
        std::abs(binding.displaySize.y - binding.viewportSize.y) > 0.01F)
      return std::nullopt;

    const double minimumX =
        (rect.minimumX - binding.viewportPosition.x) *
        binding.framebufferScale.x;
    const double minimumY =
        (rect.minimumY - binding.viewportPosition.y) *
        binding.framebufferScale.y;
    const double maximumX =
        (rect.maximumX - binding.viewportPosition.x) *
        binding.framebufferScale.x;
    const double maximumY =
        (rect.maximumY - binding.viewportPosition.y) *
        binding.framebufferScale.y;
    if (minimumX < 0.0 || minimumY < 0.0 ||
        maximumX > host.client.width || maximumY > host.client.height)
      return std::nullopt;
    const double left = host.client.screenRect.left + std::floor(minimumX);
    const double top = host.client.screenRect.top + std::floor(minimumY);
    const double right = host.client.screenRect.left + std::ceil(maximumX);
    const double bottom = host.client.screenRect.top + std::ceil(maximumY);
    if (left < std::numeric_limits<std::int32_t>::min() ||
        top < std::numeric_limits<std::int32_t>::min() ||
        right > std::numeric_limits<std::int32_t>::max() ||
        bottom > std::numeric_limits<std::int32_t>::max())
      return std::nullopt;
    ScreenRect result{static_cast<std::int32_t>(left),
                      static_cast<std::int32_t>(top),
                      static_cast<std::int32_t>(right),
                      static_cast<std::int32_t>(bottom)};
    return result.valid() ? std::optional<ScreenRect>{result} : std::nullopt;
  }

  [[nodiscard]] bool validateGizmoSnapshot(const RuntimeHostSnapshot &host,
                                            const PhaseSpec &phaseSpec) {
    const bool escapeCase = isEscapeGizmoCase(config.plan.caseId);
    const bool focusCase = isFocusGizmoCase(config.plan.caseId);
    const bool sameFrameCase = isSameFrameGizmoCase(config.plan.caseId);
    const bool focusSameFrameCase =
        config.plan.caseId == kU03FocusSameFrameCase;
    const bool escapePrelude = (escapeCase || focusCase) &&
                               !phaseSpec.gizmo &&
                               !nativeGizmoTarget(phaseSpec.target.semantic);
    if (!phaseSpec.gizmo && !nativeGizmoTarget(phaseSpec.target.semantic) &&
        !escapePrelude)
      return true;
    if (!phaseSpec.gizmo && !escapePrelude) {
      fail("native target lacks an explicit gizmo observation contract");
      return false;
    }
    if (!host.gizmo) {
      pendingReason = "gizmo-snapshot-unavailable";
      return false;
    }
    const GizmoProbe &actual = *host.gizmo;
    if (actual.renderedFrame > frame.frameSerial ||
        actual.documentRevision > host.authority.documentRevision ||
        actual.selectionRevision > host.authority.selectionRevision) {
      fail("gizmo snapshot belongs to a future frame or authority revision");
      return false;
    }
    if (actual.renderedFrame != frame.frameSerial ||
        actual.documentRevision != host.authority.documentRevision ||
        actual.selectionRevision != host.authority.selectionRevision) {
      pendingReason = "gizmo-frame-authority-mismatch";
      return false;
    }
    if (actual.inputFrame == 0u || actual.mappingEpoch == 0u ||
        actual.phase > 3u ||
        (actual.hoveredComponent > 12u && actual.hoveredComponent != 255u) ||
        (actual.activeComponent > 12u && actual.activeComponent != 255u) ||
        (actual.hasPointer && (!std::isfinite(actual.pointer.x) ||
                               !std::isfinite(actual.pointer.y))) ||
        !std::all_of(actual.translation.begin(), actual.translation.end(),
                     [](double value) { return std::isfinite(value); }) ||
        !std::all_of(actual.rotation.begin(), actual.rotation.end(),
                     [](double value) { return std::isfinite(value); }) ||
        !std::all_of(actual.scale.begin(), actual.scale.end(),
                     [](double value) { return std::isfinite(value); })) {
      fail("gizmo snapshot contains invalid frame, state, or transform data");
      return false;
    }
    if ((escapeCase || focusCase) && !actual.ndcYDown) {
      fail("native Vulkan Gizmo observation lacks the actual downward NDC Y convention");
      return false;
    }
    if ((sameFrameCase || focusCase) && lastPublishedGizmo &&
        actual.nativeCallbackSequence < lastPublishedGizmo->nativeCallbackSequence) {
      fail("native gizmo callback clock regressed behind accepted evidence");
      return false;
    }
    if (escapePrelude) {
      if (actual.beginCount != 0u || actual.previewCount != 0u ||
          actual.commitCount != 0u || actual.detachCount != 0u ||
          actual.latchCount != 0u || actual.gestureNode || actual.captureHeld ||
          actual.activeComponent != 255u || actual.phase > 1u ||
          actual.deliveredReleaseCount != 0u || actual.escapePressCount != 0u ||
          actual.focusLossCount != 0u || actual.suppressedReleaseCount != 0u ||
          actual.lastTerminalFrame != 0u || actual.lastTerminalSequence != 0u ||
          !actual.lastTerminalEvents.empty() || actual.lastTerminalEventsOverflowed ||
          (!actual.terminal.empty() && actual.terminal != "none") ||
          host.authority.undoCount != 0u || host.authority.redoCount != 0u ||
          !host.authority.undoDescription.empty() || !host.authority.redoDescription.empty()) {
        fail("native Gizmo prelude contains an active gesture, terminal proof, or history");
        return false;
      }
      if (lastPublishedGizmo && actual.inputFrame < lastPublishedGizmo->inputFrame) {
        fail("Escape prelude input frame regressed behind accepted evidence");
        return false;
      }
      if (actual.node &&
          (!actual.node->valid() || actual.node->kind != EditorObjectKind::node ||
           actual.node->authority.value != host.authority.documentAuthority)) {
        fail("Escape prelude contains an invalid or foreign attached node");
        return false;
      }
      if (actual.node && actual.node != host.authority.selectedObject) {
        pendingReason = "gizmo-node-selection-synchronizing";
        return false;
      }
      return true;
    }
    const GizmoExpectation &expected = *phaseSpec.gizmo;
    if (expected.noSelection && !escapeCase) {
      fail("gizmo selection clearing is not authorized by this replay case");
      return false;
    }
    if (!physicalRect(actual.viewportRect, host) ||
        !physicalRect(actual.outsideRect, host) ||
        !disjoint(actual.viewportRect, actual.outsideRect) ||
        (actual.componentValid &&
         (!physicalRect(actual.componentRect, host) ||
          !containsRect(actual.viewportRect, actual.componentRect)))) {
      fail("gizmo snapshot geometry is not bound to the live client viewport");
      return false;
    }
    const bool hasTerminal = actual.commitCount != 0u ||
                             actual.detachCount != 0u || actual.latchCount != 0u;
    if (actual.detachCount > expected.detachCount ||
        actual.latchCount > expected.latchCount ||
        actual.beginCount > expected.beginCount ||
        actual.previewCount > expected.previewCount ||
        actual.commitCount > expected.commitCount ||
        actual.commitCount > actual.beginCount ||
        (actual.previewCount != 0u && actual.beginCount == 0u) ||
        actual.detachCount > actual.beginCount ||
        actual.latchCount > actual.beginCount ||
        (!hasTerminal && !actual.terminal.empty() &&
         actual.terminal != "none") ||
        (!escapeCase && !hasTerminal &&
         (actual.lastTerminalFrame != 0u || actual.lastTerminalSequence != 0u)) ||
        (actual.commitCount != 0u && actual.terminal != "commit") ||
        (actual.detachCount != 0u && actual.terminal != "detach") ||
        (actual.latchCount != 0u && actual.terminal != "latch") ||
        (hasTerminal &&
         (actual.lastTerminalFrame == 0u ||
          actual.lastTerminalFrame > actual.inputFrame ||
          actual.lastTerminalSequence == 0u))) {
      fail("gizmo observation has excess actions or an unexpected terminal");
      return false;
    }
    if (lastPublishedGizmo &&
        (actual.inputFrame < lastPublishedGizmo->inputFrame ||
         actual.beginCount < lastPublishedGizmo->beginCount ||
         actual.previewCount < lastPublishedGizmo->previewCount ||
         actual.commitCount < lastPublishedGizmo->commitCount ||
         actual.detachCount < lastPublishedGizmo->detachCount ||
         actual.latchCount < lastPublishedGizmo->latchCount)) {
      fail("gizmo observation regressed behind an accepted phase");
      return false;
    }
    if (lastPublishedGizmo &&
        (actual.commitCount > lastPublishedGizmo->commitCount ||
         actual.detachCount > lastPublishedGizmo->detachCount ||
         actual.latchCount > lastPublishedGizmo->latchCount) &&
        (!sameTransform(actual, *lastPublishedGizmo) ||
         actual.lastTerminalFrame <= lastPublishedGizmo->inputFrame)) {
      fail("gizmo release changed the preview or predates its input boundary");
      return false;
    }
    const bool missingActions = actual.beginCount < expected.beginCount ||
        actual.previewCount < expected.previewCount ||
        actual.commitCount < expected.commitCount ||
        actual.detachCount < expected.detachCount ||
        actual.latchCount < expected.latchCount;
    if (escapeCase) {
      const std::optional<EditorObjectId> recordedGesture =
          stepBeforeGizmo && stepBeforeGizmo->node
              ? stepBeforeGizmo->node
              : lastPublishedGizmo ? lastPublishedGizmo->gestureNode
                                   : std::nullopt;
      if ((actual.beginCount == 0u && actual.gestureNode) ||
          (actual.beginCount != 0u &&
           (!recordedGesture || !actual.gestureNode ||
            !actual.gestureNode->valid() ||
            actual.gestureNode->kind != EditorObjectKind::node ||
            actual.gestureNode->authority.value != host.authority.documentAuthority ||
            actual.gestureNode != recordedGesture))) {
        fail("Escape gesture identity differs from the observed selected node");
        return false;
      }
      if (host.authority.undoCount != 0u || host.authority.redoCount != 0u ||
          !host.authority.undoDescription.empty() ||
          !host.authority.redoDescription.empty()) {
        fail("Escape no-history gesture changed the history envelope");
        return false;
      }
      const std::uint64_t maximumReleaseCount =
          sameFrameCase && expected.detachCount != 0u ? 1u : 0u;
      if (actual.deliveredReleaseCount > maximumReleaseCount || actual.focusLossCount != 0u ||
          actual.suppressedReleaseCount != 0u || actual.escapePressCount > 1u ||
          actual.lastTerminalEventsOverflowed ||
          (expected.detachCount == 0u && actual.escapePressCount != 0u)) {
        fail("Escape gesture contains unexpected terminal input events");
        return false;
      }
      if (actual.escapePressCount == 0u) {
        if (actual.deliveredReleaseCount != 0u ||
            !actual.lastTerminalEvents.empty() || actual.lastTerminalFrame != 0u ||
            actual.lastTerminalSequence != 0u || actual.detachCount != 0u) {
          fail("Escape terminal evidence exists without a routed Escape press");
          return false;
        }
      } else if (sameFrameCase) {
        const bool escapeFirst = config.plan.caseId == kU03EscapeReleaseCase;
        if (actual.deliveredReleaseCount != 1u ||
            actual.lastTerminalEvents.size() != 2u) {
          fail("same-frame gizmo journal does not contain exactly two routed terminal events");
          return false;
        }
        const auto &first = actual.lastTerminalEvents[0];
        const auto &second = actual.lastTerminalEvents[1];
        const auto &escape = escapeFirst ? first : second;
        if (first.kind != (escapeFirst ? "escape-pressed" : "delivered-release") ||
            second.kind != (escapeFirst ? "delivered-release" : "escape-pressed") ||
            first.sequence == 0u || second.sequence <= first.sequence ||
            first.callbackSequence == 0u ||
            second.callbackSequence <= first.callbackSequence ||
            second.callbackSequence > actual.nativeCallbackSequence ||
            actual.lastTerminalSequence != escape.sequence ||
            actual.lastTerminalFrame == 0u ||
            actual.lastTerminalFrame > actual.inputFrame) {
          fail("same-frame gizmo journal has invalid native order, event identity, or frame");
          return false;
        }
        if (!lastPublishedGizmo ||
            (lastPublishedGizmo->detachCount == 0u &&
             (first.callbackSequence <= lastPublishedGizmo->nativeCallbackSequence ||
              actual.lastTerminalFrame <= lastPublishedGizmo->inputFrame))) {
          fail("same-frame gizmo terminal events predate their published native input boundary");
          return false;
        }
      } else if (actual.lastTerminalEvents.size() != 1u ||
                 actual.lastTerminalEvents.front().kind != "escape-pressed" ||
                 actual.lastTerminalEvents.front().sequence == 0u ||
                 actual.lastTerminalEvents.front().sequence != actual.lastTerminalSequence ||
                 actual.lastTerminalFrame == 0u ||
                 actual.lastTerminalFrame > actual.inputFrame) {
        fail("Escape journal does not contain one actual frame-bound Escape press");
        return false;
      }
      if (lastPublishedGizmo && actual.detachCount != 0u &&
          lastPublishedGizmo->detachCount != 0u &&
          (!sameTransform(actual, *lastPublishedGizmo) ||
           !sameTerminalJournal(actual, *lastPublishedGizmo) ||
           actual.documentRevision != lastPublishedGizmo->documentRevision ||
           actual.selectionRevision != lastPublishedGizmo->selectionRevision ||
           actual.gestureNode != lastPublishedGizmo->gestureNode)) {
        fail("detached Escape gesture changed its preview, authority, or journal");
        return false;
      }
    } else if (focusCase) {
      const std::optional<EditorObjectId> recordedGesture =
          stepBeforeGizmo && stepBeforeGizmo->node
              ? stepBeforeGizmo->node
              : lastPublishedGizmo ? lastPublishedGizmo->gestureNode
                                   : std::nullopt;
      if ((actual.beginCount == 0u && actual.gestureNode) ||
          (actual.beginCount != 0u &&
           (!recordedGesture || !actual.gestureNode ||
            !actual.gestureNode->valid() ||
            actual.gestureNode->kind != EditorObjectKind::node ||
            actual.gestureNode->authority.value != host.authority.documentAuthority ||
            actual.gestureNode != recordedGesture))) {
        fail("focus-loss gesture identity differs from the observed selected node");
        return false;
      }
      if (actual.escapePressCount != expected.escapePressCount ||
          actual.deliveredReleaseCount != expected.deliveredReleaseCount ||
          actual.focusLossCount != expected.focusLossCount ||
          actual.suppressedReleaseCount != expected.suppressedReleaseCount) {
        if (actual.escapePressCount > expected.escapePressCount ||
            actual.deliveredReleaseCount > expected.deliveredReleaseCount ||
            actual.focusLossCount > expected.focusLossCount ||
            actual.suppressedReleaseCount > expected.suppressedReleaseCount) {
          fail("focus-loss observation contains excess terminal input at phase " +
               phaseSpec.id + " actual=" +
               std::to_string(actual.escapePressCount) + ":" +
               std::to_string(actual.deliveredReleaseCount) + ":" +
               std::to_string(actual.focusLossCount) + ":" +
               std::to_string(actual.suppressedReleaseCount) +
               " expected=" + std::to_string(expected.escapePressCount) + ":" +
               std::to_string(expected.deliveredReleaseCount) + ":" +
               std::to_string(expected.focusLossCount) + ":" +
               std::to_string(expected.suppressedReleaseCount) +
               " callback=" +
               std::to_string(actual.nativeCallbackSequence));
          return false;
        }
        pendingReason = "focus-loss-terminal-prefix-mismatch";
        return false;
      }
      // A native callback ordinal is part of the proof, not merely a
      // non-decreasing diagnostic clock.  Terminal callbacks are often
      // delivered between the input phase and the following ready boundary
      // (for example, focus-loss is observed while the next frame is being
      // assembled).  Detect advancement from the observed counters instead
      // of keying this fence to the current phase name, then require the
      // callback clock to be strictly newer than the previous accepted ready.
      const bool terminalCountersAdvanced =
          lastPublishedGizmo.has_value() &&
          (actual.deliveredReleaseCount >
               lastPublishedGizmo->deliveredReleaseCount ||
           actual.escapePressCount > lastPublishedGizmo->escapePressCount ||
           actual.focusLossCount > lastPublishedGizmo->focusLossCount ||
           actual.suppressedReleaseCount >
               lastPublishedGizmo->suppressedReleaseCount);
      const bool callbackBackedTerminal =
          std::any_of(actual.lastTerminalEvents.begin(),
                      actual.lastTerminalEvents.end(),
                      [](const GizmoTerminalProbe &event) {
                        return event.callbackSequence != 0u;
                      });
      // A release callback that followed a focus marker is intentionally
      // replayed on the first independent focused frame.  Its original
      // callback ordinal was already included in the focus-loss ready clock,
      // even though the terminal itself was held back by the restore fence.
      // Permit only that narrowly identifiable suppression-only transition;
      // every other callback-backed terminal must advance the native clock.
      const bool deferredFocusSuppression =
          lastPublishedGizmo.has_value() &&
          actual.suppressedReleaseCount >
              lastPublishedGizmo->suppressedReleaseCount &&
          lastPublishedGizmo->focusLossCount != 0u &&
          actual.deliveredReleaseCount ==
              lastPublishedGizmo->deliveredReleaseCount &&
          actual.escapePressCount == lastPublishedGizmo->escapePressCount &&
          actual.focusLossCount == lastPublishedGizmo->focusLossCount &&
          actual.lastTerminalEvents.size() == 1u &&
          actual.lastTerminalEvents.front().kind ==
              "suppressed-physical-mouse-up" &&
          actual.lastTerminalEvents.front().callbackSequence != 0u &&
          actual.lastTerminalEvents.front().callbackSequence <=
              lastPublishedGizmo->nativeCallbackSequence &&
          actual.lastTerminalEvents.front().callbackSequence <=
              actual.nativeCallbackSequence;
      if (terminalCountersAdvanced && callbackBackedTerminal &&
          actual.nativeCallbackSequence <=
              lastPublishedGizmo->nativeCallbackSequence &&
          !deferredFocusSuppression) {
        fail("focus-loss terminal callback ordinal predates its ready boundary");
        return false;
      }
      if (host.authority.selectionKind != "Node" ||
          !host.authority.selectedObject ||
          (actual.gestureNode &&
           actual.gestureNode != host.authority.selectedObject)) {
        fail("focus-loss gesture did not retain its selected node");
        return false;
      }
      if (expected.latchCount != 0u && actual.terminal != "latch") {
        fail("focus-loss gesture did not latch after focus loss");
        return false;
      }
      if (expected.commitCount != 0u && actual.terminal != "commit") {
        fail("delivered focus-loss release did not commit exactly once");
        return false;
      }
      if (expected.latchCount != 0u && actual.commitCount != 0u) {
        fail("focus-loss latch unexpectedly committed history");
        return false;
      }
      if (actual.lastTerminalEventsOverflowed) {
        fail("focus-loss terminal journal overflowed");
        return false;
      }
      const bool hasFocusEvidence =
          expected.deliveredReleaseCount != 0u ||
          expected.focusLossCount != 0u ||
          expected.suppressedReleaseCount != 0u ||
          expected.latchCount != 0u || expected.commitCount != 0u;
      if (!hasFocusEvidence) {
        if (actual.lastTerminalEvents.size() != 0u ||
            (!actual.terminal.empty() && actual.terminal != "none")) {
          fail("focus-loss pre-terminal phase contains terminal evidence");
          return false;
        }
        return true;
      }
      const bool delivered = expected.deliveredReleaseCount != 0u;
      const bool firstFocusBatch =
          expected.focusLossCount != 0u &&
          expected.suppressedReleaseCount == 0u;
      // `lastTerminalEvents` is the most recent nonempty application-frame
      // batch, rather than a cumulative journal.  A focus-loss batch therefore
      // contains only focus-lost; the first suppressed polling release in a
      // later focused frame replaces it with a one-event batch.  The only
      // two-event O07 batch is the explicit same-frame release/focus order.
      const std::array<std::string_view, 2> expectedKinds =
          delivered ? std::array<std::string_view, 2>{"delivered-release", ""}
                    : focusSameFrameCase
                          ? std::array<std::string_view, 2>{
                                "suppressed-physical-mouse-up", "focus-lost"}
                          : firstFocusBatch
                                ? std::array<std::string_view, 2>{"focus-lost",
                                                                  ""}
                                : std::array<std::string_view, 2>{
                                      "suppressed-physical-mouse-up", ""};
      const std::size_t expectedEventCount = focusSameFrameCase ? 2u : 1u;
      if (actual.lastTerminalEvents.size() != expectedEventCount) {
        pendingReason = "focus-loss-terminal-journal-prefix-mismatch";
        return false;
      }
      for (std::size_t index = 0u; index < expectedEventCount; ++index) {
        const auto &event = actual.lastTerminalEvents[index];
        const std::uint64_t callbackSequence = event.callbackSequence;
        const bool pollingSuppressed =
            event.kind == "suppressed-physical-mouse-up" &&
            !focusSameFrameCase && expectedEventCount == 1u;
        if (event.kind != expectedKinds[index] || event.sequence == 0u ||
            (!pollingSuppressed && callbackSequence == 0u) ||
            callbackSequence > actual.nativeCallbackSequence ||
            (index != 0u &&
             (event.sequence <= actual.lastTerminalEvents[index - 1u].sequence ||
                  callbackSequence <=
                      actual.lastTerminalEvents[index - 1u].callbackSequence))) {
          fail("focus-loss journal order or suppression classification is invalid at phase " +
               phaseSpec.id + " index=" + std::to_string(index) +
               " actualKind=" + event.kind +
               " expectedKind=" + std::string(expectedKinds[index]) +
               " sequence=" + std::to_string(event.sequence) +
               " callback=" + std::to_string(callbackSequence) +
               " nativeCallback=" +
               std::to_string(actual.nativeCallbackSequence));
          return false;
        }
      }
      if (delivered &&
          (actual.lastTerminalSequence == 0u ||
           actual.lastTerminalEvents.front().sequence !=
               actual.lastTerminalSequence)) {
        fail("delivered focus-loss release lacks terminal sequence");
        return false;
      }
      if (!delivered && actual.lastTerminalSequence == 0u) {
        fail("focus-loss terminal lacks its latest input sequence");
        return false;
      }
      if (!delivered && expectedEventCount == 1u &&
          actual.lastTerminalEvents.front().sequence !=
              actual.lastTerminalSequence) {
        fail("focus-loss terminal sequence does not bind its latest batch");
        return false;
      }
    }
    if (expected.noSelection && !missingActions) {
      if (actual.node || !noSelection(host.authority)) {
        fail("accepted Escape detach retained an attached or selected node");
        return false;
      }
    } else if (!expected.noSelection) {
      if (!actual.node) {
        if (escapeCase && actual.beginCount != 0u) {
          fail("Escape gesture lost selection before its detach boundary");
          return false;
        }
        pendingReason = "gizmo-node-unavailable";
        return false;
      }
      if (!actual.node->valid() || actual.node->kind != EditorObjectKind::node ||
          actual.node->authority.value != host.authority.documentAuthority ||
          host.authority.selectionKind != "Node" ||
          host.authority.selectedObject != actual.node) {
        fail("gizmo snapshot node differs from the current selection authority");
        return false;
      }
      if (!phaseSpec.target.graphRowRole.empty()) {
        const RoleState *role = findRole(phaseSpec.target.graphRowRole);
        if (role == nullptr || !role->current) {
          pendingReason = "target-role-unavailable";
          return false;
        }
        if (actual.node != role->current->object) {
          fail("gizmo snapshot node differs from the target graph role");
          return false;
        }
      }
    } else if (actual.node &&
               (!actual.node->valid() || actual.node->kind != EditorObjectKind::node ||
                actual.node != actual.gestureNode)) {
      fail("pending Escape detach changed its attached node identity");
      return false;
    }
    if (missingActions) {
      pendingReason = "gizmo-action-prefix-mismatch";
      return false;
    }
    if (actual.phase != expected.phase ||
        actual.captureHeld != expected.captureHeld ||
        (expected.captureHeld && actual.activeComponent != 1u) ||
        (!expected.captureHeld && actual.activeComponent != 255u)) {
      if (escapeCase && expected.noSelection) {
        fail("accepted Escape detach retained an active interaction state");
        return false;
      }
      pendingReason = "gizmo-interaction-state-mismatch";
      return false;
    }
    if (expected.pointerOutside &&
        (!actual.hasPointer ||
         (!containsPoint(actual.viewportRect, actual.pointer)) !=
             *expected.pointerOutside)) {
      pendingReason = "gizmo-pointer-location-mismatch";
      return false;
    }
    if (stepBeforeGizmo && actual.previewCount > stepBeforeGizmo->previewCount &&
        (!sameNonYTransform(*stepBeforeGizmo, actual) ||
         actual.translation[1] <= stepBeforeGizmo->translation[1])) {
      fail("gizmo preview did not change only the local translation Y value");
      return false;
    }
    return true;
  }

  [[nodiscard]] bool roleMatchesSelection(const RoleState &role,
                                          const ui::SelectObject &selection) const {
    if (!role.current)
      return false;
    const auto *node = std::get_if<NodeSelection>(&selection.selection.value);
    return node != nullptr &&
           selection.selection.authority == role.current->object.authority &&
           node->scene == role.current->scene &&
           node->node.untyped() == role.current->object &&
           node->parent == role.current->directParent;
  }

  [[nodiscard]] bool normalizeDispatch(
      const ui::UiCommand &command, const ui::RuntimeActionResult &result,
      ObservedDispatch &observed) {
    const StepSpec *stepSpec = currentStep();
    if (stepSpec == nullptr || dispatches.size() >=
                                   stepSpec->expectedDispatches.size()) {
      fail("observed excess UI dispatch");
      return false;
    }
    const DispatchExpectation &expected =
        stepSpec->expectedDispatches[dispatches.size()];
    const RoleState *target = expected.targetRole.empty()
                                  ? nullptr
                                  : findRole(expected.targetRole);
    const RoleState *secondary = expected.secondaryRole.empty()
                                     ? nullptr
                                     : findRole(expected.secondaryRole);
    if ((!expected.targetRole.empty() &&
         (target == nullptr || !target->current)) ||
        (!expected.secondaryRole.empty() &&
         (secondary == nullptr || !secondary->current))) {
      fail("dispatch references an unresolved graph role");
      return false;
    }

    DispatchKind kind{};
    std::string operation;
    std::string text;
    std::string propertyPhase;
    std::string propertyValue;
    bool payloadMatches = std::visit(
        [&](const auto &payload) {
          using T = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<T, ui::InvokeCommand>) {
            kind = DispatchKind::invokeCommand;
            operation = invokeOperation(payload.id);
            return expected.targetRole.empty();
          } else if constexpr (std::is_same_v<T, ui::SelectObject>) {
            kind = DispatchKind::selectObject;
            operation = "select";
            return target != nullptr && roleMatchesSelection(*target, payload);
          } else if constexpr (std::is_same_v<T, ui::HierarchyAction>) {
            kind = DispatchKind::hierarchyAction;
            operation = hierarchyOperation(payload.operation);
            if (target == nullptr || !target->current ||
                payload.scene != target->current->scene)
              return false;
            if (payload.operation == ui::HierarchyOperation::addRootNode)
              return !payload.node && !payload.directParent &&
                     target->current->object.kind == EditorObjectKind::scene;
            return payload.node &&
                   payload.node->untyped() == target->current->object &&
                   payload.directParent == target->current->directParent;
          } else if constexpr (std::is_same_v<T, ui::RenameObject>) {
            kind = DispatchKind::renameObject;
            operation = "rename";
            text = payload.name;
            return target != nullptr && target->current &&
                   payload.object == target->current->object;
          } else if constexpr (std::is_same_v<T, ui::ReparentNode>) {
            kind = DispatchKind::reparentNode;
            operation = "reparent";
            if (target == nullptr || secondary == nullptr ||
                !target->current || !secondary->current ||
                payload.source.scene != target->current->scene ||
                payload.source.node.untyped() != target->current->object ||
                payload.source.parent != target->current->directParent ||
                payload.targetScene != secondary->current->scene ||
                !stepBefore ||
                payload.source.revision != stepBefore->documentRevision)
              return false;
            const std::optional<NodeId> targetParent =
                secondary->current->object.kind == EditorObjectKind::node
                    ? NodeId::from(secondary->current->object)
                    : std::optional<NodeId>{};
            return payload.targetParent == targetParent;
          } else if constexpr (std::is_same_v<T, ui::PropertyEdit>) {
            kind = DispatchKind::propertyEdit;
            const std::string actualField = propertyFieldName(payload.field);
            operation = actualField;
            propertyPhase = editPhaseName(payload.phase);
            propertyValue = propertyValueText(payload.value);
            if (target == nullptr || !target->current ||
                payload.target != target->current->object)
              return false;
            const auto *vector =
                std::get_if<std::array<double, 3>>(&payload.value);
            if ((payload.field == ui::FieldId::translation ||
                 payload.field == ui::FieldId::rotationDegrees ||
                 payload.field == ui::FieldId::scale) &&
                (vector == nullptr ||
                 !std::all_of(vector->begin(), vector->end(),
                              [](double component) {
                                return std::isfinite(component);
                              })))
              return false;
            if (expected.operation != actualField ||
                propertyPhase != expected.propertyPhase ||
                propertyValue != expected.propertyValue)
              return false;
            return true;
          }
          return false;
        },
        command.payload);
    const bool textMatches = text == expected.text;
    if (!payloadMatches || command.sequence == 0u ||
        command.sequence <= lastDispatchSequence ||
        command.source != expected.source || kind != expected.kind ||
        operation != expected.operation || !textMatches ||
        result.accepted != expected.accepted ||
        result.refresh != expected.refresh || !result.error.empty()) {
      fail("actual UI dispatch differs from the frozen plan");
      return false;
    }
    lastDispatchSequence = command.sequence;
    observed = ObservedDispatch{command.sequence,
                                command.source,
                                kind,
                                std::move(operation),
                                expected.targetRole,
                                expected.secondaryRole,
                                std::move(text),
                                result.accepted,
                                result.refresh,
                                result.error,
                                std::move(propertyPhase),
                                std::move(propertyValue)};
    return true;
  }

  [[nodiscard]] std::optional<std::size_t>
  expectedDispatchPrefix() const noexcept {
    const StepSpec *stepSpec = currentStep();
    const PhaseSpec *phaseSpec = currentPhase();
    if (stepSpec == nullptr || phaseSpec == nullptr)
      return std::nullopt;
    if (config.plan.caseId != kU02Case)
      return phaseSpec->expectedDispatchCount;
    if (stepSpec->expectedDispatches.empty())
      return 0u;
    if (stepSpec->expectedDispatches.size() == 2u) {
      if (phase <= 1u)
        return 0u;
      return phase < stepSpec->phases.size() - 1u ? 1u : 2u;
    }
    return phase + 1u == stepSpec->phases.size() ? 1u : 0u;
  }

  [[nodiscard]] bool validateTerminal(const AuthorityProbe &after,
                                       const std::optional<GizmoProbe> &gizmo) {
    const StepSpec *stepSpec = currentStep();
    if (stepSpec == nullptr || !stepBefore)
      return false;
    const AuthorityProbe &before = *stepBefore;
    const bool escapeDetach = isEscapeGizmoCase(config.plan.caseId) &&
        stepSpec->id == (isSameFrameGizmoCase(config.plan.caseId)
                            ? "drag-same-frame" : "drag-escape-only");
    const bool focusDrag = isFocusGizmoCase(config.plan.caseId) &&
                           stepSpec->id.starts_with("drag-focus-");
    if ((config.plan.caseId == kU03InspectorCase ||
         config.plan.caseId == kU03GizmoCase ||
         isEscapeGizmoCase(config.plan.caseId) ||
         isFocusGizmoCase(config.plan.caseId)) &&
        stepSpec->effect != EffectKind::selectionOnly &&
        !escapeDetach && !focusDrag &&
        !sameSelection(before, after))
      return false;
    if (config.plan.caseId == kU03GizmoCase &&
        stepSpec->effect == EffectKind::documentMutation) {
      if (!stepBeforeGizmo || !gizmo || !sameNonYTransform(*stepBeforeGizmo, *gizmo) ||
          gizmo->translation[1] <= stepBeforeGizmo->translation[1] ||
          gizmo->node != stepBeforeGizmo->node || gizmo->captureHeld ||
          gizmo->phase != 1u || gizmo->activeComponent != 255u ||
          gizmo->beginCount != stepBeforeGizmo->beginCount + 1u ||
          gizmo->previewCount <= stepBeforeGizmo->previewCount ||
          gizmo->commitCount != stepBeforeGizmo->commitCount + 1u ||
          gizmo->detachCount != stepBeforeGizmo->detachCount ||
          gizmo->latchCount != stepBeforeGizmo->latchCount ||
          gizmo->terminal != "commit")
        return false;
    }
    if (escapeDetach) {
      if (!stepBeforeGizmo || !gizmo ||
          stepSpec->effect != EffectKind::documentMutation ||
          stepSpec->history != HistoryEffect::unchanged ||
          stepSpec->expectedRevisionDelta != 1 ||
          before.selectionKind != "Node" ||
          before.selectedObject != stepBeforeGizmo->node ||
          !before.selectedGraphRowKey || !noSelection(after) || gizmo->node ||
          gizmo->gestureNode != before.selectedObject ||
          after.selectionRevision != before.selectionRevision + 1u ||
          !sameNonYTransform(*stepBeforeGizmo, *gizmo) ||
          gizmo->translation[1] <= stepBeforeGizmo->translation[1] ||
          gizmo->captureHeld || gizmo->phase != 0u ||
          gizmo->activeComponent != 255u ||
          gizmo->beginCount != stepBeforeGizmo->beginCount + 1u ||
          gizmo->previewCount <= stepBeforeGizmo->previewCount ||
          gizmo->commitCount != stepBeforeGizmo->commitCount ||
          gizmo->detachCount != stepBeforeGizmo->detachCount + 1u ||
          gizmo->latchCount != stepBeforeGizmo->latchCount ||
          gizmo->terminal != "detach" || before.undoCount != 0u ||
          before.redoCount != 0u || !before.undoDescription.empty() ||
          !before.redoDescription.empty() || after.undoCount != 0u ||
          after.redoCount != 0u || !after.undoDescription.empty() ||
          !after.redoDescription.empty())
        return false;
    }
    if (focusDrag) {
      if (!stepBeforeGizmo || !gizmo ||
          stepSpec->effect != EffectKind::documentMutation ||
          stepSpec->expectedRevisionDelta != 1 ||
          before.selectionKind != "Node" || !before.selectedObject ||
          !sameSelection(before, after) ||
          gizmo->node != stepBeforeGizmo->node ||
          !sameNonYTransform(*stepBeforeGizmo, *gizmo) ||
          gizmo->translation[1] <= stepBeforeGizmo->translation[1] ||
          gizmo->beginCount != stepBeforeGizmo->beginCount + 1u ||
          gizmo->previewCount <= stepBeforeGizmo->previewCount ||
          gizmo->detachCount != stepBeforeGizmo->detachCount ||
          (stepSpec->id == "drag-focus-delivered-release"
               ? (gizmo->commitCount != stepBeforeGizmo->commitCount + 1u ||
                  gizmo->latchCount != stepBeforeGizmo->latchCount ||
                  gizmo->phase != 1u || gizmo->terminal != "commit")
               : (gizmo->commitCount != stepBeforeGizmo->commitCount ||
                  gizmo->latchCount != stepBeforeGizmo->latchCount + 1u ||
                  gizmo->phase != 3u || gizmo->terminal != "latch")) ||
          gizmo->captureHeld)
        return false;
    }
    if (isEscapeGizmoCase(config.plan.caseId) && gizmo &&
        gizmo->detachCount != 0u && !escapeDetach) {
      if (!stepBeforeGizmo ||
          stepSpec->effect != EffectKind::presentationOnly ||
          !noSelection(before) || !noSelection(after) ||
          after.selectionRevision != before.selectionRevision ||
          !sameTransform(*stepBeforeGizmo, *gizmo) ||
          !sameTerminalJournal(*stepBeforeGizmo, *gizmo) ||
          gizmo->node || gizmo->gestureNode != stepBeforeGizmo->gestureNode ||
          gizmo->beginCount != stepBeforeGizmo->beginCount ||
          gizmo->previewCount != stepBeforeGizmo->previewCount ||
          gizmo->commitCount != stepBeforeGizmo->commitCount ||
          gizmo->detachCount != stepBeforeGizmo->detachCount ||
          gizmo->latchCount != stepBeforeGizmo->latchCount)
        return false;
    }
    if (stepSpec->effect == EffectKind::authorityReset) {
      return before.documentAuthority == after.documentAuthority &&
             before.documentDigest == after.documentDigest &&
             before.projectionDigest == after.projectionDigest &&
             before.runtimeContentDigest == after.runtimeContentDigest &&
             before.hierarchyOrderDigest == after.hierarchyOrderDigest &&
             after.undoCount == 0u && after.redoCount == 0u;
    }
    if (before.documentAuthority != after.documentAuthority ||
        after.documentRevision != before.documentRevision +
                                      stepSpec->expectedRevisionDelta)
      return false;
    const bool digestsEqual =
        before.documentDigest == after.documentDigest &&
        before.projectionDigest == after.projectionDigest &&
        before.runtimeContentDigest == after.runtimeContentDigest;
    const bool digestsChanged =
        before.documentDigest != after.documentDigest &&
        before.projectionDigest != after.projectionDigest &&
        before.runtimeContentDigest != after.runtimeContentDigest;
    if (stepSpec->effect == EffectKind::documentMutation && !digestsChanged)
      return false;
    if ((stepSpec->effect == EffectKind::presentationOnly ||
         stepSpec->effect == EffectKind::selectionOnly ||
         stepSpec->effect == EffectKind::documentNoopHistoryEnvelope) &&
        !digestsEqual)
      return false;
    if (stepSpec->effect == EffectKind::historyMove &&
        ((stepSpec->expectedRevisionDelta == 0 && !digestsEqual) ||
         (stepSpec->expectedRevisionDelta != 0 && !digestsChanged)))
      return false;
    if (stepSpec->expectHierarchyOrderDigestChange ==
        (before.hierarchyOrderDigest == after.hierarchyOrderDigest))
      return false;
    if (stepSpec->effect == EffectKind::documentNoopHistoryEnvelope &&
        stepSpec->id != "unique-root-delete-confirm" &&
        (!beforeDrop || !sameSelection(*beforeDrop, after)))
      return false;
    switch (stepSpec->history) {
    case HistoryEffect::unchanged:
      return after.undoCount == before.undoCount &&
             after.redoCount == before.redoCount;
    case HistoryEffect::pushUndoClearRedo:
      return after.undoCount == before.undoCount + 1u &&
             after.redoCount == 0u && !after.undoDescription.empty() &&
             after.redoDescription.empty();
    case HistoryEffect::moveUndoToRedo:
      return before.undoCount != 0u &&
             after.undoCount + 1u == before.undoCount &&
             after.redoCount == before.redoCount + 1u &&
             !after.redoDescription.empty();
    case HistoryEffect::moveRedoToUndo:
      return before.redoCount != 0u &&
             after.redoCount + 1u == before.redoCount &&
             after.undoCount == before.undoCount + 1u &&
             !after.undoDescription.empty();
    case HistoryEffect::reset:
      return after.undoCount == 0u && after.redoCount == 0u;
    }
    return false;
  }

  [[nodiscard]] bool usableLifecycle(ui::LifecycleState state) const noexcept {
    return state == ui::LifecycleState::readyClean ||
           state == ui::LifecycleState::readyModified ||
           state == ui::LifecycleState::modal ||
           (config.plan.caseId != kU02Case &&
            (state == ui::LifecycleState::gizmoHover ||
             state == ui::LifecycleState::gizmoDragging));
  }

  [[nodiscard]] bool prepareAndPublishReady() {
    if (runtimeStatus != RuntimeStatus::running || !frameSealed) {
      pendingReason = "runtime-or-frame-not-ready";
      return false;
    }
    frameSealed = false;
    if (!resolveRoles() || runtimeStatus == RuntimeStatus::failed) {
      pendingReason = "graph-role-resolution";
      return false;
    }
    std::optional<RuntimeHostSnapshot> host;
    try {
      host = callbacks.snapshot();
    } catch (...) {
      fail("runtime host snapshot callback threw");
      return false;
    }
    if (!host) {
      pendingReason = "host-snapshot-unavailable";
      return false;
    }
    if (!host->rendererSettled) {
      pendingReason = "renderer-not-settled";
      return false;
    }
    if (!usableLifecycle(frame.lifecycle)) {
      pendingReason = "lifecycle-not-usable";
      return false;
    }
    if (frame.documentRevision != host->authority.documentRevision ||
        frame.selectionRevision != host->authority.selectionRevision ||
        frame.frameSerial == 0u || frame.mainViewportId == 0u ||
        host->authority.projectionDigest !=
            host->authority.runtimeContentDigest) {
      pendingReason = "frame-authority-mismatch";
      return false;
    }
    const StepSpec *stepSpec = currentStep();
    const PhaseSpec *phaseSpec = currentPhase();
    const std::optional<std::size_t> expectedPrefix = expectedDispatchPrefix();
    if (stepSpec == nullptr || phaseSpec == nullptr || !expectedPrefix) {
      fail("current phase lacks an explicit dispatch prefix contract");
      return false;
    }
    if (config.plan.caseId != kU02Case &&
        dispatches.size() > *expectedPrefix) {
      fail("actual UI dispatch exceeds the current phase prefix");
      return false;
    }
    if (dispatches.size() != *expectedPrefix ||
        (phase + 1u == stepSpec->phases.size() &&
         dispatches.size() != stepSpec->expectedDispatches.size())) {
      pendingReason = "phase-or-dispatch-prefix-mismatch";
      return false;
    }
    if (!validateGizmoSnapshot(*host, *phaseSpec))
      return false;

    ReadyProbe candidate;
    candidate.schemaVersion = kSchemaVersion;
    candidate.protocol = std::string(kProtocol);
    candidate.runNonce = config.runNonce;
    candidate.caseId = config.plan.caseId;
    candidate.planSha256 = config.planSha256;
    candidate.executableSha256 = config.executableSha256;
    candidate.sourceSha256 = config.sourceSha256;
    candidate.step = step;
    candidate.stepId = stepSpec->id;
    candidate.phaseId = phaseSpec->id;
    candidate.renderedFrame = frame.frameSerial;
    candidate.frame = frame;
    candidate.client = host->client;
    candidate.authority = host->authority;
    candidate.gizmo = host->gizmo;
    candidate.observedDispatches = dispatches;
    candidate.target.semantic = phaseSpec->target.semantic;
    candidate.target.role = phaseSpec->target.role;
    candidate.target.graphRowRole = phaseSpec->target.graphRowRole;
    candidate.target.revision = host->authority.documentRevision;

    if (phaseSpec->target.semantic == TargetSemantic::client) {
      candidate.target.matchCount = 1u;
      candidate.target.visible = true;
      candidate.target.enabled = true;
      candidate.target.hovered = false;
      candidate.target.focused = host->clientFocused;
      candidate.target.rect = host->client.screenRect;
    } else if (nativeGizmoTarget(phaseSpec->target.semantic)) {
      const GizmoProbe &gizmo = *host->gizmo;
      const bool componentTarget =
          phaseSpec->target.semantic == TargetSemantic::gizmoTranslateY;
      if (componentTarget &&
          (!gizmo.componentValid || !gizmo.componentEnabled)) {
        pendingReason = "gizmo-component-not-observed-or-enabled";
        return false;
      }
      const ui::EditorUiProbeRect &rect =
          componentTarget ? gizmo.componentRect : gizmo.outsideRect;
      const std::optional<ScreenRect> converted = physicalRect(rect, *host);
      if (!converted) {
        fail("native gizmo target rectangle is not convertible");
        return false;
      }
      if (!phaseSpec->target.graphRowRole.empty())
        candidate.target.graphRowKey =
            findRole(phaseSpec->target.graphRowRole)->current;
      candidate.target.observationSource = "editor-gizmo-adapter-v1";
      candidate.target.label = std::string(targetSemanticName(
          phaseSpec->target.semantic));
      candidate.target.probeRect = rect;
      candidate.target.rect = *converted;
      candidate.target.matchCount = 1u;
      candidate.target.visible = true;
      candidate.target.enabled = !componentTarget || gizmo.componentEnabled;
      candidate.target.hovered =
          gizmo.hasPointer && containsPoint(rect, gizmo.pointer) &&
          (!componentTarget || gizmo.hoveredComponent == 1u);
      candidate.target.focused = host->clientFocused;
    } else {
      const std::optional<ui::EditorUiProbeSemantic> semantic =
          editorProbeSemantic(phaseSpec->target.semantic);
      if (!semantic) {
        fail("current phase target has no real semantic observation: " +
             std::string(targetSemanticName(phaseSpec->target.semantic)));
        return false;
      }
      RoleState *role = findRole(phaseSpec->target.graphRowRole);
      if (role == nullptr || !role->current) {
        pendingReason = "target-role-unavailable";
        return false;
      }
      std::vector<const OwnedItem *> matches;
      const bool inspectorTarget =
          phaseSpec->target.semantic >= TargetSemantic::inspectorTranslationX &&
          phaseSpec->target.semantic <= TargetSemantic::inspectorScaleZ;
      for (const OwnedItem &item : items) {
        if (item.key.semantic == *semantic &&
            (inspectorTarget ? item.key.object == role->current->object
                             : item.key.row == *role->current))
          matches.push_back(&item);
      }
      if (matches.size() > 1u) {
        fail("current phase target has duplicate probe observations");
        return false;
      }
      if (matches.empty()) {
        pendingReason = "target-not-observed";
        return false;
      }
      const OwnedItem &item = *matches.front();
      const std::optional<ScreenRect> converted = physicalRect(item.rect, *host);
      if (!converted) {
        pendingReason = "target-rect-not-convertible";
        return false;
      }
      if (!item.visible || !item.enabled) {
        pendingReason = "target-not-visible-or-enabled";
        return false;
      }
      candidate.target.graphRowKey = role->current;
      candidate.target.label = item.label;
      candidate.target.order = item.order;
      candidate.target.itemId = item.itemId;
      candidate.target.windowToken = item.windowToken;
      candidate.target.popupToken = item.popupToken;
      candidate.target.modalToken = item.modalToken;
      candidate.target.probeCoordinateSpace = item.coordinateSpace;
      candidate.target.probeRect = item.rect;
      candidate.target.rect = *converted;
      candidate.target.matchCount = 1u;
      candidate.target.visible = item.visible;
      candidate.target.enabled = item.enabled;
      candidate.target.hovered = item.hovered;
      candidate.target.focused = item.focused;
      candidate.target.dropAccepted = item.dropAccepted;
      const bool modalTarget =
          phaseSpec->target.semantic == TargetSemantic::renameInput ||
          phaseSpec->target.semantic == TargetSemantic::deleteAccept;
      if (modalTarget != (frame.lifecycle == ui::LifecycleState::modal)) {
        pendingReason = "target-modal-lifecycle-mismatch";
        return false;
      }
    }
    if (phaseSpec->requireHovered && !candidate.target.hovered) {
      pendingReason = "target-not-hovered";
      return false;
    }
    if (phaseSpec->requireFocused && !candidate.target.focused) {
      pendingReason = "target-not-focused";
      return false;
    }
    if (phaseSpec->requireDropAccepted && !candidate.target.dropAccepted) {
      pendingReason = "drop-not-accepted";
      return false;
    }
    if (phase == 0u && !stepBefore) {
      stepBefore = host->authority;
      stepBeforeGizmo = host->gizmo;
    }
    if (phase == 0u && previousStepAfter &&
        stepSpec->effect != EffectKind::authorityReset) {
      const AuthorityProbe &previous = *previousStepAfter;
      if (previous.documentAuthority != host->authority.documentAuthority ||
          previous.documentRevision != host->authority.documentRevision ||
          previous.undoCount != host->authority.undoCount ||
          previous.redoCount != host->authority.redoCount ||
          previous.documentDigest != host->authority.documentDigest ||
          previous.projectionDigest != host->authority.projectionDigest ||
          previous.runtimeContentDigest !=
              host->authority.runtimeContentDigest ||
          previous.hierarchyOrderDigest !=
              host->authority.hierarchyOrderDigest ||
          !sameSelection(previous, host->authority)) {
        fail("authority changed between replay steps");
        return false;
      }
    }
    if (phase + 1u == stepSpec->phases.size() &&
        !validateTerminal(host->authority, host->gizmo)) {
      fail("terminal authority/history effect differs from the plan");
      return false;
    }
    const PlanValidation validation = candidate.validateAgainst(config.plan);
    if (!validation.valid) {
      fail(validation.error.empty() ? "ready probe validation failed"
                                    : validation.error);
      return false;
    }
    if (config.requiredStableFrames > 1u) {
      ReadyProbe identityCandidate = candidate;
      identityCandidate.renderedFrame = 0u;
      identityCandidate.frame.lifecycle = {};
      identityCandidate.frame.lifecycleRevision = 0u;
      identityCandidate.frame.documentRevision = 0u;
      identityCandidate.frame.selectionRevision = 0u;
      identityCandidate.frame.frameSerial = 0u;
      identityCandidate.authority = {};
      identityCandidate.target.revision = 0u;
      identityCandidate.target.hovered = false;
      identityCandidate.target.focused = false;
      identityCandidate.target.dropAccepted = false;
      identityCandidate.gizmo.reset();
      identityCandidate.observedDispatches.clear();
      const std::string identity = identityCandidate.toJson();
      const bool consecutive =
          stableCandidateFrameSerial != 0u &&
          candidate.frame.frameSerial == stableCandidateFrameSerial + 1u;
      if (!consecutive || identity != stableCandidateIdentity) {
        stableCandidateIdentity = identity;
        stableCandidateFrames = 1u;
      } else if (stableCandidateFrames < config.requiredStableFrames) {
        ++stableCandidateFrames;
      }
      stableCandidateFrameSerial = candidate.frame.frameSerial;
      if (stableCandidateFrames < config.requiredStableFrames) {
        pendingReason = "target-geometry-stabilizing";
        return false;
      }
    }
    stableCandidateIdentity.clear();
    stableCandidateFrameSerial = 0u;
    stableCandidateFrames = 0u;
    pendingReason.clear();
    ReadyPublication publication;
    try {
      publication = callbacks.publishReady(candidate);
    } catch (...) {
      fail("ready publication callback threw");
      return false;
    }
    if (!publication.published ||
        !lowercaseHex(publication.readySha256, 64u)) {
      fail(publication.error.empty() ? "ready publication failed"
                                     : publication.error);
      return false;
    }
    ready = std::move(candidate);
    if (ready->gizmo)
      lastPublishedGizmo = ready->gizmo;
    readySha256 = std::move(publication.readySha256);
    if (phaseSpec->requireDropAccepted &&
        phaseSpec->input.kind == InputKind::leftUp)
      beforeDrop = host->authority;
    if (phase + 1u == stepSpec->phases.size())
      previousStepAfter = host->authority;
    runtimeStatus = RuntimeStatus::waitingForContinue;
    return true;
  }
};

EditorUiOsReplayRuntime::EditorUiOsReplayRuntime(
    RuntimeConfig config, RuntimeCallbacks callbacks)
    : m_impl(std::make_unique<Impl>(std::move(config),
                                    std::move(callbacks))) {}

EditorUiOsReplayRuntime::~EditorUiOsReplayRuntime() = default;
EditorUiOsReplayRuntime::EditorUiOsReplayRuntime(
    EditorUiOsReplayRuntime &&) noexcept = default;
EditorUiOsReplayRuntime &EditorUiOsReplayRuntime::operator=(
    EditorUiOsReplayRuntime &&) noexcept = default;

void EditorUiOsReplayRuntime::beginFrame(
    const ui::EditorUiProbeFrameBinding &binding) {
  if (m_impl->runtimeStatus == RuntimeStatus::complete ||
      m_impl->runtimeStatus == RuntimeStatus::failed) {
    m_impl->ignoreFrame = true;
    return;
  }
  if (m_impl->frameOpen || m_impl->frameSealed) {
    m_impl->fail("probe frame overlap or missing onFrameRendered call");
    return;
  }
  if (binding.frameSerial == 0u ||
      binding.frameSerial <= m_impl->lastFrameSerial) {
    m_impl->fail("probe frame serial is stale");
    return;
  }
  m_impl->frameOpen = true;
  m_impl->frame = binding;
  m_impl->items.clear();
}

void EditorUiOsReplayRuntime::observeItem(const ui::EditorUiProbeItem &item) {
  if (m_impl->ignoreFrame || m_impl->runtimeStatus == RuntimeStatus::failed)
    return;
  if (!m_impl->frameOpen || !item.valid() || item.label.empty() ||
      item.windowToken == 0u ||
      item.rectCoordinateSpace !=
          ui::EditorUiProbeCoordinateSpace::imguiScreenLogical ||
      item.order != m_impl->items.size() + 1u ||
      std::any_of(m_impl->items.begin(), m_impl->items.end(),
                  [&](const Impl::OwnedItem &owned) {
                    return owned.key == item.key;
                  })) {
    m_impl->fail("probe item is invalid, duplicate, or out of order");
    return;
  }
  m_impl->items.push_back(Impl::OwnedItem{
      item.key,          std::string(item.label), item.order,
      item.itemId,       item.windowToken,        item.popupToken,
      item.modalToken,   item.rectCoordinateSpace,
      item.rect,         item.visible,            item.hovered,
      item.active,       item.focused,            item.enabled,
      item.dropAccepted});
}

void EditorUiOsReplayRuntime::observeDispatch(
    const ui::UiCommand &command, const ui::RuntimeActionResult &result) {
  if (m_impl->runtimeStatus == RuntimeStatus::failed ||
      m_impl->runtimeStatus == RuntimeStatus::complete)
    return;
  if (!m_impl->frameOpen) {
    m_impl->fail("UI dispatch was observed outside an open probe frame");
    return;
  }
  ObservedDispatch observed;
  if (m_impl->normalizeDispatch(command, result, observed))
    m_impl->dispatches.push_back(std::move(observed));
}

void EditorUiOsReplayRuntime::endFrame() {
  if (m_impl->ignoreFrame) {
    m_impl->ignoreFrame = false;
    return;
  }
  if (m_impl->runtimeStatus == RuntimeStatus::failed)
    return;
  if (!m_impl->frameOpen) {
    m_impl->fail("probe endFrame has no matching beginFrame");
    return;
  }
  m_impl->frameOpen = false;
  m_impl->frameSealed = true;
  m_impl->lastFrameSerial = m_impl->frame.frameSerial;
}

bool EditorUiOsReplayRuntime::onFrameRendered() {
  if (!m_impl->frameSealed)
    return false;
  if (m_impl->runtimeStatus == RuntimeStatus::waitingForContinue) {
    m_impl->frameSealed = false;
    return false;
  }
  return m_impl->prepareAndPublishReady();
}

bool EditorUiOsReplayRuntime::acceptContinue(
    const ContinueReceipt &receipt) {
  if (m_impl->runtimeStatus != RuntimeStatus::waitingForContinue ||
      !m_impl->ready)
    return false;
  const StepSpec *stepSpec = m_impl->currentStep();
  const PhaseSpec *phaseSpec = m_impl->currentPhase();
  const std::optional<std::uint32_t> inputCount =
      phaseSpec != nullptr ? expectedInputCount(phaseSpec->input)
                           : std::nullopt;
  if (stepSpec == nullptr || phaseSpec == nullptr || !inputCount ||
      receipt.schemaVersion != kSchemaVersion || receipt.protocol != kProtocol ||
      receipt.runNonce != m_impl->config.runNonce ||
      receipt.caseId != m_impl->config.plan.caseId ||
      receipt.planSha256 != m_impl->config.planSha256 ||
      receipt.step != m_impl->step || receipt.stepId != stepSpec->id ||
      receipt.phaseId != phaseSpec->id ||
      receipt.readySha256 != m_impl->readySha256 ||
      !lowercaseHex(receipt.readySha256, 64u) ||
      receipt.inputKind != phaseSpec->input.kind ||
      receipt.sendInputCount != *inputCount)
    return false;

  m_impl->ready.reset();
  m_impl->readySha256.clear();
  m_impl->stableCandidateIdentity.clear();
  m_impl->stableCandidateFrameSerial = 0u;
  m_impl->stableCandidateFrames = 0u;
  m_impl->pendingReason.clear();
  m_impl->frameSealed = false;
  m_impl->runtimeStatus = RuntimeStatus::running;
  if (m_impl->phase + 1u < stepSpec->phases.size()) {
    ++m_impl->phase;
    return true;
  }
  if (m_impl->step == m_impl->config.stopStep) {
    m_impl->runtimeStatus = RuntimeStatus::complete;
    return true;
  }
  ++m_impl->step;
  m_impl->phase = 0u;
  m_impl->dispatches.clear();
  m_impl->stepBefore.reset();
  m_impl->stepBeforeGizmo.reset();
  m_impl->beforeDrop.reset();
  return true;
}

RuntimeStatus EditorUiOsReplayRuntime::status() const noexcept {
  return m_impl->runtimeStatus;
}

bool EditorUiOsReplayRuntime::complete() const noexcept {
  return status() == RuntimeStatus::complete;
}

bool EditorUiOsReplayRuntime::failed() const noexcept {
  return status() == RuntimeStatus::failed;
}

bool EditorUiOsReplayRuntime::waitingForContinue() const noexcept {
  return status() == RuntimeStatus::waitingForContinue;
}

std::string_view EditorUiOsReplayRuntime::error() const noexcept {
  return m_impl->failure;
}

std::uint32_t EditorUiOsReplayRuntime::currentStep() const noexcept {
  return m_impl->step;
}

std::size_t EditorUiOsReplayRuntime::currentPhase() const noexcept {
  return m_impl->phase;
}

std::uint32_t
EditorUiOsReplayRuntime::pendingStableFrameCount() const noexcept {
  return m_impl->stableCandidateFrames;
}

std::uint64_t
EditorUiOsReplayRuntime::pendingStableFrameSerial() const noexcept {
  return m_impl->stableCandidateFrameSerial;
}

std::string_view EditorUiOsReplayRuntime::pendingReason() const noexcept {
  return m_impl->pendingReason;
}

const std::optional<ReadyProbe> &
EditorUiOsReplayRuntime::lastReady() const noexcept {
  return m_impl->ready;
}

std::optional<ui::GraphRowKey>
EditorUiOsReplayRuntime::graphRole(std::string_view role) const noexcept {
  const Impl::RoleState *state = m_impl->findRole(role);
  return state != nullptr ? state->current : std::nullopt;
}

} // namespace demo::editor::testing::osreplay
