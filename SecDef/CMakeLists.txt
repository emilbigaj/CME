project(SecDef)

# --- The Library (SecDefLib) ---
# STATIC with Empty.cpp (not INTERFACE) so CMake generates compile commands for the headers.
add_library(SecDefLib STATIC
	"Empty.cpp"
	"SecDefFile.hpp"
)

# DataLib brings the HFT instrument headers (Data::FutureHeader / SpreadHeader) we fill in;
# ToolsLib brings StringN and Timestamp.
target_link_libraries(SecDefLib PUBLIC DataLib ToolsLib)

# Anyone linking SecDefLib can include headers from this folder.
target_include_directories(SecDefLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Test executables live in the top-level Tests/ folder, not here.
