include(ExternalProject)
set(CARES_TAR_URL https://github.com/c-ares/c-ares/releases/download/cares-1_15_0/c-ares-1.15.0.tar.gz)
set(CARES_URL_HASH d2391da274653f7643270623e822dff7)
set(CARES_SRC ${THIRD_PARTY_DIR}/cares)

set(CARES_INCLUDE_DIR ${THIRD_PARTY_DIR}/cares/include)
set(CARES_LIBRARY_DIR ${THIRD_PARTY_DIR}/cares/lib)

ExternalProject_Add(cares
  PREFIX ${CARES_SRC}
  URL ${CARES_TAR_URL}
  URL_HASH MD5=${CARES_URL_HASH}
  UPDATE_COMMAND ""
  CMAKE_CACHE_ARGS
    -DCARES_STATIC:BOOL=ON 
    -DCARES_SHARED:BOOL=OFF
    -DCARES_STATIC_PIC:BOOL=ON
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DCMAKE_INSTALL_PREFIX:STRING=${CARES_SRC}
)
