#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <iostream>
#include <iomanip>
#include <boost/asio.hpp>

class UdpSocket {
    public:
        UdpSocket(boost::asio::io_context& context, const std::string& host, uint16_t port, std::function<void(const std::vector<uint8_t>&)> callback,uint32_t bufferSize = 24832);
        ~UdpSocket();
        void startReceive();
        void stop();

    private:
        boost::asio::ip::udp::socket socket_;
        boost::asio::ip::udp::endpoint senderEndpoint_;
        std::vector<uint8_t> buffer_;
        std::function<void(const std::vector<uint8_t>&)> callback_;
};