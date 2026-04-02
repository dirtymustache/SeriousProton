#ifndef NETWORK_AUDIOSTREAM_H
#define NETWORK_AUDIOSTREAM_H

#include <audio/source.h>
#include <memory>
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <vector>


struct OpusDecoder;
struct _SDL_AudioStream;
class NetworkAudioStream: public sp::audio::Source
{
public:
    NetworkAudioStream();
    ~NetworkAudioStream();

    void receivedPacketFromNetwork(const unsigned char* packet, int packet_size);
    void finalize();
    bool isFinished();
protected:
    // Inherited functions
    virtual void onMixSamples(int16_t* stream, int sample_count) override;

    //Members
    unsigned int sample_rate;
    std::mutex             samples_lock;
    std::vector<int16_t>   samples;
    int empty_mix_callbacks = 0;

    OpusDecoder* decoder = nullptr;
    _SDL_AudioStream* resample_stream = nullptr;
};

class NetworkAudioStreamManager
{
public:
    void start(int32_t id);
    void receivedPacketFromNetwork(int32_t id, const unsigned char* packet, int packet_size);
    void stop(int32_t id);

private:
    std::unordered_map<int32_t, std::unique_ptr<NetworkAudioStream>> streams;
};

#endif //NETWORK_AUDIOSTREAM_H
