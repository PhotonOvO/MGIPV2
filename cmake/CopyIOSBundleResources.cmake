if(NOT DEFINED SOURCE_RESOURCES_DIR OR
   NOT IS_DIRECTORY "${SOURCE_RESOURCES_DIR}/GLTF_Sponza")
  message(FATAL_ERROR "The iOS Sponza resource directory is missing")
endif()
if(DEFINED BUNDLE_DIR AND DEFINED ENV{EFFECTIVE_PLATFORM_NAME})
  string(REPLACE "\${EFFECTIVE_PLATFORM_NAME}" "$ENV{EFFECTIVE_PLATFORM_NAME}"
    BUNDLE_DIR "${BUNDLE_DIR}")
endif()
if(NOT DEFINED BUNDLE_DIR OR NOT IS_DIRECTORY "${BUNDLE_DIR}")
  message(FATAL_ERROR "The iOS application bundle does not exist: ${BUNDLE_DIR}")
endif()

set(IOS_RESOURCES_DIR "${BUNDLE_DIR}/assets/resources")

# Remove output from the previous Xcode Resources phase. A top-level lowercase
# resources directory prevents iOS from recognizing the bundle on the default
# case-insensitive filesystem.
if(IS_DIRECTORY "${BUNDLE_DIR}/resources/GLTF_Sponza")
  file(REMOVE_RECURSE "${BUNDLE_DIR}/resources")
endif()

file(REMOVE_RECURSE
  "${IOS_RESOURCES_DIR}/GLTF_Sponza"
  "${IOS_RESOURCES_DIR}/Sponza"
  "${IOS_RESOURCES_DIR}/environment")
file(MAKE_DIRECTORY
  "${IOS_RESOURCES_DIR}/Sponza"
  "${IOS_RESOURCES_DIR}/environment")

file(COPY "${SOURCE_RESOURCES_DIR}/GLTF_Sponza"
  DESTINATION "${IOS_RESOURCES_DIR}"
  PATTERN "Cache" EXCLUDE)
file(COPY
  "${SOURCE_RESOURCES_DIR}/Sponza/7815564343179553343.jpg"
  "${SOURCE_RESOURCES_DIR}/Sponza/466164707995436622.jpg"
  DESTINATION "${IOS_RESOURCES_DIR}/Sponza")
file(COPY "${SOURCE_RESOURCES_DIR}/environment/lilienstein_4k.ktx2"
  DESTINATION "${IOS_RESOURCES_DIR}/environment")
