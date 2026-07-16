// This library is header-only, but the build tool needs at least one source file per library
// to produce a compile step (which also gives the editor's code intelligence a valid context
// for the headers). This file includes the library's headers so they are actually compiled
// on every build. The generated wire structs are compiled the same way by the Schema library.
#include "ILink3Config.hpp"
#include "Hmac.hpp"
#include "Wire.hpp"
#include "TcpConnection.hpp"
#include "CmeLogger.hpp"
