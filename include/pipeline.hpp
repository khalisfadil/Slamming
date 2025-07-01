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

#include <UdpSocket.hpp>

#include <LidarDataframe.hpp>
#include <LidarIMUDataFrame.hpp>

#include <OusterLidarCallback.hpp>
#include <odometry/lidarinertialodometry.hpp>

class SLAMPipeline {

    struct LidarIMUVecDataFrame {
        std::vector<lidarDecode::LidarIMUDataFrame> IMUVec;
        lidarDecode::LidarDataFrame Lidar;
    };

    public:

        static std::atomic<bool> running_;
        static std::condition_variable globalCV_;
        static std::atomic<int> dropped_logs_;
        static boost::lockfree::spsc_queue<lidarDecode::LidarDataFrame, boost::lockfree::capacity<128>> lidar_buffer_;
        static boost::lockfree::spsc_queue<std::vector<lidarDecode::LidarIMUDataFrame>, boost::lockfree::capacity<128>> imu_vec_buffer_;
        static boost::lockfree::spsc_queue<lidarDecode::LidarIMUDataFrame, boost::lockfree::capacity<128>> imu_buffer_;
        static boost::lockfree::spsc_queue<LidarIMUVecDataFrame, boost::lockfree::capacity<128>> lidar_imu_buffer_;
        static boost::lockfree::spsc_queue<std::string, boost::lockfree::capacity<128>> log_queue_;

        explicit SLAMPipeline(const std::string& odom_json_path, const std::string& lidar_json_path); // Constructor with JSON file path
    
        static void signalHandler(int signal);
        void setThreadAffinity(const std::vector<int>& coreIDs);

        //### application listener
        void runOusterLidarListenerSingleReturn(boost::asio::io_context& ioContext, const std::string& host, uint16_t port, uint32_t bufferSize, const std::vector<int>& allowedCores);
        void runOusterLidarListenerLegacy(boost::asio::io_context& ioContext, const std::string& host, uint16_t port, uint32_t bufferSize, const std::vector<int>& allowedCores);  
        void runOusterLidarIMUListener(boost::asio::io_context& ioContext, const std::string& host, uint16_t port, uint32_t bufferSize, const std::vector<int>& allowedCores); 
        void runGnssCompassListener(boost::asio::io_context& ioContext, const std::string& host, uint16_t port, uint32_t bufferSize, const std::vector<int>& allowedCores); 

        // application for logging
        void processLogQueue(const std::string& filename, const std::vector<int>& allowedCores);
        void logMessage(const std::string& level, const std::string& message);

        // application for slam
        void dataAlignment(const std::vector<int>& allowedCores);
        void runLioStateEstimation(const std::vector<int>& allowedCores);
        
        // application for DynamicMapping
        void runDynamicMapping(const std::vector<int>& allowedCores);
        
    private:

        // finalState
        stateestimate::lidarinertialodom lioOdometry;

        // runOusterLidarListener
        lidarDecode::OusterLidarCallback lidarCallback;
        lidarDecode::LidarDataFrame temp_lidar_data_;
        uint16_t frame_id_= 0;

        // runOusterLidarIMUListener
        lidarDecode::LidarIMUDataFrame temp_IMU_data_;
        std::vector<lidarDecode::LidarIMUDataFrame> temp_IMU_vec_data_;
        const size_t VECTOR_SIZE_IMU = 15;

        // runOusterLidarIMUListener
        uint64_t Normalized_Timestamp_s_ = 0.0;

}; // namespace SLAMPipeline