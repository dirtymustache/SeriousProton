#include <string.h>
#include <string>

#include "networkRecorder.h"
#include "multiplayer.h"
#include "multiplayer_client.h"
#include "multiplayer_internal.h"
#include "logging.h"

#include <SDL.h>
#include <opus.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
static void browserDiag(const std::string& message)
{
    EM_ASM({
        if (typeof window.EmptyEpsilonDiag === "function")
            window.EmptyEpsilonDiag(UTF8ToString($0));
    }, message.c_str());
}
#endif

static SDL_AudioDeviceID record_device_id;
static SDL_AudioStream* record_audio_stream;
static NetworkAudioRecorder* active_recorder;

NetworkAudioRecorder::NetworkAudioRecorder()
{
    active_recorder = this;
}

NetworkAudioRecorder::~NetworkAudioRecorder()
{
    if (record_device_id != 0)
    {
        SDL_PauseAudioDevice(record_device_id, 1);
        SDL_CloseAudioDevice(record_device_id);
        record_device_id = 0;
    }
    if (record_audio_stream)
    {
        SDL_FreeAudioStream(record_audio_stream);
        record_audio_stream = nullptr;
    }

    if (encoder)
    {
        opus_encoder_destroy(encoder);
    }
    active_recorder = nullptr;
}

void NetworkAudioRecorder::addKeyActivation(sp::io::Keybinding* key, int target_identifier)
{
    keys.push_back({key, target_identifier});
}

bool NetworkAudioRecorder::ensureRecordingDeviceOpened()
{
    if (record_device_id != 0)
        return true;

    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0)
    {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        {
            LOG(ERROR) << "Failed to initialize SDL audio subsystem for voice capture: " << SDL_GetError();
#ifdef __EMSCRIPTEN__
            browserDiag("voice: SDL audio subsystem init failed");
#endif
            return false;
        }
    }

    SDL_AudioSpec want, obtained;
    memset(&want, 0, sizeof(want));
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.samples = frame_size;
    want.channels = 1;
    want.callback = &NetworkAudioRecorder::SDLCallback;

    record_device_id = SDL_OpenAudioDevice(nullptr, true, &want, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (record_device_id == 0)
    {
        LOG(ERROR) << "Failed to open voice capture device: " << SDL_GetError();
#ifdef __EMSCRIPTEN__
        browserDiag("voice: failed to open capture device");
#endif
        return false;
    }

    record_audio_stream = SDL_NewAudioStream(obtained.format, obtained.channels, obtained.freq, AUDIO_S16SYS, 1, 48000);
    if (!record_audio_stream)
    {
        LOG(ERROR) << "Failed to create voice capture conversion stream: " << SDL_GetError();
        SDL_CloseAudioDevice(record_device_id);
        record_device_id = 0;
#ifdef __EMSCRIPTEN__
        browserDiag("voice: failed to create capture conversion stream");
#endif
        return false;
    }

    LOG(INFO) << "Voice capture opened freq=" << obtained.freq << " channels=" << int(obtained.channels) << " samples=" << obtained.samples << " format=" << int(obtained.format);
#ifdef __EMSCRIPTEN__
    browserDiag("voice: capture device opened");
#endif
    return true;
}

/// Called from a seperate thread, be sure to watch for thread safety!
void NetworkAudioRecorder::SDLCallback(void* userdata, uint8_t* stream, int len)
{
    if (!active_recorder || !record_audio_stream)
        return;

    if (SDL_AudioStreamPut(record_audio_stream, stream, len) < 0)
    {
        LOG(ERROR) << "Failed to queue voice capture samples for conversion: " << SDL_GetError();
        return;
    }

    int available = SDL_AudioStreamAvailable(record_audio_stream);
    if (available <= 0)
        return;

    std::vector<uint8_t> converted(static_cast<size_t>(available));
    int received = SDL_AudioStreamGet(record_audio_stream, converted.data(), available);
    if (received <= 0)
        return;

    active_recorder->onProcessSamples(reinterpret_cast<int16_t*>(converted.data()), static_cast<std::size_t>(received / sizeof(int16_t)));
}

void NetworkAudioRecorder::onProcessSamples(const int16_t* samples, std::size_t sample_count)
{
    //Add samples to the sample buffer. The update function (which is run from the main thread) will handle sending of the actual audio packet.
    sample_buffer_mutex.lock();
    auto old_size = sample_buffer.size();
    sample_buffer.resize(old_size + sample_count);
    memcpy(&sample_buffer[old_size], samples, sizeof(int16_t) * sample_count);
    sample_buffer_mutex.unlock();
}

void NetworkAudioRecorder::update(float /*delta*/)
{
    for(size_t idx=0; idx<keys.size(); idx++)
    {
        if (keys[idx].key->getDown() && active_key_index == -1)
        {
            if (ensureRecordingDeviceOpened())
            {
                samples_till_stop = -1;
                active_key_index = static_cast<int>(idx);
                SDL_PauseAudioDevice(record_device_id, 0);
                startSending();
            }
        }
    } 
    while(sendAudioPacket())
    {
    }
    if (active_key_index != -1)
    {
        if (keys[active_key_index].key->getUp())
        {
            samples_till_stop = 44100 / 2;
        }
    }
    if (samples_till_stop == 0)
    {
        SDL_PauseAudioDevice(record_device_id, 1);
        finishSending();
        active_key_index = -1;
        samples_till_stop = -1;
    }
}

void NetworkAudioRecorder::startSending()
{
    int error = 0;
    encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder)
    {
        LOG(ERROR) << "Failed to create opus encoder:" << error;
    }
    else
    {
        opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
        opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
    }

    if (game_client)
    {
        sp::io::DataBuffer audio_packet;
        audio_packet << CMD_AUDIO_COMM_START << game_client->getClientId() << int32_t(keys[active_key_index].target_identifier);
        game_client->sendPacket(audio_packet);
    }
    else if (game_server)
    {
        game_server->startAudio(0, keys[active_key_index].target_identifier);
    }
}

bool NetworkAudioRecorder::sendAudioPacket()
{
    bool result = false;
    std::lock_guard<std::mutex> guard(sample_buffer_mutex);
    if (sample_buffer.size() >= frame_size)
    {
        unsigned char packet_buffer[4096];
        int packet_size = 0;
        if (encoder)
            packet_size = opus_encode(encoder, sample_buffer.data(), frame_size, packet_buffer, sizeof(packet_buffer));
        if (packet_size <= 0)
        {
            LOG(ERROR) << "Failed to encode voice frame: " << packet_size;
            sample_buffer.erase(sample_buffer.begin(), sample_buffer.begin() + frame_size);
            return true;
        }
        if (game_client)
        {
            sp::io::DataBuffer audio_packet;
            audio_packet << CMD_AUDIO_COMM_DATA << game_client->getClientId();
            audio_packet.appendRaw(packet_buffer, packet_size);

            game_client->sendPacket(audio_packet);
        }
        else if (game_server)
        {
            game_server->gotAudioPacket(0, packet_buffer, packet_size);
        }
        sample_buffer.erase(sample_buffer.begin(), sample_buffer.begin() + frame_size);
        result = true;

        if (samples_till_stop > -1)
        {
            samples_till_stop = std::max(0, samples_till_stop - frame_size);
        }
    }
    return result;
}

void NetworkAudioRecorder::finishSending()
{
    while(sendAudioPacket())
    {
    }

    {
        std::lock_guard<std::mutex> guard(sample_buffer_mutex);
        while(sample_buffer.size() < frame_size)
            sample_buffer.push_back(0);
    }
    
    sendAudioPacket();
    opus_encoder_destroy(encoder);
    encoder = nullptr;
    if (record_audio_stream)
        SDL_AudioStreamClear(record_audio_stream);

    if (game_client)
    {
        sp::io::DataBuffer audio_packet;
        audio_packet << CMD_AUDIO_COMM_STOP << game_client->getClientId();
        game_client->sendPacket(audio_packet);
    }
    else if (game_server)
    {
        game_server->stopAudio(0);
    }
}
