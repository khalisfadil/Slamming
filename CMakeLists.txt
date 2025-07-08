cmake_minimum_required(VERSION 3.16)
project(slam LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -ffast-math -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

# Silence deprecated policy warning from Open3D
if(POLICY CMP0072)
    cmake_policy(SET CMP0072 NEW)
endif()

# Find dependencies
set(Open3D_DIR "/usr/local/lib/cmake/Open3D/" CACHE PATH "Path to Open3D CMake config")
set(TBB_DIR "/usr/lib/x86_64-linux-gnu/cmake/TBB" CACHE PATH "Path to TBB CMake config")
set(Unwind_INCLUDE_DIR /usr/include/libunwind CACHE PATH "Path to libunwind headers")

find_library(Unwind_LIBRARY NAMES libunwind.so.8 PATHS /usr/lib/x86_64-linux-gnu)
if(NOT Unwind_LIBRARY)
    message(FATAL_ERROR "libunwind.so.8 not found")
endif()

find_package(Eigen3 REQUIRED NO_MODULE)
find_package(glog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(Open3D REQUIRED)
find_package(TBB REQUIRED)
find_library(CPUID_LIBRARY NAMES cpuid libcpuid)

# Check for CPUID
if(CPUID_LIBRARY)
    message(STATUS "Found cpuid library: ${CPUID_LIBRARY}")
else()
    message(STATUS "cpuid library not found; runtime CPU detection disabled")
    add_compile_definitions(NO_CPUID)
endif()

# Check for AVX2 and FMA support
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
check_cxx_compiler_flag("-mfma" COMPILER_SUPPORTS_FMA)
if(COMPILER_SUPPORTS_AVX2 AND COMPILER_SUPPORTS_FMA)
    message(STATUS "AVX2 and FMA are supported by the compiler")
    add_compile_options(-mavx2 -mfma)
else()
    message(WARNING "AVX2 or FMA not supported; using scalar implementation")
endif()

# Define include directories
set(SLAM_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include")
set(FINALICP_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/common"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/evaluable"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/evaluable/imu"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/evaluable/p2p"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/evaluable/se3"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/evaluable/vspace"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/matrixoperator"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/problem"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/problem/costterm"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/problem/lossfunc"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/problem/noisemodel"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/solver"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/trajectory"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/trajectory/bspline"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/trajectory/constacc"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/trajectory/constvel"
    "${CMAKE_SOURCE_DIR}/extern/FinalICP/include/trajectory/singer"
)
set(FINALSTATEESTIMATION_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/extern/FinalStateEstimation/include"
    "${CMAKE_SOURCE_DIR}/extern/FinalStateEstimation/include/odometry"
    "${CMAKE_SOURCE_DIR}/extern/FinalStateEstimation/include/utils"
)
set(GNSSKOMPASS_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/extern/GNSSKompass/include")
set(LIDARDECODE_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/extern/LidarDecode/include")
set(LIEGROUPMATH_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/extern/liegroupMath/include"
    "${CMAKE_SOURCE_DIR}/extern/liegroupMath/include/r3"
    "${CMAKE_SOURCE_DIR}/extern/liegroupMath/include/se3"
    "${CMAKE_SOURCE_DIR}/extern/liegroupMath/include/so3"
)
set(ROBIN_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/3rdparty/robin-map/include/tsl")

# Combine include directories
set(PROJECT_INCLUDE_DIRS
    ${SLAM_INCLUDE_DIRS}
    ${FINALICP_INCLUDE_DIRS}
    ${FINALSTATEESTIMATION_INCLUDE_DIRS}
    ${GNSSKOMPASS_INCLUDE_DIRS}
    ${LIDARDECODE_INCLUDE_DIRS}
    ${LIEGROUPMATH_INCLUDE_DIRS}
    ${ROBIN_INCLUDE_DIRS}
)

# Source file groups
set(SOURCE_GROUPS
    MATRIXOPERATOR
    EVALUABLE_IMU
    EVALUABLE_P2P
    EVALUABLE_SE3
    PROBLEM
    SOLVER
    TRAJECTORY_BSPLINE
    TRAJECTORY_CONSTACC
    TRAJECTORY_CONSTVEL
    LGMATH
    LIDARDECODE
    GNSSKOMPASS
    FINALSTATEESTIMATION
    SLAMMING
)

# Define source files for each group
set(MATRIXOPERATOR_SOURCES
    extern/FinalICP/src/matrixoperator/matrix.cpp
    extern/FinalICP/src/matrixoperator/matrixbase.cpp
    extern/FinalICP/src/matrixoperator/matrixindexing.cpp
    extern/FinalICP/src/matrixoperator/matrixsparse.cpp
    extern/FinalICP/src/matrixoperator/vector.cpp
)

set(EVALUABLE_IMU_SOURCES
    extern/FinalICP/src/evaluable/imu/accerrorevaluator.cpp
    extern/FinalICP/src/evaluable/imu/dmierrorevaluator.cpp
    extern/FinalICP/src/evaluable/imu/gyroerrorevaluator.cpp
    extern/FinalICP/src/evaluable/imu/gyroerrorevaluatorse2.cpp
    extern/FinalICP/src/evaluable/imu/imuerrorevaluator.cpp
)

set(EVALUABLE_P2P_SOURCES
    extern/FinalICP/src/evaluable/p2p/p2perrorevaluator.cpp
    extern/FinalICP/src/evaluable/p2p/p2planeerrorevaluator.cpp
    extern/FinalICP/src/evaluable/p2p/p2planeerrorglobalperturbevaluator.cpp
    extern/FinalICP/src/evaluable/p2p/velerrorevaluator.cpp
    extern/FinalICP/src/evaluable/p2p/yawerrorevaluator.cpp
    extern/FinalICP/src/evaluable/p2p/yawvelerrorevaluator.cpp
)

set(EVALUABLE_SE3_SOURCES
    extern/FinalICP/src/evaluable/se3/composeevaluator.cpp
    extern/FinalICP/src/evaluable/se3/composeinverseevaluator.cpp
    extern/FinalICP/src/evaluable/se3/composevelocityevaluator.cpp
    extern/FinalICP/src/evaluable/se3/expmapevaluator.cpp
    extern/FinalICP/src/evaluable/se3/inverseevaluator.cpp
    extern/FinalICP/src/evaluable/se3/logmapevaluator.cpp
    extern/FinalICP/src/evaluable/se3/poseinterpolator.cpp
    extern/FinalICP/src/evaluable/se3/se3errorevaluator.cpp
    extern/FinalICP/src/evaluable/se3/se3globalperturbevaluator.cpp
    extern/FinalICP/src/evaluable/se3/se3statevar.cpp
    extern/FinalICP/src/evaluable/se3/se3statevarglobalperturbevaluator.cpp
)

set(PROBLEM_SOURCES
    extern/FinalICP/src/problem/costterm/gyrosupercostterm.cpp
    extern/FinalICP/src/problem/costterm/imusupercostterm.cpp
    extern/FinalICP/src/problem/costterm/p2pconstvelsupercostterm.cpp
    extern/FinalICP/src/problem/costterm/p2pglobalperturbsupercostterm.cpp
    extern/FinalICP/src/problem/costterm/p2psupercostterm.cpp
    extern/FinalICP/src/problem/costterm/preintegratedimucostterm.cpp
    extern/FinalICP/src/problem/costterm/preintgratedaccelcostterm.cpp
    extern/FinalICP/src/problem/optimizationproblem.cpp
    extern/FinalICP/src/problem/slidingwindowfilter.cpp
    extern/FinalICP/src/problem/statevector.cpp
)

set(SOLVER_SOURCES
    extern/FinalICP/src/solver/covariance.cpp
    extern/FinalICP/src/solver/dogleggaussnewtonsolver.cpp
    extern/FinalICP/src/solver/gausnewtonsolver.cpp
    extern/FinalICP/src/solver/gausnewtonsolvernva.cpp
    extern/FinalICP/src/solver/levmarqgaussnewtonsolver.cpp
    extern/FinalICP/src/solver/linesearchgaussnewtonsolver.cpp
    extern/FinalICP/src/solver/solverbase.cpp
)

set(TRAJECTORY_BSPLINE_SOURCES
    extern/FinalICP/src/trajectory/bspline/interface.cpp
    extern/FinalICP/src/trajectory/bspline/velocityinterpolator.cpp
)

set(TRAJECTORY_CONSTACC_SOURCES
    extern/FinalICP/src/trajectory/constacc/evaluable/composecurlyhatevaluator.cpp
    extern/FinalICP/src/trajectory/constacc/accelerationextrapolator.cpp
    extern/FinalICP/src/trajectory/constacc/accelerationinterpolator.cpp
    extern/FinalICP/src/trajectory/constacc/interface.cpp
    extern/FinalICP/src/trajectory/constacc/poseinterpolator.cpp
    extern/FinalICP/src/trajectory/constacc/posextrapolator.cpp
    extern/FinalICP/src/trajectory/constacc/priorfactor.cpp
    extern/FinalICP/src/trajectory/constacc/velocityextrapolator.cpp
    extern/FinalICP/src/trajectory/constacc/velocityinterpolator.cpp
)

set(TRAJECTORY_CONSTVEL_SOURCES
    extern/FinalICP/src/trajectory/constvel/evaluable/jinvvelocityevaluator.cpp
    extern/FinalICP/src/trajectory/constvel/evaluable/jvelocityevaluator.cpp
    extern/FinalICP/src/trajectory/constvel/interface.cpp
    extern/FinalICP/src/trajectory/constvel/poseextrapolator.cpp
    extern/FinalICP/src/trajectory/constvel/poseinterpolator.cpp
    extern/FinalICP/src/trajectory/constvel/priorfactor.cpp
    extern/FinalICP/src/trajectory/constvel/velocityinterpolator.cpp
)

set(LGMATH_SOURCES
    extern/liegroupMath/src/r3/operations.cpp
    extern/liegroupMath/src/se3/operations.cpp
    extern/liegroupMath/src/se3/transformations.cpp
    extern/liegroupMath/src/se3/transformationswithcovariance.cpp
    extern/liegroupMath/src/commonmath.cpp
    extern/liegroupMath/src/so3/operations.cpp
    extern/liegroupMath/src/so3/rotations.cpp
)

set(LIDARDECODE_SOURCES
    extern/LidarDecode/src/OusterLidarCallback.cpp
)

set(GNSSKOMPASS_SOURCES
    extern/GNSSKompass/src/callback_gnssComp.cpp
    extern/GNSSKompass/src/callback_navMsg.cpp
    extern/GNSSKompass/src/navMath.cpp
)

set(FINALSTATEESTIMATION_SOURCES
    extern/FinalStateEstimation/src/odometry/lidarinertialodometry.cpp
)

set(SLAMMING_SOURCES
    src/pipeline.cpp
    src/UdpSocket.cpp
)

# Combine all sources
set(SLAM_SOURCES)
foreach(GROUP ${SOURCE_GROUPS})
    list(APPEND SLAM_SOURCES ${${GROUP}_SOURCES})
endforeach()

# Create core library
add_library(slam_core SHARED ${SLAM_SOURCES})

# Set include directories and link libraries
target_include_directories(slam_core
    PUBLIC
        ${PROJECT_INCLUDE_DIRS}
)

target_link_libraries(slam_core
    PUBLIC
        Eigen3::Eigen
        glog::glog
        nlohmann_json::nlohmann_json
        TBB::tbb
        Boost::system
        Boost::thread
        Open3D::Open3D
        Threads::Threads
        ${Unwind_LIBRARY}
    PRIVATE
        $<$<BOOL:${CPUID_LIBRARY}>:${CPUID_LIBRARY}>
)

# Create executable
add_executable(slam src/main.cpp)
target_include_directories(slam
    PUBLIC
        ${PROJECT_INCLUDE_DIRS}
)
target_link_libraries(slam PRIVATE slam_core)

# Generate compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)