#ifndef SP2_IO_NETWORK_WEBSOCKET_STREAM_SOCKET_H
#define SP2_IO_NETWORK_WEBSOCKET_STREAM_SOCKET_H

#include <io/http/websocket.h>
#include <io/network/streamSocket.h>

namespace sp {
namespace io {
namespace network {

class WebsocketStreamSocket : public StreamSocket
{
public:
    WebsocketStreamSocket() = default;

    bool connect(const string& url);
    virtual void close() override;
    virtual State getState() override;

protected:
    virtual size_t _send(const void* data, size_t size) override;
    virtual size_t _receive(void* data, size_t size) override;

private:
    http::Websocket websocket;
    std::vector<uint8_t> receive_buffer;
    size_t receive_offset = 0;
};

}//namespace network
}//namespace io
}//namespace sp

#endif//SP2_IO_NETWORK_WEBSOCKET_STREAM_SOCKET_H
