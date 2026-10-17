#include "pbo/version.h"

#ifndef PBO_VERSION
#define PBO_VERSION "0.0.0"
#endif

namespace pbo {

const char* version() { return PBO_VERSION; }

}  // namespace pbo
