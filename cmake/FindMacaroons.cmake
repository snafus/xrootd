
FIND_PATH(MACAROONS_INCLUDES macaroons.h
  HINTS
  ${MACAROONS_DIR}
  $ENV{MACAROONS_DIR}
  /usr
  PATH_SUFFIXES include
)

FIND_LIBRARY(MACAROONS_LIB macaroons
  HINTS
  ${MACAROONS_DIR}
  $ENV{MACAROONS_DIR}
  /usr
  PATH_SUFFIXES lib
  PATH_SUFFIXES .libs
)

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Macaroons DEFAULT_MSG MACAROONS_INCLUDES MACAROONS_LIB)

