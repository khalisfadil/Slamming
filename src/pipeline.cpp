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

SLAMPipeline::SLAMPipeline(const std::string& slam_registration, const std::string& odom_json_path, const std::string& lidar_json_path, const lidarDecode::OusterLidarCallback::LidarTransformPreset& T_preset) 
    : odometry_(stateestimate::Odometry::Get(slam_registration, odom_json_path)), // <-- INITIALIZE HERE, 
    lidarCallback_(lidar_json_path, T_preset) {// You can initialize other members here too
    temp_IMU_vec_data_.reserve(VECTOR_SIZE_IMU);
    odometry_->T_i_r_gt_poses.reserve(GT_SIZE_COMPASS);

#ifdef DEBUG
    logMessage("LOGGING", "SLAMPipeline and Odometry object initialized.");
#endif
}

// -----------------------------------------------------------------------------

void SLAMPipeline::signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        running_.store(false, std::memory_order_release);
        globalCV_.notify_all();

        // constexpr const char* message = "[signalHandler] Shutting down...\n";
        // constexpr size_t messageLen = sizeof(message) - 1;
        // ssize_t result = write(STDOUT_FILENO, message, messageLen);
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
#ifdef DEBUG
        std::ostringstream oss;
        oss << "Lidar Listener: Invalid host or port. Host: " << host << ", Port: " << port;
        logMessage("ERROR", oss.str());    
#endif
        return;
    }

    try {

        UdpSocket listener(ioContext, host, port, [&](const std::vector<uint8_t>& packet_data) {

            lidarDecode::LidarDataFrame temp_lidar_data_;
            lidarCallback_.decode_packet_single_return(packet_data, temp_lidar_data_);
           
            if (temp_lidar_data_.numberpoints > 0 && temp_lidar_data_.frame_id != this->frame_id_) {

                this->frame_id_ = temp_lidar_data_.frame_id;
                
                if (!lidar_buffer_.push(std::move(temp_lidar_data_))) {
#ifdef DEBUG
                    logMessage("WARNING", "Lidar Listener: SPSC Lidar buffer push failed."); 
#endif
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
#ifdef DEBUG
                 logMessage("WARNING", "Lidar Listener: Exception in ioContext."); 
#endif

                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart(); 
#ifdef DEBUG
                    logMessage("WARNING", "Lidar Listener: ioContext restarted.");
#endif
                } else {
                    break; // Exit loop if shutting down.
                }
            }
        }
    } catch(const std::exception& e){
#ifdef DEBUG
        logMessage("WARNING", "Lidar Listener: Setup exception.");
#endif
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }
#ifdef DEBUG
    logMessage("LOGGING", "Lidar Listener: listener stopped.");
#endif
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runOusterLidarListenerLegacy(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {
    
    setThreadAffinity(allowedCores); 

    if (host.empty() || port == 0) {
#ifdef DEBUG
        std::ostringstream oss;
        oss << "runOusterLidarListenerLegacy: Invalid host or port. Host: " << host << ", Port: " << port;
        logMessage("ERROR", oss.str());
#endif 
        return;
    }

    try {
        UdpSocket listener(ioContext, host, port, [&](const std::vector<uint8_t>& packet_data) {

            lidarDecode::LidarDataFrame temp_lidar_data_;
            lidarCallback_.decode_packet_legacy(packet_data, temp_lidar_data_);

            if (temp_lidar_data_.numberpoints > 0 && temp_lidar_data_.frame_id != this->frame_id_) {
                this->frame_id_ = temp_lidar_data_.frame_id;
                
                if (!lidar_buffer_.push(std::move(temp_lidar_data_))) {
#ifdef DEBUG
                    logMessage("WARNING", "runOusterLidarListenerLegacy: SPSC LidarDataFrame push failed.");
#endif
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
#ifdef DEBUG
                logMessage("ERROR", "runOusterLidarListenerLegacy: Exception in ioContext.");
#endif

                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart(); // Restart Asio io_context to attempt recovery.
#ifdef DEBUG
                    logMessage("LOGGING", "runOusterLidarListenerLegacy: ioContext restarted.");
#endif

                } else {
                    break; // Exit loop if shutting down.
                }
            }
        }
    } catch(const std::exception& e){
#ifdef DEBUG
        logMessage("ERROR", "runOusterLidarListenerLegacy: Setup exception.");
#endif
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }
#ifdef DEBUG
    logMessage("LOGGING", "runOusterLidarListenerLegacy: listener stopped."); 
#endif
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runOusterLidarIMUListener(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {
    
    setThreadAffinity(allowedCores); // Sets affinity for this listener thread

    if (host.empty() || port == 0) {
#ifdef DEBUG
        std::ostringstream oss;
        oss << "Lidar Listener: Invalid host or port. Host: " << host << ", Port: " << port;
        logMessage("ERROR", oss.str());
#endif
        return;
    }

    try {
        UdpSocket listener(ioContext, host, port,[&](const std::vector<uint8_t>& packet_data) {

            // Decode the packet into frame_data_IMU_copy
            lidarCallback_.decode_packet_LidarIMU(packet_data, temp_IMU_data_);

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
#ifdef DEBUG
                    logMessage("WARNING", "IMU Listener: SPSC IMU Vec buffer push failed.");
#endif 
                }

                // Push the copy into the SPSC queue
                if (!imu_buffer_.push(temp_IMU_data_)) {
#ifdef DEBUG
                    logMessage("WARNING", "IMU Listener: SPSC IMU buffer push failed."); 
#endif
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
#ifdef DEBUG
                logMessage("ERROR", "IMU Listener: Exception in ioContext."); 
#endif
                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart();
#ifdef DEBUG
                    logMessage("LOGGING", "IMU Listener: ioContext restarted.");  
#endif
                } else {
                    break; 
                }
            }
        }
    }
    catch(const std::exception& e){
#ifdef DEBUG
        logMessage("ERROR", "IMU Listener: Setup exception.");
#endif
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }
#ifdef DEBUG
    logMessage("LOGGING", "IMU Listener: listener stopped.");
#endif
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
#ifdef DEBUG
        std::ostringstream oss;
        oss << "runGNSSID20Listener: Invalid host or port. Host: " << host << ", Port: " << port;
        logMessage("ERROR", oss.str());
#endif
        return;
    }

    auto processGNSSID20Frames = [&]() {
        decodeNav::DataFrameID20 new_frame;
        if (gnss_intern_buffer_.pop(new_frame)) {
            if (new_frame.unixTime > 0 && new_frame.unixTime != this->unixTime) { // this part make sure no NAN value of new frame is readed.
                this->unixTime = new_frame.unixTime;
                
#ifdef DEBUG
                    // std::ostringstream oss;
                    // oss << "runGNSSID20Listener: Input Value. Latitude: " << new_frame.latitude << ", Longitude: " << new_frame.longitude << ", Altitude: " << new_frame.altitude;
                    // logMessage("LOGGING", oss.str());
#endif
                // **OPTIMIZATION 1: Use pop_front() on the deque.**
                // When full, remove the oldest element (O(1) operation).
                if (gnss_data_window_.size() >= DATA_SIZE_GNSS) {
                    gnss_data_window_.pop_front();
                }
                gnss_data_window_.push_back(new_frame);

                if (gnss_data_window_.size() == DATA_SIZE_GNSS) {
                    if (!gnss_window_buffer_.push(gnss_data_window_)) {
#ifdef DEBUG
                        logMessage("WARNING", "runGNSSID20Listener: SPSC ID20 Vec buffer push failed.");
#endif
                    }
                    if (!gnss_buffer_.push(new_frame)) {
#ifdef DEBUG
                    logMessage("WARNING", "runGNSSID20Listener: SPSC ID20 buffer push failed.");
#endif
                    }
                }
                
            }
        }
    };

    try {
        UdpSocket listener(ioContext, host, port,[&](const std::vector<uint8_t>& packet_data) {
            decodeNav::DataFrameID20 new_frame;
            gnssCallback_.decode_ID20(packet_data, new_frame);

            if (!gnss_intern_buffer_.push(new_frame)) {
#ifdef DEBUG
                logMessage("WARNING", "runGNSSID20Listener: SPSC ID20 intern buffer push failed.");
#endif
            }
        }, bufferSize);

        std::thread ioThread([&]() {
            // 2. Set affinity for this new I/O (producer) thread to the SAME core(s).
            setThreadAffinity(allowedCores);

            try {
                ioContext.run();
            } catch (const std::exception& e) {
#ifdef DEBUG
                logMessage("ERROR", "runGNSSID20Listener: Exception in I/O thread.");
#endif
            }
#ifdef DEBUG
            logMessage("LOGGING", "runGNSSID20Listener: I/O thread finished.");
#endif
        });

#ifdef DEBUG
        logMessage("LOGGING", "runGNSSID20Listener: Consumer loop started.");
#endif
        while (running_.load(std::memory_order_acquire)) {
            processGNSSID20Frames();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#ifdef DEBUG
        logMessage("LOGGING", "runGNSSID20Listener: Stopping I/O context.");
#endif
        if (!ioContext.stopped()) {
            ioContext.stop();
        }
        if (ioThread.joinable()) {
            ioThread.join();
        }

    }
    catch(const std::exception& e){
#ifdef DEBUG
        logMessage("ERROR", "runGNSSID20Listener: Setup exception.");
#endif
        if (!ioContext.stopped()) {
            ioContext.stop();
        }
    }
#ifdef DEBUG
    logMessage("LOGGING", "runGNSSID20Listener Listener stopped.");
#endif
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
#ifdef DEBUG
                logMessage("WARNING", "DataAlignment : Failed to retrieved Lidar SPSC."); 
#endif
                continue;
            }

            // Skip if lidar data is empty
            if (temp_lidar_data__.timestamp_points.empty()) {
#ifdef DEBUG
                logMessage("WARNING", "DataAlignment : Empty lidar data.");
#endif
                continue;
            }

            // Get min and max lidar timestamps (sorted, so use front and back)
            double min_lidar_time = temp_lidar_data__.timestamp_points.front();
            double max_lidar_time = temp_lidar_data__.timestamp_points.back();

            // Validate lidar timestamp range
            if (min_lidar_time > max_lidar_time) {
#ifdef DEBUG
                logMessage("ERROR", "DataAlignment : Invalid lidar timestamp.");
#endif
                continue;
            }

            // Loop to find an IMU vector that aligns with the current lidar frame
            bool aligned = false;

            while (!aligned && imu_vec_buffer_.read_available() > 0){

                std::vector<lidarDecode::LidarIMUDataFrame> temp_IMU_vec_data_;

                if (!imu_vec_buffer_.pop(temp_IMU_vec_data_)) {
#ifdef DEBUG
                    logMessage("WARNING", "DataAlignment : Failed to retrieved IMU vec SPSC.");
#endif
                    break;
                }

                // Skip if IMU data is empty
                if (temp_IMU_vec_data_.empty()) {
#ifdef DEBUG
                    logMessage("WARNING", "DataAlignment : Empty IMU vec data.");
#endif
                    continue;
                }

                // find min/max
                double min_imu_time = temp_IMU_vec_data_.front().Normalized_Timestamp_s;
                double max_imu_time = temp_IMU_vec_data_.back().Normalized_Timestamp_s;

                // Verify IMU timestamps are valid and ordered
                if (min_imu_time > max_imu_time) {
#ifdef DEBUG
                    logMessage("WARNING", "DataAlignment : Invalid IMU timestamp.");
#endif
                    continue;
                }

                // Check if lidar timestamps are within IMU range
                if (min_lidar_time >= min_imu_time && max_lidar_time <= max_imu_time) {
                    // Timestamps are aligned; process the data
                    aligned = true;
#ifdef DEBUG
                    logMessage("WARNING", "DataAlignment : Timestamps Lidar and IMU are aligned.");
#endif

                    LidarIMUVecDataFrame temp_lidar_IMU_vec_data_;
                    temp_lidar_IMU_vec_data_.IMUVec = temp_IMU_vec_data_;
                    temp_lidar_IMU_vec_data_.Lidar = temp_lidar_data__;

                    if (!lidar_imu_buffer_.push(std::move(temp_lidar_IMU_vec_data_))) {
#ifdef DEBUG
                        logMessage("WARNING", "DataAlignment : SPSC Lidar and IMU Vec buffer push failed.");
#endif
                    }
                    
                } else if (min_lidar_time > min_imu_time && max_lidar_time > max_imu_time){
                    // Lidar is too new or partially overlaps; pop another newer IMU vector >> skip while
                    continue;
                } else if (min_lidar_time < min_imu_time){
                    // Lidar impossible to catch up with the IMU timestamp, need to discard this Lidar frame.
                    // Potential Solution, increase the size buffer frame.
#ifdef DEBUG
                    logMessage("ERROR", "DataAlignment : Lidar cannot catch up with IMU data, please increase Buffer size.");
#endif
                    break;                       
                }
            }
        } catch (const std::exception& e) {
#ifdef DEBUG
            logMessage("ERROR", "DataAlignment : Exception occurred.");
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::dataAlignmentID20(const std::vector<int>& allowedCores) {
    setThreadAffinity(allowedCores);

    while (running_.load(std::memory_order_acquire)) {
        try {
            // 1. Pop a LiDAR frame from the buffer
            lidarDecode::LidarDataFrame lidar_frame;
            if (!lidar_buffer_.pop(lidar_frame) || lidar_frame.timestamp_points.empty()) {
#ifdef DEBUG
                // logMessage("WARNING", "dataAlignmentID20: Failed to retrieved LidarDataFrame SPSC."); 
#endif
                // If pop fails or the frame is empty, wait and try again
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // 2. Get the time range for the LiDAR scan
            // Data is sorted, so we only need the first and last points.
            const double min_lidar_time = lidar_frame.timestamp_points.front();
            const double max_lidar_time = lidar_frame.timestamp_points.back();

            if (min_lidar_time > max_lidar_time) {
#ifdef DEBUG
                logMessage("ERROR", "dataAlignmentID20: Invalid lidar timestamp range.");
#endif
                continue;
            }

            // 3. Find a corresponding IMU data packet
            bool aligned = false;
            while (!aligned && gnss_window_buffer_.read_available() > 0) {
                std::deque<decodeNav::DataFrameID20> gnss_window_packet;
                if (!gnss_window_buffer_.pop(gnss_window_packet) || gnss_window_packet.empty()) {
                    // If pop fails or packet is empty, break inner loop to try next IMU packet
#ifdef DEBUG
                // logMessage("WARNING", "dataAlignmentID20 : Failed to retrieved DataFrameID20 SPSC."); 
#endif
                    break;
                }

                const double min_gnss_time = gnss_window_packet.front().unixTime;
                const double max_gnss_time = gnss_window_packet.back().unixTime;

                if (min_gnss_time > max_gnss_time) {
#ifdef DEBUG
                    logMessage("WARNING", "dataAlignmentID20: Invalid IMU timestamp range in packet.");
#endif
                    continue; // Get the next IMU packet
                }

                // --- Core Alignment Logic ---

                // Case 1: The IMU packet completely envelops the LiDAR frame. This is the match we want.
                if (min_lidar_time >= min_gnss_time && max_lidar_time <= max_gnss_time) {
                    aligned = true;
#ifdef DEBUG
                    // logMessage("LOGGING", "dataAlignmentID20: Found alignment envelope.");
#endif
                    std::vector<decodeNav::DataFrameID20> filtered_gnss_window_packet;
                    
                    // Use std::copy_if for efficient filtering. It's clearer and often faster.
                    std::copy_if(gnss_window_packet.begin(), gnss_window_packet.end(), std::back_inserter(filtered_gnss_window_packet),
                        [&](const decodeNav::DataFrameID20& data) {
                            return data.unixTime >= min_lidar_time && data.unixTime <= max_lidar_time;
                        });

                    // CRITICAL: Only proceed if the filtered data is not empty.
                    if (!filtered_gnss_window_packet.empty()) {
                        LidarGnssWindowDataFrame combined_data;
                        combined_data.Lidar = std::move(lidar_frame); // Avoid copy with std::move
                        combined_data.GnssWindow = std::move(filtered_gnss_window_packet); // Avoid copy with std::move

#ifdef DEBUG
                    std::ostringstream oss1, oss2;
                    oss1 << std::fixed << std::setprecision(12);
                    oss1 << "dataAlignmentID20: Lidar timestamp start: " << combined_data.Lidar.timestamp << 
                    ", timestamp end: " << combined_data.Lidar.timestamp_end;
                    logMessage("LOGGING", oss1.str());
                    oss2 << std::fixed << std::setprecision(12);
                    oss2 <<"dataAlignmentID20: Gnss Window timestamp start: " << combined_data.GnssWindow.front().unixTime <<
                    ", timestamp end: " << combined_data.GnssWindow.back().unixTime;
                    logMessage("LOGGING", oss2.str());
#endif

                        if (!lidar_gnsswindow_buffer_.push(std::move(combined_data))) {
#ifdef DEBUG
                            logMessage("WARNING", "DataAlignment ID20: Failed to push combined data to buffer.");
#endif
                        }
                    } else {
#ifdef DEBUG
                        logMessage("WARNING", "DataAlignment ID20: Alignment envelope found, but no IMU points within LiDAR time span.");
#endif
                    }

                } 
                // Case 2: The IMU packet is older than the LiDAR frame. Discard this IMU packet and get the next one.
                else if (min_lidar_time > min_gnss_time && max_lidar_time > max_gnss_time) {
                    // This IMU packet is entirely before the LiDAR frame.
                    // Continue to pop newer IMU packets.
                    continue;
                } 
                // Case 3: The LiDAR frame is too old for this IMU packet. Discard the LiDAR frame.
                else { 
                    // This happens if min_lidar_time < min_imu_time.
                    // Since IMU packets are chronologically ordered, no future IMU packet will ever contain this
                    // old LiDAR frame. We must discard the current LiDAR frame and get a new one.
#ifdef DEBUG
                    logMessage("ERROR", "dataAlignmentID20: LiDAR frame is too old. Discarding LiDAR frame.");
#endif
                    break; // Exit inner loop to fetch a new lidar_frame.
                }
            }
        } catch (const std::exception& e) {
#ifdef DEBUG
            logMessage("ERROR", "dataAlignmentID20: Exception occurred: " + std::string(e.what()));
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runLioStateEstimation(const std::vector<int>& allowedCores){
    setThreadAffinity(allowedCores);
    tbb::global_control gc(tbb::global_control::max_allowed_parallelism, allowedCores.size());
    while (running_.load(std::memory_order_acquire)) {
        try {
            
            LidarGnssWindowDataFrame tempCombineddata;
            if (!lidar_gnsswindow_buffer_.pop(tempCombineddata)) {
#ifdef DEBUG
                // logMessage("WARNING", "runLioStateEstimation : Failed to retrieved LidarGnssWindowDataFrame SPSC."); 
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            } else {
#ifdef DEBUG
                // --- Start Timer ---
                auto start_time = std::chrono::high_resolution_clock::now();
#endif 

                if (!init_) {
                    // --- Handle the very first frame: Initialize with T_rm ---
                    decodeNav::DataFrameID20 currFrame = tempCombineddata.GnssWindow.back();

                    // 1. Calculate the rotation from robot to map (R_mr) as before. R navigation frame <- body
                    Eigen::Matrix3d Rb2m = navMath::Cb2n(navMath::getQuat(
                        currFrame.roll, currFrame.pitch, currFrame.yaw
                    ));

                    // 2. Efficiently get the inverse rotation (R_rm) via transpose.
                    // This calculates the rotation from map to robot.
                    Eigen::Matrix3d Rm2b = Rb2m.transpose();

                    // 3. Construct the T_rm transformation matrix.
                    // Since initial translation is zero, no other calculation is needed.
                    Eigen::Matrix4d Tm2b = Eigen::Matrix4d::Identity();
                    Tm2b.block<3, 3>(0, 0) = Rm2b;

                    // 4. Initialize the odometry with the T_rm pose.
                    //    (Assuming the odometry class is the one from the previous context that expects T_rm).
                    odometry_->initT(Tm2b);

                    init_ = true;
                    finalicp::traj::Time Time(currFrame.unixTime);

                    std::ostringstream oss;
                    oss << "runLioStateEstimation (ORIGIN): " << Time.nanosecs() << " " 
                    << currFrame.latitude << " " << currFrame.longitude << " " << currFrame.altitude 
                    << currFrame.roll << " " << currFrame.pitch << " " << currFrame.yaw; 
                    logMessage("LOGGING", oss.str());
                }

                stateestimate::DataFrame currDataFrame;
                std::vector<lidarDecode::Point3D> tempLidarframe = tempCombineddata.Lidar.toPoint3D();
                currDataFrame.timestamp = tempCombineddata.Lidar.timestamp;
                // Use tbb::parallel_invoke to run both conversion tasks concurrently
                tbb::parallel_invoke(
                    [&] {
                        // --- Task 1: Parallel LiDAR Point Cloud Conversion (still beneficial) ---
                        currDataFrame.pointcloud.resize(tempLidarframe.size());
                        
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, tempLidarframe.size()),
                            [&](const tbb::blocked_range<size_t>& r) {
                                for (size_t i = r.begin(); i != r.end(); ++i) {
                                    // ... (point cloud data assignment) ...
                                    currDataFrame.pointcloud[i].raw_pt = tempLidarframe[i].raw_pt;
                                    currDataFrame.pointcloud[i].pt = tempLidarframe[i].pt;
                                    currDataFrame.pointcloud[i].radial_velocity = tempLidarframe[i].radial_velocity;
                                    currDataFrame.pointcloud[i].alpha_timestamp = tempLidarframe[i].alpha_timestamp;
                                    currDataFrame.pointcloud[i].timestamp = tempLidarframe[i].timestamp;
                                    currDataFrame.pointcloud[i].beam_id = tempLidarframe[i].beam_id;
                                }
                            }
                        );
                    },
                    [&] {
                        // --- Task 2: Sequential IMU Data Conversion (more efficient for small N) ---
                        const auto& gnssWindowsource = tempCombineddata.GnssWindow;
                        currDataFrame.imu_data_vec.resize(gnssWindowsource.size());
                        
                        // A simple sequential loop is faster here due to low iteration count
                        for (size_t i = 0; i < gnssWindowsource.size(); ++i) {
                            currDataFrame.imu_data_vec[i].lin_acc = Eigen::Vector3d(
                                static_cast<double>(gnssWindowsource[i].accelX),
                                static_cast<double>(gnssWindowsource[i].accelY),
                                static_cast<double>(gnssWindowsource[i].accelZ)
                            );
                            currDataFrame.imu_data_vec[i].ang_vel = Eigen::Vector3d(
                                static_cast<double>(gnssWindowsource[i].angularVelocityX),
                                static_cast<double>(gnssWindowsource[i].angularVelocityY),
                                static_cast<double>(gnssWindowsource[i].angularVelocityZ)
                            );
                            currDataFrame.imu_data_vec[i].timestamp = gnssWindowsource[i].unixTime;
                        }
                    }
                ); // End of parallel_invoke

                // ################################# MAIN !!!!
                const auto summary = odometry_->registerFrame(currDataFrame);

                // --- Stop Timer ---
#ifdef DEBUG
                auto end_time = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);


                std::ostringstream oss_timer;
                oss_timer << "LIO frame processing time: " << duration.count() << " ms.";
                logMessage("TIMER", oss_timer.str());
#endif 

                if (!summary.success){
#ifdef DEBUG
                    logMessage("WARNING", "State estimation failed.");
#endif 
                }
            }
        } catch (const std::exception& e) {
#ifdef DEBUG
            logMessage("ERROR", "Exception in runLioStateEstimation: " + std::string(e.what()));
#endif 
        }
    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runLoStateEstimation(const std::vector<int>& allowedCores){
    setThreadAffinity(allowedCores);
    tbb::global_control gc(tbb::global_control::max_allowed_parallelism, allowedCores.size());
    while (running_.load(std::memory_order_acquire)) {
        try {
            
            LidarGnssWindowDataFrame tempCombineddata;
            if (!lidar_gnsswindow_buffer_.pop(tempCombineddata)) {
#ifdef DEBUG
                // logMessage("WARNING", "runLioStateEstimation : Failed to retrieved LidarGnssWindowDataFrame SPSC."); 
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            } else {
#ifdef DEBUG
                // --- Start Timer ---
                auto start_time = std::chrono::high_resolution_clock::now();
#endif 

                if (!init_) {
                    // --- Handle the very first frame: Initialize with T_rm ---
                    decodeNav::DataFrameID20 currFrame = tempCombineddata.GnssWindow.back();

                    // 1. Calculate the rotation from robot to map (R_mr) as before. R navigation frame <- body
                    Eigen::Matrix3d Rb2m = navMath::Cb2n(navMath::getQuat(
                        currFrame.roll, currFrame.pitch, currFrame.yaw
                    ));

                    // 2. Efficiently get the inverse rotation (R_rm) via transpose.
                    // This calculates the rotation from map to robot.
                    Eigen::Matrix3d Rm2b = Rb2m.transpose();

                    // 3. Construct the T_rm transformation matrix.
                    // Since initial translation is zero, no other calculation is needed.
                    Eigen::Matrix4d Tm2b = Eigen::Matrix4d::Identity();
                    Tm2b.block<3, 3>(0, 0) = Rm2b;

                    // 4. Initialize the odometry with the T_rm pose.
                    //    (Assuming the odometry class is the one from the previous context that expects T_rm).
                    odometry_->initT(Tm2b);

                    init_ = true;
                    finalicp::traj::Time Time(currFrame.unixTime);

                    std::ostringstream oss;
                    oss << "runLoStateEstimation (ORIGIN): " << Time.nanosecs() << " " 
                    << currFrame.latitude << " " << currFrame.longitude << " " << currFrame.altitude 
                    << currFrame.roll << " " << currFrame.pitch << " " << currFrame.yaw; 
                    logMessage("LOGGING", oss.str());
                }

                stateestimate::DataFrame currDataFrame;
                std::vector<lidarDecode::Point3D> tempLidarframe = tempCombineddata.Lidar.toPoint3D();
                currDataFrame.timestamp = tempCombineddata.Lidar.timestamp;
                // Use tbb::parallel_invoke to run both conversion tasks concurrently
                tbb::parallel_invoke(
                    [&] {
                        // --- Task 1: Parallel LiDAR Point Cloud Conversion (still beneficial) ---
                        currDataFrame.pointcloud.resize(tempLidarframe.size());
                        
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, tempLidarframe.size()),
                            [&](const tbb::blocked_range<size_t>& r) {
                                for (size_t i = r.begin(); i != r.end(); ++i) {
                                    // ... (point cloud data assignment) ...
                                    currDataFrame.pointcloud[i].raw_pt = tempLidarframe[i].raw_pt;
                                    currDataFrame.pointcloud[i].pt = tempLidarframe[i].pt;
                                    currDataFrame.pointcloud[i].radial_velocity = tempLidarframe[i].radial_velocity;
                                    currDataFrame.pointcloud[i].alpha_timestamp = tempLidarframe[i].alpha_timestamp;
                                    currDataFrame.pointcloud[i].timestamp = tempLidarframe[i].timestamp;
                                    currDataFrame.pointcloud[i].beam_id = tempLidarframe[i].beam_id;
                                }
                            }
                        );
                    },
                    [&] {
                        // --- Task 2: Sequential IMU Data Conversion (more efficient for small N) ---
                        const auto& gnssWindowsource = tempCombineddata.GnssWindow;
                        currDataFrame.imu_data_vec.resize(gnssWindowsource.size());
                        
                        // A simple sequential loop is faster here due to low iteration count
                        for (size_t i = 0; i < gnssWindowsource.size(); ++i) {
                            currDataFrame.imu_data_vec[i].lin_acc = Eigen::Vector3d(
                                static_cast<double>(gnssWindowsource[i].accelX),
                                static_cast<double>(gnssWindowsource[i].accelY),
                                static_cast<double>(gnssWindowsource[i].accelZ)
                            );
                            currDataFrame.imu_data_vec[i].ang_vel = Eigen::Vector3d(
                                static_cast<double>(gnssWindowsource[i].angularVelocityX),
                                static_cast<double>(gnssWindowsource[i].angularVelocityY),
                                static_cast<double>(gnssWindowsource[i].angularVelocityZ)
                            );
                            currDataFrame.imu_data_vec[i].timestamp = gnssWindowsource[i].unixTime;
                        }
                    }
                ); // End of parallel_invoke

                // ################################# MAIN !!!!
                const auto summary = odometry_->registerFrame(currDataFrame);

                // --- Stop Timer ---
#ifdef DEBUG
                auto end_time = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);


                std::ostringstream oss_timer;
                oss_timer << "LO frame processing time: " << duration.count() << " ms.";
                logMessage("TIMER", oss_timer.str());
#endif 

                if (!summary.success){
#ifdef DEBUG
                    logMessage("WARNING", "State estimation failed.");
#endif 
                }
            }
        } catch (const std::exception& e) {
#ifdef DEBUG
            logMessage("ERROR", "Exception in runLioStateEstimation: " + std::string(e.what()));
#endif 
        }
    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runGroundTruthEstimation(const std::string& filename, const std::vector<int>& allowedCores) {
    setThreadAffinity(allowedCores);

    std::ofstream outfile(filename);
    if (!outfile.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << "[" << std::put_time(std::gmtime(&now_time_t), "%Y-%m-%dT%H:%M:%SZ") << "] "
            << "[ERROR] failed to open file " << filename << " for writing.\n";
        std::cerr << oss.str();
        return;
    }

    while (running_.load(std::memory_order_acquire)) {
        try {
            decodeNav::DataFrameID20 currFrame;
            if (!gnss_buffer_.pop(currFrame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (is_firstFrame_) {
                // --- Handle the very first frame ---
                // 1. Calculate the rotation from robot to map (R_mr)
                Eigen::Matrix3d Rb2m = navMath::Cb2n(navMath::getQuat(
                    currFrame.roll, currFrame.pitch, currFrame.yaw
                ));

                // 2. Get the inverse rotation (R_rm) via transpose
                Eigen::Matrix3d Rm2b = Rb2m.transpose();

                // 3. Assemble T_rm (initial translation is zero)
                Eigen::Matrix4d Tm2b = Eigen::Matrix4d::Identity();
                Tm2b.block<3, 3>(0, 0) = Rm2b;

                // Set trackers for the next iteration
                originFrame_ = currFrame;
                is_firstFrame_ = false;

                // Output the original LLA data for the origin
                finalicp::traj::Time Time(currFrame.unixTime);
                outfile << std::fixed << std::setprecision(12) << Time.nanosecs() << " " 
                << currFrame.latitude << " " << currFrame.longitude << " " << currFrame.altitude << " "
                << currFrame.roll << " " << currFrame.pitch << " " << currFrame.yaw << "\n";

                
                outfile << std::fixed << std::setprecision(12) << Time.nanosecs() << " " 
                << Tm2b(0, 0) << " " << Tm2b(0, 1) << " " << Tm2b(0, 2) << " " << Tm2b(0, 3) << " "
                << Tm2b(1, 0) << " " << Tm2b(1, 1) << " " << Tm2b(1, 2) << " " << Tm2b(1, 3) << " "
                << Tm2b(2, 0) << " " << Tm2b(2, 1) << " " << Tm2b(2, 2) << " " << Tm2b(2, 3) << " "
                << Tm2b(3, 0) << " " << Tm2b(3, 1) << " " << Tm2b(3, 2) << " " << Tm2b(3, 3) << "\n";
                
            } else {
                // --- Handle all subsequent frames ---
                // OPTIMIZATION 1: Avoid re-calculating the previous rotation matrix
                Eigen::Matrix3d Rb2m = navMath::Cb2n(navMath::getQuat(
                    currFrame.roll, currFrame.pitch, currFrame.yaw
                ));

                Eigen::Vector3d tb2m = navMath::LLA2NED(    // recheck if it is true b2m
                    currFrame.latitude, currFrame.longitude, currFrame.altitude,
                    originFrame_.latitude, originFrame_.longitude, originFrame_.altitude
                );

                // 2. Efficiently compute the inverse components (for Tm2b)
                Eigen::Matrix3d Rm2b = Rb2m.transpose();
                Eigen::Vector3d tm2b = -Rm2b * tb2m; // tm2b = -Rb2m^T * tb2m

                // 3. Assemble the final T_rm matrix
                Eigen::Matrix4d Tm2b = Eigen::Matrix4d::Identity();
                Tm2b.block<3, 3>(0, 0) = Rm2b;
                Tm2b.block<3, 1>(0, 3) = tm2b;
                
                odometry_->T_i_r_gt_poses.push_back(Tm2b);

                std::ostringstream oss;
                finalicp::traj::Time Time(currFrame.unixTime);

                outfile << std::fixed << std::setprecision(12) << Time.nanosecs() << " " 
                << Tm2b(0, 0) << " " << Tm2b(0, 1) << " " << Tm2b(0, 2) << " " << Tm2b(0, 3) << " "
                << Tm2b(1, 0) << " " << Tm2b(1, 1) << " " << Tm2b(1, 2) << " " << Tm2b(1, 3) << " "
                << Tm2b(2, 0) << " " << Tm2b(2, 1) << " " << Tm2b(2, 2) << " " << Tm2b(2, 3) << " "
                << Tm2b(3, 0) << " " << Tm2b(3, 1) << " " << Tm2b(3, 2) << " " << Tm2b(3, 3) << "\n";

            }
        } catch (const std::exception& e) {
#ifdef DEBUG
            logMessage("ERROR", "Exception in runGroundTruthEstimation: " + std::string(e.what()));
#endif 
        }
    }
    outfile.flush(); // Ensure data is written
    outfile.close();
}

// -----------------------------------------------------------------------------

void SLAMPipeline::saveOdometryResults(const std::string& timestamp) {
    if (odometry_) {
        odometry_->saveResults(timestamp);
    }
}