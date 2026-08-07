#include "D3D12Pipelines.h"

#include "json.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>

#ifndef DEMO_SLANGC_PATH
#error DEMO_SLANGC_PATH must be provided by CMake
#endif
#ifndef DEMO_DXC_PATH
#error DEMO_DXC_PATH must be provided by CMake
#endif
#ifndef DEMO_SHADER_SOURCE_DIR
#error DEMO_SHADER_SOURCE_DIR must be provided by CMake
#endif
#ifndef DEMO_SHADER_AUTOGEN_DIR
#error DEMO_SHADER_AUTOGEN_DIR must be provided by CMake
#endif

namespace demo::rhi::d3d12 {
namespace {

using Json = nlohmann::json;

std::vector<uint8_t> readBytes(const std::filesystem::path& path)
{
  std::ifstream stream(path, std::ios::binary);
  if(!stream)
    throw std::runtime_error("Could not open generated shader file: " + path.string());
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  stream.seekg(0, std::ios::beg);
  if(size < 0)
    throw std::runtime_error("Could not query generated shader size: " + path.string());
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if(!bytes.empty())
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
  if(!stream)
    throw std::runtime_error("Could not read generated shader file: " + path.string());
  return bytes;
}

std::string readText(const std::filesystem::path& path)
{
  std::ifstream stream(path, std::ios::binary);
  if(!stream)
    return {};
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

std::filesystem::path locateShaderSource(const ShaderEntry& stage, std::span<const uint8_t> shaderBytes)
{
  if(shaderBytes.empty())
    throw std::runtime_error("DX12 pipeline stage is missing SPIR-V identity bytes");

  const auto* expected = shaderBytes.data();
  const std::filesystem::path autogen(DEMO_SHADER_AUTOGEN_DIR);
  for(const std::filesystem::directory_entry& entry :
      std::filesystem::directory_iterator(autogen))
  {
    if(!entry.is_regular_file() || entry.path().extension() != ".spv")
      continue;
    if(entry.file_size() != shaderBytes.size())
      continue;

    const std::vector<uint8_t> candidate = readBytes(entry.path());
    if(std::memcmp(candidate.data(), expected, shaderBytes.size()) != 0)
      continue;

    std::string sourceName = entry.path().filename().string();
    sourceName.resize(sourceName.size() - std::string(".spv").size());
    if(sourceName == "shader.light.final.dxc")
      sourceName = "shader.light_gpu_driven.slang";
    const std::filesystem::path source =
      std::filesystem::path(DEMO_SHADER_SOURCE_DIR) / sourceName;
    if(!std::filesystem::exists(source))
      throw std::runtime_error("Matched SPIR-V but not its Slang source: " + source.string());
    return source;
  }

  throw std::runtime_error(
    "DX12 could not map the pipeline SPIR-V bytes back to a Slang source file");
}

uint64_t appendHash(uint64_t hash, const void* data, size_t size)
{
  constexpr uint64_t prime = 1099511628211ull;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for(size_t index = 0; index < size; ++index)
  {
    hash ^= bytes[index];
    hash *= prime;
  }
  return hash;
}

uint64_t shaderCacheKey(const ShaderEntry& stage, std::span<const uint8_t> shaderBytes)
{
  uint64_t hash = 1469598103934665603ull;
  hash = appendHash(hash, shaderBytes.data(), shaderBytes.size());
  const std::string_view entry = stage.entryPoint.empty() ? std::string_view{"main"} : stage.entryPoint;
  hash = appendHash(hash, entry.data(), entry.size());
  hash = appendHash(hash, &stage.stage, sizeof(stage.stage));
  hash = appendHash(hash, &stage.specializationVariant,
                    sizeof(stage.specializationVariant));
  if(!stage.specializationData.bytes.empty())
    hash = appendHash(hash, stage.specializationData.bytes.data(),
                      stage.specializationData.bytes.size());
  return hash;
}

const char* slangStageName(ShaderStage stage)
{
  const auto hasStage = [stage](ShaderStage bit) {
    return (static_cast<uint32_t>(stage) & static_cast<uint32_t>(bit)) != 0;
  };
  if(hasStage(ShaderStage::vertex))
    return "vertex";
  if(hasStage(ShaderStage::fragment))
    return "fragment";
  if(hasStage(ShaderStage::compute))
    return "compute";
  throw std::runtime_error("DX12 received an unsupported Slang shader stage");
}

std::string quote(const std::filesystem::path& path)
{
  return '"' + path.string() + '"';
}

std::string specializationArguments(const ShaderEntry& stage)
{
  std::ostringstream result;
  const auto* bytes = reinterpret_cast<const uint8_t*>(stage.specializationData.bytes.data());
  for(uint32_t index = 0; index < static_cast<uint32_t>(stage.specializationConstants.size()); ++index)
  {
    const SpecializationConstant& constant = stage.specializationConstants[index];
    if(bytes == nullptr || constant.size == 0 ||
       constant.offset + constant.size > stage.specializationData.bytes.size() ||
       constant.size > sizeof(uint64_t))
    {
      continue;
    }

    uint64_t value = 0;
    std::memcpy(&value, bytes + constant.offset, constant.size);
    result << " -DDEMO_SPECIALIZATION_" << constant.constantId << '=' << value;
  }
  return result.str();
}

void runSlang(const std::string& command, const std::filesystem::path& logPath)
{
  const std::string redirected =
    "cmd.exe /D /S /C \"" + command + " > " + quote(logPath) + " 2>&1\"";
  const int result = std::system(redirected.c_str());
  if(result == 0)
    return;

  std::string log = readText(logPath);
  if(log.empty())
    log = "slangc returned " + std::to_string(result);
  throw std::runtime_error("DXIL compilation failed:\n" + log + "\nCommand: " + command);
}


void runDxc(const std::string& command, const std::filesystem::path& logPath)
{
  const std::string redirected =
    "cmd.exe /D /S /C \"" + command + " > " + quote(logPath) + " 2>&1\"";
  const int result = std::system(redirected.c_str());
  if(result == 0)
    return;

  std::string log = readText(logPath);
  if(log.empty())
    log = "dxc returned " + std::to_string(result);
  throw std::runtime_error("DXC DXIL compilation failed:\n" + log + "\nCommand: " + command);
}

Json loadJson(const std::filesystem::path& path)
{
  std::ifstream stream(path);
  if(!stream)
    throw std::runtime_error("Could not open Slang reflection: " + path.string());
  Json json;
  stream >> json;
  return json;
}

std::vector<Json> parameterBindings(const Json& parameter)
{
  if(parameter.contains("bindings"))
    return parameter.at("bindings").get<std::vector<Json>>();
  if(parameter.contains("binding"))
    return {parameter.at("binding")};
  return {};
}

const Json* findParameter(const Json& reflection, const std::string& name)
{
  if(reflection.contains("entryPoints"))
  {
    for(const Json& entryPoint : reflection.at("entryPoints"))
    {
      if(!entryPoint.contains("bindings"))
        continue;
      for(const Json& parameter : entryPoint.at("bindings"))
      {
        if(parameter.value("name", std::string{}) == name)
          return &parameter;
      }
    }
  }
  if(!reflection.contains("parameters"))
    return nullptr;
  for(const Json& parameter : reflection.at("parameters"))
  {
    if(parameter.value("name", std::string{}) == name)
      return &parameter;
  }
  return nullptr;
}

D3D12ShaderBindingKind toBindingKind(const std::string& kind)
{
  if(kind == "constantBuffer")
    return D3D12ShaderBindingKind::constantBuffer;
  if(kind == "shaderResource")
    return D3D12ShaderBindingKind::shaderResource;
  if(kind == "unorderedAccess")
    return D3D12ShaderBindingKind::unorderedAccess;
  if(kind == "samplerState")
    return D3D12ShaderBindingKind::sampler;
  throw std::runtime_error("Unsupported DXIL reflection binding kind: " + kind);
}

uint32_t reflectedCount(const Json& binding, bool& unbounded)
{
  unbounded = false;
  if(!binding.contains("count"))
    return 1;
  const Json& count = binding.at("count");
  if(count.is_string())
  {
    unbounded = count.get<std::string>() == "unbounded";
    return unbounded ? 0u : static_cast<uint32_t>(std::stoul(count.get<std::string>()));
  }
  return count.get<uint32_t>();
}

std::vector<D3D12ShaderBinding> correlateBindings(const Json& spirvReflection,
                                                   const Json& dxilReflection)
{
  std::vector<D3D12ShaderBinding> result;
  if(!spirvReflection.contains("parameters"))
    return result;

  for(const Json& spirvParameter : spirvReflection.at("parameters"))
  {
    const std::string name = spirvParameter.value("name", std::string{});
    if(name.empty())
      continue;
    const Json* dxilParameter = findParameter(dxilReflection, name);
    if(dxilParameter == nullptr)
      continue;

    const std::vector<Json> logicalBindings = parameterBindings(spirvParameter);
    if(logicalBindings.empty())
      continue;
    const Json& logical = logicalBindings.front();
    const std::string logicalKind = logical.value("kind", std::string{});
    const bool pushConstants = logicalKind == "pushConstantBuffer";
    if(!pushConstants && logicalKind != "descriptorTableSlot")
      continue;

    for(const Json& native : parameterBindings(*dxilParameter))
    {
      if(native.value("used", 1u) == 0u)
        continue;
      const std::string nativeKind = native.value("kind", std::string{});
      D3D12ShaderBinding binding{};
      binding.name = name;
      binding.logicalSet = logical.value("space", 0u);
      binding.logicalBinding = logical.value("index", 0u);
      binding.kind = pushConstants ? D3D12ShaderBindingKind::pushConstants
                                   : toBindingKind(nativeKind);
      binding.shaderRegister = native.value("index", 0u);
      binding.registerSpace = native.value("space", 0u);
      binding.descriptorCount = reflectedCount(native, binding.unbounded);
      result.push_back(std::move(binding));
    }
  }
  return result;
}

std::vector<D3D12VertexInput> extractVertexInputs(const Json& reflection)
{
  std::vector<D3D12VertexInput> result;
  if(!reflection.contains("entryPoints") || reflection.at("entryPoints").empty())
    return result;

  const Json& entryPoint = reflection.at("entryPoints").front();
  if(!entryPoint.contains("parameters"))
    return result;

  for(const Json& parameter : entryPoint.at("parameters"))
  {
    if(!parameter.contains("type"))
      continue;
    const Json& type = parameter.at("type");
    if(!type.contains("fields"))
      continue;
    for(const Json& field : type.at("fields"))
    {
      if(!field.contains("binding") ||
         field.at("binding").value("kind", std::string{}) != "varyingInput")
      {
        continue;
      }
      const std::string semantic = field.value("semanticName", std::string{});
      if(semantic.empty())
        continue;
      result.push_back(D3D12VertexInput{
        .location = field.at("binding").value("index", 0u),
        .semanticName = semantic,
        .semanticIndex = field.value("semanticIndex", 0u),
      });
    }
  }
  return result;
}

}  // namespace

D3D12CompiledShader compileShaderToDxil(const ShaderEntry& stage, std::span<const uint8_t> shaderBytes)
{
  static std::mutex compilerMutex;
  std::scoped_lock lock(compilerMutex);

  const std::filesystem::path source = locateShaderSource(stage, shaderBytes);
  const uint64_t key = shaderCacheKey(stage, shaderBytes);
  std::ostringstream keyText;
  keyText << std::hex << std::setw(16) << std::setfill('0') << key;

  const std::filesystem::path cacheDirectory =
    std::filesystem::path(DEMO_SHADER_AUTOGEN_DIR) / "_dxil_cache";
  std::filesystem::create_directories(cacheDirectory);
  const std::string stem = source.filename().string() + "." +
                           (stage.entryPoint.empty() ? std::string{"main"} : std::string{stage.entryPoint}) +
                           "." + keyText.str();
  const std::filesystem::path dxilPath = cacheDirectory / (stem + ".dxil");
  const std::filesystem::path dxReflectionPath =
    cacheDirectory / (stem + ".dx.json");
  const std::filesystem::path spvReflectionPath =
    cacheDirectory / (stem + ".spv.json");
  const std::filesystem::path reflectedSpvPath =
    cacheDirectory / (stem + ".reflect.spv");
  const std::filesystem::path reflectedDxilPath =
    cacheDirectory / (stem + ".reflect.dxil");
  const std::filesystem::path logPath = cacheDirectory / (stem + ".log");

  if(!std::filesystem::exists(dxilPath) ||
     !std::filesystem::exists(dxReflectionPath) ||
     !std::filesystem::exists(spvReflectionPath))
  {
    const std::string entry = stage.entryPoint.empty() ? std::string{"main"} : std::string{stage.entryPoint};
    const std::string common =
      quote(std::filesystem::path(DEMO_SLANGC_PATH)) + " " + quote(source) +
      " -lang slang -matrix-layout-column-major -O0 -g1 -entry " + entry +
      " -stage " + slangStageName(stage.stage) +
      " -I " + quote(std::filesystem::path(DEMO_SHADER_SOURCE_DIR).parent_path()) +
      " -DDEMO_D3D12=1" + specializationArguments(stage);

    const bool useGeneratedDxcHlsl =
      source.filename() == "shader.light_gpu_driven.slang" &&
      entry == "fragmentFinalColorMain";
    if(useGeneratedDxcHlsl)
    {
      const std::filesystem::path generatedHlsl =
        std::filesystem::path(DEMO_SHADER_AUTOGEN_DIR) / "shader.light.final.dxc.hlsl";
      if(!std::filesystem::exists(generatedHlsl))
        throw std::runtime_error("Generated DXC final-color HLSL is missing: " + generatedHlsl.string());
      runDxc(
        quote(std::filesystem::path(DEMO_DXC_PATH)) +
          " -T ps_6_6 -E fragmentFinalColorMain -Od -enable-16bit-types -HV 2021 -Fo " +
          quote(dxilPath) + " " + quote(generatedHlsl),
        logPath);
      runSlang(common + " -target dxil -profile sm_6_6 -reflection-json " +
                 quote(dxReflectionPath) + " -o " + quote(reflectedDxilPath),
               logPath);
    }
    else
    {
      runSlang(common + " -target dxil -profile sm_6_6 -reflection-json " +
                 quote(dxReflectionPath) + " -o " + quote(dxilPath),
               logPath);
    }
    runSlang(common +
               " -target spirv -profile spirv_1_6 -emit-spirv-directly"
               " -force-glsl-scalar-layout -fvk-use-entrypoint-name"
               " -reflection-json " + quote(spvReflectionPath) +
               " -o " + quote(reflectedSpvPath),
             logPath);
  }

  D3D12CompiledShader compiled{};
  compiled.bytecode = readBytes(dxilPath);
  const Json dxReflection = loadJson(dxReflectionPath);
  compiled.bindings = correlateBindings(loadJson(spvReflectionPath), dxReflection);
  compiled.vertexInputs = extractVertexInputs(dxReflection);
  compiled.sourcePath = source;
  return compiled;
}

}  // namespace demo::rhi::d3d12
