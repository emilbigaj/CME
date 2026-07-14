project(Schema)

# --- A. The Library (SchemaLib) ---
# The generated CME SBE structs (produced by regen.sh/gen_sbe.py):
#   ILink3Sbe.hpp — iLink 3 order entry, namespace ILink3
#   Mdp3Sbe.hpp   — MDP 3.0 market data, namespace Mdp3
# STATIC with Empty.cpp so the headers are compiled on every build — all
# static_assert(sizeof == BlockLength) layout checks run here.
add_library(SchemaLib STATIC
	"ILink3Sbe.hpp"
	"Mdp3Sbe.hpp"
	"Empty.cpp"
)

# StringN / Json (glaze) / magic_enum all come via ToolsLib.
target_link_libraries(SchemaLib PUBLIC ToolsLib)

# Anyone linking SchemaLib can #include "ILink3Sbe.hpp".
target_include_directories(SchemaLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- B. The Test App (SchemaTests) ---
add_executable(SchemaTests "Main.cpp")
target_link_libraries(SchemaTests PRIVATE SchemaLib)
