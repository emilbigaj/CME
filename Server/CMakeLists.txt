project(Server)

# --- A. The Library (ServerLib) ---
# STATIC with Empty.cpp (not INTERFACE) so CMake generates compile commands for the headers.
add_library(ServerLib STATIC
	"Empty.cpp"
	"CmeServer.hpp"
	"CmeServerConfig.hpp"
	"MarketSegments.hpp"
)

# ILink3Lib brings the gateway/router/logger and (through it) the HFT Provider seam; SecDefLib
# brings the instrument catalog; Mdp3Lib brings the market-data receivers and book builder.
target_link_libraries(ServerLib PUBLIC ILink3Lib SecDefLib Mdp3Lib)

# Anyone linking ServerLib can include headers from this folder.
target_include_directories(ServerLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- B. The server process ---
# Links the kernel-bypass libraries (order entry and market data) because the process
# compiles every transport flavor and picks from the settings at start-up.
add_executable(CmeServer "Main.cpp")
target_link_libraries(CmeServer PRIVATE ServerLib onload_zf ciul1)

# Test executables live in the top-level Tests/ folder, not here.
