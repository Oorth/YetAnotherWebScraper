cmake_minimum_required(VERSION 3.10)
project(CS_Price_Tool)

set(CMAKE_CXX_STANDARD 17)

# Find packages installed via vcpkg
find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(CS_Price_Tool main.cpp)

# Link the libraries and Windows system dependencies
target_link_libraries(CS_Price_Tool PRIVATE 
    ixwebsocket::ixwebsocket 
    nlohmann_json::nlohmann_json
    ws2_32 
    crypt32
)