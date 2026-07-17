cmake_minimum_required (VERSION 3.25)
project ("CME")

# Force C++23 globally for all CME targets (HFT subproject pins itself to C++20)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -Wshadow and -Wconversion are lifesavers for math/logic bugs
add_compile_options(
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
	-fno-strict-aliasing
	-fPIC
	-Wno-missing-braces
	-Wodr
	$<$<CONFIG:Release>:-O3>
	$<$<CONFIG:Release>:-march=native>
	$<$<CONFIG:Debug>:-g>
)

# Pull in the HFT library (ToolsLib, SocketLib, DataLib, ExecutionLib, ProviderLib, StrategyLib).
# Lives side-by-side with this repo: ~/cpp/HFT next to ~/cpp/CME.
add_subdirectory ("${CMAKE_CURRENT_SOURCE_DIR}/../HFT" "${CMAKE_BINARY_DIR}/HFT")

# CME connectivity built on top of HFT. iLink3 (order entry) first, MDP3 (market data) next.
# Schema holds the generated SBE structs (SchemaLib); protocol libs link it.
add_subdirectory ("Schema")
add_subdirectory ("ILink3")

# SecDef loads the CME security-definition file into the HFT instrument headers.
add_subdirectory ("SecDef")

# Server composes iLink3 + SecDef into the HFT trading-server seam strategies connect to.
add_subdirectory ("Server")

# All test executables (each links the libraries it exercises). Added last so every target above
# is in scope.
add_subdirectory ("Tests")