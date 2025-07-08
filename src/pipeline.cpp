#include <pipeline.hpp>

using json = nlohmann::json;

std::atomic<bool> SLAMPipeline::running_{true};
std::condition_variable SLAMPipeline::globalCV_;

// -----------------------------------------------------------------------------

void SLAMPipeline::logMessage(const std::string& level, const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << "[" << std::put_time(std::gmtime(&now_time_t), "%Y-%m-%dT%H:%M:%SZ") << "] "
        << "[" << level << "] " << message << "\n";
    if (!log_queue_.push(oss.str())) {
        dropped_logs_.fetch_add(1, std::memory_order_relaxed);
    }
}

// -----------------------------------------------------------------------------

SLAMPipeline::SLAMPipeline(const std::string& odom_json_path, const std::string& lidar_json_path) 
    : lioOdometry(odom_json_path), lidarCallback(lidar_json_path){
    temp_IMU_vec_data_.reserve(VECTOR_SIZE_IMU);
}

// -----------------------------------------------------------------------------

void SLAMPipeline::signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        running_.store(false, std::memory_order_release);
        globalCV_.notify_all();

        constexpr const char* message = "[signalHandler] Shutting down...\n";
        constexpr size_t messageLen = sizeof(message) - 1;
        ssize_t result = write(STDOUT_FILENO, message, messageLen);
    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::setThreadAffinity(const std::vector<int>& coreIDs) {
    if (coreIDs.empty()) {return;}
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    const unsigned int maxCores = std::thread::hardware_concurrency();
    uint32_t validCores = 0;

    for (int coreID : coreIDs) {
        if (coreID >= 0 && static_cast<unsigned>(coreID) < maxCores) {
            CPU_SET(coreID, &cpuset);
            validCores |= (1 << coreID);
        }
    }
    if (!validCores) {
            return;
        }

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        running_.store(false); // Optionally terminate
    }

}

// -----------------------------------------------------------------------------

void SLAMPipeline::processLogQueue(const std::string& filename, const std::vector<int>& allowedCores) {
    setThreadAffinity(allowedCores); // Pin logging thread to specified cores

    std::ofstream outfile(filename);
    if (!outfile.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << "[" << std::put_time(std::gmtime(&now_time_t), "%Y-%m-%dT%H:%M:%SZ") << "] "
            << "[ERROR] failed to open file " << filename << " for writing.\n";
        std::cerr << oss.str(); // Fallback to cerr if file cannot be opened
        return;
    }

    std::string message;
    int lastReportedDrops = 0;
    while (running_.load(std::memory_order_acquire)) {
        if (log_queue_.pop(message)) {
            outfile << message;
            int currentDrops = dropped_logs_.load(std::memory_order_relaxed);
            if (currentDrops > lastReportedDrops && (currentDrops - lastReportedDrops) >= 100) {
                auto now = std::chrono::system_clock::now();
                auto now_time_t = std::chrono::system_clock::to_time_t(now);
                std::ostringstream oss;
                oss << "[" << std::put_time(std::gmtime(&now_time_t), "%Y-%m-%dT%H:%M:%SZ") << "] "
                    << "[WARNING] " << (currentDrops - lastReportedDrops) << " log messages dropped due to queue overflow.\n";
                outfile << oss.str();
                lastReportedDrops = currentDrops;
            }
        } else {
            std::this_thread::yield();
        }
    }
    while (log_queue_.pop(message)) {
        outfile << message;
    }
    int finalDrops = dropped_logs_.load(std::memory_order_relaxed);
    if (finalDrops > lastReportedDrops) {
        outfile << "[LOGGING] Final report: " << (finalDrops - lastReportedDrops) << " log messages dropped.\n";
    }

    outfile.flush(); // Ensure data is written
    outfile.close();
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runOusterLidarListenerSingleReturn(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {
    
    setThreadAffinity(allowedCores); 

    if (host.empty() || port == 0) {
        std::ostringstream oss;
        oss << "Lidar Listener: Invalid host or port. Host: " << host << ", Port: " << port;
        logMessage("ERROR", oss.str());    
        return;
    }

    try {

        UdpSocket listener(ioContext, host, port, [&](const std::vector<uint8_t>& packet_data) {

            lidarCallback.decode_packet_single_return(packet_data, temp_lidar_data_);
           
            if (temp_lidar_data_.numberpoints > 0 && temp_lidar_data_.frame_id != this->frame_id_) {

                this->frame_id_ = temp_lidar_data_.frame_id;
                
                if (!lidar_buffer_.push(std::move(temp_lidar_data_))) {
                    logMessage("WARNING", "Lidar Listener: SPSC Lidar buffer push failed.");  
                }
            }
        }, bufferSize);

        // Main loop to run Asio's I/O event processing.
        while (running_.load(std::memory_order_acquire)) {
            try {
                ioContext.run(); 
                if (!running_.load(std::memory_order_acquire)) { 
                    break;
                }
               
                break; 
            } catch (const std::exception& e) {
                 logMessage("WARNING", "Lidar Listener: Exception in ioContext."); 

                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart(); 
                    logMessage("WARNING", "Lidar Listener: ioContext restarted.");

                } else {
                    break; // Exit loop if shutting down.
                }
            }
        }
    } catch(const std::exception& e){
        logMessage("WARNING", "Lidar Listener: Setup exception.");
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }

    logMessage("LOGGING", "Lidar Listener: listener stopped.");
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runOusterLidarListenerLegacy(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {
    
    setThreadAffinity(allowedCores); 

    if (host.empty() || port == 0) {
        std::ostringstream oss;
        oss << "Lidar Listener: Invalid host or port. Host: " << host << ", Port: " << port;
        logMessage("ERROR", oss.str());      
        return;
    }

    try {
        UdpSocket listener(ioContext, host, port, [&](const std::vector<uint8_t>& packet_data) {
           
            lidarCallback.decode_packet_legacy(packet_data, temp_lidar_data_);

            if (temp_lidar_data_.numberpoints > 0 && temp_lidar_data_.frame_id != this->frame_id_) {
                this->frame_id_ = temp_lidar_data_.frame_id;
                
                if (!lidar_buffer_.push(std::move(temp_lidar_data_))) {
                    logMessage("WARNING", "Lidar Listener: SPSC Lidar buffer push failed.");    
                }
            }
           
        }, bufferSize);

        while (running_.load(std::memory_order_acquire)) {
            try {
                ioContext.run(); 
                if (!running_.load(std::memory_order_acquire)) { 
                    break;
                }
                
                break; 
            } catch (const std::exception& e) {
                logMessage("ERROR", "Lidar Listener: Exception in ioContext.");

                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart(); // Restart Asio io_context to attempt recovery.
                    logMessage("LOGGING", "Lidar Listener: ioContext restarted.");

                } else {
                    break; // Exit loop if shutting down.
                }
            }
        }
    } catch(const std::exception& e){
        logMessage("ERROR", "Lidar Listener: Setup exception.");
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }
    logMessage("LOGGING", "Lidar Listener: listener stopped."); 
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runOusterLidarIMUListener(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {
    
    setThreadAffinity(allowedCores); // Sets affinity for this listener thread

    if (host.empty() || port == 0) {
        std::ostringstream oss;
        oss << "Lidar Listener: Invalid host or port. Host: " << host << ", Port: " << port;
        logMessage("ERROR", oss.str());  
        return;
    }

    try {
        UdpSocket listener(ioContext, host, port,[&](const std::vector<uint8_t>& packet_data) {

            // Decode the packet into frame_data_IMU_copy
            lidarCallback.decode_packet_LidarIMU(packet_data, temp_IMU_data_);

            // Check if the frame is valid
            if (temp_IMU_data_.Normalized_Timestamp_s > 0 && 
                temp_IMU_data_.Normalized_Timestamp_s != this->Normalized_Timestamp_s_ ) {

                this->Normalized_Timestamp_s_ = temp_IMU_data_.Normalized_Timestamp_s;

                // If the vector is full, remove the oldest frame
                if (temp_IMU_vec_data_.size() >= VECTOR_SIZE_IMU) {
                    temp_IMU_vec_data_.erase(temp_IMU_vec_data_.begin()); // Remove the oldest element
                }

                // Push the new frame into the vector
                temp_IMU_vec_data_.push_back(temp_IMU_data_); // Deep copy into vector
                
                // Push the copy into the SPSC queue
                if (!imu_vec_buffer_.push(temp_IMU_vec_data_)) {
                    logMessage("WARNING", "IMU Listener: SPSC IMU Vec buffer push failed."); 
                }

                // Push the copy into the SPSC queue
                if (!imu_buffer_.push(temp_IMU_data_)) {
                    logMessage("WARNING", "IMU Listener: SPSC IMU buffer push failed."); 
                }
            }  
        }, bufferSize);

        // Main loop to run Asio's I/O event processing.
        while (running_.load(std::memory_order_acquire)) {
            try {
                ioContext.run(); 
                if (!running_.load(std::memory_order_acquire)) { 
                }
                
                break; 
            } catch (const std::exception& e) {
                logMessage("ERROR", "IMU Listener: Exception in ioContext."); 
                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart();
                    logMessage("LOGGING", "IMU Listener: ioContext restarted.");  
                } else {
                    break; 
                }
            }
        }
    }
    catch(const std::exception& e){
        logMessage("ERROR", "IMU Listener: Setup exception.");
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }
    logMessage("LOGGING", "IMU Listener: listener stopped.");
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runGNSSID20Listener(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {

    // 1. Set affinity for the current (consumer) thread
    setThreadAffinity(allowedCores);

    if (host.empty() || port == 0) {
        std::ostringstream oss;
        oss << "ID28 Listener: Invalid host or port. Host: " << host << ", Port: " << port;
        logMessage("ERROR", oss.str());
        return;
    }

    auto processGNSSID20Frames = [&]() {
        decodeNav::DataFrameID20 temp_gnss_ID20_intern_data_;
        if (ID20_intern_buffer_.pop(temp_gnss_ID20_intern_data_)) {
            if (temp_gnss_ID20_intern_data_.unixTime > 0 && temp_gnss_ID20_intern_data_.unixTime != this->unixTime) {
                this->unixTime = temp_gnss_ID20_intern_data_.unixTime;

                // debug
                // std::ostringstream oss;
                // oss << "ID28 Listener: Input Value. Latitude: " << temp_gnss_ID20_intern_data_.latitude << ", Longitude: " << temp_gnss_ID20_intern_data_.longitude << ", Altitude: " << temp_gnss_ID20_intern_data_.altitude;
                // logMessage("LOGGING", oss.str());

                if (temp_gnss_ID20_vec_data_.size() >= VECTOR_SIZE_ID20) {
                    temp_gnss_ID20_vec_data_.erase(temp_gnss_ID20_vec_data_.begin());
                }
                temp_gnss_ID20_vec_data_.push_back(temp_gnss_ID20_intern_data_);

                if (!ID20_vec_buffer_.push(temp_gnss_ID20_vec_data_)) {
                    logMessage("WARNING", "ID20 Listener: SPSC ID20 Vec buffer push failed.");
                }
                if (!ID20_buffer_.push(temp_gnss_ID20_intern_data_)) {
                    logMessage("WARNING", "ID20 Listener: SPSC ID20 buffer push failed.");
                }
            }
        }
    };

    try {
        UdpSocket listener(ioContext, host, port,[&](const std::vector<uint8_t>& packet_data) {
            decodeNav::DataFrameID20 temp_gnss_ID20_data_;
            gnssCallback.decode_ID20(packet_data, temp_gnss_ID20_data_);

            if (!ID20_intern_buffer_.push(temp_gnss_ID20_data_)) {
                logMessage("WARNING", "ID20 Listener: SPSC ID20 intern buffer push failed.");
            }
        }, bufferSize);

        std::thread ioThread([&]() {
            // 2. Set affinity for this new I/O (producer) thread to the SAME core(s).
            setThreadAffinity(allowedCores);

            try {
                ioContext.run();
            } catch (const std::exception& e) {
                logMessage("ERROR", "ID20 Listener: Exception in I/O thread.");
            }
            logMessage("LOGGING", "ID20 Listener: I/O thread finished.");
        });

        logMessage("LOGGING", "ID20 Listener: Consumer loop started.");
        while (running_.load(std::memory_order_acquire)) {
            processGNSSID20Frames();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        logMessage("LOGGING", "ID20 Listener: Stopping I/O context.");
        if (!ioContext.stopped()) {
            ioContext.stop();
        }
        if (ioThread.joinable()) {
            ioThread.join();
        }

    }
    catch(const std::exception& e){
        logMessage("ERROR", "ID20 Listener: Setup exception.");
        if (!ioContext.stopped()) {
            ioContext.stop();
        }
    }

    logMessage("LOGGING", "ID20 Listener: Listener stopped.");
}

// -----------------------------------------------------------------------------

void SLAMPipeline::dataAlignmentLocalIMU(const std::vector<int>& allowedCores){

    setThreadAffinity(allowedCores);

    while (running_.load(std::memory_order_acquire)) {
        try {
            // If no lidar data is available, wait briefly and retry
            if (lidar_buffer_.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            lidarDecode::LidarDataFrame temp_lidar_data__;
            if (!lidar_buffer_.pop(temp_lidar_data__)) {
                logMessage("WARNING", "DataAlignment : Failed to retrieved Lidar SPSC."); 
                continue;
            }

            // Skip if lidar data is empty
            if (temp_lidar_data__.timestamp_points.empty()) {
                logMessage("WARNING", "DataAlignment : Empty lidar data.");
                continue;
            }

            // Get min and max lidar timestamps (sorted, so use front and back)
            double min_lidar_time = temp_lidar_data__.timestamp_points.front();
            double max_lidar_time = temp_lidar_data__.timestamp_points.back();

            // Validate lidar timestamp range
            if (min_lidar_time > max_lidar_time) {
                logMessage("ERROR", "DataAlignment : Invalid lidar timestamp.");
                continue;
            }

            // Loop to find an IMU vector that aligns with the current lidar frame
            bool aligned = false;

            while (!aligned && imu_vec_buffer_.read_available() > 0){

                std::vector<lidarDecode::LidarIMUDataFrame> temp_IMU_vec_data_;

                if (!imu_vec_buffer_.pop(temp_IMU_vec_data_)) {
                    logMessage("WARNING", "DataAlignment : Failed to retrieved IMU vec SPSC.");
                    break;
                }

                // Skip if IMU data is empty
                if (temp_IMU_vec_data_.empty()) {
                    logMessage("WARNING", "DataAlignment : Empty IMU vec data.");
                    continue;
                }

                // find min/max
                double min_imu_time = temp_IMU_vec_data_.front().Normalized_Timestamp_s;
                double max_imu_time = temp_IMU_vec_data_.back().Normalized_Timestamp_s;

                // Verify IMU timestamps are valid and ordered
                if (min_imu_time > max_imu_time) {
                    logMessage("WARNING", "DataAlignment : Invalid IMU timestamp.");
                    continue;
                }

                // Check if lidar timestamps are within IMU range
                if (min_lidar_time >= min_imu_time && max_lidar_time <= max_imu_time) {
                    // Timestamps are aligned; process the data
                    aligned = true;

                    logMessage("WARNING", "DataAlignment : Timestamps Lidar and IMU are aligned.");

                    LidarIMUVecDataFrame temp_lidar_IMU_vec_data_;
                    temp_lidar_IMU_vec_data_.IMUVec = temp_IMU_vec_data_;
                    temp_lidar_IMU_vec_data_.Lidar = temp_lidar_data__;

                    if (!lidar_imu_buffer_.push(std::move(temp_lidar_IMU_vec_data_))) {
                        logMessage("WARNING", "DataAlignment : SPSC Lidar and IMU Vec buffer push failed.");
                    }
                    
                } else if (min_lidar_time > min_imu_time && max_lidar_time > max_imu_time){
                    // Lidar is too new or partially overlaps; pop another newer IMU vector >> skip while
                    continue;
                } else if (min_lidar_time < min_imu_time){
                    // Lidar impossible to catch up with the IMU timestamp, need to discard this Lidar frame.
                    // Potential Solution, increase the size buffer frame.
                    logMessage("ERROR", "DataAlignment : Lidar cannot catch up with IMU data, please increase Buffer size.");
                    break;                       
                }
            }
        } catch (const std::exception& e) {
            logMessage("ERROR", "DataAlignment : Exception occurred.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::dataAlignmentID20(const std::vector<int>& allowedCores){

    setThreadAffinity(allowedCores);

    while (running_.load(std::memory_order_acquire)) {
        try {
            // If no lidar data is available, wait briefly and retry
            if (lidar_buffer_.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            lidarDecode::LidarDataFrame temp_lidar_data__;
            if (!lidar_buffer_.pop(temp_lidar_data__)) {
                logMessage("WARNING", "DataAlignment ID20 : Failed to retrieved Lidar SPSC."); 
                continue;
            }

            // Skip if lidar data is empty
            if (temp_lidar_data__.timestamp_points.empty()) {
                logMessage("WARNING", "DataAlignment ID20 : Empty lidar data.");
                continue;
            }

            // Get min and max lidar timestamps (sorted, so use front and back)
            double min_lidar_time = temp_lidar_data__.timestamp_points.front();
            double max_lidar_time = temp_lidar_data__.timestamp_points.back();

            // debug
            // logMessage("LOGGING", "New Lidar Frame.");
            // std::ostringstream oss;
            // oss << std::fixed << std::setprecision(12);
            // oss << "DataAlignment ID20 : Lidar Time. Min: " << min_lidar_time << ", Max: " << max_lidar_time;
            // logMessage("LOGGING", oss.str());

            // Validate lidar timestamp range
            if (min_lidar_time > max_lidar_time) {
                logMessage("ERROR", "DataAlignment ID20 : Invalid lidar timestamp.");
                continue;
            }

            // Loop to find an IMU vector that aligns with the current lidar frame
            bool aligned = false;

            while (!aligned && ID20_vec_buffer_.read_available() > 0){

                std::vector<decodeNav::DataFrameID20> temp_gnss_ID20_vec_data__;

                if (!ID20_vec_buffer_.pop(temp_gnss_ID20_vec_data__)) {
                    logMessage("WARNING", "DataAlignment ID20 : Failed to retrieved IMU vec SPSC.");
                    break;
                }

                // Skip if IMU data is empty
                if (temp_gnss_ID20_vec_data__.empty()) {
                    logMessage("WARNING", "DataAlignment ID20 : Empty ID20 vec data.");
                    break;
                }

                // find min/max
                double min_id20_time = temp_gnss_ID20_vec_data__.front().unixTime;
                double max_id20_time = temp_gnss_ID20_vec_data__.back().unixTime;

                // debug
                // std::ostringstream oss;
                // oss << std::fixed << std::setprecision(12);
                // oss << "DataAlignment ID20 : Compass Time. Min: " << min_id20_time << ", Max: " << max_id20_time;
                // logMessage("LOGGING", oss.str());

                // Verify IMU timestamps are valid and ordered
                if (min_id20_time > max_id20_time) {
                    logMessage("WARNING", "DataAlignment ID20 : Invalid ID20 timestamp.");
                    continue;
                }

                // Check if lidar timestamps are within IMU range
                if (min_lidar_time >= min_id20_time && max_lidar_time <= max_id20_time) {
                    // Timestamps are aligned; process the data
                    aligned = true;

                    logMessage("LOGGING", "DataAlignment ID20 : Timestamps Lidar and ID20 are aligned.");

                    // Filter temp_gnss_ID20_vec_data__ to include only readings within min_lidar_time and max_lidar_time
                    std::vector<decodeNav::DataFrameID20> filtered_gnss_ID20_vec_data__;
                    // filtered_gnss_ID20_vec_data__.reserve(temp_gnss_ID20_vec_data__.size()); // Reserve space for efficiency
                    for (const auto& id20_data : temp_gnss_ID20_vec_data__) {
                        if (id20_data.unixTime >= min_lidar_time && id20_data.unixTime <= max_lidar_time) {
                            filtered_gnss_ID20_vec_data__.push_back(id20_data);
                        }
                    }

                    // find min/max
                    double min_filtered_id20_time = filtered_gnss_ID20_vec_data__.front().unixTime;
                    double max_filtered_id20_time = filtered_gnss_ID20_vec_data__.back().unixTime;

                    if (min_filtered_id20_time >= min_lidar_time && max_filtered_id20_time <= max_lidar_time){
                        // Debug
                        // std::ostringstream oss;
                        // oss << std::fixed << std::setprecision(12);
                        // oss << "DataAlignment ID20 : Compass Time. Min: " << filtered_gnss_ID20_vec_data__.front().unixTime << ", Max: " << filtered_gnss_ID20_vec_data__.back().unixTime << ", Size: " << filtered_gnss_ID20_vec_data__.size();
                        // logMessage("LOGGING", oss.str());

                        LidarID20VecDataFrame temp_lidar_ID20_vec_data_;
                        temp_lidar_ID20_vec_data_.ID20Vec = std::move(filtered_gnss_ID20_vec_data__); // Use filtered data
                        temp_lidar_ID20_vec_data_.Lidar = temp_lidar_data__;

                        if (!lidar_ID20_buffer_.push(std::move(temp_lidar_ID20_vec_data_))) {
                            logMessage("WARNING", "DataAlignment ID20 : SPSC Lidar and ID20 Vec buffer push failed.");
                        }
                    }
                    
                } else if (min_lidar_time > min_id20_time && max_lidar_time > max_id20_time){
                    // Lidar is too new or partially overlaps; pop another newer IMU vector >> skip while
                    continue;
                } else {
                    // Lidar impossible to catch up with the IMU timestamp, need to discard this Lidar frame.
                    // Potential Solution, increase the size buffer frame.
                    logMessage("ERROR", "DataAlignment ID20 : Lidar cannot catch up with ID20 data, please increase Buffer size.");
                    break;                       
                }
            }
        } catch (const std::exception& e) {
            logMessage("ERROR", "DataAlignment ID20 : Exception occurred.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
    }
}

// -----------------------------------------------------------------------------

// void SLAMPipeline::runLioStateEstimation(const std::vector<int>& allowedCores){
//     setThreadAffinity(allowedCores);

//     try {

//     } catch (const std::exception& e) {

//     }
// }

// // -----------------------------------------------------------------------------

// void SLAMPipeline::runLioStateEstimation(const std::vector<int>& allowedCores){
//     setThreadAffinity(allowedCores);

//     try {

//     } catch (const std::exception& e) {

//     }
// }

// // -----------------------------------------------------------------------------

// void SLAMPipeline::runDynamicMapping(const std::vector<int>& allowedCores){
//     setThreadAffinity(allowedCores);

//     try {

//     } catch (const std::exception& e) {

//     }
// }



