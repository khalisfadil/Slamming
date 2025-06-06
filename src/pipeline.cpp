#include <pipeline.hpp>

using json = nlohmann::json;

std::atomic<bool> SLAMPipeline::running_{true};
std::condition_variable SLAMPipeline::globalCV_;
boost::lockfree::spsc_queue<LidarDataFrame, boost::lockfree::capacity<128>> SLAMPipeline::decodedPoint_buffer_;
boost::lockfree::spsc_queue<LidarIMUDataFrame, boost::lockfree::capacity<128>> SLAMPipeline::decodedLidarIMU_buffer_;


// -----------------------------------------------------------------------------
SLAMPipeline::SLAMPipeline(const std::string& odom_json_path, const std::string& lidar_json_path){
    std::ifstream odomfile(odom_json_path), lidarfile(lidar_json_path);

    if (!odomfile.is_open()) {
         throw std::runtime_error("Failed to open JSON file: " + odom_json_path);
    }

    if (!lidarfile.is_open()) {
         throw std::runtime_error("Failed to open JSON file: " + lidar_json_path);
    }

    json json_odom_data, json_lidar_data;

    try {
        odomfile >> json_odom_data;
        
    } catch (const json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + odom_json_path + ": " + e.what());
    }

    try {
        lidarfile >> json_lidar_data;
        
    } catch (const json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + lidar_json_path + ": " + e.what());
    }


}

// -----------------------------------------------------------------------------

SLAMPipeline::SLAMconfig SLAMPipeline::parse_metadata(const json& json_data){
    if (!json_data.is_object()) {
        throw std::runtime_error("JSON data must be an object");
    }

    


}