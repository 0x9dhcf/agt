foreach(example complete agent mcp)
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE agt::agt)
endforeach()
