project(ILink3)

# --- A. The Library (ILink3Lib) ---
# STATIC with Empty.cpp (not INTERFACE) so CMake generates compile commands
# for the headers — same IntelliSense trick as the HFT lib.
add_library(ILink3Lib STATIC
	"Empty.cpp"
)

# iLink3 plugs into the Provider Server seam, so it sees the whole HFT stack.
target_link_libraries(ILink3Lib PUBLIC ToolsLib SocketLib DataLib ExecutionLib ProviderLib)

# Anyone linking ILink3Lib can include headers from this folder.
target_include_directories(ILink3Lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# HMAC-SHA256 for FIXP Negotiate/Establish signatures (cold path only).
find_package(OpenSSL REQUIRED)
target_link_libraries(ILink3Lib PUBLIC OpenSSL::Crypto)

# --- B. The Test App (ILink3Tests) ---
add_executable(ILink3Tests "Main.cpp")
target_link_libraries(ILink3Tests PRIVATE ILink3Lib)