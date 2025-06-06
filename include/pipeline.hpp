#pragma once

#include <boost/asio.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <thread>
#include <iostream>
#include <fstream>
#include <chrono>
#include <Eigen/Dense>

#include <LidarDataframe.hpp>
#include <LidarIMUDataFrame.hpp>

#include <OusterLidarCallback.hpp>
#include <odometry/lidarinertialodometry.hpp>


class SLAMPipeline {
    public:

        static std::atomic<bool> running_;
        static std::condition_variable globalCV_;
        static boost::lockfree::spsc_queue<LidarDataFrame, boost::lockfree::capacity<128>> decodedPoint_buffer_;
        static boost::lockfree::spsc_queue<LidarIMUDataFrame, boost::lockfree::capacity<128>> decodedLidarIMU_buffer_;

        SLAMPipeline(const std::string& odom_json_path, const std::string& lidar_json_path); // Constructor with JSON file path
        SLAMPipeline(const nlohmann::json& odom_json_data, const nlohmann::json& lidar_json_data); // Constructor with JSON data
    
        static void signalHandler(int signal);
        void setThreadAffinity(const std::vector<int>& coreIDs);

        //### application function
        void runOusterLidarListenerSingleReturn(boost::asio::io_context& ioContext, const std::string& host, uint16_t port, uint32_t bufferSize, const std::vector<int>& allowedCores);
        void runOusterLidarListenerLegacy(boost::asio::io_context& ioContext, const std::string& host, uint16_t port, uint32_t bufferSize, const std::vector<int>& allowedCores);  
        void runOusterLidarIMUListener(boost::asio::io_context& ioContext, const std::string& host, uint16_t port, uint32_t bufferSize, const std::vector<int>& allowedCores); 
        void runGnssCompassListener(boost::asio::io_context& ioContext, const std::string& host, uint16_t port, uint32_t bufferSize, const std::vector<int>& allowedCores); 

        struct SLAMconfig {
            std::string output_dir = "./outputs";
            stateestimate::lidarinertialodom::Options config_param;
        };

        SLAMconfig parse_metadata(const json& json_data);

    private:

        // runOusterLidarListener
        OusterLidarCallback lidarCallback;
        uint16_t frame_id_= 0;
        LidarDataFrame frame_data_copy_;

        // runOusterLidarIMUListener
        uint64_t Accelerometer_Read_Time_ = 0.0;
        uint64_t Gyroscope_Read_Time_ = 0.0;

}; // namespace SLAMPipeline