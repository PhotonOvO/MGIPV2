#include "EditorUiOsReplayHost.h"

#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include "../GlfwSdl3Compat.h"
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace demo::editor::testing::osreplay {
namespace {

[[nodiscard]] std::string digestHex(const SceneDigest &digest) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(digest.size() * 2u);
  for (const std::uint8_t byte : digest) {
    output.push_back(hex[(byte >> 4u) & 0x0fu]);
    output.push_back(hex[byte & 0x0fu]);
  }
  return output;
}

[[nodiscard]] std::string sha256(std::string_view bytes) {
  const auto *begin = reinterpret_cast<const std::uint8_t *>(bytes.data());
  return digestHex(SceneChangeCanonicalizer::computeSha256(
      std::span<const std::uint8_t>(begin, bytes.size())));
}

[[nodiscard]] std::string readFile(const std::filesystem::path &path,
                                   std::size_t maximumBytes = 0u) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw std::runtime_error("could not open OS replay file: " +
                             path.string());
  const std::streamoff end = input.tellg();
  if (end < 0 || (maximumBytes != 0u &&
                  static_cast<std::uint64_t>(end) > maximumBytes))
    throw std::runtime_error("OS replay file size is invalid: " +
                             path.string());
  std::string bytes(static_cast<std::size_t>(end), '\0');
  input.seekg(0, std::ios::beg);
  if (!bytes.empty())
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input)
    throw std::runtime_error("could not read OS replay file: " +
                             path.string());
  return bytes;
}

[[nodiscard]] std::filesystem::path currentExecutablePath() {
#ifdef _WIN32
  std::vector<wchar_t> buffer(32768u, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                        static_cast<DWORD>(buffer.size()));
  if (size == 0u || size >= buffer.size())
    throw std::runtime_error("could not resolve OS replay module path");
  return std::filesystem::path(std::wstring(buffer.data(), size));
#else
  throw std::runtime_error("OS replay host requires Win32");
#endif
}

[[nodiscard]] bool ordinaryDirectory(const std::filesystem::path &path) {
  std::error_code error;
  const auto link = std::filesystem::symlink_status(path, error);
  return !error && !std::filesystem::is_symlink(link) &&
         std::filesystem::is_directory(path, error) && !error;
}

[[nodiscard]] bool ordinaryFile(const std::filesystem::path &path) {
  std::error_code error;
  const auto link = std::filesystem::symlink_status(path, error);
  return !error && !std::filesystem::is_symlink(link) &&
         std::filesystem::is_regular_file(path, error) && !error;
}

void writeAtomic(const std::filesystem::path &path, std::string_view bytes) {
  const std::filesystem::path temporary = path.string() + ".tmp";
  std::error_code error;
  if (std::filesystem::exists(path, error) || error ||
      std::filesystem::exists(temporary, error) || error)
    throw std::runtime_error("OS replay artifact already exists");
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output)
    throw std::runtime_error("could not write OS replay artifact");
  std::filesystem::rename(temporary, path, error);
  if (error)
    throw std::runtime_error("could not publish OS replay artifact");
}

[[nodiscard]] std::string paddedStep(std::uint32_t step) {
  const std::string value = std::to_string(step);
  return std::string(value.size() < 3u ? 3u - value.size() : 0u, '0') + value;
}

[[nodiscard]] std::string readyStem(std::string_view nonce,
                                    std::uint32_t step,
                                    std::string_view phase) {
  return std::string(nonce) + "." + paddedStep(step) + "." +
         std::string(phase);
}

[[nodiscard]] std::string_view commandSourceName(
    ui::CommandSource source) noexcept {
  switch (source) {
  case ui::CommandSource::menu:
    return "menu";
  case ui::CommandSource::toolbar:
    return "toolbar";
  case ui::CommandSource::shortcut:
    return "shortcut";
  case ui::CommandSource::sceneBrowser:
    return "scene-browser";
  case ui::CommandSource::inspector:
    return "inspector";
  case ui::CommandSource::dialog:
    return "dialog";
  case ui::CommandSource::settings:
    return "settings";
  }
  return "invalid";
}

[[nodiscard]] std::string_view refreshName(
    ui::ProjectionRefresh refresh) noexcept {
  switch (refresh) {
  case ui::ProjectionRefresh::none:
    return "none";
  case ui::ProjectionRefresh::selectionOnly:
    return "selection-only";
  case ui::ProjectionRefresh::properties:
    return "properties";
  case ui::ProjectionRefresh::hierarchy:
    return "hierarchy";
  case ui::ProjectionRefresh::topology:
    return "topology";
  case ui::ProjectionRefresh::fullScene:
    return "full-scene";
  }
  return "invalid";
}

void appendJsonString(std::string &output, std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20u) {
        output += "\\u00";
        output.push_back(hex[(character >> 4u) & 0x0fu]);
        output.push_back(hex[character & 0x0fu]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
}

[[nodiscard]] InputKind parseInputKind(std::string_view name) {
  for (const InputKind value :
       {InputKind::none, InputKind::movePointer, InputKind::leftClick,
        InputKind::rightClick, InputKind::rightDown, InputKind::rightUp,
        InputKind::leftDown, InputKind::leftUp, InputKind::keyChord,
        InputKind::replaceText, InputKind::pressKey, InputKind::keyDown,
        InputKind::keyUp, InputKind::escapeReleaseBatch, InputKind::focusLoss,
        InputKind::focusRestore, InputKind::leftUpFocusLoss}) {
    if (inputKindName(value) == name)
      return value;
  }
  throw std::runtime_error("continue input kind is unknown");
}

class ContinueParser final {
public:
  explicit ContinueParser(std::string_view source) : m_source(source) {}

  [[nodiscard]] ContinueReceipt parse() {
    ContinueReceipt result;
    beginObject();
    key("documentKind");
    if (string() != "mgif.editor-ui-os-replay-continue")
      fail("document kind");
    comma();
    key("schemaVersion");
    result.schemaVersion = unsignedValue();
    comma();
    key("protocol");
    result.protocol = string();
    comma();
    key("runNonce");
    result.runNonce = string();
    comma();
    key("case");
    result.caseId = string();
    comma();
    key("planSha256");
    result.planSha256 = string();
    comma();
    key("step");
    beginObject();
    key("index");
    result.step = unsignedValue();
    comma();
    key("id");
    result.stepId = string();
    comma();
    key("phase");
    result.phaseId = string();
    endObject();
    comma();
    key("readySha256");
    result.readySha256 = string();
    comma();
    key("input");
    beginObject();
    key("kind");
    result.inputKind = parseInputKind(string());
    comma();
    key("sendInputCount");
    result.sendInputCount = unsignedValue();
    endObject();
    endObject();
    whitespace();
    if (m_offset != m_source.size())
      fail("trailing bytes");
    return result;
  }

private:
  [[noreturn]] void fail(std::string_view field) const {
    throw std::runtime_error("strict continue JSON rejected " +
                             std::string(field));
  }

  void whitespace() {
    while (m_offset < m_source.size() &&
           (m_source[m_offset] == ' ' || m_source[m_offset] == '\t' ||
            m_source[m_offset] == '\r' || m_source[m_offset] == '\n'))
      ++m_offset;
  }

  void character(char expected) {
    whitespace();
    if (m_offset >= m_source.size() || m_source[m_offset] != expected)
      fail("punctuation");
    ++m_offset;
  }

  void beginObject() { character('{'); }
  void endObject() { character('}'); }
  void comma() { character(','); }

  void key(std::string_view expected) {
    if (string() != expected)
      fail("object key/order");
    character(':');
  }

  [[nodiscard]] static std::uint32_t hex(char value) {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint32_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<std::uint32_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
      return static_cast<std::uint32_t>(value - 'A' + 10);
    throw std::runtime_error("strict continue JSON rejected unicode escape");
  }

  void appendCodePoint(std::string &output, std::uint32_t codePoint) {
    if (codePoint <= 0x7fu) {
      output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffu) {
      output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
      output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu &&
               !(codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
      output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
      output.push_back(
          static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
      output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else {
      fail("unicode scalar");
    }
  }

  [[nodiscard]] std::string string() {
    whitespace();
    if (m_offset >= m_source.size() || m_source[m_offset++] != '"')
      fail("string opening quote");
    std::string output;
    while (m_offset < m_source.size()) {
      const unsigned char value =
          static_cast<unsigned char>(m_source[m_offset++]);
      if (value == '"')
        return output;
      if (value < 0x20u)
        fail("string control byte");
      if (value != '\\') {
        output.push_back(static_cast<char>(value));
        continue;
      }
      if (m_offset >= m_source.size())
        fail("string escape");
      const char escaped = m_source[m_offset++];
      switch (escaped) {
      case '"': output.push_back('"'); break;
      case '\\': output.push_back('\\'); break;
      case '/': output.push_back('/'); break;
      case 'b': output.push_back('\b'); break;
      case 'f': output.push_back('\f'); break;
      case 'n': output.push_back('\n'); break;
      case 'r': output.push_back('\r'); break;
      case 't': output.push_back('\t'); break;
      case 'u': {
        if (m_source.size() - m_offset < 4u)
          fail("unicode escape length");
        std::uint32_t codePoint = 0u;
        for (std::size_t index = 0u; index < 4u; ++index)
          codePoint = (codePoint << 4u) | hex(m_source[m_offset++]);
        appendCodePoint(output, codePoint);
        break;
      }
      default: fail("string escape kind");
      }
    }
    fail("unterminated string");
  }

  [[nodiscard]] std::uint32_t unsignedValue() {
    whitespace();
    if (m_offset >= m_source.size() || m_source[m_offset] < '0' ||
        m_source[m_offset] > '9')
      fail("unsigned integer");
    if (m_source[m_offset] == '0' && m_offset + 1u < m_source.size() &&
        m_source[m_offset + 1u] >= '0' && m_source[m_offset + 1u] <= '9')
      fail("integer leading zero");
    std::uint64_t value = 0u;
    while (m_offset < m_source.size() && m_source[m_offset] >= '0' &&
           m_source[m_offset] <= '9') {
      value = value * 10u +
              static_cast<std::uint64_t>(m_source[m_offset++] - '0');
      if (value > 0xffffffffu)
        fail("integer overflow");
    }
    return static_cast<std::uint32_t>(value);
  }

  std::string_view m_source;
  std::size_t m_offset{0u};
};

} // namespace

struct EditorUiOsReplayHost::Impl final {
  GLFWwindow *window{nullptr};
  CliOptions options;
  ReplayPlan plan;
  std::vector<ReadyProbe> terminalReady;
  std::vector<std::filesystem::path> acceptedContinue;
  std::optional<std::filesystem::path> pendingContinue;
  std::optional<ContinueReceipt> pendingReceipt;
  std::function<std::size_t()> pendingNativeTerminals;
  std::function<bool()> nativeFocusInterrupted;

  Impl(GLFWwindow *value, CliOptions values,
       std::function<std::size_t()> nativeTerminals,
       std::function<bool()> focusInterrupted)
      : window(value), options(std::move(values)),
        pendingNativeTerminals(std::move(nativeTerminals)),
        nativeFocusInterrupted(std::move(focusInterrupted)) {
    plan = isSameFrameGizmoCase(options.caseId)
               ? makeU03SameFrameOsReplayPlan(options.caseId == kU03EscapeReleaseCase)
           : isFocusGizmoCase(options.caseId)
               ? makeU03FocusOsReplayPlan(options.caseId)
           : options.caseId == kU03EscapeCase ? makeU03EscapeOsReplayPlan()
           : options.caseId == kU03GizmoCase ? makeU03GizmoOsReplayPlan()
           : options.caseId == kU03InspectorCase ? makeU03InspectorOsReplayPlan()
           : options.caseId == kU03Case ? makeU03OsReplayPlan()
                                       : makeU02OsReplayPlan();
    if (window == nullptr)
      throw std::invalid_argument("OS replay host configuration is invalid");
    const std::string trace = plan.contractTrace();
    if (sha256(trace) != options.planSha256)
      throw std::invalid_argument("OS replay plan SHA does not match case");
    const std::filesystem::path executable = currentExecutablePath();
    if (sha256(readFile(executable)) != options.executableSha256)
      throw std::invalid_argument("OS replay executable SHA does not match module");
    const std::filesystem::path appDirectory =
        options.evidenceDirectory.lexically_normal().parent_path();
    const std::filesystem::path authorityRoot = appDirectory.parent_path();
    if (appDirectory.filename() != "app" ||
        options.syncDirectory.lexically_normal() != authorityRoot / "sync" ||
        !ordinaryDirectory(authorityRoot) || !ordinaryDirectory(appDirectory) ||
        !ordinaryDirectory(options.syncDirectory))
      throw std::invalid_argument("OS replay sync directory is not ordinary");
    std::error_code error;
    if (std::filesystem::exists(options.evidenceDirectory, error) || error ||
        !std::filesystem::create_directory(options.evidenceDirectory, error) ||
        error || !ordinaryDirectory(options.evidenceDirectory))
      throw std::invalid_argument("OS replay evidence directory is not fresh");
  }

  [[nodiscard]] ClientProbe clientProbe() const {
#ifdef _WIN32
    const HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == nullptr)
      throw std::runtime_error("GLFW OS replay window has no HWND");
    RECT rect{};
    POINT origin{};
    DWORD pid = 0u;
    if (!GetClientRect(hwnd, &rect) || !ClientToScreen(hwnd, &origin) ||
        GetWindowThreadProcessId(hwnd, &pid) == 0u ||
        pid != GetCurrentProcessId())
      throw std::runtime_error("could not bind OS replay HWND/client/PID");
    const std::int32_t width = rect.right - rect.left;
    const std::int32_t height = rect.bottom - rect.top;
    const UINT dpi = GetDpiForWindow(hwnd);
    if (width <= 0 || height <= 0 || dpi == 0u ||
        static_cast<std::uint32_t>(width) != options.width ||
        static_cast<std::uint32_t>(height) != options.height)
      throw std::runtime_error("OS replay client extent/DPI is invalid");
    return ClientProbe{
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd)),
        static_cast<std::uint32_t>(pid), static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height), static_cast<std::uint32_t>(dpi),
        {origin.x, origin.y, origin.x + width, origin.y + height}};
#else
    throw std::runtime_error("OS replay ClientProbe requires Win32");
#endif
  }

  [[nodiscard]] bool clientFocused() const noexcept {
#ifdef _WIN32
    const HWND hwnd = glfwGetWin32Window(window);
    return hwnd != nullptr && GetForegroundWindow() == hwnd &&
           glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
#else
    return false;
#endif
  }

  [[nodiscard]] std::filesystem::path
  readyPath(const ReadyProbe &ready) const {
    return options.syncDirectory /
           (readyStem(options.runNonce, ready.step, ready.phaseId) +
            ".ready.json");
  }

  [[nodiscard]] std::filesystem::path
  continuePath(const ReadyProbe &ready) const {
    return options.syncDirectory /
           (readyStem(options.runNonce, ready.step, ready.phaseId) +
            ".continue.json");
  }
};

EditorUiOsReplayHost::EditorUiOsReplayHost(GLFWwindow *window,
                                           CliOptions options,
                                           std::function<std::size_t()> pendingNativeTerminals,
                                           std::function<bool()> nativeFocusInterrupted)
    : m_impl(std::make_unique<Impl>(window, std::move(options),
                                    std::move(pendingNativeTerminals),
                                    std::move(nativeFocusInterrupted))) {}

EditorUiOsReplayHost::~EditorUiOsReplayHost() = default;
EditorUiOsReplayHost::EditorUiOsReplayHost(EditorUiOsReplayHost &&) noexcept =
    default;
EditorUiOsReplayHost &EditorUiOsReplayHost::operator=(
    EditorUiOsReplayHost &&) noexcept = default;

std::optional<RuntimeHostSnapshot>
EditorUiOsReplayHost::snapshot(AuthorityProbe authority,
                               bool rendererSettled) const {
  try {
    RuntimeHostSnapshot result;
    result.client = m_impl->clientProbe();
    result.authority = std::move(authority);
    result.rendererSettled = rendererSettled;
    result.clientFocused = m_impl->clientFocused();
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

ReadyPublication EditorUiOsReplayHost::publishReady(const ReadyProbe &ready) {
  try {
    if (ready.renderedFrame != ready.frame.frameSerial ||
        ready.runNonce != m_impl->options.runNonce ||
        ready.planSha256 != m_impl->options.planSha256 ||
        ready.executableSha256 != m_impl->options.executableSha256 ||
        ready.sourceSha256 != m_impl->options.sourceSha256)
      throw std::runtime_error("ready identity/frame does not match host");
    const std::string payload = ready.toJson() + "\n";
    const std::string hash = sha256(payload);
    const std::filesystem::path path = m_impl->readyPath(ready);
    writeAtomic(path, payload);
    const StepSpec &step = m_impl->plan.steps.at(ready.step);
    if (!step.phases.empty() && step.phases.back().id == ready.phaseId)
      m_impl->terminalReady.push_back(ready);
    return ReadyPublication{true, hash, {}};
  } catch (const std::exception &error) {
    return ReadyPublication{false, {}, error.what()};
  }
}

ContinueReceipt
EditorUiOsReplayHost::waitForContinue(const ReadyProbe &ready) {
  const std::filesystem::path expected = m_impl->continuePath(ready);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(
                            m_impl->options.timeoutMilliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    glfwPollEvents();
    if (glfwWindowShouldClose(m_impl->window) == GLFW_TRUE)
      throw std::runtime_error("OS replay window closed while awaiting continue");
    std::error_code error;
    for (std::filesystem::directory_iterator current(
             m_impl->options.syncDirectory, error),
         end;
         !error && current != end; current.increment(error)) {
      const std::string name = current->path().filename().generic_string();
      const bool currentNonce =
          name.starts_with(m_impl->options.runNonce + ".");
      const bool continueFile = name.size() >= 14u &&
                                name.ends_with(".continue.json");
      const bool alreadyAccepted =
          std::find(m_impl->acceptedContinue.begin(),
                    m_impl->acceptedContinue.end(), current->path()) !=
          m_impl->acceptedContinue.end();
      if (currentNonce && continueFile && current->path() != expected &&
          !alreadyAccepted)
        throw std::runtime_error("unexpected OS replay continue artifact");
    }
    if (error)
      throw std::runtime_error("could not enumerate OS replay sync directory");
    error.clear();
    if (std::filesystem::exists(expected, error) && !error) {
      if (!ordinaryFile(expected))
        throw std::runtime_error("OS replay continue is not an ordinary file");
      const std::string bytes = readFile(expected, 64u * 1024u);
      ContinueReceipt receipt = ContinueParser(bytes).parse();
      const auto &phases = m_impl->plan.steps.at(ready.step).phases;
      const auto phase = std::find_if(phases.begin(), phases.end(),
          [&](const PhaseSpec &value) { return value.id == ready.phaseId; });
      if (phase == phases.end())
        throw std::runtime_error("OS replay ready phase is not registered");
      if (phase->input.kind == InputKind::escapeReleaseBatch) {
        if (!m_impl->pendingNativeTerminals)
          throw std::runtime_error("same-frame replay has no native callback queue");
        const auto count = m_impl->pendingNativeTerminals();
        if (count > 2u)
          throw std::runtime_error("same-frame replay received excess native callbacks");
        if (count < 2u) {
          // Keep pumping the real window, without opening another app frame.
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
      }
      if (phase->input.kind == InputKind::leftUpFocusLoss) {
        if (!m_impl->pendingNativeTerminals || !m_impl->nativeFocusInterrupted)
          throw std::runtime_error("focus-loss batch has no native callback queue");
        const auto count = m_impl->pendingNativeTerminals();
        if (count > 2u || count == 0u || !m_impl->nativeFocusInterrupted()) {
          if (count > 2u)
            throw std::runtime_error("focus-loss batch received excess native callbacks");
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
      }
      if (phase->input.kind == InputKind::focusLoss) {
        if (!m_impl->nativeFocusInterrupted)
          throw std::runtime_error("focus-loss phase has no native focus observer");
        if (!m_impl->nativeFocusInterrupted()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
      }
      if (m_impl->pendingContinue || m_impl->pendingReceipt)
        throw std::runtime_error("OS replay continue was already pending");
      m_impl->pendingContinue = expected;
      m_impl->pendingReceipt = receipt;
      return receipt;
    }
    if (error)
      throw std::runtime_error("could not inspect OS replay continue path");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  throw std::runtime_error("timed out waiting for OS replay continue receipt");
}

void EditorUiOsReplayHost::confirmAcceptedContinue(
    const ContinueReceipt &receipt) {
  if (!m_impl->pendingContinue || !m_impl->pendingReceipt ||
      m_impl->pendingReceipt->schemaVersion != receipt.schemaVersion ||
      m_impl->pendingReceipt->protocol != receipt.protocol ||
      m_impl->pendingReceipt->runNonce != receipt.runNonce ||
      m_impl->pendingReceipt->caseId != receipt.caseId ||
      m_impl->pendingReceipt->planSha256 != receipt.planSha256 ||
      m_impl->pendingReceipt->step != receipt.step ||
      m_impl->pendingReceipt->stepId != receipt.stepId ||
      m_impl->pendingReceipt->phaseId != receipt.phaseId ||
      m_impl->pendingReceipt->readySha256 != receipt.readySha256 ||
      m_impl->pendingReceipt->inputKind != receipt.inputKind ||
      m_impl->pendingReceipt->sendInputCount != receipt.sendInputCount)
    throw std::runtime_error("accepted OS replay receipt differs from pending");
  m_impl->acceptedContinue.push_back(*m_impl->pendingContinue);
  m_impl->pendingContinue.reset();
  m_impl->pendingReceipt.reset();
}

void EditorUiOsReplayHost::writeResult(bool gpuIdleWaited) {
  std::size_t expectedPhases = 0u;
  for (std::uint32_t index = m_impl->options.startStep;
       index <= m_impl->options.stopStep; ++index)
    expectedPhases += m_impl->plan.steps[index].phases.size();
  const std::size_t expectedSteps =
      m_impl->options.stopStep - m_impl->options.startStep + 1u;
  if (!gpuIdleWaited || m_impl->terminalReady.size() != expectedSteps ||
      m_impl->acceptedContinue.size() != expectedPhases)
    throw std::runtime_error("OS replay result cardinality is incomplete");

  std::string output =
      "{\"documentKind\":\"mgif.editor-ui-os-replay-result\","
      "\"schemaVersion\":1,\"protocol\":";
  appendJsonString(output, kProtocol);
  output += ",\"runNonce\":";
  appendJsonString(output, m_impl->options.runNonce);
  output += ",\"case\":";
  appendJsonString(output, m_impl->options.caseId);
  output += ",\"subcase\":";
  appendJsonString(output, m_impl->options.subcase);
  output += ",\"planSha256\":";
  appendJsonString(output, m_impl->options.planSha256);
  output += ",\"identity\":{\"executableSha256\":";
  appendJsonString(output, m_impl->options.executableSha256);
  output += ",\"sourceSha256\":";
  appendJsonString(output, m_impl->options.sourceSha256);
  output += "},\"dispatchMode\":\"win32-send-input\","
            "\"complete\":true,\"passed\":true,\"gpuIdleWaited\":true,"
            "\"startStep\":" +
            std::to_string(m_impl->options.startStep) + ",\"stopStep\":" +
            std::to_string(m_impl->options.stopStep) + ",\"stepCount\":" +
            std::to_string(expectedSteps) + ",\"phaseCount\":" +
            std::to_string(expectedPhases) +
            ",\"validatedReceiptCount\":" +
            std::to_string(m_impl->acceptedContinue.size()) +
            ",\"steps\":[";
  for (std::size_t index = 0u; index < m_impl->terminalReady.size(); ++index) {
    if (index != 0u)
      output.push_back(',');
    const ReadyProbe &ready = m_impl->terminalReady[index];
    output += "{\"index\":" + std::to_string(ready.step) + ",\"id\":";
    appendJsonString(output, ready.stepId);
    output += ",\"observedDispatches\":[";
    for (std::size_t dispatch = 0u;
         dispatch < ready.observedDispatches.size(); ++dispatch) {
      if (dispatch != 0u)
        output.push_back(',');
      const ObservedDispatch &value = ready.observedDispatches[dispatch];
      output += "{\"sequence\":" + std::to_string(value.sequence) +
                ",\"source\":";
      appendJsonString(output, commandSourceName(value.source));
      output += ",\"payloadKind\":";
      appendJsonString(output, dispatchKindName(value.kind));
      output += ",\"operation\":";
      appendJsonString(output, value.operation);
      output += ",\"targetRole\":";
      appendJsonString(output, value.targetRole);
      output += ",\"secondaryRole\":";
      appendJsonString(output, value.secondaryRole);
      output += ",\"text\":";
      appendJsonString(output, value.text);
      output += ",\"accepted\":";
      output += value.accepted ? "true" : "false";
      output += ",\"refresh\":";
      appendJsonString(output, refreshName(value.refresh));
      output += ",\"error\":";
      appendJsonString(output, value.error);
      if (m_impl->options.caseId != kU02Case) {
        output += ",\"propertyPhase\":";
        appendJsonString(output, value.propertyPhase);
        output += ",\"propertyValue\":";
        appendJsonString(output, value.propertyValue);
      }
      output += "}";
    }
    output += "]";
    if (ready.gizmo) output += ",\"gizmo\":" + ready.gizmo->toJson();
    output += "}";
  }
  output += "]}\n";
  writeAtomic(m_impl->options.evidenceDirectory / "result.json", output);
}

const CliOptions &EditorUiOsReplayHost::options() const noexcept {
  return m_impl->options;
}

const std::filesystem::path &
EditorUiOsReplayHost::evidenceDirectory() const noexcept {
  return m_impl->options.evidenceDirectory;
}

std::size_t EditorUiOsReplayHost::validatedReceiptCount() const noexcept {
  return m_impl->acceptedContinue.size();
}

} // namespace demo::editor::testing::osreplay
