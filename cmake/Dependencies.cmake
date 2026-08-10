include(FetchContent)

# CMake 4 dropped compatibility with projects declaring
# cmake_minimum_required(VERSION <3.5). nlohmann/json 3.11.3 predates that, so
# give the sub-builds a policy floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# --- SQLiteCpp 3.3.2 (bundles sqlite3) -------------------------------------
set(SQLITECPP_INTERNAL_SQLITE ON  CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPLINT     OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK    OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SQLiteCpp
  GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
  GIT_TAG        3.3.2
  GIT_SHALLOW    ON
)

# --- nlohmann/json v3.12.0 --------------------------------------------------
# Version, URL and hash deliberately match what sherpa-onnx expects. FetchContent
# de-duplicates by declaration name, and sherpa's cmake/json.cmake does its own
# unguarded add_subdirectory() on this one -- so when voice is enabled we declare
# it here and let sherpa add it, and only add it ourselves when it otherwise
# would not exist at all.
FetchContent_Declare(json
  URL      https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
  URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
)

# --- cpp-httplib v0.18.3 (plain HTTP to the local model; header-only, no TLS) -
set(HTTPLIB_COMPILE                  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL          OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB             OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_BROTLI           OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.3
  GIT_SHALLOW    ON
)

if(TT_ENABLE_VOICE)
  FetchContent_MakeAvailable(SQLiteCpp httplib)   # json comes in via sherpa-onnx
else()
  FetchContent_MakeAvailable(SQLiteCpp json httplib)
endif()

# ---------------------------------------------------------------------------
# Voice pipeline. Gated because these two are by far the slowest part of the
# build, and nothing but the `voice` command needs them. miniaudio is not here:
# it is a single vendored header in third_party/, with no build of its own.
# ---------------------------------------------------------------------------
if(TT_ENABLE_VOICE)
  # --- whisper.cpp v1.9.2 (speech to text; Metal-accelerated on Apple) ------
  set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        v1.9.2
    GIT_SHALLOW    ON
  )

  # --- sherpa-onnx v1.13.4 (Kokoro TTS runtime; C API only) ----------------
  set(SHERPA_ONNX_ENABLE_C_API          ON  CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_TTS            ON  CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_PYTHON         OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_TESTS          OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_CHECK          OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_PORTAUDIO      OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_JNI            OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_WEBSOCKET      OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_BINARY         OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_BUILD_C_API_EXAMPLES  OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(sherpa_onnx
    GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
    GIT_TAG        v1.13.4
    GIT_SHALLOW    ON
  )

  FetchContent_MakeAvailable(whisper sherpa_onnx)
endif()
