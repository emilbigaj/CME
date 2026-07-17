# SecDef standalone test programs. Each source is its own executable linking SecDefLib.

# Offline: load a secdef file, report future/spread counts, list a product's front contracts.
add_executable(SecDefTests "SecDefTests.cpp")
target_link_libraries(SecDefTests PRIVATE SecDefLib)
