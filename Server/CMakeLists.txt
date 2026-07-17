project(Server)

# --- The Library (ServerLib) ---
# STATIC with Empty.cpp (not INTERFACE) so CMake generates compile commands for the headers.
add_library(ServerLib STATIC
	"Empty.cpp"
	"CmeServer.hpp"
)

# ILink3Lib brings the gateway/router/logger and (through it) the HFT Provider seam; SecDefLib
# brings the instrument catalog.
target_link_libraries(ServerLib PUBLIC ILink3Lib SecDefLib)

# Anyone linking ServerLib can include headers from this folder.
target_include_directories(ServerLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Test executables live in the top-level Tests/ folder, not here.
