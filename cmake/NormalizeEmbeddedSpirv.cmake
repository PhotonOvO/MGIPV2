if(NOT DEFINED INPUT_FILE)
  message(FATAL_ERROR "NormalizeEmbeddedSpirv.cmake requires INPUT_FILE")
endif()

file(READ "${INPUT_FILE}" _embedded_spirv)
string(REPLACE "const uint32_t " "inline constexpr uint32_t "
  _embedded_spirv "${_embedded_spirv}")
file(WRITE "${INPUT_FILE}"
  "#pragma once\n\n#include <cstdint>\n\n${_embedded_spirv}")
