#include <pipeline.hpp>

int main() {
    std::string lidar_json = "../json/2025047_1054_OS-2-128_122446000745.json";
    std::string config_json = "../config/odom_config.json";
    uint32_t lidar_packet_size = 24896;

    // --- These could also be moved to a config file or command-line options ---
    const std::string udp_dest_all = "192.168.75.10";
    const uint16_t udp_port_gnss = 6597;
    const uint32_t id20_packet_size = 105;
    uint32_t lidar_packet_size = 0; // Will be determined by profile

    // --- The rest of your setup logic ---
    std::string udp_profile_lidar;
    uint16_t udp_port_lidar;
    uint16_t udp_port_imu;
    std::string udp_dest = "192.168.75.10";

    // --- Generate UTC timestamp for filename
    auto now = std::chrono::system_clock::now();
    auto utc_time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&utc_time), "%Y%m%d_%H%M%S");
    std::string timestamp = ss.str();
    std::string log_filename = "../report/log/log_report_" + timestamp + ".txt";
    std::string gt_filename = "../report/gt/gt_report_" + timestamp + ".txt";

    try {
        std::ifstream json_file(lidar_json);
        if (!json_file.is_open()) {
            throw std::runtime_error("[Main] Error: Could not open JSON file: " + lidar_json);
        }
        nlohmann::json metadata_;
        json_file >> metadata_;
        json_file.close();

        if (!metadata_.contains("lidar_data_format") || !metadata_["lidar_data_format"].is_object()) {
                throw std::runtime_error("Missing or invalid 'lidar_data_format' object");
            return EXIT_FAILURE;
        }
        if (!metadata_.contains("config_params") || !metadata_["config_params"].is_object()) {
                throw std::runtime_error("Missing or invalid 'config_params' object");
            return EXIT_FAILURE;
        }
        if (!metadata_.contains("beam_intrinsics") || !metadata_["beam_intrinsics"].is_object()) {
                throw std::runtime_error("Missing or invalid 'beam_intrinsics' object");
            return EXIT_FAILURE;
        }
        if (!metadata_.contains("lidar_intrinsics") || !metadata_["lidar_intrinsics"].is_object() ||
            !metadata_["lidar_intrinsics"].contains("lidar_to_sensor_transform")) {
                throw std::runtime_error("Missing or invalid 'lidar_intrinsics.lidar_to_sensor_transform'");
            return EXIT_FAILURE;
        }

        udp_profile_lidar = metadata_["config_params"]["udp_profile_lidar"].get<std::string>();
        udp_port_lidar = metadata_["config_params"]["udp_port_lidar"].get<uint16_t>();
        udp_port_imu = metadata_["config_params"]["udp_port_imu"].get<uint16_t>();
        udp_dest = metadata_["config_params"]["udp_dest"].get<std::string>();

    } catch (const std::exception& e) {
        std::cerr << "[Main] Error parsing JSON: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    //"SLAM_LIDAR_ODOM" or "SLAM_LIDAR_INERTIAL_ODOM"
    SLAMPipeline pipeline("SLAM_LIDAR_ODOM", config_json, lidar_json, 
                            lidarDecode::OusterLidarCallback::LidarTransformPreset::GEHLSDORF20250410);

    struct sigaction sigIntHandler;
    sigIntHandler.sa_handler = SLAMPipeline::signalHandler;
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;

    sigaction(SIGINT, &sigIntHandler, nullptr);
    sigaction(SIGTERM, &sigIntHandler, nullptr);

#ifdef DEBUG
    std::cout << "[Main] Starting pipeline processes..." << std::endl;
#endif

    try {
            std::vector<std::thread> threads;
            boost::asio::io_context ioContextPoints;

            enum class LidarProfile { RNG19_RFL8_SIG16_NIR16, LEGACY, UNKNOWN };
            LidarProfile profile;
            if (udp_profile_lidar == "RNG19_RFL8_SIG16_NIR16") {
                profile = LidarProfile::RNG19_RFL8_SIG16_NIR16;
                lidar_packet_size = 24832;
            } else if (udp_profile_lidar == "LEGACY") {
                profile = LidarProfile::LEGACY;
                lidar_packet_size = 24896;
            } else {
                profile = LidarProfile::UNKNOWN;
            }

            switch (profile) {
                case LidarProfile::RNG19_RFL8_SIG16_NIR16:
                    threads.emplace_back([&]() { pipeline.runOusterLidarListenerSingleReturn(ioContextPoints, udp_dest_all, udp_port_lidar, lidar_packet_size, {0}); });
                    break;
                case LidarProfile::LEGACY:
                    threads.emplace_back([&]() { pipeline.runOusterLidarListenerLegacy(ioContextPoints, udp_dest_all, udp_port_lidar, lidar_packet_size, {0}); });
                    break;
                case LidarProfile::UNKNOWN:
                default:
                    std::cerr << "[Main] Error: Unknown or unsupported udp_profile_lidar: " << udp_profile_lidar << std::endl;
                    return EXIT_FAILURE;
            }

            threads.emplace_back([&]() { pipeline.runGNSSID20Listener(ioContextPoints, udp_dest_all, udp_port_gnss, id20_packet_size, std::vector<int>{1}); });
            threads.emplace_back([&]() { pipeline.dataAlignmentID20(std::vector<int>{2}); });
            threads.emplace_back([&]() { pipeline.processLogQueue(log_filename,std::vector<int>{3}); });
            threads.emplace_back([&]() { pipeline.runLoStateEstimation(std::vector<int>{4,5,6,7,8,9,10,11}); });
            threads.emplace_back([&]() { pipeline.runGroundTruthEstimation(gt_filename, std::vector<int>{12}); });

        // --- IMPROVEMENT 3: Use a condition variable for the main thread wait ---
            // The current sleep loop is acceptable, but a CV is more efficient.
            {
                std::mutex cv_m;
                std::unique_lock<std::mutex> lock(cv_m);
                SLAMPipeline::globalCV_.wait(lock, []{ return !SLAMPipeline::running_.load(); });
            }

            ioContextPoints.stop();

            for (auto& thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }

            // --- SAFE SHUTDOWN POINT ---
            // All threads have stopped. It is now 100% safe to access pipeline data.
            std::cout << "[Main] All threads joined. Saving final results..." << std::endl;
            pipeline.saveOdometryResults(timestamp); // Call the new safe method
            
    } catch (const std::exception& e) {
        std::cerr << "Error: [Main] " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "[Main] All processes stopped. Exiting program." << std::endl;
    return EXIT_SUCCESS;
}

