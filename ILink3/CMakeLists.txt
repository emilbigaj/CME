project(ILink3)

# --- A. The Library (ILink3Lib) ---
# STATIC with Empty.cpp (not INTERFACE) so CMake generates compile commands
# for the headers — same IntelliSense trick as the HFT lib.
add_library(ILink3Lib STATIC
	"Empty.cpp"
	"ILink3Config.hpp"
	"Hmac.hpp"
	"Wire.hpp"
	"TcpConnection.hpp"
	"CmeLogger.hpp"
	"MarketSegmentGateway.hpp"
)

# SchemaLib carries the generated SBE structs; the rest is the HFT stack for the
# Provider Server seam iLink3 plugs into.
target_link_libraries(ILink3Lib PUBLIC SchemaLib ToolsLib SocketLib DataLib ExecutionLib ProviderLib)

# Anyone linking ILink3Lib can include headers from this folder.
target_include_directories(ILink3Lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# HMAC-SHA256 for FIXP Negotiate/Establish signatures (cold path only).
find_package(OpenSSL REQUIRED)
target_link_libraries(ILink3Lib PUBLIC OpenSSL::Crypto)

# --- B. The smoke-test app ---
add_executable(ILink3Tests "Main.cpp")
target_link_libraries(ILink3Tests PRIVATE ILink3Lib)

# Test executables live in the top-level Tests/ folder, not here.