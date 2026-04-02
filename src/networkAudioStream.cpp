#include "networkAudioStream.h"
#include "logging.h"

#include <SDL.h>
#include <array>
#include <opus.h>

NetworkAudioStream::NetworkAudioStream()
{
    sample_rate = sp::audio::Source::getOutputSampleRate();
    
    //Reserve 10 seconds of playback in our buffers.
    samples.reserve(sample_rate * 10);
    
    int error = 0;
    decoder = opus_decoder_create(48000, 1, &error);
    resample_stream = SDL_NewAudioStream(AUDIO_S16SYS, 1, 48000, AUDIO_S16SYS, 1, sample_rate);
}

NetworkAudioStream::~NetworkAudioStream()
{
    if (decoder)
        opus_decoder_destroy(decoder);
    if (resample_stream)
        SDL_FreeAudioStream(resample_stream);
}

void NetworkAudioStream::onMixSamples(int16_t* stream, int sample_count)
{
    std::lock_guard<std::mutex> guard(samples_lock);    //Get exclusive access to the samples vector.

    //Copy all new samples to the playback buffer. And clear our sample buffer.
    int mix_count = std::min(sample_count / 2, int(samples.size()));
    for(int index=0; index<mix_count; index++) {
        int sample = samples[index];
        mix(stream[index*2+0], sample);
        mix(stream[index*2+1], sample);
    }
    samples.erase(samples.begin(), samples.begin() + mix_count);

    if (samples.empty())
    {
        // Brief underruns are common in browser/networked audio; tolerate a few
        // callbacks of silence before stopping to avoid scratchy stop/start churn.
        empty_mix_callbacks++;
        if (empty_mix_callbacks >= 6)
            stop();
    }
    else
    {
        empty_mix_callbacks = 0;
    }
}

void NetworkAudioStream::receivedPacketFromNetwork(const unsigned char* packet, int packet_size)
{
    std::array<int16_t, 2880> samples_buffer{};
    int sample_count = opus_decode(decoder, packet, packet_size, samples_buffer.data(), static_cast<int>(samples_buffer.size()), 0);
    if (sample_count > 0)
    {
        std::vector<int16_t> resampled;
        if (resample_stream)
        {
            if (SDL_AudioStreamPut(resample_stream, samples_buffer.data(), sample_count * int(sizeof(int16_t))) < 0)
            {
                LOG(ERROR) << "voice recv: SDL_AudioStreamPut failed: " << SDL_GetError();
                return;
            }
            const int available_bytes = SDL_AudioStreamAvailable(resample_stream);
            if (available_bytes > 0)
            {
                resampled.resize(static_cast<size_t>(available_bytes / int(sizeof(int16_t))));
                const int received_bytes = SDL_AudioStreamGet(resample_stream, resampled.data(), available_bytes);
                if (received_bytes <= 0)
                {
                    resampled.clear();
                }
                else
                {
                    resampled.resize(static_cast<size_t>(received_bytes / int(sizeof(int16_t))));
                }
            }
        }
        else
        {
            resampled.reserve((sample_count * sample_rate + 47999) / 48000);
            for (int out_index = 0; ; ++out_index)
            {
                const int src_numerator = out_index * 48000;
                const int src_index = src_numerator / sample_rate;
                if (src_index >= sample_count)
                    break;

                const int next_index = std::min(src_index + 1, sample_count - 1);
                const int remainder = src_numerator % sample_rate;
                const int16_t a = samples_buffer[src_index];
                const int16_t b = samples_buffer[next_index];
                const int mixed = (int(a) * (sample_rate - remainder) + int(b) * remainder) / sample_rate;
                resampled.push_back(static_cast<int16_t>(mixed));
            }
        }

        std::lock_guard<std::mutex> guard(samples_lock);
        this->samples.insert(this->samples.end(), resampled.begin(), resampled.end());
        const int target_buffer_samples = sample_rate / 12; // ~80ms
        const int max_buffer_samples = sample_rate / 6;     // ~160ms
        if (static_cast<int>(this->samples.size()) > max_buffer_samples)
        {
            const auto drop_count = this->samples.size() - target_buffer_samples;
            this->samples.erase(this->samples.begin(), this->samples.begin() + static_cast<std::ptrdiff_t>(drop_count));
        }
        empty_mix_callbacks = 0;
    }
    // Start after ~60ms of buffered voice to smooth jitter without adding too much latency.
    if (this->samples.size() >= (sample_rate / 16) && !isPlaying())
    {
        start();
    }
}

void NetworkAudioStream::finalize()
{
    if (this->samples.size() > 0 && !isPlaying())
    {
        start();
    }
}

bool NetworkAudioStream::isFinished()
{
    return !isPlaying() && this->samples.size() == 0;
}

void NetworkAudioStreamManager::start(int32_t id)
{
    streams[id] = std::unique_ptr<NetworkAudioStream>(new NetworkAudioStream());
}

void NetworkAudioStreamManager::receivedPacketFromNetwork(int32_t id, const unsigned char* packet, int packet_size)
{
    auto it = streams.find(id);
    if (it == streams.end())
        return;
    it->second->receivedPacketFromNetwork(packet, packet_size);
}

void NetworkAudioStreamManager::stop(int32_t id)
{
    auto it = streams.find(id);
    if (it == streams.end())
        return;
    it->second->finalize();
}
