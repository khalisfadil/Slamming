#include <pipeline.hpp>

using json = nlohmann::json;

std::atomic<bool> SLAMPipeline::running_{true};
std::condition_variable SLAMPipeline::globalCV_;
boost::lockfree::spsc_queue<LidarDataFrame, boost::lockfree::capacity<128>> SLAMPipeline::decodedPoint_buffer_;
boost::lockfree::spsc_queue<std::vector<LidarIMUDataFrame>, boost::lockfree::capacity<128>> SLAMPipeline::decodedLidarIMU_buffer_;


// -----------------------------------------------------------------------------
SLAMPipeline::SLAMPipeline(const std::string& odom_json_path, const std::string& lidar_json_path) 
: lioOdometry(odom_json_path), lidarCallback(lidar_json_path){
    frame_buffer_IMU_vec.reserve(VECTOR_SIZE_IMU);
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

void SLAMPipeline::runOusterLidarListenerSingleReturn(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {
    
    setThreadAffinity(allowedCores); // Sets affinity for this listener thread

    if (host.empty() || port == 0) {
        std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
        std::cerr << "[SLAMPipeline] Listener: Invalid host or port. Host: " << host << ", Port: " << port << std::endl;
        return;
    }

    try {
    UdpSocket listener(ioContext, host, port,
        // Lambda callback:
        [&](const std::vector<uint8_t>& packet_data) {
            // LidarDataFrame frame_data_copy; // 1. Local DataFrame created.
                                       //    It will hold a deep copy of the lidar data.

            // 2. lidarCallback processes the packet.
            //    Inside decode_packet_single_return, frame_data_copy is assigned
            //    (via DataFrame::operator=) the contents of lidarCallback's completed buffer.
            //    This results in a deep copy into frame_data_copy.
            lidarCallback.decode_packet_single_return(packet_data, frame_data_copy_);

            // 3. Now frame_data_copy is an independent, deep copy of the relevant frame.
            //    We can safely use it and then move it into the queue.
            if (frame_data_copy_.numberpoints > 0 && frame_data_copy_.frame_id != this->frame_id_) {
                this->frame_id_ = frame_data_copy_.frame_id;
                
                // 4. Move frame_data_copy into the SPSC queue.
                //    This transfers ownership of frame_data_copy's internal resources (vector data)
                //    to the element constructed in the queue, avoiding another full copy.
                //    frame_data_copy is left in a valid but unspecified (likely empty) state.
                if (!decodedPoint_buffer_.push(std::move(frame_data_copy_))) {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "[SLAMPipeline] Listener: SPSC buffer push failed for frame " 
                              << this->frame_id_ // Use this->frame_id_ as frame_data_copy might be moved-from
                              << ". Buffer Lidar Points might be full." << std::endl;
                }
            }
            // frame_data_copy goes out of scope here. If it was moved, its destruction is trivial.
            // If it was not pushed (e.g., due to condition not met), it's destructed normally (releasing its copied data).
        }, // End of lambda
        bufferSize);

        // Main loop to run Asio's I/O event processing.
        while (running_.load(std::memory_order_acquire)) {
            try {
                ioContext.run(); // This will block until work is done or ioContext is stopped.
                                 // If it returns without an exception, it implies all work is done.
                if (!running_.load(std::memory_order_acquire)) { // Check running_ again if run() returned cleanly
                    break;
                }
                // If run() returns and there's still potentially work (or to handle stop signals),
                // you might need to reset and run again, or break if shutting down.
                // For a continuous listener, run() might not return unless stopped or an error occurs.
                // If ioContext.run() returns because it ran out of work, and we are still 'running_',
                // we should probably restart it if the intent is to keep listening.
                // However, typically for a server/listener, io_context.run() is expected to block until stop() is called.
                // If it returns prematurely, ensure io_context is reset if needed before next run() call.
                // For this pattern, if run() returns, we break, assuming stop() was called elsewhere or an error occurred.
                break; 
            } catch (const std::exception& e) {
                // Handle exceptions from ioContext.run()
                std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
                std::cerr << "[SLAMPipeline] Listener: Exception in ioContext.run(): " << e.what() << std::endl;
                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart(); // Restart Asio io_context to attempt recovery.
                    std::cerr << "[SLAMPipeline] Listener: ioContext restarted." << std::endl;
                } else {
                    break; // Exit loop if shutting down.
                }
            }
        }
    } catch(const std::exception& e){
        // Handle exceptions from UdpSocket creation or other setup.
        std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
        std::cerr << "[SLAMPipeline] Listener: Setup exception: " << e.what() << std::endl;
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }
    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cerr << "[SLAMPipeline] Ouster LiDAR listener stopped." << std::endl;
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runOusterLidarListenerLegacy(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {
    
    setThreadAffinity(allowedCores); // Sets affinity for this listener thread

    if (host.empty() || port == 0) {
        std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
        std::cerr << "[SLAMPipeline] Listener: Invalid host or port. Host: " << host << ", Port: " << port << std::endl;
        return;
    }

    try {
        UdpSocket listener(ioContext, host, port,
        // Lambda callback:
        [&](const std::vector<uint8_t>& packet_data) {
            // LidarDataFrame frame_data_copy; // 1. Local DataFrame created.
                                       //    It will hold a deep copy of the lidar data.

            // 2. lidarCallback processes the packet.
            //    Inside decode_packet_single_return, frame_data_copy is assigned
            //    (via DataFrame::operator=) the contents of lidarCallback's completed buffer.
            //    This results in a deep copy into frame_data_copy.
            lidarCallback.decode_packet_legacy(packet_data, frame_data_copy_);

            // 3. Now frame_data_copy is an independent, deep copy of the relevant frame.
            //    We can safely use it and then move it into the queue.
            if (frame_data_copy_.numberpoints > 0 && frame_data_copy_.frame_id != this->frame_id_) {
                this->frame_id_ = frame_data_copy_.frame_id;
                
                // 4. Move frame_data_copy into the SPSC queue.
                //    This transfers ownership of frame_data_copy's internal resources (vector data)
                //    to the element constructed in the queue, avoiding another full copy.
                //    frame_data_copy is left in a valid but unspecified (likely empty) state.
                if (!decodedPoint_buffer_.push(std::move(frame_data_copy_))) {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "[SLAMPipeline] Listener: SPSC buffer push failed for frame " 
                              << this->frame_id_ // Use this->frame_id_ as frame_data_copy might be moved-from
                              << ". Buffer Lidar Points might be full." << std::endl;
                }
            }
            // frame_data_copy goes out of scope here. If it was moved, its destruction is trivial.
            // If it was not pushed (e.g., due to condition not met), it's destructed normally (releasing its copied data).
        }, // End of lambda
        bufferSize);

        // Main loop to run Asio's I/O event processing.
        while (running_.load(std::memory_order_acquire)) {
            try {
                ioContext.run(); // This will block until work is done or ioContext is stopped.
                                 // If it returns without an exception, it implies all work is done.
                if (!running_.load(std::memory_order_acquire)) { // Check running_ again if run() returned cleanly
                    break;
                }
                // If run() returns and there's still potentially work (or to handle stop signals),
                // you might need to reset and run again, or break if shutting down.
                // For a continuous listener, run() might not return unless stopped or an error occurs.
                // If ioContext.run() returns because it ran out of work, and we are still 'running_',
                // we should probably restart it if the intent is to keep listening.
                // However, typically for a server/listener, io_context.run() is expected to block until stop() is called.
                // If it returns prematurely, ensure io_context is reset if needed before next run() call.
                // For this pattern, if run() returns, we break, assuming stop() was called elsewhere or an error occurred.
                break; 
            } catch (const std::exception& e) {
                // Handle exceptions from ioContext.run()
                std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
                std::cerr << "[SLAMPipeline] Listener: Exception in ioContext.run(): " << e.what() << std::endl;
                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart(); // Restart Asio io_context to attempt recovery.
                    std::cerr << "[SLAMPipeline] Listener: ioContext restarted." << std::endl;
                } else {
                    break; // Exit loop if shutting down.
                }
            }
        }
    } catch(const std::exception& e){
        // Handle exceptions from UdpSocket creation or other setup.
        std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
        std::cerr << "[SLAMPipeline] Listener: Setup exception: " << e.what() << std::endl;
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }
    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cerr << "[SLAMPipeline] Ouster LiDAR listener stopped." << std::endl;
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runOusterLidarIMUListener(boost::asio::io_context& ioContext,
                                      const std::string& host,
                                      uint16_t port,
                                      uint32_t bufferSize,
                                      const std::vector<int>& allowedCores) {
    
    setThreadAffinity(allowedCores); // Sets affinity for this listener thread

    if (host.empty() || port == 0) {
        std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
        std::cerr << "[Pipeline] Listener: Invalid host or port. Host: " << host << ", Port: " << port << std::endl;
        return;
    }

    try {
    UdpSocket listener(ioContext, host, port,
        // Lambda callback:
        [&](const std::vector<uint8_t>& packet_data) {
            LidarIMUDataFrame frame_data_IMU_copy; // Local DataFrame created

            // Decode the packet into frame_data_IMU_copy
            lidarCallback.decode_packet_LidarIMU(packet_data, frame_data_IMU_copy);

            // Check if the frame is valid
            if (frame_data_IMU_copy.Normalized_Timestamp_s > 0 && 
                frame_data_IMU_copy.Normalized_Timestamp_s != this->Normalized_Timestamp_s_ ) {

                this->Normalized_Timestamp_s_ = frame_data_IMU_copy.Normalized_Timestamp_s;

                // If the vector is full, remove the oldest frame
                if (frame_buffer_IMU_vec.size() >= VECTOR_SIZE_IMU) {
                    frame_buffer_IMU_vec.erase(frame_buffer_IMU_vec.begin()); // Remove the oldest element
                }

                // Push the new frame into the vector
                frame_buffer_IMU_vec.push_back(frame_data_IMU_copy); // Deep copy into vector
                
                // Push the copy into the SPSC queue
                if (!decodedLidarIMU_buffer_.push(frame_buffer_IMU_vec)) {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "[Pipeline] Listener: SPSC buffer push failed for frame. Buffer Lidar(IMU) might be full." << std::endl;
                }
            }
            // frame_data_copy goes out of scope here. If it was moved, its destruction is trivial.
            // If it was not pushed (e.g., due to condition not met), it's destructed normally (releasing its copied data).
        }, // End of lambda
        bufferSize);

        // Main loop to run Asio's I/O event processing.
        while (running_.load(std::memory_order_acquire)) {
            try {
                ioContext.run(); // This will block until work is done or ioContext is stopped.
                                 // If it returns without an exception, it implies all work is done.
                if (!running_.load(std::memory_order_acquire)) { // Check running_ again if run() returned cleanly
                    break;
                }
                // If run() returns and there's still potentially work (or to handle stop signals),
                // you might need to reset and run again, or break if shutting down.
                // For a continuous listener, run() might not return unless stopped or an error occurs.
                // If ioContext.run() returns because it ran out of work, and we are still 'running_',
                // we should probably restart it if the intent is to keep listening.
                // However, typically for a server/listener, io_context.run() is expected to block until stop() is called.
                // If it returns prematurely, ensure io_context is reset if needed before next run() call.
                // For this pattern, if run() returns, we break, assuming stop() was called elsewhere or an error occurred.
                break; 
            } catch (const std::exception& e) {
                // Handle exceptions from ioContext.run()
                std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
                std::cerr << "[Pipeline] Listener: Exception in ioContext.run(): " << e.what() << std::endl;
                if (running_.load(std::memory_order_acquire)) {
                    ioContext.restart(); // Restart Asio io_context to attempt recovery.
                    std::cerr << "[Pipeline] Listener: ioContext restarted." << std::endl;
                } else {
                    break; // Exit loop if shutting down.
                }
            }
        }
    }
    catch(const std::exception& e){
        // Handle exceptions from UdpSocket creation or other setup.
        std::lock_guard<std::mutex> lock(consoleMutex); // Protect std::cerr
        std::cerr << "[Pipeline] Listener: Setup exception: " << e.what() << std::endl;
    }

    // Ensure ioContext is stopped when the listener is done or an error occurs.
    if (!ioContext.stopped()) {
        ioContext.stop();
    }
    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cerr << "[Pipeline] Ouster LiDAR(IMU) listener stopped." << std::endl;
}

// -----------------------------------------------------------------------------

void SLAMPipeline::dataAlignment(const std::vector<int>& allowedCores){

    setThreadAffinity(allowedCores);

    while (running_.load(std::memory_order_acquire)) {
        try {
            // If no lidar data is available, wait briefly and retry
            if (decodedPoint_buffer_.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            LidarDataFrame temp_LidarData;
            if (!decodedPoint_buffer_.pop(temp_LidarData)) {
                std::lock_guard<std::mutex> lock(consoleMutex);
                std::cerr << "[Pipeline] DataAlignment: Failed to pop from decodedPoint_buffer_." << std::endl;
                continue;
            }

            // Skip if lidar data is empty
            if (temp_LidarData.timestamp_points.empty()) {
                std::lock_guard<std::mutex> lock(consoleMutex);
                std::cerr << "[Pipeline] DataAlignment: Empty lidar points for frame ID " << temp_LidarData.frame_id << "." << std::endl;
                continue;
            }

            // Get min and max lidar timestamps (sorted, so use front and back)
            double min_lidar_time = temp_LidarData.timestamp_points.front();
            double max_lidar_time = temp_LidarData.timestamp_points.back();

            // Validate lidar timestamp range
            if (min_lidar_time > max_lidar_time) {
                std::lock_guard<std::mutex> lock(consoleMutex);
                std::cerr << "[Pipeline] DataAlignment: Invalid lidar timestamp range: min "
                          << min_lidar_time << " > max " << max_lidar_time 
                          << " for frame ID " << temp_LidarData.frame_id << "." << std::endl;
                continue;
            }

            // Loop to find an IMU vector that aligns with the current lidar frame
            bool aligned = false;
            while (!aligned){
                std::vector<LidarIMUDataFrame> temp_LidarIMUDataVec;
                if (!decodedLidarIMU_buffer_.pop(temp_LidarIMUDataVec)) {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "[Pipeline] DataAlignment: Failed to pop from decodedLidarIMU_buffer_." << std::endl;
                    break;
                }

                // Skip if IMU data is empty
                if (temp_LidarIMUDataVec.empty()) {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "[Pipeline] DataAlignment: Empty IMU data vector for lidar frame ID " 
                              << temp_LidarData.frame_id << "." << std::endl;
                    continue;
                }

                // find min/max
                double min_imu_time = temp_LidarIMUDataVec.front().Normalized_Timestamp_s;
                double max_imu_time = temp_LidarIMUDataVec.back().Normalized_Timestamp_s;

                // Verify IMU timestamps are valid and ordered
                if (min_imu_time > max_imu_time) {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "[Pipeline] DataAlignment: Invalid IMU timestamp range: min "
                              << min_imu_time << " > max " << max_imu_time 
                              << " for lidar frame ID " << temp_LidarData.frame_id << "." << std::endl;
                    continue;
                }

                // Check if lidar timestamps are within IMU range
                if (min_lidar_time >= min_imu_time && max_lidar_time <= max_imu_time) {
                    // Timestamps are aligned; process the data
                    aligned = true;
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cout << "[Pipeline] DataAlignment: Timestamps aligned for frame ID " 
                              << temp_LidarData.frame_id << ". Lidar range: [" 
                              << min_lidar_time << ", " << max_lidar_time << "], IMU range: ["
                              << min_imu_time << ", " << max_imu_time << "]" << std::endl;
                    // Add processing logic here (e.g., store aligned data, interpolate IMU, pass to lioOdometry)
                    // todo>>
                
                } else if (min_lidar_time > min_imu_time && max_lidar_time > max_imu_time){
                    // Lidar is too new or partially overlaps; pop another newer IMU vector >> skip while
                    continue;
                } else {
                    // Lidar impossible to catch up with the IMU timestamp, need to discard this Lidar frame.
                    // Potential Solution, increase the size buffer frame.
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "[Pipeline] DataAlignment: Lidar cannot catch up with IMU data, please increase Buffer size" << std::endl;
                    break;
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(consoleMutex);
            std::cerr << "[Pipeline] DataAlignment: Exception occurred: " << e.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runLioStateEstimation(const std::vector<int>& allowedCores){
    setThreadAffinity(allowedCores);

    try {

    } catch (const std::exception& e) {

    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runLioStateEstimation(const std::vector<int>& allowedCores){
    setThreadAffinity(allowedCores);

    try {

    } catch (const std::exception& e) {

    }
}

// -----------------------------------------------------------------------------

void SLAMPipeline::runDynamicMapping(const std::vector<int>& allowedCores){
    setThreadAffinity(allowedCores);

    try {

    } catch (const std::exception& e) {

    }
}



