add_executable(testbeast testbeast.cpp)
target_link_libraries(testbeast $<TARGET_OBJECTS:beast> Fcitx5::Core)
add_test(NAME testbeast COMMAND testbeast)

# Covers the config write that restarts the server answering it, which needs a
# host that marshals the write to the fcitx thread, as fcitx5-macos does.
add_executable(testbeastmt testbeastmt.cpp)
target_link_libraries(testbeastmt $<TARGET_OBJECTS:beast> Fcitx5::Core)
add_test(NAME testbeastmt COMMAND testbeastmt)

# Both move the server between endpoints, so they must not run at the same time.
foreach(test testbeast testbeastmt)
    set_tests_properties(${test} PROPERTIES RESOURCE_LOCK beast_listener)
endforeach()
