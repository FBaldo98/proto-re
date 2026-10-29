// binkw32.dll for the native host: the engine's movies (intro logos, cutscenes, the Web of Intrigue memories) are
// Bink 1 videos. The engine imports the Bink 1.x API, decodes through the frame buffers it registers and renders the
// Y/cR/cB planes itself with D3D9; the port only has to replace the decoder. FFmpeg supplies the Bink demuxer and
// the bink/binkaudio decoders (`PROTORE_FFMPEG`; android/build-deps.sh builds a static FFmpeg into the deps prefix).
//
// Where the file comes from: the engine installs an IO callback with BinkSetIO, so the .bik is read through the
// engine's own virtual file system (the movies are stored inside the .rcf archives). BinkOpen also accepts a Bink
// image in guest memory (BINKFROMMEMORY) and, when the engine installed no IO (or its open failed), a loose .bik in
// the game directory.
//
// The structures below are the public Bink 1.9 layouts (bink.h): the engine reads BINK fields directly and fills
// BINKIO with its own callbacks, so the guest-side layouts have to match byte for byte. 32-bit guest: pointers are
// 4 bytes.
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "guest.h"

#if PROTORE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <SDL2/SDL.h>
#endif

namespace native {

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Bink 1.9 guest structures
// ---------------------------------------------------------------------------------------------------------------

struct BinkPlane {
    int32_t allocate;
    uint32_t buffer;
    uint32_t pitch;
};
struct BinkPlaneSet {
    BinkPlane y, cr, cb, a;
};
struct BinkFrameBuffers {
    int32_t total_frames;
    uint32_t ya_width, ya_height;
    uint32_t crcb_width, crcb_height;
    uint32_t frame_num;  // index of the frame set to write (0 for a single set)
    BinkPlaneSet frames[2];
};
static_assert(sizeof(BinkPlane) == 12, "Bink 1.9 BINKPLANE");
static_assert(sizeof(BinkPlaneSet) == 48, "Bink 1.9 BINKFRAMEPLANESET");
static_assert(offsetof(BinkFrameBuffers, crcb_width) == 12, "Bink 1.9 BINKFRAMEBUFFERS");
static_assert(offsetof(BinkFrameBuffers, frames) == 24, "Bink 1.9 BINKFRAMEBUFFERS");
static_assert(sizeof(BinkFrameBuffers) == 120, "Bink 1.9 BINKFRAMEBUFFERS");

// BINK (Bink 1.9i) field offsets the engine can read. The whole structure is 0x408 bytes on a 32-bit build.
constexpr uint32_t kOffWidth = 0x00, kOffHeight = 0x04, kOffFrames = 0x08, kOffFrameNum = 0x0c;
constexpr uint32_t kOffLastFrameNum = 0x10, kOffFrameRate = 0x14, kOffFrameRateDiv = 0x18;
constexpr uint32_t kOffOpenFlags = 0x20, kOffBinkType = 0x24;
constexpr uint32_t kOffSize = 0x28;
constexpr uint32_t kOffFrameBuffers = 0xbc, kOffLargestFrameSize = 0xd8;
constexpr uint32_t kOffInternalFrames = 0xdc, kOffNumTracks = 0xe0, kOffPaused = 0xec;
constexpr uint32_t kOffSoundOn = 0xf8, kOffVideoOn = 0xfc;
constexpr uint32_t kBinkStructSize = 0x408;

// BINKIO (Bink 1.9i): the open callback the engine installs fills in the read callbacks; the engine's file object
// lives in iodata (+0x54).
constexpr uint32_t kIoReadHeader = 0x00, kIoReadFrame = 0x04, kIoGetBufferSize = 0x08, kIoSetInfo = 0x0c;
constexpr uint32_t kIoIdle = 0x10, kIoClose = 0x14, kIoBink = 0x1c;
constexpr uint32_t kIoStructSize = 0x144;     // BINKIO (bink.h 1.9i)
constexpr uint32_t kOffBio = 0x150;           // BINK::bio, where the engine's IO lives

constexpr uint32_t kBinkFromMemory = 0x04000000;
constexpr uint32_t kBinkFileHandle = 0x00800000;
constexpr uint32_t kBinkAlpha = 0x00100000;

// ---------------------------------------------------------------------------------------------------------------
// FFmpeg decoder (or the stub without it)
// ---------------------------------------------------------------------------------------------------------------

#if PROTORE_FFMPEG

std::once_flag g_av_once;

void AvLog(void*, int level, const char* fmt, va_list ap) {
    if (level > AV_LOG_WARNING) return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    Log("bink: ffmpeg: %s", buf);
}

void InitFfmpeg() {
    std::call_once(g_av_once, [] {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(AvLog);
    });
}

// Where the compressed file comes from.
struct Source {
    enum class Kind { None, EngineIo, File, Memory } kind = Kind::None;

    // EngineIo: the engine's BINKIO in guest memory and its ReadHeader callback.
    uint32_t io = 0;
    uint32_t read_header = 0;
    uint32_t scratch = 0;
    uint32_t scratch_size = 0;
    uint32_t io_buffer = 0;

    // File
    FILE* file = nullptr;

    // Memory (BINKFROMMEMORY): a Bink image already in guest memory.
    uint32_t memory = 0;
    int64_t memory_size = 0;

    int64_t pos = 0;
    int64_t size = 0;

    int Read(uint8_t* out, int count);
    int64_t Seek(int64_t offset, int whence);
    void Close();
};

bool Callable(uint32_t va) { return va >= g_engine_lo && va < g_engine_hi; }

int Source::Read(uint8_t* out, int count) {
    if (count <= 0) return 0;
    switch (kind) {
        case Kind::EngineIo: {
            if (!Callable(read_header) || pos < 0 || pos > 0x7fffffff) return AVERROR(EIO);
            uint32_t want = uint32_t(count);
            if (want > scratch_size) want = scratch_size;
            uint32_t got = GuestCall(read_header, {io, uint32_t(pos), scratch, want});
            if (got > want) got = want;
            if (got) {
                memcpy(out, Ptr<uint8_t>(scratch), got);
                pos += got;
            }
            return int(got);
        }
        case Kind::File: {
            size_t got = fread(out, 1, size_t(count), file);
            pos += int64_t(got);
            if (got == 0 && ferror(file)) return AVERROR(EIO);
            return int(got);
        }
        case Kind::Memory: {
            if (pos >= memory_size) return 0;
            int64_t left = memory_size - pos;
            int n = int(left < count ? left : count);
            memcpy(out, Ptr<uint8_t>(memory) + pos, size_t(n));
            pos += n;
            return n;
        }
        case Kind::None:
            return AVERROR(EIO);
    }
    return AVERROR(EIO);
}

int64_t Source::Seek(int64_t offset, int whence) {
    int64_t target = offset;
    if (whence == SEEK_CUR) target = pos + offset;
    else if (whence == SEEK_END) target = size + offset;
    else if (whence != SEEK_SET) return AVERROR(EINVAL);
    if (target < 0) return AVERROR(EINVAL);
    if (kind == Kind::File) {
        if (fseeko(file, off_t(target), SEEK_SET) != 0) return AVERROR(EIO);
    }
    pos = target;
    return pos;
}

void Source::Close() {
    if (kind == Kind::EngineIo) {
        uint32_t close_fn = io ? R32(io + kIoClose) : 0;
        if (Callable(close_fn)) GuestCall(close_fn, {io});
        if (io_buffer) HeapFree(io_buffer);
        if (scratch) HeapFree(scratch);
        // `io` is BINK::bio inside the guest BINK block, freed with it.
    }
    if (file) fclose(file);
    *this = Source{};
}

// Reads `count` bytes at `offset` (used for the Bink header before FFmpeg takes over).
bool ReadAt(Source& s, int64_t offset, void* out, int count) {
    if (s.Seek(offset, SEEK_SET) != offset) return false;
    return s.Read(static_cast<uint8_t*>(out), count) == count;
}

struct BinkHeader {
    uint32_t marker = 0, size = 0, frames = 0, largest_frame = 0, internal_frames = 0;
    uint32_t width = 0, height = 0, rate = 0, rate_div = 0, flags = 0, num_tracks = 0;
};
static_assert(sizeof(BinkHeader) == 44, "Bink 1.x BINKHDR");

bool ParseHeader(Source& s, BinkHeader* h) {
    if (!ReadAt(s, 0, h, sizeof(*h))) return false;
    // "BIKb" / "BIKi" / "BIKj" / "BIKk" ...: the first three bytes are "BIK".
    if ((h->marker & 0x00ffffffu) != 0x004b4942u) return false;
    int64_t file_size = int64_t(h->size) + 8;
    if (s.size && file_size > s.size) file_size = s.size;
    s.size = file_size;
    return h->width && h->height && h->frames;
}

int AvRead(void* opaque, uint8_t* buf, int size) {
    return static_cast<Source*>(opaque)->Read(buf, size);
}

int64_t AvSeek(void* opaque, int64_t offset, int whence) {
    Source* s = static_cast<Source*>(opaque);
    if (whence == AVSEEK_SIZE) return s->size;
    return s->Seek(offset, whence);
}

struct Bink {
    uint32_t self = 0;  // guest BINK
    uint32_t io = 0;    // guest BINKIO (EngineIo)
    uint32_t frame_buffers = 0;
    // Bink allocates frame buffers itself unless BINKNOFRAMEBUFFERS is set; this is that set, used when the engine
    // never registers its own (the normal path replaces it with the engine's struct).
    uint32_t own_frame_buffers = 0;
    Source source;

    AVFormatContext* format = nullptr;
    AVIOContext* avio = nullptr;
    uint8_t* avio_buffer = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwrContext* swr = nullptr;
    int video_stream = -1;
    int audio_stream = -1;

    int64_t next_video = 0;   // index of the next video frame FFmpeg will return
    int64_t shown_video = -1; // index of the frame kept in `frame`
    std::vector<uint8_t> pcm;
    std::vector<uint32_t> own_planes;  // guest plane buffers allocated when the engine did not lock one

    SDL_AudioDeviceID device = 0;
    int out_rate = 0, out_channels = 0;
    int64_t next_audio_sample = 0;  // output sample position of the end of the queued sound
    int volume = 0x8000;  // Bink: 0..32768
    bool paused = false;
    bool sound_on = true, video_on = true;
    bool have_alpha = false;
    uint32_t open_flags = 0;

    std::chrono::steady_clock::time_point opened;
    std::chrono::steady_clock::time_point paused_at;

    int64_t frame_num() const { return R32(self + kOffFrameNum); }
    int64_t frame_count() const { return R32(self + kOffFrames); }
    int rate_num() const { return R32(self + kOffFrameRate); }
    int rate_div() const { return R32(self + kOffFrameRateDiv); }
};

std::recursive_mutex g_mutex;
std::map<uint32_t, Bink*> g_binks;
uint32_t g_io_open = 0;           // BinkSetIO callback
uint32_t g_sound_open = 0;        // BinkSetSoundSystem callback (not used: audio goes to SDL)
uint32_t g_sound_param = 0;
uint32_t g_sound_tracks = 0, g_sound_track_list = 0;

Bink* Find(uint32_t handle) {
    auto it = g_binks.find(handle);
    return it == g_binks.end() ? nullptr : it->second;
}

void CloseAudio(Bink* b) {
    if (b->device) SDL_CloseAudioDevice(b->device);
    b->device = 0;
    if (b->swr) swr_free(&b->swr);
    b->out_rate = b->out_channels = 0;
}

void Close(Bink* b, bool close_source) {
    CloseAudio(b);
    for (uint32_t p : b->own_planes) HeapFree(p);
    if (b->packet) av_packet_free(&b->packet);
    if (b->frame) av_frame_free(&b->frame);
    if (b->audio) avcodec_free_context(&b->audio);
    if (b->video) avcodec_free_context(&b->video);
    if (b->format) avformat_close_input(&b->format);
    if (b->avio) avio_context_free(&b->avio);  // frees avio_buffer too
    b->avio_buffer = nullptr;
    if (close_source) b->source.Close();
    if (b->own_frame_buffers) HeapFree(b->own_frame_buffers);
    if (b->self) HeapFree(b->self);
    delete b;
}

bool OpenAudioDevice(Bink* b) {
    if (b->device) return true;
    AVCodecContext* a = b->audio;
    int channels = a->ch_layout.nb_channels > 0 ? a->ch_layout.nb_channels : 2;
    if (channels > 2) channels = 2;  // the movies are stereo; surround is downmixed

    SDL_AudioSpec want{}, have{};
    want.freq = a->sample_rate > 0 ? a->sample_rate : 48000;
    want.format = AUDIO_S16SYS;
    want.channels = Uint8(channels);
    want.samples = 1024;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        Log("bink: no audio (SDL: %s)", SDL_GetError());
        return false;
    }
    b->device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!b->device) {
        Log("bink: no audio (SDL: %s)", SDL_GetError());
        return false;
    }
    b->out_rate = have.freq;
    b->out_channels = have.channels;

    AVChannelLayout in_layout = a->ch_layout;
    if (in_layout.nb_channels == 0) av_channel_layout_default(&in_layout, want.channels);
    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, b->out_channels);
    int err = swr_alloc_set_opts2(&b->swr, &out_layout, AV_SAMPLE_FMT_S16, b->out_rate, &in_layout, a->sample_fmt,
                                  a->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (err < 0 || swr_init(b->swr) < 0) {
        Log("bink: audio resampler failed");
        CloseAudio(b);
        return false;
    }
    SDL_PauseAudioDevice(b->device, 0);
    Log("bink: audio %d Hz %d ch (file %d Hz, %d ch)", b->out_rate, b->out_channels, a->sample_rate, channels);
    return true;
}

void QueueSilence(Bink* b, int64_t samples) {
    static const int16_t zero[4096] = {};
    const int64_t max_samples = int64_t(b->out_rate) * 10;  // longer than any gap in a game movie
    if (samples > max_samples) samples = max_samples;
    while (samples > 0) {
        int64_t chunk = samples < int64_t(sizeof(zero) / sizeof(zero[0])) ? samples
                                                                           : int64_t(sizeof(zero) / sizeof(zero[0]));
        // Frames are made of out_channels interleaved samples; keep whole frames.
        chunk -= chunk % b->out_channels;
        if (chunk <= 0) break;
        SDL_QueueAudio(b->device, zero, Uint32(chunk * sizeof(int16_t)));
        samples -= chunk;
    }
}

// `pts` is the audio packet position in `b->audio->pkt_timebase`; Bink leaves silence out of the file, so the
// timestamps (not the packet order) say when a decoded chunk belongs on the movie's timeline.
void QueueAudio(Bink* b, AVFrame* frame, int64_t pts) {
    if (!b->audio || !b->sound_on) return;
    if (!OpenAudioDevice(b)) return;

    int out_samples = swr_get_out_samples(b->swr, frame->nb_samples);
    if (out_samples <= 0 || out_samples > (1 << 20)) return;
    size_t bytes = size_t(out_samples) * size_t(b->out_channels) * sizeof(int16_t);
    b->pcm.resize(bytes);
    uint8_t* start = b->pcm.data();
    uint8_t* out = start;
    int got = swr_convert(b->swr, &out, out_samples,
                          reinterpret_cast<const uint8_t* const*>(frame->extended_data), frame->nb_samples);
    if (got <= 0) return;

    int64_t pos = -1;
    if (pts != AV_NOPTS_VALUE && b->audio->pkt_timebase.num > 0) {
        pos = av_rescale_q(pts, b->audio->pkt_timebase, AVRational{1, b->out_rate});
    }
    // A long stall can make the engine decode several frames at once; keep the queue close to the picture. The
    // threshold is well above a silent stretch the movie itself contains.
    if (SDL_GetQueuedAudioSize(b->device) > Uint32(b->out_rate) * Uint32(b->out_channels) * 2 * 12) {
        SDL_ClearQueuedAudio(b->device);
        b->next_audio_sample = pos >= 0 ? pos : b->next_audio_sample;
    }
    if (pos >= 0) {
        if (pos + got <= b->next_audio_sample) return;  // already covered (a seek or a duplicate)
        if (pos < b->next_audio_sample) {  // overlap: skip what is already queued
            int64_t skip = b->next_audio_sample - pos;
            if (skip >= got) return;
            start += skip * b->out_channels * sizeof(int16_t);
            got -= int(skip);
            pos = b->next_audio_sample;
        }
        if (pos > b->next_audio_sample) QueueSilence(b, pos - b->next_audio_sample);
        b->next_audio_sample = pos + got;
    } else {
        b->next_audio_sample += got;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(start);
    size_t count = size_t(got) * size_t(b->out_channels);
    if (b->volume < 0x8000) {
        for (size_t i = 0; i < count; ++i) {
            int32_t v = (int32_t(samples[i]) * b->volume) >> 15;
            samples[i] = int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
    }
    SDL_QueueAudio(b->device, start, Uint32(count * sizeof(int16_t)));
}

void DecodeAudioPacket(Bink* b) {
    if (!b->audio) {
        av_packet_unref(b->packet);
        return;
    }
    const int64_t packet_pts = b->packet->pts;
    int sent = avcodec_send_packet(b->audio, b->packet);
    av_packet_unref(b->packet);
    if (sent < 0) return;
    AVFrame* pcm = av_frame_alloc();
    if (!pcm) return;
    bool first = true;
    for (;;) {
        int got = avcodec_receive_frame(b->audio, pcm);
        if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) break;
        if (got < 0) break;
        // BinkAudio sets the timestamp only on the first frame a packet produces; the rest follow it back to back.
        int64_t pts = pcm->pts;
        if (pts == AV_NOPTS_VALUE && first) pts = packet_pts;
        QueueAudio(b, pcm, pts);
        first = false;
        av_frame_unref(pcm);
    }
    av_frame_free(&pcm);
}

// Decodes up to the video frame with the given 0-based index; audio packets are played on the way. After the frame is
// decoded, audio up to the next video packet is decoded too (the bink demuxer interleaves "video, audio, audio"), so
// the sound of a frame is queued with the picture and not one frame later. The next video packet stays in `packet`.
void DecodeTo(Bink* b, int64_t target) {
    if (b->shown_video == target) return;
    while (b->shown_video < target) {
        if (!b->packet->data && av_read_frame(b->format, b->packet) < 0) return;
        if (b->packet->stream_index == b->video_stream) {
            int sent = avcodec_send_packet(b->video, b->packet);
            av_packet_unref(b->packet);
            if (sent < 0) continue;
            for (;;) {
                int got = avcodec_receive_frame(b->video, b->frame);
                if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) break;
                if (got < 0) return;
                ++b->next_video;
                b->shown_video = b->next_video - 1;
                if (b->shown_video >= target) break;
                av_frame_unref(b->frame);
            }
        } else if (b->packet->stream_index == b->audio_stream) {
            DecodeAudioPacket(b);
        } else {
            av_packet_unref(b->packet);
        }
    }
    // Bring the audio up to the next picture; keep that packet for the next call.
    while (b->shown_video == target) {
        if (!b->packet->data && av_read_frame(b->format, b->packet) < 0) return;
        if (b->packet->stream_index == b->video_stream) return;
        if (b->packet->stream_index == b->audio_stream) DecodeAudioPacket(b);
        else av_packet_unref(b->packet);
    }
}

// Copies one plane of a decoded frame into the guest buffer the engine locked for it. `pitch` is in bytes.
void CopyPlane(const AVFrame* frame, int plane, uint32_t buffer, uint32_t pitch, int width, int height) {
    if (!buffer || !pitch || width <= 0 || height <= 0) return;
    const uint8_t* src = frame->data[plane];
    if (!src || frame->linesize[plane] <= 0) return;
    uint8_t* dst = Ptr<uint8_t>(buffer);
    int row = width;
    if (row > frame->linesize[plane]) row = frame->linesize[plane];
    if (uint32_t(row) > pitch) row = int(pitch);
    for (int y = 0; y < height; ++y) {
        memcpy(dst + size_t(y) * pitch, src + size_t(y) * frame->linesize[plane], size_t(row));
    }
}

void CopyFrame(Bink* b) {
    BinkFrameBuffers* fb = Ptr<BinkFrameBuffers>(b->frame_buffers ? b->frame_buffers : b->own_frame_buffers);
    if (!fb || !b->frame->data[0]) return;
    int index = fb->total_frames > 1 ? int(fb->frame_num % uint32_t(fb->total_frames)) : 0;
    if (index < 0 || index >= 2) index = 0;
    BinkPlaneSet& set = fb->frames[index];
    // The engine's textures are sized from the Bink header; never copy past them.
    const int gw = int(R32(b->self + kOffWidth)), gh = int(R32(b->self + kOffHeight));
    const int w = gw > 0 && b->frame->width > gw ? gw : b->frame->width;
    const int h = gh > 0 && b->frame->height > gh ? gh : b->frame->height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;

    auto copy = [&](BinkPlane& p, int plane, int pw, int ph, uint32_t default_pitch) {
        if (!p.buffer) {
            // The engine did not lock this plane: give the decoder its own memory so nothing writes to null.
            uint32_t pitch = p.pitch ? p.pitch : default_pitch;
            uint32_t size = pitch * uint32_t(ph);
            p.buffer = HeapAlloc(size);
            p.pitch = pitch;
            if (p.buffer) b->own_planes.push_back(p.buffer);
        }
        CopyPlane(b->frame, plane, p.buffer, p.pitch, pw, ph);
    };
    copy(set.y, 0, w, h, uint32_t(w));
    const bool swap = SettingInt("bink_swap_uv", 0) != 0;
    // Bink names its chroma planes after the colour they carry: cR = Cr (V, FFmpeg plane 2), cB = Cb (U, plane 1).
    copy(set.cr, swap ? 1 : 2, cw, ch, uint32_t(cw));
    copy(set.cb, swap ? 2 : 1, cw, ch, uint32_t(cw));
    if (b->frame->data[3] && set.a.buffer) CopyPlane(b->frame, 3, set.a.buffer, set.a.pitch, w, h);

    // The engine uses its own texture sizes; keep the Bink view of the current frame in step.
    if (BinkFrameBuffers* guest = Ptr<BinkFrameBuffers>(b->frame_buffers)) guest->frame_num = uint32_t(index);
}

bool OpenFfmpeg(Bink* b, const BinkHeader& header) {
    InitFfmpeg();
    const int kAvioSize = 32768;
    b->avio_buffer = static_cast<uint8_t*>(av_malloc(kAvioSize));
    if (!b->avio_buffer) return false;
    b->avio = avio_alloc_context(b->avio_buffer, kAvioSize, 0, &b->source, AvRead, nullptr, AvSeek);
    if (!b->avio) return false;
    b->format = avformat_alloc_context();
    if (!b->format) return false;
    b->format->pb = b->avio;
    b->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    // The Bink header is self-describing; do not read megabytes through the engine's file system to start a movie.
    b->format->probesize = 1 << 18;
    b->format->max_analyze_duration = AV_TIME_BASE;  // 1 s
    // The demuxer probes the header; it does not need a URL.
    if (avformat_open_input(&b->format, nullptr, nullptr, nullptr) < 0) {
        Log("bink: FFmpeg cannot open the stream");
        return false;
    }
    if (avformat_find_stream_info(b->format, nullptr) < 0) {
        Log("bink: FFmpeg found no streams");
        return false;
    }

    const AVCodec* vcodec = nullptr;
    b->video_stream = av_find_best_stream(b->format, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
    if (b->video_stream < 0 || !vcodec) {
        Log("bink: no Bink video stream");
        return false;
    }
    b->video = avcodec_alloc_context3(vcodec);
    if (!b->video) return false;
    AVStream* vs = b->format->streams[b->video_stream];
    if (avcodec_parameters_to_context(b->video, vs->codecpar) < 0) return false;
    b->video->pkt_timebase = vs->time_base;
    if (avcodec_open2(b->video, vcodec, nullptr) < 0) {
        Log("bink: the Bink video decoder is not available in this FFmpeg build");
        return false;
    }

    if (SettingInt("bink_audio", 1)) {
        const AVCodec* acodec = nullptr;
        b->audio_stream = av_find_best_stream(b->format, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
        if (b->audio_stream >= 0 && acodec) {
            b->audio = avcodec_alloc_context3(acodec);
            if (b->audio) {
                AVStream* as = b->format->streams[b->audio_stream];
                if (avcodec_parameters_to_context(b->audio, as->codecpar) == 0 &&
                    avcodec_open2(b->audio, acodec, nullptr) == 0) {
                    b->audio->pkt_timebase = as->time_base;
                } else {
                    avcodec_free_context(&b->audio);
                    b->audio_stream = -1;
                }
            } else {
                b->audio_stream = -1;
            }
        }
    }

    b->frame = av_frame_alloc();
    b->packet = av_packet_alloc();
    if (!b->frame || !b->packet) return false;

    // The Bink header is authoritative for the frame count and timing; the decoder tells us the picture size.
    if (!header.width || !header.height) return false;
    return true;
}

// Reads the BINKIO the engine installed and prepares the source for FFmpeg.
bool OpenEngineIo(Bink* b, uint32_t name, uint32_t flags) {
    if (!Callable(g_io_open)) return false;
    // The engine's open callback gets BINK::bio itself (some file layers recover the BINK from that pointer), not a
    // separate BINKIO.
    b->io = b->self + kOffBio;
    memset(Ptr<uint8_t>(b->io), 0, kIoStructSize);
    W32(b->io + kIoBink, b->self);
    if (!GuestCall(g_io_open, {b->io, name, flags})) {
        Log("bink: the engine's IO open callback refused the movie");
        b->io = 0;
        return false;
    }
    uint32_t read_header = R32(b->io + kIoReadHeader);
    if (!Callable(read_header)) {
        Log("bink: the engine's IO open callback set no usable ReadHeader (%08x)", read_header);
        b->io = 0;
        return false;
    }
    b->source.kind = Source::Kind::EngineIo;
    b->source.io = b->io;
    b->source.read_header = read_header;
    b->source.scratch_size = 1u << 16;
    b->source.scratch = HeapAlloc(b->source.scratch_size);
    if (!b->source.scratch) {
        b->source.Close();
        b->io = 0;
        return false;
    }

    // The Bink header's second word is the file size minus 8.
    uint32_t file_header[2] = {};
    uint32_t file_size = 0;
    if (ReadAt(b->source, 0, file_header, sizeof(file_header))) file_size = file_header[1] + 8;

    // Bink 1.9 asks the IO for its buffer and hands it over; the engine's file layer expects the same sequence.
    uint32_t get_size = R32(b->io + kIoGetBufferSize);
    uint32_t set_info = R32(b->io + kIoSetInfo);
    uint32_t io_size = 1u << 16;
    if (Callable(get_size)) {
        uint32_t wanted = GuestCall(get_size, {b->io, io_size});
        if (wanted >= 4096 && wanted <= (1u << 20)) io_size = wanted;
    }
    b->source.io_buffer = HeapAlloc(io_size);
    if (!b->source.io_buffer) {
        b->source.Close();
        b->io = 0;
        return false;
    }
    if (Callable(set_info)) GuestCall(set_info, {b->io, b->source.io_buffer, io_size, file_size, 0});
    b->source.size = file_size;
    b->source.Seek(0, SEEK_SET);
    return true;
}

bool OpenFile(Bink* b, const char* name) {
    std::string path = name;
    for (char& ch : path) {
        if (ch == '\\') ch = '/';
    }
    std::vector<std::string> candidates;
    candidates.push_back(path);
    if (g_game_dir) candidates.push_back(std::string(g_game_dir) + "/" + path);
    for (const std::string& candidate : candidates) {
        if (FILE* f = fopen(candidate.c_str(), "rb")) {
            b->source.kind = Source::Kind::File;
            b->source.file = f;
            fseeko(f, 0, SEEK_END);
            b->source.size = int64_t(ftello(f));
            fseeko(f, 0, SEEK_SET);
            Log("bink: opened %s", candidate.c_str());
            return true;
        }
    }
    return false;
}

#else  // !PROTORE_FFMPEG

struct Bink {};
std::recursive_mutex g_mutex;
std::map<uint32_t, Bink*> g_binks;
uint32_t g_io_open = 0, g_sound_open = 0, g_sound_param = 0, g_sound_tracks = 0, g_sound_track_list = 0;

#endif  // PROTORE_FFMPEG

// ---------------------------------------------------------------------------------------------------------------
// Import handlers
// ---------------------------------------------------------------------------------------------------------------

#define B(name) void b_##name(Ctx* c)

B(BinkSetMemory) { Return(c, 0, 2); }

B(BinkSetIO) {
    uint32_t open = Arg(c, 0);
    g_io_open = open;
    Log("bink: BinkSetIO(%08x)", open);
    Return(c, 0, 1);
}

B(BinkSetSoundSystem) {
    g_sound_open = Arg(c, 0);
    g_sound_param = Arg(c, 1);
    Log("bink: BinkSetSoundSystem(%08x, %08x)", g_sound_open, g_sound_param);
    // The port decodes Bink audio itself and plays it through SDL; claiming success keeps the engine's movie path
    // unchanged.
    Return(c, 1, 2);
}

B(BinkSetSoundTrack) {
    g_sound_tracks = Arg(c, 0);
    g_sound_track_list = Arg(c, 1);
    Return(c, 0, 2);
}

B(BinkOpen) {
#if PROTORE_FFMPEG
    const uint32_t name_or_memory = Arg(c, 0);
    const uint32_t flags = Arg(c, 1);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    Bink* b = new Bink;
    b->self = HeapAlloc(kBinkStructSize);
    if (!b->self) {
        delete b;
        Return(c, 0, 2);
        return;
    }
    memset(Ptr<uint8_t>(b->self), 0, kBinkStructSize);
    b->open_flags = flags;
    b->have_alpha = (flags & kBinkAlpha) != 0;
    const char* label = (flags & kBinkFromMemory) ? "<memory>"
                      : (flags & kBinkFileHandle) ? "<file handle>"
                      : name_or_memory ? Str(name_or_memory) : "<null>";

    bool opened = false;
    if (flags & kBinkFromMemory) {
        if (name_or_memory) {
            b->source.kind = Source::Kind::Memory;
            b->source.memory = name_or_memory;
            uint32_t header_size = R32(name_or_memory + 4);
            b->source.memory_size = int64_t(header_size) + 8;
            b->source.size = b->source.memory_size;
            opened = true;
        }
    } else if (flags & kBinkFileHandle) {
        // The engine passes a Win32 HANDLE instead of a name: not used by this port (the movies come through BinkSetIO
        // or BINKFROMMEMORY). Fail instead of reading the handle as a string.
        Log("bink: BINKFILEHANDLE opens are not supported");
    } else {
        // The engine's virtual file system first, then a loose file.
        opened = OpenEngineIo(b, name_or_memory, flags);
        if (!opened && name_or_memory) opened = OpenFile(b, Str(name_or_memory));
    }

    BinkHeader header;
    if (opened) b->source.Seek(0, SEEK_SET);
    if (!opened || !ParseHeader(b->source, &header)) {
        Log("bink: cannot open %s", label);
        b->source.Close();
        HeapFree(b->self);
        delete b;
        Return(c, 0, 2);
        return;
    }

    b->source.Seek(0, SEEK_SET);
    if (!OpenFfmpeg(b, header)) {
        Close(b, true);
        Return(c, 0, 2);
        return;
    }

    // The stream header carries both; the codec context sometimes reports 0/1 for Bink.
    int64_t rate = int64_t(header.rate), rate_div = int64_t(header.rate_div ? header.rate_div : 1);
    const AVRational fr = b->video->framerate;
    if (fr.num > 0 && fr.den > 0) {
        rate = fr.num;
        rate_div = fr.den;
    }
    const int64_t frames = header.frames;
    const int64_t width = header.width;
    const int64_t height = header.height;

    W32(b->self + kOffWidth, uint32_t(width));
    W32(b->self + kOffHeight, uint32_t(height));
    W32(b->self + kOffFrames, uint32_t(frames));
    W32(b->self + kOffFrameNum, 1);
    W32(b->self + kOffLastFrameNum, 0);
    W32(b->self + kOffFrameRate, uint32_t(rate));
    W32(b->self + kOffFrameRateDiv, uint32_t(rate_div > 0 ? rate_div : 1));
    W32(b->self + kOffOpenFlags, flags);
    W32(b->self + kOffSize, uint32_t(b->source.size));
    W32(b->self + kOffLargestFrameSize, header.largest_frame);
    W32(b->self + kOffInternalFrames, header.internal_frames);
    W32(b->self + kOffNumTracks, b->audio ? 1u : 0u);
    W32(b->self + kOffBinkType, header.flags);
    W32(b->self + kOffPaused, 0);
    W32(b->self + kOffSoundOn, 1);
    W32(b->self + kOffVideoOn, 1);

    // Bink's own frame buffer set, for engines that do not call BinkRegisterFrameBuffers (0x408-byte BINK's
    // FrameBuffers points here; BinkRegisterFrameBuffers points it at the engine's set instead).
    b->own_frame_buffers = HeapAlloc(sizeof(BinkFrameBuffers));
    if (b->own_frame_buffers) {
        BinkFrameBuffers* fb = Ptr<BinkFrameBuffers>(b->own_frame_buffers);
        memset(fb, 0, sizeof(*fb));
        fb->total_frames = 1;
        fb->ya_width = uint32_t(width);
        fb->ya_height = uint32_t(height);
        fb->crcb_width = (uint32_t(width) + 1) / 2;
        fb->crcb_height = (uint32_t(height) + 1) / 2;
        fb->frames[0].y.allocate = 1;
        fb->frames[0].cr.allocate = 1;
        fb->frames[0].cb.allocate = 1;
        fb->frames[0].a.allocate = b->have_alpha ? 1 : 0;
        b->frame_buffers = b->own_frame_buffers;
        W32(b->self + kOffFrameBuffers, b->own_frame_buffers);
    }
    b->opened = std::chrono::steady_clock::now();

    g_binks[b->self] = b;
    Log("bink: open %s %dx%d, %lld frames at %lld/%lld fps, audio %s, size %lld", label, int(width), int(height),
        (long long)frames, (long long)rate, (long long)(rate_div > 0 ? rate_div : 1), b->audio ? "yes" : "no",
        (long long)b->source.size);
    Return(c, b->self, 2);
#else
    Log("bink: BinkOpen(%s) -> no decoder, movie skipped", Arg(c, 0) ? Str(Arg(c, 0)) : "");
    Return(c, 0, 2);
#endif
}

B(BinkClose) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (Bink* b = Find(Arg(c, 0))) {
        g_binks.erase(Arg(c, 0));
        Close(b, true);
    }
#endif
    Return(c, 0, 1);
}

B(BinkWait) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Bink* b = Find(Arg(c, 0));
    if (!b || b->paused) {
        Return(c, 0, 1);
        return;
    }
    const int64_t frame = b->frame_num();
    const int64_t num = b->rate_num(), div = b->rate_div();
    if (frame <= 1 || num <= 0 || div <= 0 || frame > b->frame_count()) {
        Return(c, 0, 1);
        return;
    }
    using Clock = std::chrono::steady_clock;
    const int64_t due_us = (frame - 1) * 1000000 * div / num;
    const Clock::time_point due = b->opened + std::chrono::microseconds(due_us);
    const Clock::time_point now = Clock::now();
    if (now >= due) {
        Return(c, 0, 1);
        return;
    }
    const int64_t wait_us = std::chrono::duration_cast<std::chrono::microseconds>(due - now).count();
    Return(c, uint32_t((wait_us + 999) / 1000), 1);
#else
    Return(c, 0, 1);
#endif
}

B(BinkDoFrame) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Bink* b = Find(Arg(c, 0));
    if (!b || b->paused || !b->video_on || !(b->frame_buffers || b->own_frame_buffers)) {
        Return(c, 1, 1);
        return;
    }
    const int64_t frame = b->frame_num();
    if (frame < 1 || frame > b->frame_count()) {
        Return(c, 1, 1);
        return;
    }
    DecodeTo(b, frame - 1);
    if (b->shown_video != frame - 1 || !b->frame->data[0]) {
        Return(c, 1, 1);
        return;
    }
    CopyFrame(b);
    Return(c, 0, 1);
#else
    Return(c, 0, 1);
#endif
}

B(BinkNextFrame) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Bink* b = Find(Arg(c, 0));
    if (b) {
        uint32_t frame = R32(b->self + kOffFrameNum);
        const uint32_t count = R32(b->self + kOffFrames);
        W32(b->self + kOffLastFrameNum, frame);
        if (frame <= count) ++frame;  // stays at Frames+1: the engine sees the end of the movie
        W32(b->self + kOffFrameNum, frame);
    }
#endif
    Return(c, 0, 1);
}

B(BinkPause) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Bink* b = Find(Arg(c, 0));
    if (!b) {
        Return(c, 0, 2);
        return;
    }
    using Clock = std::chrono::steady_clock;
    const bool pause = Arg(c, 1) != 0;
    const int32_t was = b->paused ? 1 : 0;
    if (pause != b->paused) {
        if (pause) {
            b->paused_at = Clock::now();
        } else {
            // Do not jump ahead by the paused time.
            b->opened += Clock::now() - b->paused_at;
        }
        b->paused = pause;
        W32(b->self + kOffPaused, pause ? 1u : 0u);
        if (b->device) SDL_PauseAudioDevice(b->device, pause ? 1 : 0);
    }
    Return(c, uint32_t(was), 2);
#else
    Return(c, 0, 2);
#endif
}

B(BinkGetFrameBuffersInfo) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Bink* b = Find(Arg(c, 0));
    uint32_t out = Arg(c, 1);
    if (!b || !out) {
        Return(c, 0, 2);
        return;
    }
    BinkFrameBuffers* fb = Ptr<BinkFrameBuffers>(out);
    const uint32_t w = R32(b->self + kOffWidth), h = R32(b->self + kOffHeight);
    fb->total_frames = 1;
    fb->ya_width = w;
    fb->ya_height = h;
    fb->crcb_width = (w + 1) / 2;
    fb->crcb_height = (h + 1) / 2;
    fb->frame_num = 0;
    // Which planes the engine has to allocate. Alpha only for movies opened with BINKALPHA.
    fb->frames[0] = BinkPlaneSet{};
    fb->frames[0].y.allocate = 1;
    fb->frames[0].cr.allocate = 1;
    fb->frames[0].cb.allocate = 1;
    fb->frames[0].a.allocate = b->have_alpha ? 1 : 0;
    Log("bink: frame buffers %ux%u (chroma %ux%u), alpha %s", w, h, fb->crcb_width, fb->crcb_height,
        b->have_alpha ? "yes" : "no");
    Return(c, 0, 2);
#else
    Return(c, 0, 2);
#endif
}

B(BinkRegisterFrameBuffers) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Bink* b = Find(Arg(c, 0));
    uint32_t fb = Arg(c, 1);
    if (b) {
        b->frame_buffers = fb;
        W32(b->self + kOffFrameBuffers, fb);
        if (fb) {
            BinkFrameBuffers* p = Ptr<BinkFrameBuffers>(fb);
            Log("bink: %d frame buffer(s) registered, Y %08x/%u, A %08x, cR %08x/%u, cB %08x/%u",
                int(p->total_frames), p->frames[0].y.buffer, p->frames[0].y.pitch, p->frames[0].a.buffer,
                p->frames[0].cr.buffer, p->frames[0].cr.pitch, p->frames[0].cb.buffer, p->frames[0].cb.pitch);
        }
    }
    Return(c, 0, 2);
#else
    Return(c, 0, 2);
#endif
}

B(BinkSetVolume) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (Bink* b = Find(Arg(c, 0))) {
        const int32_t v = int32_t(Arg(c, 2));
        b->volume = v < 0 ? 0 : v > 0x8000 ? 0x8000 : v;
    }
#endif
    Return(c, 0, 3);
}

B(BinkSetMixBins) {
#if PROTORE_FFMPEG
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("bink: BinkSetMixBins ignored (audio plays as stereo)");
    }
#endif
    Return(c, 0, 4);
}

B(BinkOpenTrack) {
#if PROTORE_FFMPEG
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Bink* b = Find(Arg(c, 0));
    const uint32_t index = Arg(c, 1);
    if (!b || !b->audio || index != 0) {
        Return(c, 0, 2);
        return;
    }
    // BINKTRACK (Bink 1.9): Frequency, Bits, Channels, MaxSize, bink, sndcomp, trackindex.
    uint32_t track = HeapAlloc(0x1c);
    if (!track) {
        Return(c, 0, 2);
        return;
    }
    memset(Ptr<uint8_t>(track), 0, 0x1c);
    W32(track + 0, uint32_t(b->audio->sample_rate));
    W32(track + 4, 16);
    W32(track + 8, uint32_t(b->audio->ch_layout.nb_channels > 0 ? b->audio->ch_layout.nb_channels : 2));
    W32(track + 12, 0);
    W32(track + 16, b->self);
    W32(track + 24, index);
    Return(c, track, 2);
#else
    Return(c, 0, 2);
#endif
}

B(BinkCloseTrack) {
#if PROTORE_FFMPEG
    if (Arg(c, 0)) HeapFree(Arg(c, 0));
#endif
    Return(c, 0, 1);
}

B(RADTimerRead) {
    static auto start = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    Return(c, uint32_t(ms.count()), 0);
}

#undef B

#define E(name, args) {"binkw32.dll", "_" #name "@" #args, b_##name}
const ImportDef kBink[] = {
    E(BinkSetMemory, 8), E(BinkSetIO, 4), E(BinkSetSoundSystem, 8), E(BinkSetSoundTrack, 8), E(BinkOpen, 8),
    E(BinkClose, 4), E(BinkWait, 4), E(BinkDoFrame, 4), E(BinkNextFrame, 4), E(BinkPause, 8),
    E(BinkGetFrameBuffersInfo, 8), E(BinkRegisterFrameBuffers, 8), E(BinkSetVolume, 12), E(BinkSetMixBins, 16),
    E(BinkOpenTrack, 8), E(BinkCloseTrack, 4), E(RADTimerRead, 0),
};
#undef E
ImportRegistrar g_bink(kBink);

}  // namespace

}  // namespace native
