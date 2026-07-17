project(MDP3)

# --- The Library (Mdp3Lib) ---
# STATIC with Empty.cpp (not INTERFACE) so CMake generates compile commands for the headers.
add_library(Mdp3Lib STATIC
	"Empty.cpp"
	"ChannelConfig.hpp"
	"PacketWalker.hpp"
	"UdpReceiver.hpp"
)

# SchemaLib brings the generated market-data structs (packet header, messages, groups).
target_link_libraries(Mdp3Lib PUBLIC SchemaLib ToolsLib)

# Anyone linking Mdp3Lib can include headers from this folder.
target_include_directories(Mdp3Lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Test executables live in the top-level Tests/ folder, not here.
