#include <io/network/websocketStreamSocket.h>

#include <algorithm>

namespace sp {
namespace io {
namespace network {

bool WebsocketStreamSocket::connect(const string& url)
{
    clearQueue();
    receive_buffer.clear();
    receive_offset = 0;
    return websocket.connect(url);
}

void WebsocketStreamSocket::close()
{
    websocket.close();
    clearQueue();
    receive_buffer.clear();
    receive_offset = 0;
}

StreamSocket::State WebsocketStreamSocket::getState()
{
    if (websocket.isConnected())
        return State::Connected;
    if (websocket.isConnecting())
        return State::Connecting;
    return State::Closed;
}

size_t WebsocketStreamSocket::_send(const void* data, size_t size)
{
    if (getState() != State::Connected)
        return 0;

    io::DataBuffer buffer;
    buffer.appendRaw(data, size);
    websocket.send(buffer);
    return size;
}

size_t WebsocketStreamSocket::_receive(void* data, size_t size)
{
    if (receive_offset >= receive_buffer.size())
    {
        io::DataBuffer buffer;
        if (!websocket.receive(buffer))
            return 0;
        receive_buffer.resize(buffer.getDataSize());
        std::copy_n(reinterpret_cast<const uint8_t*>(buffer.getData()), buffer.getDataSize(), receive_buffer.begin());
        receive_offset = 0;
    }

    auto available = receive_buffer.size() - receive_offset;
    auto amount = std::min(size, available);
    std::copy_n(receive_buffer.data() + receive_offset, amount, reinterpret_cast<uint8_t*>(data));
    receive_offset += amount;
    if (receive_offset >= receive_buffer.size())
    {
        receive_buffer.clear();
        receive_offset = 0;
    }
    return amount;
}

}//namespace network
}//namespace io
}//namespace sp
