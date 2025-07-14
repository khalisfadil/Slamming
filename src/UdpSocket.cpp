#include <UdpSocket.hpp>

UdpSocket::UdpSocket(boost::asio::io_context& context, const std::string& host, uint16_t port, std::function<void(const std::vector<uint8_t>&)> callback,uint32_t bufferSize)
    : socket_(context, boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string(host), port)), callback_(std::move(callback)) {
    
    buffer_.resize(bufferSize);
    startReceive();
}

UdpSocket::~UdpSocket() {
    stop();
    socket_.close();
}

void UdpSocket::startReceive() {
    socket_.async_receive_from(
        boost::asio::buffer(buffer_), senderEndpoint_,
        [this](const boost::system::error_code& ec, std::size_t bytesReceived) {
            if (!ec && bytesReceived > 0) {
                std::vector<uint8_t> packetData(buffer_.begin(), buffer_.begin() + bytesReceived);
                if (callback_) {callback_(packetData);}
            } else if (ec) {
                std::cerr << "Receive error: " << ec.message() << " (value: " << ec.value() << ")" << std::endl;
            } else {
                std::cerr << "No data received (bytes: " << bytesReceived << ")" << std::endl;
            }
            startReceive();
        }
    );
}

void UdpSocket::stop() {
    boost::system::error_code ec;
    socket_.close(ec); // Close the socket
    if (ec) {
        std::cerr << "Error closing socket: " << ec.message() << std::endl;
    }
}
