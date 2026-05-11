#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#endif

namespace {

bool isSupportedTexture(const std::filesystem::path& path)
{
  const std::string ext = path.extension().string();
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".PNG" || ext == ".JPG" || ext == ".JPEG";
}

std::string quote(const std::filesystem::path& path)
{
  return "\"" + path.string() + "\"";
}

std::string quote(const std::string& value)
{
  return "\"" + value + "\"";
}

std::wstring toLower(std::wstring value)
{
  for(wchar_t& ch : value)
  {
    if(ch >= L'A' && ch <= L'Z')
    {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  return value;
}

std::wstring selectCompressedFormat(const std::filesystem::path& inputPath)
{
  const std::wstring stem = toLower(inputPath.stem().wstring());

  if(stem.find(L"albedo") != std::wstring::npos
     || stem.find(L"basecolor") != std::wstring::npos
     || stem.find(L"base_color") != std::wstring::npos
     || stem.find(L"diffuse") != std::wstring::npos
     || stem.find(L"emissive") != std::wstring::npos)
  {
    return L"BC7_SRGB_BLOCK";
  }

  return L"BC7_UNORM_BLOCK";
}

int runCommand(const std::string& ktxExecutable, const std::vector<std::wstring>& arguments)
{
#ifdef _WIN32
  const std::wstring exe = std::filesystem::path(ktxExecutable).wstring();
  std::vector<const wchar_t*> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(exe.c_str());
  for(const std::wstring& argument : arguments)
  {
    argv.push_back(argument.c_str());
  }
  argv.push_back(nullptr);

  return _wspawnvp(_P_WAIT, exe.c_str(), argv.data());
#else
  std::string command = quote(ktxExecutable);
  for(const std::wstring& argument : arguments)
  {
    command += " " + quote(std::filesystem::path(argument).string());
  }
  return std::system(command.c_str());
#endif
}

int runKtxCreateCommand(const std::string& ktxExecutable,
                        const std::filesystem::path& inputPath,
                        const std::filesystem::path& outputPath)
{
  const std::wstring format = selectCompressedFormat(inputPath);
  return runCommand(ktxExecutable,
                    {
                        L"create",
                        L"--generate-mipmap",
                        L"--format",
                        format,
                        inputPath.wstring(),
                        outputPath.wstring(),
                    });
}

}  // namespace

int main(int argc, char** argv)
{
  if(argc < 3)
  {
    std::cerr << "Usage: texture_converter <input_dir> <output_dir> [ktx_path]\n";
    std::cerr << "This helper shells out to KTX-Software's recommended unified 'ktx' tool to produce BC7 KTX2 sidecars.\n";
    return 1;
  }

  const std::filesystem::path inputDir  = argv[1];
  const std::filesystem::path outputDir = argv[2];
  const std::string ktxExecutable       = argc >= 4 ? argv[3] : "ktx";

  if(!std::filesystem::exists(inputDir))
  {
    std::cerr << "Input directory does not exist: " << inputDir << "\n";
    return 1;
  }

  const int versionResult = runCommand(ktxExecutable, {L"--version"});
  if(versionResult != 0)
  {
    std::cerr << "Failed to launch KTX CLI '" << ktxExecutable << "' (exit code " << versionResult << ").\n";
#ifdef _WIN32
    if(versionResult == -1)
    {
      std::cerr << "spawn errno=" << errno << " (" << std::strerror(errno) << ")\n";
    }
#endif
    return versionResult == 0 ? 1 : versionResult;
  }

  std::filesystem::create_directories(outputDir);

  uint32_t convertedCount = 0;
  for(const auto& entry : std::filesystem::recursive_directory_iterator(inputDir))
  {
    if(!entry.is_regular_file() || !isSupportedTexture(entry.path()))
    {
      continue;
    }

    const std::filesystem::path relativePath = std::filesystem::relative(entry.path(), inputDir);
    std::filesystem::path outputPath = outputDir / relativePath;
    outputPath.replace_extension(".ktx2");
    std::filesystem::create_directories(outputPath.parent_path());

    // Use the unified `ktx create` front-end recommended by current KTX-Software docs.
    // Color textures use BC7 sRGB, while data textures use BC7 UNORM.
    std::cout << "Converting " << entry.path() << " -> " << outputPath << "\n";
    const int result = runKtxCreateCommand(ktxExecutable, entry.path(), outputPath);
    if(result != 0)
    {
      std::cerr << "Conversion failed for " << entry.path() << " (exit code " << result << ")\n";
#ifdef _WIN32
      if(result == -1)
      {
        std::cerr << "spawn errno=" << errno << " (" << std::strerror(errno) << ")\n";
      }
#endif
      return result;
    }

    ++convertedCount;
  }

  std::cout << "Converted " << convertedCount << " textures.\n";
  return 0;
}
