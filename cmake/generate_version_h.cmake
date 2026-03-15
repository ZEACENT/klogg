# Get the current working branch
execute_process(
  COMMAND git rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
message("Git branch: ${GIT_BRANCH}")

# Get the latest abbreviated commit hash of the working branch
execute_process(
  COMMAND git log -1 --format=%h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_COMMIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
message("Git commit: ${GIT_COMMIT_HASH}")

# Get the latest abbreviated commit hash of the working branch
execute_process(
  COMMAND git describe --always
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_DESCRIBE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

message("Git describe: ${GIT_DESCRIBE}")

string(TIMESTAMP BUILD_DATE "%Y-%m-%d" UTC)
string(TIMESTAMP BUILD_YEAR "%Y" UTC)

string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef VERSION_HEADER_SUFFIX)
set(VERSION_HEADER_TMP "generated/version.${VERSION_HEADER_SUFFIX}.tmp")

file(WRITE ${VERSION_HEADER_TMP} "#ifndef GENERATED_KLOGG_VERSION_H\n")
file(APPEND ${VERSION_HEADER_TMP} "#define GENERATED_KLOGG_VERSION_H\n\n")

file(APPEND ${VERSION_HEADER_TMP} "#define KLOGG_DATE \"${BUILD_DATE}\"\n\n")
file(APPEND ${VERSION_HEADER_TMP} "#define KLOGG_YEAR \"${BUILD_YEAR}\"\n\n")
file(APPEND ${VERSION_HEADER_TMP} "#define KLOGG_GIT_VERSION \"${GIT_DESCRIBE}\"\n\n")
file(APPEND ${VERSION_HEADER_TMP} "#define KLOGG_COMMIT \"${GIT_COMMIT_HASH}\"\n\n")
file(APPEND ${VERSION_HEADER_TMP} "#define KLOGG_VERSION \"${BUILD_VERSION}\"\n\n")

file(APPEND ${VERSION_HEADER_TMP} "#endif // GENERATED_KLOGG_VERSION_H\n")
file(RENAME ${VERSION_HEADER_TMP} generated/version.h)
