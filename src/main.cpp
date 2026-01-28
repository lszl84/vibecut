#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include <map>
#include <cstring>
#include <optional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include "embedded_font.h"
#include "json.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace fs = std::filesystem;

static int input_text_resize_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}

static bool input_text_string(const char* label, std::string& value, ImGuiInputTextFlags flags = 0) {
    if (!(flags & ImGuiInputTextFlags_CallbackResize)) {
        flags |= ImGuiInputTextFlags_CallbackResize;
    }
    if (value.capacity() < 256) {
        value.reserve(256);
    }
    return ImGui::InputText(label, value.data(), value.capacity() + 1, flags,
                            input_text_resize_callback, &value);
}

// Represents a segment of video on the timeline (frame-based)
// Uses half-open intervals: [start_frame, end_frame)
struct Clip {
    int source_id = 0;
    int64_t start_frame;  // First frame included (0-indexed)
    int64_t end_frame;    // First frame NOT included
    int color_id = 0;
    bool is_gap = false;  // True if this is a gap clip (empty placeholder)
    
    int64_t frame_count() const { return end_frame - start_frame; }
};

// Connected clip - overlays on top of (or below) the main timeline
// Connects at a specific timeline frame, can be positioned in lanes above/below
struct ConnectedClip {
    int source_id = 0;
    int64_t start_frame;      // First SOURCE frame included
    int64_t end_frame;        // First SOURCE frame NOT included
    int64_t connection_frame; // Timeline frame where the connection anchor is
    int64_t connection_offset = 0; // Frames into the clip where the connection is (0 = left edge)
    int lane = 1;             // Positive = above main timeline, negative = below
    int color_id = 0;
    
    int64_t frame_count() const { return end_frame - start_frame; }
    // Timeline frame where the clip visually starts (may be before connection_frame)
    int64_t timeline_start() const { return connection_frame - connection_offset; }
    // Timeline frame where the clip ends (exclusive)
    int64_t timeline_end() const { return timeline_start() + frame_count(); }
};

struct SourceInfo {
    std::string path;
    int64_t total_frames = 0;
    double fps = 30.0;
    AVRational fps_q{30, 1};
    double duration = 0.0;
    int width = 0;
    int height = 0;
    int audio_rate = 48000;
    int audio_channels = 2;
    std::vector<float> audio_pcm;
};

struct VideoPlayer {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* frame_rgb = nullptr;
    AVPacket* packet = nullptr;
    uint8_t* buffer = nullptr;
    
    int video_stream = -1;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    double fps = 30.0;              // Frames per second
    AVRational fps_q{0, 1};
    double time_base = 0.0;
    AVRational stream_time_base{0, 1};
    int64_t stream_start_pts = 0;   // Stream's start timestamp offset
    double first_keyframe_time = 0.0;  // Time of first keyframe (for seek threshold)
    
    // Frame-based tracking (primary representation)
    int64_t total_frames = 0;       // Total frames in source video
    int64_t current_frame = 0;      // Current frame number (0-indexed)
    int64_t current_timeline_frame = 0;  // Current frame in timeline order
    int next_clip_color_id = 0;
    
    GLuint texture_id = 0;
    double current_time = 0.0;      // For FFmpeg seeking (derived from current_frame)
    double play_start_time = 0.0;
    int64_t play_start_frame = 0;
    int64_t play_start_timeline_frame = 0;
    bool playing = false;
    bool loaded = false;
    bool audio_loaded = false;
    bool audio_dirty = false;
    
    
    std::vector<Clip> clips;  // Timeline clips (frame-based)
    std::vector<ConnectedClip> connected_clips;  // Overlay clips above/below timeline
    int active_clip = 0;      // Currently selected/playing clip
    int next_connected_clip_color_id = 0;
    
    std::string source_path;
    std::vector<SourceInfo> sources;
    int active_source = -1;
    bool previewing_library = false;
    int preview_restore_source = -1;
    int64_t preview_restore_timeline_frame = 0;

    struct AudioEngine {
        ma_context ctx{};
        ma_device device{};
        bool device_initialized = false;
        std::vector<float> source_pcm;
        std::vector<float> timeline_pcm;
        std::vector<float> preview_pcm;
        int sample_rate = 48000;
        int channels = 2;
        std::atomic<int64_t> playhead_frames{0};
        std::atomic<bool> playing{false};
        std::atomic<bool> use_preview{false};
    } audio;
    
    // Convert between frames and time
    double frame_to_time(int64_t frame) const { return frame / fps; }
    int64_t time_to_frame(double time) const { return (int64_t)(time * fps + 0.5); }

    double timeline_frame_to_time(int64_t timeline_frame) const {
        double t = 0.0;
        int64_t remaining = timeline_frame;
        for (const auto& clip : clips) {
            int64_t count = clip.frame_count();
            int64_t take = std::min<int64_t>(remaining, count);
            if (clip.source_id >= 0 && clip.source_id < (int)sources.size()) {
                double clip_fps = sources[clip.source_id].fps;
                if (clip_fps > 0.0) {
                    t += take / clip_fps;
                }
            }
            remaining -= take;
            if (remaining <= 0) break;
        }
        return t;
    }
    
    int64_t ts_to_frame(int64_t ts) const {
        if (ts == AV_NOPTS_VALUE || fps_q.num == 0 || fps_q.den == 0) {
            return 0;
        }
        const AVRational frame_tb{fps_q.den, fps_q.num}; // seconds per frame
        const auto rounding = static_cast<AVRounding>(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX);
        return av_rescale_q_rnd(ts, stream_time_base, frame_tb, rounding);
    }
    
    int64_t frame_to_ts(int64_t frame) const {
        if (fps_q.num == 0 || fps_q.den == 0) {
            return 0;
        }
        const AVRational frame_tb{fps_q.den, fps_q.num}; // seconds per frame
        const auto rounding = static_cast<AVRounding>(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX);
        return av_rescale_q_rnd(frame, frame_tb, stream_time_base, rounding);
    }
    
    // Total frames across all clips in the edited timeline
    int64_t total_timeline_frames() const {
        int64_t total = 0;
        for (const auto& clip : clips) total += clip.frame_count();
        return total;
    }
    
    // Check if a timeline frame falls on a gap clip
    bool is_timeline_frame_on_gap(int64_t timeline_frame) const {
        if (clips.empty()) return false;
        int64_t pos = 0;
        for (const auto& c : clips) {
            int64_t count = c.frame_count();
            if (timeline_frame >= pos && timeline_frame < pos + count) {
                return c.is_gap;
            }
            pos += count;
        }
        return false;
    }
    
    int64_t timeline_to_source_frame(int64_t timeline_frame, int* out_source_id = nullptr, bool* out_is_gap = nullptr) const {
        if (clips.empty()) return 0;
        int64_t total = total_timeline_frames();
        if (timeline_frame < 0) timeline_frame = 0;
        if (timeline_frame >= total) {
            if (out_source_id) *out_source_id = clips.back().source_id;
            if (out_is_gap) *out_is_gap = clips.back().is_gap;
            return clips.back().end_frame;  // end position
        }
        for (const auto& c : clips) {
            int64_t count = c.frame_count();
            if (timeline_frame < count) {
                if (out_source_id) *out_source_id = c.source_id;
                if (out_is_gap) *out_is_gap = c.is_gap;
                return c.start_frame + timeline_frame;
            }
            timeline_frame -= count;
        }
        if (out_source_id) *out_source_id = clips.back().source_id;
        if (out_is_gap) *out_is_gap = clips.back().is_gap;
        return clips.back().end_frame;
    }
    
    int64_t source_to_timeline_frame(int source_id, int64_t source_frame) const {
        int64_t tframe = 0;
        for (const auto& c : clips) {
            if (c.source_id == source_id && source_frame >= c.start_frame && source_frame < c.end_frame) {
                return tframe + (source_frame - c.start_frame);
            }
            tframe += c.frame_count();
        }
        if (!clips.empty() && clips.back().source_id == source_id && source_frame == clips.back().end_frame) {
            return tframe;  // end position
        }
        return 0;
    }

    static void audio_data_callback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
        auto* audio = reinterpret_cast<AudioEngine*>(device->pUserData);
        float* out = static_cast<float*>(output);
        if (!audio || !audio->playing.load()) {
            std::fill(out, out + frameCount * audio->channels, 0.0f);
            return;
        }
        const bool use_preview = audio->use_preview.load();
        const std::vector<float>& pcm = use_preview ? audio->preview_pcm : audio->timeline_pcm;
        if (pcm.empty()) {
            std::fill(out, out + frameCount * audio->channels, 0.0f);
            return;
        }
        const int channels = audio->channels;
        const int64_t total_frames = static_cast<int64_t>(pcm.size() / channels);
        int64_t playhead = audio->playhead_frames.load();
        if (playhead >= total_frames) {
            audio->playing.store(false);
            std::fill(out, out + frameCount * channels, 0.0f);
            return;
        }
        int64_t frames_to_copy = std::min<int64_t>(frameCount, total_frames - playhead);
        const float* src = pcm.data() + playhead * channels;
        std::memcpy(out, src, static_cast<size_t>(frames_to_copy) * channels * sizeof(float));
        if (frames_to_copy < static_cast<int64_t>(frameCount)) {
            std::fill(out + frames_to_copy * channels, out + frameCount * channels, 0.0f);
            audio->playing.store(false);
        }
        audio->playhead_frames.fetch_add(frames_to_copy);
    }

    void shutdown_audio() {
        audio.playing.store(false);
        if (audio.device_initialized) {
            ma_device_uninit(&audio.device);
            ma_context_uninit(&audio.ctx);
            audio.device_initialized = false;
        }
        audio.source_pcm.clear();
        audio.timeline_pcm.clear();
        audio.preview_pcm.clear();
        audio.use_preview.store(false);
        audio_loaded = false;
    }

    bool init_audio_device() {
        if (audio.device_initialized) {
            ma_device_uninit(&audio.device);
            ma_context_uninit(&audio.ctx);
            audio.device_initialized = false;
        }
        ma_result res = ma_context_init(nullptr, 0, nullptr, &audio.ctx);
        if (res != MA_SUCCESS) {
            return false;
        }
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = audio.channels;
        config.sampleRate = audio.sample_rate;
        config.dataCallback = audio_data_callback;
        config.pUserData = &audio;
        res = ma_device_init(&audio.ctx, &config, &audio.device);
        if (res != MA_SUCCESS) {
            ma_context_uninit(&audio.ctx);
            return false;
        }
        audio.device_initialized = true;
        ma_device_start(&audio.device);
        return true;
    }

    void set_audio_playhead_from_timeline(int64_t timeline_frame) {
        if (!audio_loaded || audio.timeline_pcm.empty()) return;
        double timeline_time = timeline_frame_to_time(timeline_frame);
        int64_t sample_frame = static_cast<int64_t>(timeline_time * audio.sample_rate);
        int64_t max_frame = static_cast<int64_t>(audio.timeline_pcm.size() / audio.channels);
        sample_frame = std::clamp(sample_frame, (int64_t)0, max_frame);
        audio.playhead_frames.store(sample_frame);
    }

    bool decode_audio_for_source(SourceInfo& source) {
        if (!source.audio_pcm.empty()) return true;
        AVFormatContext* a_fmt = nullptr;
        if (avformat_open_input(&a_fmt, source.path.c_str(), nullptr, nullptr) < 0) {
            return false;
        }
        if (avformat_find_stream_info(a_fmt, nullptr) < 0) {
            avformat_close_input(&a_fmt);
            return false;
        }
        int audio_stream = -1;
        for (unsigned i = 0; i < a_fmt->nb_streams; i++) {
            if (a_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream = static_cast<int>(i);
                break;
            }
        }
        if (audio_stream < 0) {
            avformat_close_input(&a_fmt);
            return false;
        }
        AVStream* stream = a_fmt->streams[audio_stream];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            avformat_close_input(&a_fmt);
            return false;
        }
        AVCodecContext* dec_ctx = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(dec_ctx, stream->codecpar);
        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&a_fmt);
            return false;
        }
        int out_rate = 48000;
        AVChannelLayout in_layout = dec_ctx->ch_layout;
        if (in_layout.nb_channels == 0) {
            int fallback_channels = stream->codecpar->ch_layout.nb_channels;
            if (fallback_channels <= 0) fallback_channels = 2;
            av_channel_layout_default(&in_layout, fallback_channels);
        }
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, 2);
        SwrContext* swr = nullptr;
        if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, out_rate,
                                &in_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate,
                                0, nullptr) < 0 || swr_init(swr) < 0) {
            if (swr) swr_free(&swr);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&a_fmt);
            return false;
        }
        std::vector<float> decoded;
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        while (av_read_frame(a_fmt, pkt) >= 0) {
            if (pkt->stream_index != audio_stream) {
                av_packet_unref(pkt);
                continue;
            }
            avcodec_send_packet(dec_ctx, pkt);
            av_packet_unref(pkt);
            while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
                int out_samples = swr_get_out_samples(swr, frame->nb_samples);
                std::vector<float> temp(static_cast<size_t>(out_samples) * out_layout.nb_channels);
                uint8_t* out_data[] = { reinterpret_cast<uint8_t*>(temp.data()) };
                int converted = swr_convert(swr, out_data, out_samples,
                                            const_cast<const uint8_t**>(frame->data), frame->nb_samples);
                if (converted > 0) {
                    size_t count = static_cast<size_t>(converted) * out_layout.nb_channels;
                    decoded.insert(decoded.end(), temp.begin(), temp.begin() + count);
                }
            }
        }
        avcodec_send_packet(dec_ctx, nullptr);
        while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
            int out_samples = swr_get_out_samples(swr, frame->nb_samples);
            std::vector<float> temp(static_cast<size_t>(out_samples) * out_layout.nb_channels);
            uint8_t* out_data[] = { reinterpret_cast<uint8_t*>(temp.data()) };
            int converted = swr_convert(swr, out_data, out_samples,
                                        const_cast<const uint8_t**>(frame->data), frame->nb_samples);
            if (converted > 0) {
                size_t count = static_cast<size_t>(converted) * out_layout.nb_channels;
                decoded.insert(decoded.end(), temp.begin(), temp.begin() + count);
            }
        }
        av_frame_free(&frame);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&a_fmt);

        source.audio_pcm = std::move(decoded);
        source.audio_rate = out_rate;
        source.audio_channels = out_layout.nb_channels;
        return !source.audio_pcm.empty();
    }

    void rebuild_timeline_audio() {
        audio.playing.store(false);
        audio.timeline_pcm.clear();
        if (clips.empty()) {
            audio_loaded = false;
            return;
        }
        audio.sample_rate = 48000;
        audio.channels = 2;
        for (const auto& clip : clips) {
            if (clip.source_id < 0 || clip.source_id >= (int)sources.size()) continue;
            SourceInfo& src = sources[clip.source_id];
            if (src.audio_pcm.empty()) {
                decode_audio_for_source(src);
            }
            double start_time = clip.start_frame / src.fps;
            double end_time = clip.end_frame / src.fps;
            int64_t start_sample = static_cast<int64_t>(start_time * audio.sample_rate);
            int64_t end_sample = static_cast<int64_t>(end_time * audio.sample_rate);
            int64_t clip_samples = std::max<int64_t>(0, end_sample - start_sample);
            if (clip_samples == 0) continue;
            if (src.audio_pcm.empty()) {
                audio.timeline_pcm.insert(audio.timeline_pcm.end(),
                                          static_cast<size_t>(clip_samples) * audio.channels, 0.0f);
                continue;
            }
            const int64_t source_frames = static_cast<int64_t>(src.audio_pcm.size() / src.audio_channels);
            start_sample = std::clamp<int64_t>(start_sample, 0, source_frames);
            end_sample = std::clamp<int64_t>(end_sample, 0, source_frames);
            int64_t available = std::max<int64_t>(0, end_sample - start_sample);
            if (available > 0) {
                size_t begin = static_cast<size_t>(start_sample) * audio.channels;
                size_t end = static_cast<size_t>(end_sample) * audio.channels;
                audio.timeline_pcm.insert(audio.timeline_pcm.end(),
                                          src.audio_pcm.begin() + begin,
                                          src.audio_pcm.begin() + end);
            }
            if (available < clip_samples) {
                int64_t pad_samples = clip_samples - available;
                audio.timeline_pcm.insert(audio.timeline_pcm.end(),
                                          static_cast<size_t>(pad_samples) * audio.channels, 0.0f);
            }
        }
        audio_loaded = !audio.timeline_pcm.empty();
        if (audio_loaded && !audio.device_initialized) {
            init_audio_device();
        }
        set_audio_playhead_from_timeline(current_timeline_frame);
    }
    
    std::optional<SourceInfo> probe_source(const std::string& path) {
        AVFormatContext* ctx = nullptr;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "ignore_editlist", "1", 0);
        if (avformat_open_input(&ctx, path.c_str(), nullptr, &opts) < 0) {
            av_dict_free(&opts);
            return std::nullopt;
        }
        av_dict_free(&opts);
        if (avformat_find_stream_info(ctx, nullptr) < 0) {
            avformat_close_input(&ctx);
            return std::nullopt;
        }
        int video_idx = -1;
        for (unsigned i = 0; i < ctx->nb_streams; i++) {
            if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_idx = i;
                break;
            }
        }
        if (video_idx < 0) {
            avformat_close_input(&ctx);
            return std::nullopt;
        }
        AVStream* stream = ctx->streams[video_idx];
        AVCodecParameters* codecpar = stream->codecpar;
        SourceInfo info;
        info.path = path;
        info.width = codecpar->width;
        info.height = codecpar->height;
        if (stream->avg_frame_rate.num && stream->avg_frame_rate.den) {
            info.fps_q = stream->avg_frame_rate;
        } else if (stream->r_frame_rate.num && stream->r_frame_rate.den) {
            info.fps_q = stream->r_frame_rate;
        } else {
            info.fps_q = {30, 1};
        }
        info.fps = av_q2d(info.fps_q);
        double time_base_local = av_q2d(stream->time_base);
        info.duration = (stream->duration != AV_NOPTS_VALUE)
            ? (stream->duration * time_base_local)
            : (ctx->duration / (double)AV_TIME_BASE);
        if (stream->nb_frames > 0) {
            info.total_frames = stream->nb_frames;
        } else if (stream->duration != AV_NOPTS_VALUE) {
            info.total_frames = (int64_t)(info.duration * info.fps + 0.5);
        }
        avformat_close_input(&ctx);
        return info;
    }

    int add_source(const std::string& path) {
        for (int i = 0; i < (int)sources.size(); i++) {
            if (sources[i].path == path) return i;
        }
        auto info = probe_source(path);
        if (!info.has_value()) return -1;
        sources.push_back(*info);
        return static_cast<int>(sources.size() - 1);
    }

    bool remove_source(int source_id) {
        if (source_id < 0 || source_id >= (int)sources.size()) return false;
        const bool removing_active = (source_id == active_source);
        sources.erase(sources.begin() + source_id);
        for (auto it = clips.begin(); it != clips.end(); ) {
            if (it->source_id == source_id) {
                it = clips.erase(it);
                continue;
            }
            if (it->source_id > source_id) {
                it->source_id--;
            }
            ++it;
        }
        if (sources.empty()) {
            close_decoder();
            active_source = -1;
            clips.clear();
            current_timeline_frame = 0;
            shutdown_audio();
            return true;
        }
        if (removing_active) {
            active_source = std::clamp(active_source, 0, (int)sources.size() - 1);
            open_source(active_source, false, false);
        }
        current_timeline_frame = std::clamp<int64_t>(current_timeline_frame, 0, total_timeline_frames());
        audio_dirty = true;
        return true;
    }

    void close_decoder() {
        playing = false;
        loaded = false;
        
        if (buffer) { av_free(buffer); buffer = nullptr; }
        if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
        if (packet) { av_packet_free(&packet); packet = nullptr; }
        if (frame_rgb) { av_frame_free(&frame_rgb); frame_rgb = nullptr; }
        if (frame) { av_frame_free(&frame); frame = nullptr; }
        if (codec_ctx) { avcodec_free_context(&codec_ctx); codec_ctx = nullptr; }
        if (format_ctx) { avformat_close_input(&format_ctx); format_ctx = nullptr; }
        
        video_stream = -1;
        width = height = 0;
        duration = 0.0;
        current_time = 0.0;
        current_frame = 0;
        total_frames = 0;
        stream_start_pts = 0;
        first_keyframe_time = 0.0;
        source_path.clear();
    }

    bool open_source(int source_id, bool reset_timeline, bool keep_playing = false) {
        if (source_id < 0 || source_id >= (int)sources.size()) return false;
        bool was_playing = playing;
        close_decoder();
        const std::string& path = sources[source_id].path;
        source_path = path;
        active_source = source_id;
        
        // Ignore edit list to avoid seeking issues with videos that have non-keyframe start points
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "ignore_editlist", "1", 0);
        
        int ret = avformat_open_input(&format_ctx, path.c_str(), nullptr, &opts);
        
        // Check if the option was consumed (recognized)
        AVDictionaryEntry* e = av_dict_get(opts, "", nullptr, AV_DICT_IGNORE_SUFFIX);
        while (e) {
            std::printf("[WARN] Unrecognized option: %s=%s\n", e->key, e->value);
            e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX);
        }
        av_dict_free(&opts);
        
        if (ret < 0) {
            std::fprintf(stderr, "Could not open file: %s\n", path.c_str());
            return false;
        }
        
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            std::fprintf(stderr, "Could not find stream info\n");
            close_decoder();
            return false;
        }
        
        for (unsigned i = 0; i < format_ctx->nb_streams; i++) {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream = i;
                break;
            }
        }
        
        if (video_stream < 0) {
            std::fprintf(stderr, "No video stream found\n");
            close_decoder();
            return false;
        }
        
        AVStream* stream = format_ctx->streams[video_stream];
        AVCodecParameters* codecpar = stream->codecpar;
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            std::fprintf(stderr, "Codec not found\n");
            close_decoder();
            return false;
        }
        
        codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx, codecpar);
        
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::fprintf(stderr, "Could not open codec\n");
            close_decoder();
            return false;
        }
        
        width = codec_ctx->width;
        height = codec_ctx->height;
        stream_time_base = stream->time_base;
        time_base = av_q2d(stream_time_base);
        duration = (stream->duration != AV_NOPTS_VALUE)
            ? (stream->duration * time_base)
            : (format_ctx->duration / (double)AV_TIME_BASE);
        stream_start_pts = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
        
        if (stream->avg_frame_rate.num && stream->avg_frame_rate.den) {
            fps_q = stream->avg_frame_rate;
        } else if (stream->r_frame_rate.num && stream->r_frame_rate.den) {
            fps_q = stream->r_frame_rate;
        } else if (codec_ctx->framerate.num && codec_ctx->framerate.den) {
            fps_q = codec_ctx->framerate;
        }
        if (fps_q.num && fps_q.den) {
            fps = av_q2d(fps_q);
        } else {
            fps_q = { (int)(fps * 1000.0 + 0.5), 1000 };
            fps = av_q2d(fps_q);
        }
        
        frame = av_frame_alloc();
        frame_rgb = av_frame_alloc();
        packet = av_packet_alloc();
        
        buffer = (uint8_t*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1));
        av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, buffer, AV_PIX_FMT_RGB24, width, height, 1);
        
        sws_ctx = sws_getContext(
            width, height, codec_ctx->pix_fmt,
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!texture_id) {
            glGenTextures(1, &texture_id);
        }
        glBindTexture(GL_TEXTURE_2D, texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        
        loaded = true;
        if (!keep_playing) {
            current_time = 0.0;
            current_frame = 0;
            playing = false;
        } else {
            playing = was_playing;
        }
        
        // Detect first keyframe position by doing an initial decode
        decode_frame();
        first_keyframe_time = current_time;
        
        // Calculate total frames
        if (stream->nb_frames > 0) {
            total_frames = stream->nb_frames;
        } else if (stream->duration != AV_NOPTS_VALUE && fps_q.num && fps_q.den) {
            const AVRational frame_tb{fps_q.den, fps_q.num};
            const auto rounding = static_cast<AVRounding>(AV_ROUND_UP | AV_ROUND_PASS_MINMAX);
            total_frames = av_rescale_q_rnd(stream->duration, stream_time_base, frame_tb, rounding);
        } else {
            total_frames = (int64_t)(duration * fps + 0.5);
        }
        if (total_frames < 1) total_frames = 1;
        
        sources[source_id].fps = fps;
        sources[source_id].fps_q = fps_q;
        sources[source_id].duration = duration;
        sources[source_id].total_frames = total_frames;
        sources[source_id].width = width;
        sources[source_id].height = height;
        
        std::printf("Video: %d x %d, %.5f fps, %lld frames, %.3f sec (tb=%d/%d, fps_q=%d/%d)\n",
                    width, height, fps, (long long)total_frames, duration,
                    stream_time_base.num, stream_time_base.den, fps_q.num, fps_q.den);
        
        if (reset_timeline) {
            // Initialize with single clip covering entire video [0, total_frames)
            clips.clear();
            next_clip_color_id = 0;
            clips.push_back({source_id, 0, total_frames, next_clip_color_id++});
            active_clip = 0;
            current_frame = 0;
            current_timeline_frame = 0;
            audio_dirty = true;
            if (decode_audio_for_source(sources[source_id])) {
                init_audio_device();
                rebuild_timeline_audio();
            }
            audio_dirty = false;
        }
        
        return true;
    }

    bool open(const std::string& path) {
        close();
        int source_id = add_source(path);
        if (source_id < 0) return false;
        return open_source(source_id, true);
    }
    
    void close() {
        close_decoder();
        if (texture_id) { glDeleteTextures(1, &texture_id); texture_id = 0; }
        clips.clear();
        sources.clear();
        active_source = -1;
        active_clip = 0;
        current_timeline_frame = 0;
        next_clip_color_id = 0;
        previewing_library = false;
        preview_restore_source = -1;
        preview_restore_timeline_frame = 0;
        shutdown_audio();
        audio_dirty = false;
    }
    
    bool decode_frame() {
        while (av_read_frame(format_ctx, packet) >= 0) {
            if (packet->stream_index == video_stream) {
                if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                    if (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                                  frame_rgb->data, frame_rgb->linesize);
                        
                        // Use best-effort timestamp to avoid B-frame reordering issues
                        int64_t ts = frame->best_effort_timestamp;
                        if (ts == AV_NOPTS_VALUE) ts = frame->pts;
                        if (ts == AV_NOPTS_VALUE) ts = stream_start_pts;
                        ts = ts - stream_start_pts;
                
                        current_frame = ts_to_frame(ts);
                        current_time = frame_to_time(current_frame);
                        
                        
                        glBindTexture(GL_TEXTURE_2D, texture_id);
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame_rgb->linesize[0] / 3);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, frame_rgb->data[0]);
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                        
                        av_packet_unref(packet);
                        return true;
                    }
                }
            }
            av_packet_unref(packet);
        }
        return false;
    }
    
    // Check if current_frame is at the end of the timeline (past all clips)
    bool at_timeline_end() const {
        if (clips.empty()) return false;
        return current_timeline_frame >= total_timeline_frames();
    }
    
    // Seek to a specific frame number
    // Allows seeking to end_frame (one past last valid frame) - will display last frame
    void seek_to_frame(int64_t target_frame, bool reset_play_clock = true, bool update_timeline_frame = true, int64_t display_frame_override = -1, bool update_audio_playhead = true) {
        if (!loaded) return;
        
        // Allow target_frame to be at the end position (for "at end" state)
        int64_t max_frame = total_frames;
        target_frame = std::clamp(target_frame, (int64_t)0, max_frame);
        
        // For display, seek to the actual displayable frame
        int64_t max_display_frame = std::max<int64_t>(0, max_frame - 1);
        int64_t display_frame = display_frame_override >= 0
            ? std::clamp(display_frame_override, (int64_t)0, max_display_frame)
            : std::min(target_frame, max_display_frame);
        double target_time = frame_to_time(display_frame);
        target_time = std::clamp(target_time, 0.0, duration);
        int64_t target_ts = stream_start_pts + frame_to_ts(display_frame);
        
        
        // For positions before the first keyframe, seek from start
        // For other positions, use fast keyframe-based seeking
        if (target_time <= first_keyframe_time + 0.1) {
            // Seek to very beginning, then decode forward
            avformat_seek_file(format_ctx, video_stream, 0, 0, target_ts, 0);
        } else {
            // Seek backward to nearest keyframe before target (fast)
            avformat_seek_file(format_ctx, video_stream, 0, target_ts, target_ts, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(codec_ctx);
        
        // Decode forward to reach the exact target frame
        while (decode_frame()) {
            if (current_frame >= display_frame) break;
        }
        
        // Set frame-based position (authoritative) - can be at "end" position
        current_frame = target_frame;
        current_time = frame_to_time(target_frame);
        if (update_timeline_frame) {
            current_timeline_frame = source_to_timeline_frame(active_source, current_frame);
        }
        
        if (playing && reset_play_clock) {
            play_start_time = glfwGetTime();
            play_start_frame = current_frame;
            play_start_timeline_frame = current_timeline_frame;
        }
        if (update_audio_playhead) {
            set_audio_playhead_from_timeline(current_timeline_frame);
        }
    }
    
    // Legacy seek by time (converts to frame, then seeks)
    void seek(double time) {
        seek_to_frame(time_to_frame(time));
    }
    
    // Find topmost connected clip covering a timeline frame (highest lane takes priority)
    // Returns index into connected_clips, or -1 if none
    // Only considers the "active" part of clips (frames >= 0 on the timeline)
    int connected_clip_at_timeline_frame(int64_t timeline_frame) const {
        if (timeline_frame < 0) return -1;  // Nothing plays before timeline start
        
        int best_idx = -1;
        int best_lane = INT_MIN;
        for (int i = 0; i < (int)connected_clips.size(); i++) {
            const auto& cc = connected_clips[i];
            
            // Clip must extend into the valid timeline range (>= 0)
            // Active range is [max(0, timeline_start()), timeline_end())
            int64_t active_start = std::max((int64_t)0, cc.timeline_start());
            if (timeline_frame >= active_start && timeline_frame < cc.timeline_end()) {
                // For clips above (positive lane), higher is better
                // For clips below (negative lane), they don't normally play (future audio use)
                if (cc.lane > 0 && cc.lane > best_lane) {
                    best_lane = cc.lane;
                    best_idx = i;
                }
            }
        }
        return best_idx;
    }

    // Seek by timeline frame (clip order)
    void seek_to_timeline_frame(int64_t timeline_frame, bool reset_play_clock = true, bool update_audio_playhead = true) {
        if (clips.empty()) return;
        int64_t max_timeline = total_timeline_frames();
        timeline_frame = std::clamp(timeline_frame, (int64_t)0, max_timeline);
        current_timeline_frame = timeline_frame;
        
        // Check for connected clip at this timeline frame (topmost layer takes priority)
        int cc_idx = connected_clip_at_timeline_frame(timeline_frame);
        if (cc_idx >= 0) {
            const auto& cc = connected_clips[cc_idx];
            // Calculate offset from the clip's visual start (timeline_start)
            int64_t offset_in_clip = timeline_frame - cc.timeline_start();
            int64_t source_frame = cc.start_frame + offset_in_clip;
            if (cc.source_id != active_source && cc.source_id >= 0) {
                open_source(cc.source_id, false, playing);
                previewing_library = false;
            }
            seek_to_frame(source_frame, reset_play_clock, false, -1, update_audio_playhead);
            return;
        }
        
        // Check if we're on a gap clip - if so, don't seek to any video
        // (Connected clips are already handled above, so this is a true gap with no overlay)
        if (is_timeline_frame_on_gap(timeline_frame)) {
            return;
        }
        
        int64_t display_override = -1;
        if (timeline_frame == max_timeline) {
            int64_t last_clip_frame = clips.back().end_frame - 1;
            if (last_clip_frame >= 0) {
                display_override = last_clip_frame;
            }
        }
        int source_id = active_source;
        int64_t source_frame = timeline_to_source_frame(timeline_frame, &source_id);
        if (source_id != active_source && source_id >= 0) {
            open_source(source_id, false, playing);
            previewing_library = false;
        }
        seek_to_frame(source_frame, reset_play_clock, false, display_override, update_audio_playhead);
    }
    
    // Find which clip index contains a given source frame, returns -1 if not in any clip
    int clip_at_frame(int source_id, int64_t frame) const {
        for (int i = 0; i < (int)clips.size(); i++) {
            if (clips[i].source_id == source_id && frame >= clips[i].start_frame && frame < clips[i].end_frame) {
                return i;
            }
        }
        return -1;
    }
    
    // Split the clip at the given source frame
    void split_at_frame(int source_id, int64_t frame) {
        int idx = clip_at_frame(source_id, frame);
        if (idx < 0) return;
        
        Clip& clip = clips[idx];
        // Don't split if too close to edges (need at least 1 frame per clip)
        if (frame - clip.start_frame < 1 || clip.end_frame - frame < 1) return;
        
        Clip new_clip = {clip.source_id, frame, clip.end_frame, next_clip_color_id++};
        clip.end_frame = frame;
        clips.insert(clips.begin() + idx + 1, new_clip);
    }
    
    // Legacy split by time (converts to frame)
    void split_at(double source_time) {
        split_at_frame(active_source, time_to_frame(source_time));
    }

    bool insert_clip_at_timeline(int source_id, int64_t timeline_frame) {
        if (source_id < 0 || source_id >= (int)sources.size()) return false;
        if (sources[source_id].total_frames <= 0) return false;
        int64_t total = total_timeline_frames();
        timeline_frame = std::clamp<int64_t>(timeline_frame, 0, total);
        int insert_index = (int)clips.size();
        int64_t offset = timeline_frame;
        for (int i = 0; i < (int)clips.size(); i++) {
            int64_t count = clips[i].frame_count();
            if (offset <= count) {
                insert_index = i;
                break;
            }
            offset -= count;
        }
        Clip new_clip{source_id, 0, sources[source_id].total_frames, next_clip_color_id++};
        if (clips.empty()) {
            clips.push_back(new_clip);
            return true;
        }
        if (insert_index >= (int)clips.size()) {
            clips.push_back(new_clip);
            return true;
        }
        Clip& target = clips[insert_index];
        if (offset == 0) {
            clips.insert(clips.begin() + insert_index, new_clip);
            return true;
        }
        if (offset >= target.frame_count()) {
            clips.insert(clips.begin() + insert_index + 1, new_clip);
            return true;
        }
        int64_t split_frame = target.start_frame + offset;
        Clip right_clip{target.source_id, split_frame, target.end_frame, target.color_id};
        target.end_frame = split_frame;
        clips.insert(clips.begin() + insert_index + 1, new_clip);
        clips.insert(clips.begin() + insert_index + 2, right_clip);
        return true;
    }

    bool connect_clip_at_timeline(int source_id, int64_t timeline_frame, int preferred_lane = 1) {
        if (source_id < 0 || source_id >= (int)sources.size()) return false;
        if (sources[source_id].total_frames <= 0) return false;
        
        int64_t new_clip_frames = sources[source_id].total_frames;
        int64_t new_start = timeline_frame;
        int64_t new_end = timeline_frame + new_clip_frames;
        
        // Find available lane (check positive lanes 1, 2 first, then negative -1, -2)
        auto lane_available = [&](int lane) {
            for (const auto& cc : connected_clips) {
                if (cc.lane != lane) continue;
                int64_t cc_end = cc.connection_frame + cc.frame_count();
                // Check for overlap
                if (new_start < cc_end && new_end > cc.connection_frame) {
                    return false;  // Overlap found
                }
            }
            return true;
        };
        
        int lane = preferred_lane;
        if (!lane_available(lane)) {
            // Try other positive lanes
            for (int try_lane = 1; try_lane <= 2; try_lane++) {
                if (lane_available(try_lane)) {
                    lane = try_lane;
                    break;
                }
            }
            // If still not found, try negative lanes
            if (!lane_available(lane)) {
                for (int try_lane = -1; try_lane >= -2; try_lane--) {
                    if (lane_available(try_lane)) {
                        lane = try_lane;
                        break;
                    }
                }
            }
        }
        
        ConnectedClip cc;
        cc.source_id = source_id;
        cc.start_frame = 0;
        cc.end_frame = sources[source_id].total_frames;
        cc.connection_frame = timeline_frame;
        cc.lane = lane;
        cc.color_id = next_connected_clip_color_id++;
        connected_clips.push_back(cc);
        return true;
    }

    void stop_library_preview() {
        if (!previewing_library) return;
        pause();
        audio.use_preview.store(false);
        audio.preview_pcm.clear();
        previewing_library = false;
        if (!clips.empty()) {
            current_timeline_frame = std::clamp<int64_t>(
                preview_restore_timeline_frame, 0, total_timeline_frames());
            seek_to_timeline_frame(current_timeline_frame);
        } else if (preview_restore_source >= 0 && preview_restore_source < (int)sources.size()) {
            open_source(preview_restore_source, false, false);
        }
    }

    void start_library_preview(int source_id) {
        if (source_id < 0 || source_id >= (int)sources.size()) return;
        if (previewing_library) {
            stop_library_preview();
        }
        preview_restore_source = active_source;
        preview_restore_timeline_frame = current_timeline_frame;
        pause();
        previewing_library = true;
        open_source(source_id, false, false);
        seek_to_frame(0, true, false, -1, false);
        if (sources[source_id].audio_pcm.empty()) {
            decode_audio_for_source(sources[source_id]);
        }
        audio.playing.store(false);
        audio.use_preview.store(false);
        audio.preview_pcm = sources[source_id].audio_pcm;
        audio.playhead_frames.store(0);
        audio.use_preview.store(true);
        if (!audio.device_initialized) {
            init_audio_device();
        }
        playing = true;
        play_start_time = glfwGetTime();
        play_start_frame = 0;
        if (!audio.preview_pcm.empty()) {
            audio.playing.store(true);
        }
    }
    
    void play() {
        if (!loaded) return;
        if (previewing_library) {
            playing = true;
            play_start_time = glfwGetTime();
            play_start_frame = current_frame;
            if (audio.use_preview.load()) {
                audio.playing.store(true);
            }
            return;
        }
        if (clips.empty()) return;
        int64_t max_timeline = total_timeline_frames();
        current_timeline_frame = std::clamp(current_timeline_frame, (int64_t)0, max_timeline);
        playing = true;
        play_start_time = glfwGetTime();
        play_start_timeline_frame = current_timeline_frame;
        if (audio_loaded) {
            set_audio_playhead_from_timeline(current_timeline_frame);
            audio.playing.store(true);
        }
    }
    
    void pause() {
        playing = false;
        audio.playing.store(false);
    }
    
    void toggle_play() {
        if (playing) pause();
        else play();
    }
    
    void update() {
        if (!loaded || !playing) return;
        if (previewing_library) {
            double elapsed = glfwGetTime() - play_start_time;
            int64_t target_frame = play_start_frame + (int64_t)(elapsed * fps);
            if (target_frame >= total_frames) {
                seek_to_frame(total_frames > 0 ? total_frames - 1 : 0, true, false, -1, false);
                pause();
                return;
            }
            if (target_frame == current_frame) return;
            seek_to_frame(target_frame, false, false, -1, false);
            return;
        }
        if (clips.empty()) return;
        double elapsed = glfwGetTime() - play_start_time;
        int64_t target_timeline = play_start_timeline_frame + (int64_t)(elapsed * fps);
        int64_t max_timeline = total_timeline_frames();
        if (target_timeline >= max_timeline) {
            seek_to_timeline_frame(max_timeline, true, true);
            pause();
            return;
        }
        if (target_timeline == current_timeline_frame) return;
        seek_to_timeline_frame(target_timeline, false, false);
    }
    
    std::string format_time(int64_t frame) const {
        double t = frame_to_time(frame);
        int minutes = (int)(t / 60);
        int seconds = (int)t % 60;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%02d.%03lld", minutes, seconds, (long long)(frame % (int64_t)fps));
        return buf;
    }
    
    // Legacy format_time for double
    std::string format_time_legacy(double t) const {
        int minutes = (int)(t / 60);
        int seconds = (int)t % 60;
        int frames = (int)((t - (int)t) * fps);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", minutes, seconds, frames);
        return buf;
    }
};

// Export multiple clips with re-encoding for clean transitions
// Debug: Export frames as PNG files to a folder
bool export_frames_as_png(const std::string& input, const std::string& output_folder, const std::vector<Clip>& clips, double fps, std::atomic<bool>& exporting, std::atomic<float>& progress) {
    if (clips.empty()) {
        exporting = false;
        return false;
    }
    
    // Create output folder
    fs::create_directories(output_folder);
    
    AVFormatContext* in_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    int video_stream_idx = -1;
    
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "ignore_editlist", "1", 0);
    int ret = avformat_open_input(&in_ctx, input.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        exporting = false;
        return false;
    }
    
    if (avformat_find_stream_info(in_ctx, nullptr) < 0) {
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    for (unsigned i = 0; i < in_ctx->nb_streams; i++) {
        if (in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    
    if (video_stream_idx < 0) {
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    AVStream* in_video = in_ctx->streams[video_stream_idx];
    const AVCodec* decoder = avcodec_find_decoder(in_video->codecpar->codec_id);
    dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, in_video->codecpar);
    avcodec_open2(dec_ctx, decoder, nullptr);
    
    // Set up RGB conversion for PNG output
    sws_ctx = sws_getContext(
        dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        dec_ctx->width, dec_ctx->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    auto frame_to_time = [fps](int64_t f) -> double { return f / fps; };
    
    int64_t total_frames = 0;
    for (const auto& clip : clips) total_frames += clip.frame_count();
    
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width = dec_ctx->width;
    rgb_frame->height = dec_ctx->height;
    av_frame_get_buffer(rgb_frame, 0);
    
    int64_t output_frame_num = 0;
    bool first_clip = true;
    
    for (const auto& clip : clips) {
        if (!exporting) break;
        
        double clip_start_time = frame_to_time(clip.start_frame);
        
        std::printf("PNG_EXPORT: Processing clip [%lld - %lld)\n", 
                    (long long)clip.start_frame, (long long)clip.end_frame);
        
        if (first_clip && clip.start_frame == 0) {
            std::printf("PNG_EXPORT: Starting from beginning\n");
        } else {
            avcodec_flush_buffers(dec_ctx);
            int64_t start_ts = (int64_t)(clip_start_time * AV_TIME_BASE);
            avformat_seek_file(in_ctx, -1, INT64_MIN, start_ts, start_ts, 0);
            avcodec_flush_buffers(dec_ctx);
        }
        first_clip = false;
        
        std::map<int64_t, AVFrame*> frame_buffer;
        int64_t next_frame_to_save = clip.start_frame;
        
        auto save_ready_frames = [&]() {
            while (frame_buffer.count(next_frame_to_save)) {
                AVFrame* buffered = frame_buffer[next_frame_to_save];
                frame_buffer.erase(next_frame_to_save);
                
                // Convert to RGB
                av_frame_make_writable(rgb_frame);
                sws_scale(sws_ctx, buffered->data, buffered->linesize, 0, buffered->height,
                          rgb_frame->data, rgb_frame->linesize);
                
                // Save as PPM (simple format, can be converted to PNG)
                std::string filename = output_folder + "/frame_" + 
                    std::to_string(output_frame_num) + "_src" + 
                    std::to_string(next_frame_to_save) + ".ppm";
                
                FILE* f = fopen(filename.c_str(), "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", rgb_frame->width, rgb_frame->height);
                    for (int y = 0; y < rgb_frame->height; y++) {
                        fwrite(rgb_frame->data[0] + y * rgb_frame->linesize[0], 
                               1, rgb_frame->width * 3, f);
                    }
                    fclose(f);
                    std::printf("PNG_EXPORT: Saved %s\n", filename.c_str());
                }
                
                progress = (float)(output_frame_num + 1) / total_frames;
                output_frame_num++;
                av_frame_free(&buffered);
                next_frame_to_save++;
            }
        };
        
        // Lambda to process a decoded frame
        auto process_frame = [&](AVFrame* decoded_frame) -> bool {
            int64_t frame_pts = decoded_frame->best_effort_timestamp != AV_NOPTS_VALUE 
                                ? decoded_frame->best_effort_timestamp : decoded_frame->pts;
            double frame_time = frame_pts * av_q2d(in_video->time_base);
            int64_t source_frame_idx = (int64_t)(frame_time * fps + 0.5);
            
            if (source_frame_idx < clip.start_frame) return false;
            
            if (source_frame_idx >= clip.end_frame) {
                save_ready_frames();
                return next_frame_to_save >= clip.end_frame;
            }
            
            AVFrame* cloned = av_frame_clone(decoded_frame);
            frame_buffer[source_frame_idx] = cloned;
            save_ready_frames();
            return false;
        };
        
        bool clip_done = false;
        while (av_read_frame(in_ctx, pkt) >= 0 && exporting && !clip_done) {
            if (pkt->stream_index == video_stream_idx) {
                avcodec_send_packet(dec_ctx, pkt);
                while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
                    if (process_frame(frame)) {
                        clip_done = true;
                        break;
                    }
                }
            }
            av_packet_unref(pkt);
        }
        
        // Flush decoder to get any remaining frames
        if (!clip_done && exporting) {
            avcodec_send_packet(dec_ctx, nullptr);
            while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
                if (process_frame(frame)) break;
            }
            save_ready_frames();
        }
        
        png_done_with_clip:
        
        for (auto& [idx, f] : frame_buffer) av_frame_free(&f);
        frame_buffer.clear();
    }
    
    av_frame_free(&rgb_frame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&in_ctx);
    
    progress = 1.0f;
    exporting = false;
    return true;
}

struct ExportAudio {
    std::vector<float> timeline_pcm;
    int sample_rate = 48000;
    int channels = 2;
    bool has_audio = false;
};

// Helper to find topmost connected clip at a timeline frame
// Only considers active part of clips (frames >= 0 on timeline)
int find_connected_clip_at_frame(const std::vector<ConnectedClip>& connected_clips, int64_t timeline_frame) {
    if (timeline_frame < 0) return -1;  // Nothing plays before timeline start
    
    int best_idx = -1;
    int best_lane = INT_MIN;
    for (int i = 0; i < (int)connected_clips.size(); i++) {
        const auto& cc = connected_clips[i];
        // Active range is [max(0, timeline_start()), timeline_end())
        int64_t active_start = std::max((int64_t)0, cc.timeline_start());
        if (timeline_frame >= active_start && timeline_frame < cc.timeline_end()) {
            if (cc.lane > 0 && cc.lane > best_lane) {
                best_lane = cc.lane;
                best_idx = i;
            }
        }
    }
    return best_idx;
}

// SIMPLIFIED LINEAR EXPORT: decode → encode → write immediately, no buffering
bool export_clips(const std::vector<std::string>& source_paths, const std::string& output, const std::vector<Clip>& clips,
                  const std::vector<ConnectedClip>& connected_clips,
                  double fps, const ExportAudio& audio_export, std::atomic<bool>& exporting, std::atomic<float>& progress) {
    if (clips.empty()) {
        exporting = false;
        return false;
    }
    
    std::printf("EXPORT: === SIMPLIFIED LINEAR EXPORT ===\n");
    
    // Calculate total frames
    int64_t total_frames = 0;
    for (const auto& clip : clips) {
        total_frames += clip.frame_count();
    }
    std::printf("EXPORT: Total frames to export: %lld\n", (long long)total_frames);
    
    struct SourceDecodeState {
        AVFormatContext* in_ctx = nullptr;
        AVCodecContext* dec_ctx = nullptr;
        AVStream* in_video = nullptr;
        int video_idx = -1;
        AVRational stream_time_base{0, 1};
        int64_t stream_start_pts = 0;
        AVRational fps_q{0, 1};
        AVRational frame_tb{0, 1};
        SwsContext* debug_sws = nullptr;
        AVFrame* debug_rgb = nullptr;
    };

    auto close_source = [&](SourceDecodeState& s) {
        if (s.debug_rgb) av_frame_free(&s.debug_rgb);
        if (s.debug_sws) sws_freeContext(s.debug_sws);
        if (s.dec_ctx) avcodec_free_context(&s.dec_ctx);
        if (s.in_ctx) avformat_close_input(&s.in_ctx);
        s = {};
    };

    auto open_source = [&](const std::string& path, SourceDecodeState& s) -> bool {
        close_source(s);
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "ignore_editlist", "1", 0);
        if (avformat_open_input(&s.in_ctx, path.c_str(), nullptr, &opts) < 0) {
            av_dict_free(&opts);
            return false;
        }
        av_dict_free(&opts);
        avformat_find_stream_info(s.in_ctx, nullptr);
        for (unsigned i = 0; i < s.in_ctx->nb_streams; i++) {
            if (s.in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                s.video_idx = static_cast<int>(i);
                break;
            }
        }
        if (s.video_idx < 0) {
            close_source(s);
            return false;
        }
        s.in_video = s.in_ctx->streams[s.video_idx];
        s.stream_time_base = s.in_video->time_base;
        s.stream_start_pts = (s.in_video->start_time != AV_NOPTS_VALUE) ? s.in_video->start_time : 0;
        if (s.in_video->avg_frame_rate.num && s.in_video->avg_frame_rate.den) {
            s.fps_q = s.in_video->avg_frame_rate;
        } else if (s.in_video->r_frame_rate.num && s.in_video->r_frame_rate.den) {
            s.fps_q = s.in_video->r_frame_rate;
        } else {
            s.fps_q = av_d2q(fps > 0.0 ? fps : 30.0, 100000);
        }
        if (s.fps_q.num == 0 || s.fps_q.den == 0) {
            s.fps_q = {30, 1};
        }
        s.frame_tb = {s.fps_q.den, s.fps_q.num};

        const AVCodec* decoder = avcodec_find_decoder(s.in_video->codecpar->codec_id);
        s.dec_ctx = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(s.dec_ctx, s.in_video->codecpar);
        s.dec_ctx->thread_count = 1;
        if (avcodec_open2(s.dec_ctx, decoder, nullptr) < 0) {
            close_source(s);
            return false;
        }
        return true;
    };

    if (clips.front().source_id < 0 || clips.front().source_id >= (int)source_paths.size()) {
        exporting = false;
        return false;
    }
    SourceDecodeState source_state;
    if (!open_source(source_paths[clips.front().source_id], source_state)) {
        exporting = false;
        return false;
    }
    AVRational fps_q = av_d2q(fps > 0.0 ? fps : av_q2d(source_state.fps_q), 100000);
    if (fps_q.num == 0 || fps_q.den == 0) {
        fps_q = {30, 1};
    }
    const AVRational frame_tb{fps_q.den, fps_q.num};

    // Create output
    AVFormatContext* out_ctx = nullptr;
    avformat_alloc_output_context2(&out_ctx, nullptr, nullptr, output.c_str());
    
    // Set up encoder (software only, single-threaded)
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    
    AVStream* out_video = avformat_new_stream(out_ctx, nullptr);
    AVCodecContext* enc_ctx = avcodec_alloc_context3(encoder);
    enc_ctx->width = source_state.dec_ctx->width;
    enc_ctx->height = source_state.dec_ctx->height;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = frame_tb;
    enc_ctx->framerate = fps_q;
    enc_ctx->gop_size = 1;      // All I-frames
    enc_ctx->max_b_frames = 0;  // No B-frames
    enc_ctx->thread_count = 1;  // Single-threaded encoding
    
    if (out_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    
    AVDictionary* enc_opts = nullptr;
    av_dict_set(&enc_opts, "preset", "ultrafast", 0);
    av_dict_set(&enc_opts, "tune", "zerolatency", 0);  // Minimize latency/buffering
    avcodec_open2(enc_ctx, encoder, &enc_opts);
    av_dict_free(&enc_opts);
    
    avcodec_parameters_from_context(out_video->codecpar, enc_ctx);
    out_video->time_base = enc_ctx->time_base;

    // Audio encoder (AAC) if we have decoded audio
    AVStream* out_audio = nullptr;
    AVCodecContext* audio_enc = nullptr;
    SwrContext* audio_swr = nullptr;
    if (audio_export.has_audio) {
        const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (aac) {
            out_audio = avformat_new_stream(out_ctx, nullptr);
            audio_enc = avcodec_alloc_context3(aac);
            audio_enc->sample_rate = audio_export.sample_rate;
            av_channel_layout_default(&audio_enc->ch_layout, audio_export.channels);
            audio_enc->time_base = AVRational{1, audio_export.sample_rate};
            audio_enc->bit_rate = 128000;
            audio_enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
            const void* configs = nullptr;
            int num_configs = 0;
            if (avcodec_get_supported_config(audio_enc, aac, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                            &configs, &num_configs) >= 0 && configs && num_configs > 0) {
                auto fmts = static_cast<const AVSampleFormat*>(configs);
                bool supported = false;
                for (int i = 0; i < num_configs; ++i) {
                    if (fmts[i] == audio_enc->sample_fmt) {
                        supported = true;
                        break;
                    }
                }
                if (!supported) {
                    audio_enc->sample_fmt = fmts[0];
                }
            }
            if (out_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
                audio_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            if (avcodec_open2(audio_enc, aac, nullptr) < 0) {
                avcodec_free_context(&audio_enc);
                out_audio = nullptr;
            } else {
                avcodec_parameters_from_context(out_audio->codecpar, audio_enc);
                out_audio->time_base = audio_enc->time_base;
                AVChannelLayout in_layout;
                av_channel_layout_default(&in_layout, audio_export.channels);
                if (swr_alloc_set_opts2(&audio_swr, &audio_enc->ch_layout, audio_enc->sample_fmt,
                                        audio_enc->sample_rate, &in_layout, AV_SAMPLE_FMT_FLT,
                                        audio_export.sample_rate, 0, nullptr) < 0 ||
                    swr_init(audio_swr) < 0) {
                    if (audio_swr) swr_free(&audio_swr);
                    avcodec_free_context(&audio_enc);
                    out_audio = nullptr;
                }
            }
        }
    }
    
    std::printf("EXPORT: Encoder: %s, single-threaded, gop=1, no B-frames, zerolatency\n", encoder->name);
    
    // Debug output setup
    fs::path debug_dir = fs::path(output).parent_path() / "export_debug";
    fs::create_directories(debug_dir);
    auto ensure_source_converters = [&]() {
        if (!source_state.debug_sws) {
            source_state.debug_sws = sws_getContext(
                source_state.dec_ctx->width, source_state.dec_ctx->height, source_state.dec_ctx->pix_fmt,
                enc_ctx->width, enc_ctx->height, AV_PIX_FMT_RGB24,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
        }
        if (!source_state.debug_rgb) {
            source_state.debug_rgb = av_frame_alloc();
            source_state.debug_rgb->format = AV_PIX_FMT_RGB24;
            source_state.debug_rgb->width = enc_ctx->width;
            source_state.debug_rgb->height = enc_ctx->height;
            av_frame_get_buffer(source_state.debug_rgb, 0);
        }
    };
    ensure_source_converters();
    
    // Convert to RGB and save PPM - returns the RGB frame for encoding
    auto convert_and_save_ppm = [&](AVFrame* src, const std::string& name) -> AVFrame* {
        ensure_source_converters();
        av_frame_make_writable(source_state.debug_rgb);
        sws_scale(source_state.debug_sws, src->data, src->linesize, 0, src->height,
                  source_state.debug_rgb->data, source_state.debug_rgb->linesize);
        std::string path = (debug_dir / (name + ".ppm")).string();
        FILE* f = fopen(path.c_str(), "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", source_state.debug_rgb->width, source_state.debug_rgb->height);
            for (int y = 0; y < source_state.debug_rgb->height; y++)
                fwrite(source_state.debug_rgb->data[0] + y * source_state.debug_rgb->linesize[0], 1,
                       source_state.debug_rgb->width * 3, f);
            fclose(f);
        }
        return source_state.debug_rgb;  // Return the RGB frame we just created
    };
    
    // Open output file
    if (avio_open(&out_ctx->pb, output.c_str(), AVIO_FLAG_WRITE) < 0) {
        std::fprintf(stderr, "EXPORT: Failed to open output file: %s\n", output.c_str());
        exporting = false;
        return false;
    }
    if (avformat_write_header(out_ctx, nullptr) < 0) {
        std::fprintf(stderr, "EXPORT: Failed to write output header\n");
        exporting = false;
        return false;
    }
    
    std::printf("EXPORT: Output time_base: %d/%d\n", out_video->time_base.num, out_video->time_base.den);
    
    AVPacket* pkt = av_packet_alloc();
    AVPacket* enc_pkt = av_packet_alloc();
    AVFrame* dec_frame = av_frame_alloc();
    
    int64_t output_frame = 0;
    float last_progress_log = -1.0f;
    auto update_export_progress = [&]() {
        if (total_frames <= 0) {
            progress = 1.0f;
            return;
        }
        float pct = static_cast<float>(output_frame) / static_cast<float>(total_frames);
        progress = std::min(1.0f, std::max(0.0f, pct));
        if (progress - last_progress_log >= 0.02f || output_frame % 30 == 0) {
            std::printf("EXPORT: Progress %.1f%% (%lld/%lld)\n",
                        progress * 100.0f, (long long)output_frame, (long long)total_frames);
            last_progress_log = progress;
        }
    };
    int packet_num = 0;
    
    // Create RGB→YUV converter (we'll encode from the same RGB data used for PPM)
    SwsContext* rgb_to_yuv = sws_getContext(
        enc_ctx->width, enc_ctx->height, AV_PIX_FMT_RGB24,
        enc_ctx->width, enc_ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    auto write_packet = [&](AVPacket* packet) {
        if (out_audio) {
            av_interleaved_write_frame(out_ctx, packet);
        } else {
            av_write_frame(out_ctx, packet);
        }
    };

    // Helper to encode from the SAME RGB data we save to PPM
    auto encode_from_rgb = [&](AVFrame* rgb_frame, int64_t pts) {
        // Allocate fresh YUV frame
        AVFrame* yuv = av_frame_alloc();
        yuv->format = AV_PIX_FMT_YUV420P;
        yuv->width = enc_ctx->width;
        yuv->height = enc_ctx->height;
        av_frame_get_buffer(yuv, 0);
        
        // Convert RGB (the exact same data as PPM) to YUV
        sws_scale(rgb_to_yuv, rgb_frame->data, rgb_frame->linesize, 0, rgb_frame->height,
                  yuv->data, yuv->linesize);
        
        yuv->pts = pts;
        
        // Send to encoder
        int ret = avcodec_send_frame(enc_ctx, yuv);
        std::printf("EXPORT: Sent frame PTS=%lld to encoder (ret=%d)\n", (long long)pts, ret);
        
        av_frame_free(&yuv);
        
        // Get all available packets
        while (avcodec_receive_packet(enc_ctx, enc_pkt) >= 0) {
            enc_pkt->stream_index = 0;
            av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_video->time_base);
            std::printf("EXPORT: Wrote packet #%d, PTS=%lld\n", packet_num++, (long long)enc_pkt->pts);
            write_packet(enc_pkt);
            av_packet_unref(enc_pkt);
        }
    };
    
    int current_source_id = clips.front().source_id;
    
    // Second decoder state for connected clips
    SourceDecodeState connected_state;
    int connected_source_id = -1;
    
    // Helper to decode a single frame from a source at a specific source frame
    auto decode_frame_from_source = [&](SourceDecodeState& state, int source_id, int64_t target_source_frame, 
                                         const std::string& debug_name) -> AVFrame* {
        if (source_id != (state.in_ctx ? connected_source_id : current_source_id)) {
            if (&state == &connected_state) {
                connected_source_id = source_id;
            }
            if (!open_source(source_paths[source_id], state)) {
                return nullptr;
            }
            // Setup converters for this state
            if (!state.debug_sws) {
                state.debug_sws = sws_getContext(
                    state.dec_ctx->width, state.dec_ctx->height, state.dec_ctx->pix_fmt,
                    enc_ctx->width, enc_ctx->height, AV_PIX_FMT_RGB24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr
                );
            }
            if (!state.debug_rgb) {
                state.debug_rgb = av_frame_alloc();
                state.debug_rgb->format = AV_PIX_FMT_RGB24;
                state.debug_rgb->width = enc_ctx->width;
                state.debug_rgb->height = enc_ctx->height;
                av_frame_get_buffer(state.debug_rgb, 0);
            }
        }
        
        const auto ts_rounding = static_cast<AVRounding>(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX);
        auto ts_to_frame = [&](int64_t ts) -> int64_t {
            if (ts == AV_NOPTS_VALUE) return 0;
            return av_rescale_q_rnd(ts, state.stream_time_base, state.frame_tb, ts_rounding);
        };
        auto frame_to_ts = [&](int64_t frame) -> int64_t {
            return av_rescale_q_rnd(frame, state.frame_tb, state.stream_time_base, ts_rounding);
        };
        
        int64_t target_ts = state.stream_start_pts + frame_to_ts(target_source_frame);
        avcodec_flush_buffers(state.dec_ctx);
        avformat_seek_file(state.in_ctx, state.video_idx, 0, target_ts, target_ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(state.dec_ctx);
        
        while (av_read_frame(state.in_ctx, pkt) >= 0) {
            if (pkt->stream_index != state.video_idx) {
                av_packet_unref(pkt);
                continue;
            }
            avcodec_send_packet(state.dec_ctx, pkt);
            av_packet_unref(pkt);
            while (avcodec_receive_frame(state.dec_ctx, dec_frame) >= 0) {
                int64_t pts = dec_frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? dec_frame->best_effort_timestamp : dec_frame->pts;
                if (pts == AV_NOPTS_VALUE) pts = state.stream_start_pts;
                pts -= state.stream_start_pts;
                int64_t src_frame = ts_to_frame(pts);
                if (src_frame < target_source_frame) continue;
                // Got the frame - convert to RGB
                av_frame_make_writable(state.debug_rgb);
                sws_scale(state.debug_sws, dec_frame->data, dec_frame->linesize, 0, dec_frame->height,
                          state.debug_rgb->data, state.debug_rgb->linesize);
                return state.debug_rgb;
            }
        }
        return nullptr;
    };

    // Create a black frame for gap clips
    AVFrame* black_rgb = av_frame_alloc();
    black_rgb->format = AV_PIX_FMT_RGB24;
    black_rgb->width = enc_ctx->width;
    black_rgb->height = enc_ctx->height;
    av_frame_get_buffer(black_rgb, 0);
    memset(black_rgb->data[0], 0, black_rgb->height * black_rgb->linesize[0]);
    
    // Process each clip
    for (const auto& clip : clips) {
        // Handle gap clips - output black frames
        if (clip.is_gap) {
            std::printf("EXPORT: Processing GAP clip [%lld, %lld) - %lld black frames\n",
                        (long long)clip.start_frame, (long long)clip.end_frame, (long long)clip.frame_count());
            for (int64_t i = 0; i < clip.frame_count(); i++) {
                // Check for connected clip override even on gap clips
                int cc_idx = find_connected_clip_at_frame(connected_clips, output_frame);
                if (cc_idx >= 0) {
                    const auto& cc = connected_clips[cc_idx];
                    int64_t offset_in_cc = output_frame - cc.timeline_start();
                    int64_t cc_source_frame = cc.start_frame + offset_in_cc;
                    std::printf("EXPORT: Connected clip on gap at timeline %lld -> src %d frame %lld\n",
                                (long long)output_frame, cc.source_id, (long long)cc_source_frame);
                    AVFrame* rgb = decode_frame_from_source(connected_state, cc.source_id, cc_source_frame, 
                                                            "cc_gap_" + std::to_string(output_frame));
                    if (rgb) {
                        encode_from_rgb(rgb, output_frame);
                    } else {
                        encode_from_rgb(black_rgb, output_frame);
                    }
                } else {
                    encode_from_rgb(black_rgb, output_frame);
                }
                output_frame++;
                update_export_progress();
            }
            continue;
        }
        
        if (clip.source_id < 0 || clip.source_id >= (int)source_paths.size()) continue;
        if (clip.source_id != current_source_id) {
            current_source_id = clip.source_id;
            if (!open_source(source_paths[current_source_id], source_state)) {
                continue;
            }
            ensure_source_converters();
        }
        std::printf("EXPORT: Processing clip [%lld, %lld) src=%d\n",
                    (long long)clip.start_frame, (long long)clip.end_frame, clip.source_id);
        
        const auto ts_rounding = static_cast<AVRounding>(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX);
        auto ts_to_frame = [&](int64_t ts) -> int64_t {
            if (ts == AV_NOPTS_VALUE) return 0;
            return av_rescale_q_rnd(ts, source_state.stream_time_base, source_state.frame_tb, ts_rounding);
        };
        auto frame_to_ts = [&](int64_t frame) -> int64_t {
            return av_rescale_q_rnd(frame, source_state.frame_tb, source_state.stream_time_base, ts_rounding);
        };
        
        // Seek to clip start
        int64_t target_ts = source_state.stream_start_pts + frame_to_ts(clip.start_frame);
        avcodec_flush_buffers(source_state.dec_ctx);
        avformat_seek_file(source_state.in_ctx, source_state.video_idx, 0, target_ts, target_ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(source_state.dec_ctx);
        
        int64_t frames_needed = clip.frame_count();
        int64_t frames_got = 0;
        bool have_last_rgb = false;
        auto try_force_last_frame = [&]() -> bool {
            if (frames_needed <= 0) return false;
            int64_t target_frame = std::max<int64_t>(clip.start_frame, clip.end_frame - 1);
            int64_t target_ts = source_state.stream_start_pts + frame_to_ts(target_frame);
            avcodec_flush_buffers(source_state.dec_ctx);
            avformat_seek_file(source_state.in_ctx, source_state.video_idx, 0, target_ts, target_ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(source_state.dec_ctx);
            while (av_read_frame(source_state.in_ctx, pkt) >= 0) {
                if (pkt->stream_index != source_state.video_idx) {
                    av_packet_unref(pkt);
                    continue;
                }
                avcodec_send_packet(source_state.dec_ctx, pkt);
                av_packet_unref(pkt);
                while (avcodec_receive_frame(source_state.dec_ctx, dec_frame) >= 0) {
                    int64_t pts = dec_frame->best_effort_timestamp != AV_NOPTS_VALUE
                        ? dec_frame->best_effort_timestamp
                        : dec_frame->pts;
                    if (pts == AV_NOPTS_VALUE) pts = source_state.stream_start_pts;
                    pts -= source_state.stream_start_pts;
                    int64_t src_frame = ts_to_frame(pts);
                    if (src_frame < clip.start_frame) continue;
                    if (src_frame > clip.end_frame) break;
                    std::printf("EXPORT: Fallback decode src=%lld -> encode out=%lld\n",
                                (long long)src_frame, (long long)output_frame);
                    AVFrame* rgb = convert_and_save_ppm(dec_frame, "frame_" + std::to_string(output_frame) + "_src" + std::to_string(src_frame));
                    encode_from_rgb(rgb, output_frame);
                    have_last_rgb = true;
                    output_frame++;
                    frames_got++;
                    update_export_progress();
                    return true;
                }
            }
            return false;
        };
        
        bool clip_done = false;
        // Decode frames one by one
        while (av_read_frame(source_state.in_ctx, pkt) >= 0 && frames_got < frames_needed && !clip_done) {
            if (pkt->stream_index != source_state.video_idx) {
                av_packet_unref(pkt);
                continue;
            }
            
            avcodec_send_packet(source_state.dec_ctx, pkt);
            av_packet_unref(pkt);
            
            while (avcodec_receive_frame(source_state.dec_ctx, dec_frame) >= 0 && frames_got < frames_needed) {
                // Calculate source frame index
                int64_t pts = dec_frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? dec_frame->best_effort_timestamp
                    : dec_frame->pts;
                if (pts == AV_NOPTS_VALUE) pts = source_state.stream_start_pts;
                pts -= source_state.stream_start_pts;
                int64_t src_frame = ts_to_frame(pts);
                
                // Skip frames before clip start
                if (src_frame < clip.start_frame) {
                    std::printf("EXPORT: Skip frame %lld (before clip)\n", (long long)src_frame);
                    continue;
                }
                
                // Stop if past clip end. Allow one overflow frame to satisfy exact count.
                if (src_frame >= clip.end_frame) {
                    if (frames_got + 1 == frames_needed) {
                        std::printf("EXPORT: Frame %lld past clip end, using as last frame\n", (long long)src_frame);
                    } else {
                        std::printf("EXPORT: Frame %lld past clip end, done with clip\n", (long long)src_frame);
                        clip_done = true;
                        break;
                    }
                }
                
                // Check for connected clip override at this timeline frame
                int cc_idx = find_connected_clip_at_frame(connected_clips, output_frame);
                AVFrame* rgb = nullptr;
                if (cc_idx >= 0) {
                    const auto& cc = connected_clips[cc_idx];
                    int64_t offset_in_cc = output_frame - cc.timeline_start();
                    int64_t cc_source_frame = cc.start_frame + offset_in_cc;
                    std::printf("EXPORT: Connected clip override at timeline %lld -> src %d frame %lld\n",
                                (long long)output_frame, cc.source_id, (long long)cc_source_frame);
                    rgb = decode_frame_from_source(connected_state, cc.source_id, cc_source_frame, 
                                                   "cc_" + std::to_string(output_frame));
                    if (rgb) {
                        encode_from_rgb(rgb, output_frame);
                    }
                }
                
                if (!rgb) {
                    std::printf("EXPORT: Decode src=%lld -> encode out=%lld\n", (long long)src_frame, (long long)output_frame);
                    // Convert to RGB and save PPM - get the SAME RGB data for encoding
                    rgb = convert_and_save_ppm(dec_frame, "frame_" + std::to_string(output_frame) + "_src" + std::to_string(src_frame));
                    // Encode from the EXACT SAME RGB data we just saved to PPM
                    encode_from_rgb(rgb, output_frame);
                }
                have_last_rgb = true;
                
                output_frame++;
                frames_got++;
                update_export_progress();

                if (frames_got >= frames_needed) {
                    break;
                }
            }
            if (clip_done) {
                break;
            }
        }
        
        // Flush decoder for this clip
        avcodec_send_packet(source_state.dec_ctx, nullptr);
        while (avcodec_receive_frame(source_state.dec_ctx, dec_frame) >= 0 && frames_got < frames_needed) {
            int64_t pts = dec_frame->best_effort_timestamp != AV_NOPTS_VALUE
                ? dec_frame->best_effort_timestamp
                : dec_frame->pts;
            if (pts == AV_NOPTS_VALUE) pts = source_state.stream_start_pts;
            pts -= source_state.stream_start_pts;
            int64_t src_frame = ts_to_frame(pts);
            
            if (src_frame < clip.start_frame) continue;
            if (src_frame >= clip.end_frame && frames_got + 1 != frames_needed) continue;
            
            std::printf("EXPORT: (flush) Decode src=%lld -> encode out=%lld\n", (long long)src_frame, (long long)output_frame);
            AVFrame* rgb = convert_and_save_ppm(dec_frame, "frame_" + std::to_string(output_frame) + "_src" + std::to_string(src_frame));
            encode_from_rgb(rgb, output_frame);
            have_last_rgb = true;
            
            output_frame++;
            frames_got++;
            update_export_progress();
            if (frames_got >= frames_needed) break;
        }
        
        if (frames_got < frames_needed && !have_last_rgb) {
            try_force_last_frame();
        }
        if (frames_got < frames_needed && have_last_rgb) {
            std::printf("EXPORT: Padding %lld frames using last decoded frame\n",
                        (long long)(frames_needed - frames_got));
            while (frames_got < frames_needed) {
                encode_from_rgb(source_state.debug_rgb, output_frame);
                output_frame++;
                frames_got++;
                update_export_progress();
            }
        }
        std::printf("EXPORT: Clip done, got %lld/%lld frames\n", (long long)frames_got, (long long)frames_needed);
    }
    
    // Flush video encoder
    std::printf("EXPORT: Flushing encoder...\n");
    avcodec_send_frame(enc_ctx, nullptr);
    while (avcodec_receive_packet(enc_ctx, enc_pkt) >= 0) {
        enc_pkt->stream_index = 0;
        av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_video->time_base);
        std::printf("EXPORT: Flush wrote packet #%d, PTS=%lld\n", packet_num++, (long long)enc_pkt->pts);
        write_packet(enc_pkt);
        av_packet_unref(enc_pkt);
    }
    
    std::printf("EXPORT: Total: %lld frames encoded, %d packets written\n", (long long)output_frame, packet_num);
    if (output_frame >= total_frames) {
        progress = 1.0f;
    }
    // UX: ensure the bar reaches the end while finalizing audio/muxing.
    progress = 1.0f;
    std::printf("EXPORT: Progress forced to 100%% before audio/mux finalize\n");

    // Encode audio after video (timeline PCM -> AAC)
    if (out_audio && audio_enc && audio_swr && !audio_export.timeline_pcm.empty()) {
        std::printf("EXPORT: Audio encode start (pcm_frames=%lld)\n",
                    (long long)(audio_export.timeline_pcm.size() / std::max(1, audio_export.channels)));
        AVPacket* audio_pkt = av_packet_alloc();
        AVFrame* audio_frame = av_frame_alloc();
        audio_frame->format = audio_enc->sample_fmt;
        audio_frame->sample_rate = audio_enc->sample_rate;
        audio_frame->ch_layout = audio_enc->ch_layout;
        int frame_size = audio_enc->frame_size > 0 ? audio_enc->frame_size : 1024;
        int64_t audio_pts = 0;
        int allocated_samples = 0;
        const int channels = audio_export.channels;
        const int64_t total_frames = static_cast<int64_t>(audio_export.timeline_pcm.size() / channels);
        int64_t cursor = 0;
        int audio_packet_count = 0;
        int audio_send_count = 0;
        int audio_convert_count = 0;
        while (cursor < total_frames) {
            int64_t frames_left = total_frames - cursor;
            int nb_samples = static_cast<int>(std::min<int64_t>(frame_size, frames_left));
            int out_samples = swr_get_out_samples(audio_swr, nb_samples);
            if (out_samples <= 0) break;
            if (out_samples != allocated_samples) {
                av_frame_unref(audio_frame);
                audio_frame->format = audio_enc->sample_fmt;
                audio_frame->sample_rate = audio_enc->sample_rate;
                audio_frame->ch_layout = audio_enc->ch_layout;
                audio_frame->nb_samples = out_samples;
                if (av_frame_get_buffer(audio_frame, 0) < 0) break;
                allocated_samples = out_samples;
            } else if (av_frame_make_writable(audio_frame) < 0) break;
            const float* src = audio_export.timeline_pcm.data() + cursor * channels;
            const uint8_t* in_data[] = { reinterpret_cast<const uint8_t*>(src) };
            int converted = swr_convert(audio_swr, audio_frame->data, out_samples, in_data, nb_samples);
            if (converted < 0) break;
            if (converted > 0) {
                audio_convert_count++;
                audio_frame->nb_samples = converted;
                audio_frame->pts = audio_pts;
                audio_pts += converted;
                int send_ret = avcodec_send_frame(audio_enc, audio_frame);
                if (send_ret >= 0) {
                    audio_send_count++;
                }
                while (avcodec_receive_packet(audio_enc, audio_pkt) >= 0) {
                    audio_pkt->stream_index = out_audio->index;
                    av_packet_rescale_ts(audio_pkt, audio_enc->time_base, out_audio->time_base);
                    write_packet(audio_pkt);
                    av_packet_unref(audio_pkt);
                    audio_packet_count++;
                }
            }
            cursor += nb_samples;
        }
        avcodec_send_frame(audio_enc, nullptr);
        while (avcodec_receive_packet(audio_enc, audio_pkt) >= 0) {
            audio_pkt->stream_index = out_audio->index;
            av_packet_rescale_ts(audio_pkt, audio_enc->time_base, out_audio->time_base);
            write_packet(audio_pkt);
            av_packet_unref(audio_pkt);
            audio_packet_count++;
        }
        av_frame_free(&audio_frame);
        av_packet_free(&audio_pkt);
        std::printf("EXPORT: Audio encode done (packets=%d, sends=%d, converts=%d)\n",
                    audio_packet_count, audio_send_count, audio_convert_count);
    }
    
    av_write_trailer(out_ctx);
    
    // Cleanup
    sws_freeContext(rgb_to_yuv);
    av_frame_free(&black_rgb);
    av_frame_free(&dec_frame);
    av_packet_free(&enc_pkt);
    av_packet_free(&pkt);
    avcodec_free_context(&enc_ctx);
    if (audio_swr) swr_free(&audio_swr);
    if (audio_enc) avcodec_free_context(&audio_enc);
    avio_closep(&out_ctx->pb);
    avformat_free_context(out_ctx);
    close_source(source_state);
    close_source(connected_state);
    
    progress = 1.0f;
    exporting = false;
    return true;
}

bool export_video(const std::string& input, const std::string& output, double start, double end, std::atomic<bool>& exporting, std::atomic<float>& progress) {
    AVFormatContext* in_ctx = nullptr;
    AVFormatContext* out_ctx = nullptr;
    
    if (avformat_open_input(&in_ctx, input.c_str(), nullptr, nullptr) < 0) {
        exporting = false;
        return false;
    }
    
    if (avformat_find_stream_info(in_ctx, nullptr) < 0) {
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    if (avformat_alloc_output_context2(&out_ctx, nullptr, nullptr, output.c_str()) < 0) {
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    int* stream_mapping = (int*)av_malloc(in_ctx->nb_streams * sizeof(int));
    int stream_count = 0;
    
    for (unsigned i = 0; i < in_ctx->nb_streams; i++) {
        AVStream* in_stream = in_ctx->streams[i];
        AVCodecParameters* codecpar = in_stream->codecpar;
        
        if (codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }
        
        stream_mapping[i] = stream_count++;
        
        AVStream* out_stream = avformat_new_stream(out_ctx, nullptr);
        avcodec_parameters_copy(out_stream->codecpar, codecpar);
        out_stream->codecpar->codec_tag = 0;
    }
    
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_ctx->pb, output.c_str(), AVIO_FLAG_WRITE) < 0) {
            av_free(stream_mapping);
            avformat_free_context(out_ctx);
            avformat_close_input(&in_ctx);
            exporting = false;
            return false;
        }
    }
    
    int64_t start_ts = (int64_t)(start * AV_TIME_BASE);
    av_seek_frame(in_ctx, -1, start_ts, AVSEEK_FLAG_BACKWARD);
    
    if (avformat_write_header(out_ctx, nullptr) < 0) {
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        av_free(stream_mapping);
        avformat_free_context(out_ctx);
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    AVPacket* pkt = av_packet_alloc();
    double duration = end - start;
    
    while (av_read_frame(in_ctx, pkt) >= 0 && exporting) {
        if (stream_mapping[pkt->stream_index] < 0) {
            av_packet_unref(pkt);
            continue;
        }
        
        AVStream* in_stream = in_ctx->streams[pkt->stream_index];
        double pkt_time = pkt->pts * av_q2d(in_stream->time_base);
        
        if (pkt_time < start) {
            av_packet_unref(pkt);
            continue;
        }
        
        if (pkt_time > end) {
            av_packet_unref(pkt);
            break;
        }
        
        progress = (float)((pkt_time - start) / duration);
        
        AVStream* out_stream = out_ctx->streams[stream_mapping[pkt->stream_index]];
        
        pkt->pts = av_rescale_q_rnd(pkt->pts - (int64_t)(start / av_q2d(in_stream->time_base)),
                                     in_stream->time_base, out_stream->time_base,
                                     (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts - (int64_t)(start / av_q2d(in_stream->time_base)),
                                     in_stream->time_base, out_stream->time_base,
                                     (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
        pkt->stream_index = stream_mapping[pkt->stream_index];
        pkt->pos = -1;
        
        av_interleaved_write_frame(out_ctx, pkt);
        av_packet_unref(pkt);
    }
    
    av_write_trailer(out_ctx);
    
    av_packet_free(&pkt);
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
    av_free(stream_mapping);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);
    
    progress = 1.0f;
    exporting = false;
    return true;
}

fs::path get_config_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path(home) / ".config" / "vibecut";
}

fs::path load_last_path() {
    try {
        auto config_file = get_config_dir() / "last_path";
        if (fs::exists(config_file)) {
            std::ifstream f(config_file);
            std::string path_str;
            std::getline(f, path_str);
            if (!path_str.empty() && fs::exists(path_str)) {
                return path_str;
            }
        }
    } catch (...) {}
    return fs::current_path();
}

void save_last_path(const fs::path& p) {
    try {
        auto config_dir = get_config_dir();
        fs::create_directories(config_dir);
        std::ofstream f(config_dir / "last_path");
        f << p.string();
    } catch (...) {}
}

std::string path_display_name(const fs::path& p) {
    if (p.empty()) return "";
    auto fname = p.filename();
    if (fname.empty() || fname == "/" || fname == ".") {
        return p.string();
    }
    return fname.string();
}

struct WindowData {
    GLFWwindow* window = nullptr;
    ImGuiContext* imgui_ctx = nullptr;
    float scale = 1.0f;
};

struct FileBrowser {
    fs::path current_path;
    int selected_index = -1;
    std::vector<fs::directory_entry> entries;
    
    FileBrowser() {
        try { current_path = load_last_path(); } 
        catch (...) { current_path = "/"; }
    }
    
    void refresh() {
        entries.clear();
        selected_index = -1;
        
        try {
            current_path = fs::canonical(current_path);
        } catch (...) {
            try { current_path = fs::current_path(); }
            catch (...) { current_path = "/"; }
        }
        
        if (current_path.has_parent_path()) {
            auto parent = current_path.parent_path();
            if (!parent.empty() && parent != current_path) {
                try { entries.push_back(fs::directory_entry(parent)); }
                catch (...) {}
            }
        }
        
        std::vector<fs::directory_entry> dirs, files;
        try {
            for (auto& entry : fs::directory_iterator(current_path, fs::directory_options::skip_permission_denied)) {
                try {
                    if (entry.is_directory()) dirs.push_back(entry);
                    else files.push_back(entry);
                } catch (...) {}
            }
        } catch (...) {}
        
        auto get_name = [](const fs::directory_entry& e) { return path_display_name(e.path()); };
        std::ranges::sort(dirs, {}, get_name);
        std::ranges::sort(files, {}, get_name);
        
        entries.insert(entries.end(), dirs.begin(), dirs.end());
        entries.insert(entries.end(), files.begin(), files.end());
    }
    
    void navigate_to(const fs::path& p) {
        try {
            current_path = fs::canonical(p);
        } catch (...) {
            current_path = p;
        }
        save_last_path(current_path);
        refresh();
    }
};

using json = nlohmann::json;

bool save_project_file(const std::string& path, const VideoPlayer& player, float timeline_zoom) {
    json j;
    j["version"] = 1;
    j["current_timeline_frame"] = player.current_timeline_frame;
    j["active_source"] = player.active_source;
    j["timeline_zoom"] = timeline_zoom;
    j["sources"] = json::array();
    for (const auto& src : player.sources) {
        j["sources"].push_back({{"path", src.path}});
    }
    j["clips"] = json::array();
    for (const auto& clip : player.clips) {
        j["clips"].push_back({
            {"source", clip.source_id},
            {"start", clip.start_frame},
            {"end", clip.end_frame},
            {"color", clip.color_id},
            {"is_gap", clip.is_gap}
        });
    }
    j["connected_clips"] = json::array();
    for (const auto& cc : player.connected_clips) {
        j["connected_clips"].push_back({
            {"source", cc.source_id},
            {"start", cc.start_frame},
            {"end", cc.end_frame},
            {"connection", cc.connection_frame},
            {"connection_offset", cc.connection_offset},
            {"lane", cc.lane},
            {"color", cc.color_id}
        });
    }
    std::ofstream out(path);
    if (!out) return false;
    out << j.dump(2);
    return true;
}

bool load_project_file(const std::string& path, VideoPlayer& player, float& out_zoom) {
    std::ifstream in(path);
    if (!in) return false;
    if (in.peek() == std::ifstream::traits_type::eof()) return false;
    json j;
    try {
        in >> j;
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;
    player.close();
    if (!j.contains("sources") || !j["sources"].is_array()) return false;
    for (const auto& src : j["sources"]) {
        if (!src.contains("path")) continue;
        player.add_source(src["path"].get<std::string>());
    }
    if (player.sources.empty()) return false;
    int active = j.value("active_source", 0);
    active = std::clamp(active, 0, (int)player.sources.size() - 1);
    player.open_source(active, false);
    player.clips.clear();
    int max_color = 0;
    if (j.contains("clips") && j["clips"].is_array()) {
        for (const auto& c : j["clips"]) {
            int source_id = c.value("source", 0);
            int64_t start = c.value("start", 0);
            int64_t end = c.value("end", 0);
            int color = c.value("color", 0);
            bool is_gap = c.value("is_gap", false);
            if (source_id < 0 || source_id >= (int)player.sources.size()) continue;
            if (end <= start) continue;
            player.clips.push_back({source_id, start, end, color, is_gap});
            max_color = std::max(max_color, color);
        }
    }
    player.next_clip_color_id = max_color + 1;
    
    player.connected_clips.clear();
    int max_connected_color = 0;
    if (j.contains("connected_clips") && j["connected_clips"].is_array()) {
        for (const auto& c : j["connected_clips"]) {
            int source_id = c.value("source", 0);
            int64_t start = c.value("start", 0);
            int64_t end = c.value("end", 0);
            int64_t connection = c.value("connection", 0);
            int64_t conn_offset = c.value("connection_offset", (int64_t)0);
            int lane = c.value("lane", 1);
            int color = c.value("color", 0);
            if (source_id < 0 || source_id >= (int)player.sources.size()) continue;
            if (end <= start) continue;
            player.connected_clips.push_back({source_id, start, end, connection, conn_offset, lane, color});
            max_connected_color = std::max(max_connected_color, color);
        }
    }
    player.next_connected_clip_color_id = max_connected_color + 1;
    
    player.current_timeline_frame = j.value("current_timeline_frame", 0);
    player.current_timeline_frame = std::clamp<int64_t>(
        player.current_timeline_frame, 0, player.total_timeline_frames());
    out_zoom = j.value("timeline_zoom", 1.0f);
    out_zoom = std::clamp(out_zoom, 0.25f, 50.0f);
    player.audio_dirty = true;
    player.rebuild_timeline_audio();
    player.audio_dirty = false;
    player.seek_to_timeline_frame(player.current_timeline_frame);
    return true;
}

WindowData create_window(const char* title, int width, int height, GLFWwindow* share_context = nullptr) {
    WindowData data;
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    data.window = glfwCreateWindow(width, height, title, nullptr, share_context);
    if (!data.window) return data;
    
    glfwMakeContextCurrent(data.window);
    glfwSwapInterval(1);
    
    float x_scale, y_scale;
    glfwGetWindowContentScale(data.window, &x_scale, &y_scale);
    data.scale = x_scale > y_scale ? x_scale : y_scale;
    
    data.imgui_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(data.imgui_ctx);
    
    ImGuiIO& io = ImGui::GetIO();
    // Don't enable keyboard nav - we use arrow keys for timeline navigation
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(RobotoMedium_compressed_data_base85, 16.0f * data.scale);
    
    ImGui::StyleColorsDark();
    
    // Custom styling (before scaling)
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 4.0f;
    style.FramePadding = ImVec2(8.0f, 4.0f);  // Slightly bigger buttons
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.WindowRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    
    // Dark grey color scheme
    ImVec4 bg_dark = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);      // #212121
    ImVec4 bg_mid = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);       // Slightly lighter
    ImVec4 bg_light = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);     // Hover
    ImVec4 accent = ImVec4(0.35f, 0.35f, 0.35f, 1.0f);       // Active/pressed
    ImVec4 text = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);         // White text
    
    style.Colors[ImGuiCol_WindowBg] = bg_dark;
    style.Colors[ImGuiCol_ChildBg] = bg_dark;
    style.Colors[ImGuiCol_PopupBg] = bg_mid;
    style.Colors[ImGuiCol_FrameBg] = bg_mid;
    style.Colors[ImGuiCol_FrameBgHovered] = bg_light;
    style.Colors[ImGuiCol_FrameBgActive] = accent;
    style.Colors[ImGuiCol_TitleBg] = bg_dark;
    style.Colors[ImGuiCol_TitleBgActive] = bg_mid;
    style.Colors[ImGuiCol_Button] = bg_mid;
    style.Colors[ImGuiCol_ButtonHovered] = bg_light;
    style.Colors[ImGuiCol_ButtonActive] = accent;
    style.Colors[ImGuiCol_Header] = bg_mid;
    style.Colors[ImGuiCol_HeaderHovered] = bg_light;
    style.Colors[ImGuiCol_HeaderActive] = accent;
    style.Colors[ImGuiCol_SliderGrab] = bg_light;
    style.Colors[ImGuiCol_SliderGrabActive] = accent;
    style.Colors[ImGuiCol_Text] = text;
    style.Colors[ImGuiCol_ScrollbarBg] = bg_dark;
    style.Colors[ImGuiCol_ScrollbarGrab] = bg_light;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = accent;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
    
    style.ScaleAllSizes(data.scale);
    
    ImGui_ImplGlfw_InitForOpenGL(data.window, true);
    ImGui_ImplOpenGL3_Init("#version 460");
    
    return data;
}

void destroy_window(WindowData& data) {
    if (data.imgui_ctx) {
        ImGui::SetCurrentContext(data.imgui_ctx);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(data.imgui_ctx);
        data.imgui_ctx = nullptr;
    }
    if (data.window) {
        glfwDestroyWindow(data.window);
        data.window = nullptr;
    }
}

void render_window(WindowData& data, auto ui_func) {
    if (!data.window || glfwWindowShouldClose(data.window)) return;
    
    glfwMakeContextCurrent(data.window);
    ImGui::SetCurrentContext(data.imgui_ctx);
    
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    
    ui_func();
    
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(data.window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.129f, 0.129f, 0.129f, 1.0f);  // #212121
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    glfwSwapBuffers(data.window);
}

struct TrimTimelineState {
    int dragging = 0; // 0=none, 1=left, 2=right, 3=playhead
};

struct ClipsTimelineState {
    int dragging = 0;       // 0=none, 1=left handle, 2=right handle, 3=playhead, 4=panning, 5=clip drag, 6=scrollbar drag
    int dragging_clip = -1; // Which clip's handle we're dragging
    int pending_clip = -1;  // Clip clicked (waiting to see if it becomes a drag)
    int selected_clip = -1;
    bool pending_click = false;
    float pending_click_x = 0.0f;
    ImVec2 pending_mouse_start{0.0f, 0.0f};
    float pending_grab_offset_time = 0.0f; // Mouse offset into clip (timeline time)
    float drag_grab_offset_time = 0.0f;    // Active drag offset (timeline time)
    float drag_start_mouse_time = 0.0f;    // Timeline time when drag started
    bool clips_modified = false;
    int pending_library_source = -1;
    int64_t pending_library_timeline = -1;
    float zoom = 1.0f;      // Zoom level (1.0 = fit all, higher = zoom in)
    float scroll = 0.0f;    // Scroll position (0.0 to 1.0, normalized)
    float pan_start_x = 0.0f; // For panning
    float pan_start_scroll = 0.0f;
    float lead_in_time = 0.0f; // Timeline offset before first clip (seconds)
    float lead_in_start = 0.0f;
    float trim_mouse_start_x = 0.0f;
    int64_t trim_start_frame = 0;
    bool trim_playhead_in_clip = false;
    int64_t trim_playhead_source_frame = -1;
    int trim_playhead_source_id = -1;
    int64_t trim_playhead_timeline = -1;
    // Connected clip interaction
    int selected_connected_clip = -1;
    int dragging_connected_clip = -1;
    int connected_clip_dragging = 0;  // 0=none, 1=left handle, 2=right handle, 3=move
    int64_t connected_clip_drag_start_connection = 0;
    int64_t connected_clip_trim_start_frame = 0;
    // Left-trim connected clip tracking
    // (cc_idx, original_connection_frame, original_connection_offset)
    std::vector<std::tuple<int, int64_t, int64_t>> left_trim_original_connections;
    int64_t left_trim_clip_timeline_start = 0;  // Timeline start of the clip being trimmed
    // Right-trim connected clip tracking
    int64_t right_trim_original_end = 0;  // Original clip source end frame before trim
    int64_t right_trim_clip_timeline_start = 0;  // Timeline start of clip being trimmed
    // (cc_idx, original_connection_frame, original_connection_offset, original_timeline_start)
    std::vector<std::tuple<int, int64_t, int64_t, int64_t>> right_trim_original_connections;
    int right_trim_temp_gap_clip_idx = -1;  // Index of temporary gap clip (-1 if none)
    std::vector<int> right_trim_cc_on_gap;  // Connected clip indices that are currently on the gap clip
};

// Modern Final Cut-style magnetic timeline widget with zoom/scroll
// Clips are frame-based for precision
// Returns true if anything changed
bool ClipsTimeline(const char* label, int64_t* current_source_frame, int* current_source_id, int64_t* current_timeline_frame, std::vector<Clip>& clips, std::vector<ConnectedClip>& connected_clips, int64_t total_source_frames, double fps, const ImVec2& size, ClipsTimelineState& state) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    ImVec2 bb_min = pos;
    ImVec2 bb_max = ImVec2(pos.x + size.x, pos.y + size.y);
    
    float rounding = 12.0f;
    float handle_w = 10.0f;
    float clip_margin = 2.0f;
    float playhead_head_size = 10.0f;
    
    // Convert frames to time for display calculations (internally work in frames)
    auto frame_to_time = [fps](int64_t f) -> float { return (float)(f / fps); };
    auto time_to_frame_local = [fps](float t) -> int64_t { return (int64_t)(t * fps + 0.5); };
    
    float source_duration = frame_to_time(total_source_frames);
    float frame_duration = (float)(1.0 / fps);
    
    // Calculate total timeline frames (sum of all clip frame counts)
    int64_t total_timeline_frames = 0;
    for (const auto& c : clips) total_timeline_frames += c.frame_count();
    float total_duration = frame_to_time(total_timeline_frames);
    if (total_duration < 0.01f) total_duration = source_duration;
    float display_duration = total_duration + state.lead_in_time;
    
    // Zoom and scroll calculations - based on SOURCE duration for consistent pixel scaling
    // This means 1 second of video = same pixel width regardless of trimming
    float base_duration = std::max(source_duration, display_duration);
    
    // Dynamic max zoom: at max zoom, one frame should be ~50 pixels wide
    // visible_duration = base_duration / zoom, and we want visible_duration = frame_duration * (size.x / 50)
    float pixels_per_frame_at_max = 50.0f;
    float max_zoom = base_duration / (frame_duration * (size.x / pixels_per_frame_at_max));
    max_zoom = std::max(max_zoom, 1.0f);  // At least 1x zoom
    
    float visible_duration = base_duration / state.zoom;
    float max_scroll = std::max(0.0f, base_duration - visible_duration);
    state.scroll = std::clamp(state.scroll, 0.0f, max_scroll);
    float view_start = state.scroll;
    float view_end = state.scroll + visible_duration;
    float view_start_timeline = view_start - state.lead_in_time;
    float view_end_timeline = view_end - state.lead_in_time;
    
    // Helper to convert timeline time to screen X
    auto time_to_x = [&](float t) -> float {
        float display_t = t + state.lead_in_time;
        return bb_min.x + ((display_t - view_start) / visible_duration) * size.x;
    };
    
    // Helper to convert screen X to timeline time
    auto x_to_time = [&](float x) -> float {
        float display_t = view_start + ((x - bb_min.x) / size.x) * visible_duration;
        return display_t - state.lead_in_time;
    };
    
    // Background - dark with subtle inset look
    draw_list->AddRectFilled(bb_min, bb_max, IM_COL32(25, 25, 28, 255), rounding);
    draw_list->AddRect(bb_min, bb_max, IM_COL32(40, 40, 45, 255), rounding, 0, 1.0f);
    
    // Get mouse position early for scrollbar interaction
    ImVec2 mouse = ImGui::GetIO().MousePos;
    
    // Scrollbar dimensions - ALWAYS reserve space to prevent layout jumps
    float scroll_bar_h = 12.0f;
    float scroll_bar_y = bb_max.y - scroll_bar_h - 2;
    bool scrollbar_active = state.zoom > 1.01f;  // Can actually scroll
    
    // Always leave room for scrollbar
    float clip_area_bottom = scroll_bar_y - 4;
    
    // Lane layout: main track in center, lanes above and below
    float total_lane_area = clip_area_bottom - bb_min.y - playhead_head_size;
    float lane_height = total_lane_area / 5.0f;  // 2 lanes above, 1 main, 2 lanes below
    float main_track_top = bb_min.y + playhead_head_size + lane_height * 2;
    float main_track_bottom = main_track_top + lane_height;
    
    // Helper to get Y bounds for a lane (lane 1, 2 = above; -1, -2 = below; 0 = main)
    auto lane_y_bounds = [&](int lane) -> std::pair<float, float> {
        if (lane == 0) return {main_track_top, main_track_bottom};
        if (lane > 0) {
            float top = main_track_top - lane * lane_height;
            return {top, top + lane_height};
        } else {
            float top = main_track_bottom + (-lane - 1) * lane_height;
            return {top, top + lane_height};
        }
    };
    
    // Track if mouse is over scrollbar area (for click priority)
    bool mouse_over_scrollbar = (mouse.y >= scroll_bar_y - 2 && mouse.y <= bb_max.y &&
                                  mouse.x >= bb_min.x && mouse.x <= bb_max.x);
    
    // Check if mouse is in timeline area
    bool mouse_in_timeline = (mouse.x >= bb_min.x && mouse.x <= bb_max.x && 
                              mouse.y >= bb_min.y && mouse.y <= bb_max.y);
    
    auto clip_color_for = [](int color_id) -> ImU32 {
        static const ImU32 palette[] = {
            IM_COL32(85, 65, 125, 255),
            IM_COL32(65, 85, 125, 255),
            IM_COL32(90, 70, 95, 255),
            IM_COL32(70, 95, 95, 255),
            IM_COL32(95, 75, 70, 255),
            IM_COL32(70, 95, 75, 255)
        };
        return palette[color_id % (int)(sizeof(palette) / sizeof(palette[0]))];
    };
    
    // Draw each clip - positioned sequentially (magnetic/ripple style)
    float timeline_pos = 0.0f;  // Current position on timeline (in time units)
    for (int i = 0; i < (int)clips.size(); i++) {
        const Clip& clip = clips[i];
        float clip_dur_time = frame_to_time(clip.frame_count());
        
        // Skip clips outside visible range (convert view to timeline space)
        if (timeline_pos + clip_dur_time < view_start_timeline || timeline_pos > view_end_timeline) {
            timeline_pos += clip_dur_time;
            continue;
        }
        
        // Calculate screen positions using zoom/scroll-aware conversion
        float start_x = time_to_x(timeline_pos);
        float end_x = time_to_x(timeline_pos + clip_dur_time);
        
        // Clip bounds with margin (in main track lane)
        ImVec2 clip_min(start_x + clip_margin, main_track_top + clip_margin);
        ImVec2 clip_max(end_x - clip_margin, main_track_bottom - clip_margin);
        
        if (clip_max.x <= clip_min.x) continue;  // Skip if too small
        
        // Check if mouse is over this clip
        bool hover_clip = mouse_in_timeline && mouse.x >= start_x && mouse.x <= end_x;
        bool hover_left = hover_clip && mouse.x <= start_x + handle_w + 4;
        bool hover_right = hover_clip && mouse.x >= end_x - handle_w - 4;
        
        // Stable clip colors based on clip identity
        ImU32 clip_color = clip.is_gap ? IM_COL32(60, 60, 65, 200) : clip_color_for(clip.color_id);
        
        // Draw clip with rounded corners
        draw_list->AddRectFilled(clip_min, clip_max, clip_color, rounding);
        
        // Gap clip: draw diagonal stripe pattern
        if (clip.is_gap) {
            for (float px = clip_min.x; px < clip_max.x; px += 10.0f) {
                float x1 = px;
                float x2 = std::min(px + (clip_max.y - clip_min.y), clip_max.x);
                draw_list->AddLine(ImVec2(x1, clip_max.y), ImVec2(x2, clip_min.y), 
                                  IM_COL32(80, 80, 85, 150), 1.0f);
            }
            // Add "Gap" text if wide enough
            float clip_width_gap = clip_max.x - clip_min.x;
            if (clip_width_gap > 40) {
                const char* gap_text = "Gap";
                ImVec2 text_size = ImGui::CalcTextSize(gap_text);
                ImVec2 text_pos((clip_min.x + clip_max.x - text_size.x) / 2, 
                               (clip_min.y + clip_max.y - text_size.y) / 2);
                draw_list->AddText(text_pos, IM_COL32(120, 120, 130, 200), gap_text);
            }
        } else {
            // Subtle highlight at top edge for depth (not for gap clips)
            draw_list->AddLine(ImVec2(clip_min.x + rounding, clip_min.y + 1), 
                              ImVec2(clip_max.x - rounding, clip_min.y + 1), 
                              IM_COL32(255, 255, 255, 40), 1.0f);
        }
        
        // Handle zones - subtle darker/lighter areas at edges
        float handle_inner = handle_w;
        
        if (clip_max.x - clip_min.x > handle_w * 3 && !clip.is_gap) {
            // Left handle zone
            ImU32 left_handle_col = hover_left ? IM_COL32(255, 255, 255, 60) : IM_COL32(0, 0, 0, 30);
            draw_list->AddRectFilled(clip_min, ImVec2(clip_min.x + handle_inner, clip_max.y), 
                                     left_handle_col, rounding, ImDrawFlags_RoundCornersLeft);
            
            // Right handle zone
            ImU32 right_handle_col = hover_right ? IM_COL32(255, 255, 255, 60) : IM_COL32(0, 0, 0, 30);
            draw_list->AddRectFilled(ImVec2(clip_max.x - handle_inner, clip_min.y), clip_max, 
                                     right_handle_col, rounding, ImDrawFlags_RoundCornersRight);
        }
        
        if (state.selected_clip == i) {
            draw_list->AddRect(clip_min, clip_max, IM_COL32(255, 215, 0, 255), rounding, 0, 2.0f);
        }
        
        // Clip duration text (if clip is wide enough)
        float clip_width = clip_max.x - clip_min.x;
        if (clip_width > 60) {
            char time_str[32];
            int64_t frame_count = clip.frame_count();
            snprintf(time_str, sizeof(time_str), "%lld fr", (long long)frame_count);
            
            ImVec2 text_size = ImGui::CalcTextSize(time_str);
            ImVec2 text_pos((clip_min.x + clip_max.x - text_size.x) / 2, 
                           (clip_min.y + clip_max.y - text_size.y) / 2);
            draw_list->AddText(text_pos, IM_COL32(255, 255, 255, 180), time_str);
        }
        
        timeline_pos += clip_dur_time;
    }
    
    // Draw connected clips in their lanes
    for (int i = 0; i < (int)connected_clips.size(); i++) {
        const ConnectedClip& cc = connected_clips[i];
        // Use timeline_start() which accounts for connection_offset
        float clip_start_time = frame_to_time(cc.timeline_start());
        float clip_dur_time = frame_to_time(cc.frame_count());
        
        // Skip clips outside visible range
        if (clip_start_time + clip_dur_time < view_start_timeline || clip_start_time > view_end_timeline) {
            continue;
        }
        
        // Calculate screen positions
        float start_x = time_to_x(clip_start_time);
        float end_x = time_to_x(clip_start_time + clip_dur_time);
        
        // Get lane Y bounds
        auto [lane_top, lane_bottom] = lane_y_bounds(cc.lane);
        
        // Check if part of the clip extends before timeline frame 0 (should be greyed out)
        float timeline_zero_x = time_to_x(0.0f);
        bool has_greyed_part = (clip_start_time < 0.0f);
        float greyed_end_x = has_greyed_part ? std::min(timeline_zero_x, end_x) : start_x;
        float active_start_x = has_greyed_part ? std::max(start_x, timeline_zero_x) : start_x;
        
        // Draw greyed-out part (before timeline start)
        if (has_greyed_part && greyed_end_x > start_x) {
            ImVec2 grey_min(start_x + clip_margin, lane_top + clip_margin);
            ImVec2 grey_max(greyed_end_x - clip_margin, lane_bottom - clip_margin);
            if (grey_max.x > grey_min.x) {
                // Darker, desaturated color for inactive part
                draw_list->AddRectFilled(grey_min, grey_max, IM_COL32(50, 50, 55, 200), rounding, 
                                        ImDrawFlags_RoundCornersLeft);
                // Diagonal lines pattern to indicate inactive
                for (float px = grey_min.x; px < grey_max.x; px += 8.0f) {
                    float x1 = px;
                    float x2 = std::min(px + (grey_max.y - grey_min.y), grey_max.x);
                    draw_list->AddLine(ImVec2(x1, grey_max.y), ImVec2(x2, grey_min.y), 
                                      IM_COL32(80, 80, 85, 150), 1.0f);
                }
            }
        }
        
        // Clip bounds for active part
        ImVec2 clip_min(active_start_x + clip_margin, lane_top + clip_margin);
        ImVec2 clip_max(end_x - clip_margin, lane_bottom - clip_margin);
        
        // Check mouse hover (on full clip area)
        bool hover_cc = mouse_in_timeline && mouse.x >= start_x && mouse.x <= end_x &&
                        mouse.y >= lane_top && mouse.y <= lane_bottom;
        bool hover_cc_left = hover_cc && mouse.x <= start_x + handle_w + 6;
        bool hover_cc_right = hover_cc && mouse.x >= end_x - handle_w - 6;
        
        // Draw active part of connected clip
        if (clip_max.x > clip_min.x) {
            ImU32 clip_color = clip_color_for(cc.color_id);
            ImDrawFlags corners = has_greyed_part ? ImDrawFlags_RoundCornersRight : 0;
            draw_list->AddRectFilled(clip_min, clip_max, clip_color, rounding, corners);
            
            // Subtle highlight at top edge
            draw_list->AddLine(ImVec2(clip_min.x + (has_greyed_part ? 0 : rounding), clip_min.y + 1), 
                              ImVec2(clip_max.x - rounding, clip_min.y + 1), 
                              IM_COL32(255, 255, 255, 40), 1.0f);
            
            // Handle zones
            float handle_inner = handle_w;
            float clip_width = clip_max.x - clip_min.x;
            if (clip_width > handle_w * 3) {
                if (!has_greyed_part) {
                    ImU32 left_handle_col = hover_cc_left ? IM_COL32(255, 255, 255, 60) : IM_COL32(0, 0, 0, 30);
                    draw_list->AddRectFilled(clip_min, ImVec2(clip_min.x + handle_inner, clip_max.y), 
                                             left_handle_col, rounding, ImDrawFlags_RoundCornersLeft);
                }
                
                ImU32 right_handle_col = hover_cc_right ? IM_COL32(255, 255, 255, 60) : IM_COL32(0, 0, 0, 30);
                draw_list->AddRectFilled(ImVec2(clip_max.x - handle_inner, clip_min.y), clip_max, 
                                         right_handle_col, rounding, ImDrawFlags_RoundCornersRight);
            }
        }
        
        // Selection highlight (covers full clip including greyed part)
        if (state.selected_connected_clip == i) {
            ImVec2 full_min(start_x + clip_margin, lane_top + clip_margin);
            ImVec2 full_max(end_x - clip_margin, lane_bottom - clip_margin);
            draw_list->AddRect(full_min, full_max, IM_COL32(255, 215, 0, 255), rounding, 0, 2.0f);
        }
        
        // Connection line from clip to main timeline at connection_frame position
        // The connection_frame is where the anchor is on the timeline
        // Clamp the visual connection point to be within the clip's visual bounds
        float conn_x = time_to_x(frame_to_time(cc.connection_frame));
        // Clamp connection line to be within the clip's visual rectangle
        conn_x = std::clamp(conn_x, start_x, end_x);
        // Only draw if connection point is in visible range and connection_frame >= 0 on timeline
        if (cc.connection_frame >= 0 && conn_x >= bb_min.x && conn_x <= bb_max.x) {
            float main_y = (cc.lane > 0) ? main_track_top : main_track_bottom;
            float clip_y = (cc.lane > 0) ? lane_bottom : lane_top;
            draw_list->AddLine(ImVec2(conn_x, clip_y), ImVec2(conn_x, main_y), 
                              IM_COL32(255, 255, 255, 120), 1.5f);
            // Small circle at connection point on main timeline
            draw_list->AddCircleFilled(ImVec2(conn_x, main_y), 3.0f, IM_COL32(255, 255, 255, 180));
        }
        
        // Frame count text if wide enough
        float total_width = end_x - start_x;
        if (total_width > 60) {
            char time_str[32];
            snprintf(time_str, sizeof(time_str), "%lld fr", (long long)cc.frame_count());
            ImVec2 text_size = ImGui::CalcTextSize(time_str);
            // Center text in active part if there's a greyed part
            float text_center_x = has_greyed_part ? (active_start_x + end_x) / 2 : (start_x + end_x) / 2;
            ImVec2 text_pos(text_center_x - text_size.x / 2, 
                           (lane_top + lane_bottom - text_size.y) / 2);
            draw_list->AddText(text_pos, IM_COL32(255, 255, 255, 180), time_str);
        }
    }
    
    // Draw lane separator lines
    draw_list->AddLine(ImVec2(bb_min.x, main_track_top), ImVec2(bb_max.x, main_track_top), 
                      IM_COL32(60, 60, 65, 255), 1.0f);
    draw_list->AddLine(ImVec2(bb_min.x, main_track_bottom), ImVec2(bb_max.x, main_track_bottom), 
                      IM_COL32(60, 60, 65, 255), 1.0f);
    
    // Convert current timeline frame to timeline position
    float playhead_timeline_pos = 0.0f;
    bool at_end = false;
    int64_t clamped_timeline = std::clamp(*current_timeline_frame, (int64_t)0, total_timeline_frames);
    playhead_timeline_pos = frame_to_time(clamped_timeline);
    if (!clips.empty() && clamped_timeline == total_timeline_frames) {
        at_end = true;
    }
    
    // Playhead - red line with triangular head (only if visible)
    float curr_x = time_to_x(playhead_timeline_pos);
    bool playhead_visible = (playhead_timeline_pos >= view_start_timeline - 0.1f &&
                             playhead_timeline_pos <= view_end_timeline + 0.1f);
    
    if (playhead_visible) {
        curr_x = std::clamp(curr_x, bb_min.x, bb_max.x);
        
        // Playhead line (stop above scrollbar)
        draw_list->AddLine(ImVec2(curr_x, bb_min.y + playhead_head_size), 
                           ImVec2(curr_x, clip_area_bottom), IM_COL32(255, 80, 80, 255), 2.0f);
        
        // Playhead head (triangle)
        ImVec2 head_points[3] = {
            ImVec2(curr_x, bb_min.y + playhead_head_size),
            ImVec2(curr_x - playhead_head_size/2, bb_min.y),
            ImVec2(curr_x + playhead_head_size/2, bb_min.y)
        };
        draw_list->AddTriangleFilled(head_points[0], head_points[1], head_points[2], IM_COL32(255, 80, 80, 255));
    }
    
    // Zoom indicator (top-right corner) - show when not at 1x
    if (state.zoom < 0.95f || state.zoom > 1.05f) {
        char zoom_text[32];
        snprintf(zoom_text, sizeof(zoom_text), "%.2fx", state.zoom);
        ImVec2 text_size = ImGui::CalcTextSize(zoom_text);
        draw_list->AddText(ImVec2(bb_max.x - text_size.x - 8, bb_min.y + 4), 
                           IM_COL32(150, 150, 160, 200), zoom_text);
    }
    
    // Invisible button for interaction
    ImGui::InvisibleButton(label, size);
    bool is_hovered = ImGui::IsItemHovered();
    bool is_active = ImGui::IsItemActive();
    
    bool changed = false;

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LIB_CLIP")) {
            if (payload->DataSize == sizeof(int)) {
                int source_id = *static_cast<const int*>(payload->Data);
                float drop_time = x_to_time(mouse.x);
                drop_time = std::clamp(drop_time, 0.0f, total_duration);
                int64_t drop_frame = time_to_frame_local(drop_time);
                drop_frame = std::clamp<int64_t>(drop_frame, 0, total_timeline_frames);
                state.pending_library_source = source_id;
                state.pending_library_timeline = drop_frame;
            }
        }
        ImGui::EndDragDropTarget();
    }
    
    // Mouse wheel: Shift+wheel for scroll, plain wheel for zoom
    // Also support horizontal scroll (touchpad, horizontal mouse wheel)
    if (is_hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        float wheel_h = ImGui::GetIO().MouseWheelH;
        
        // Horizontal scroll (touchpad gesture or horizontal mouse wheel)
        if (std::abs(wheel_h) > 0.01f && state.zoom > 1.01f) {
            state.scroll -= wheel_h * visible_duration * 0.15f;
            state.scroll = std::clamp(state.scroll, 0.0f, max_scroll);
        }
        
        if (std::abs(wheel) > 0.01f) {
            if (ImGui::GetIO().KeyShift && state.zoom > 1.01f) {
                // Shift+wheel for horizontal scrolling
                state.scroll -= wheel * visible_duration * 0.15f;
                state.scroll = std::clamp(state.scroll, 0.0f, max_scroll);
            } else {
                // Plain wheel for zooming (centered on mouse position)
                float mouse_time = x_to_time(mouse.x);
                float old_zoom = state.zoom;
                
                state.zoom *= (wheel > 0) ? 1.2f : (1.0f / 1.2f);
                state.zoom = std::clamp(state.zoom, 0.25f, max_zoom);
                
                // Adjust scroll to keep mouse position stable
                if (state.zoom != old_zoom) {
                    float new_visible = base_duration / state.zoom;
                    state.scroll = mouse_time - (mouse.x - bb_min.x) / size.x * new_visible;
                    state.scroll = std::clamp(state.scroll, 0.0f, std::max(0.0f, base_duration - new_visible));
                }
            }
        }
    }
    
    // Middle mouse button panning OR Alt+Left drag for panning
    bool start_pan = is_hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || 
                                    (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt));
    if (start_pan && state.dragging == 0) {
        state.dragging = 4;  // Panning mode
        state.pan_start_x = mouse.x;
        state.pan_start_scroll = state.scroll;
    }
    
    if (state.dragging == 4) {
        bool still_panning = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || 
                             (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt);
        if (still_panning) {
            float delta_x = mouse.x - state.pan_start_x;
            float delta_time = -(delta_x / size.x) * visible_duration;
            state.scroll = state.pan_start_scroll + delta_time;
            state.scroll = std::clamp(state.scroll, 0.0f, max_scroll);
        } else {
            state.dragging = 0;
        }
    }
    
    
    // Build clip screen positions for interaction (same logic as drawing)
    struct ClipScreenPos { float start_x, end_x; int clip_idx; float timeline_start; int64_t start_frame; };
    std::vector<ClipScreenPos> clip_positions;
    {
        float tpos = 0.0f;
        int64_t frame_pos = 0;
        for (int i = 0; i < (int)clips.size(); i++) {
            float dur = frame_to_time(clips[i].frame_count());
            float sx = time_to_x(tpos);
            float ex = time_to_x(tpos + dur);
            clip_positions.push_back({sx, ex, i, tpos, frame_pos});
            tpos += dur;
            frame_pos += clips[i].frame_count();
        }
    }

    // Drag ghost: show semi-transparent clip at the dragged position
    if (state.dragging == 5 && state.dragging_clip >= 0 && state.dragging_clip < (int)clips.size()) {
        const Clip& dragged = clips[state.dragging_clip];
        float drag_dur = frame_to_time(dragged.frame_count());
        float cursor_time = x_to_time(mouse.x);
        float drag_start_time = cursor_time - state.drag_grab_offset_time;
        float max_start = std::max(0.0f, total_duration - drag_dur);
        drag_start_time = std::clamp(drag_start_time, 0.0f, max_start);
        float drag_end_time = drag_start_time + drag_dur;
        float ghost_start_x = time_to_x(drag_start_time);
        float ghost_end_x = time_to_x(drag_end_time);
        ImVec2 ghost_min(ghost_start_x + clip_margin, bb_min.y + clip_margin);
        ImVec2 ghost_max(ghost_end_x - clip_margin, clip_area_bottom - clip_margin);
        if (ghost_max.x > ghost_min.x) {
            ImVec4 col = ImGui::ColorConvertU32ToFloat4(clip_color_for(dragged.color_id));
            col.w = 0.35f;
            ImU32 ghost_col = ImGui::ColorConvertFloat4ToU32(col);
            draw_list->AddRectFilled(ghost_min, ghost_max, ghost_col, rounding);
            draw_list->AddRect(ghost_min, ghost_max, IM_COL32(255, 255, 255, 80), rounding);
        }
    }
    
    // Detect if clips are in source order (needed for safe trim constraints)
    bool clips_in_source_order = true;
    for (int i = 1; i < (int)clips.size(); i++) {
        if (clips[i].start_frame < clips[i - 1].start_frame) {
            clips_in_source_order = false;
            break;
        }
    }
    
    // Helper: map timeline frame to source frame + source id
    auto timeline_frame_to_source_frame = [&](int64_t timeline_frame, int* out_source_id) -> int64_t {
        if (clips.empty()) return 0;
        if (timeline_frame < 0) timeline_frame = 0;
        if (timeline_frame >= total_timeline_frames) {
            if (out_source_id) *out_source_id = clips.back().source_id;
            return clips.back().end_frame;  // end position
        }
        for (const auto& c : clips) {
            int64_t count = c.frame_count();
            if (timeline_frame < count) {
                if (out_source_id) *out_source_id = c.source_id;
                return c.start_frame + timeline_frame;
            }
            timeline_frame -= count;
        }
        if (out_source_id) *out_source_id = clips.back().source_id;
        return clips.back().end_frame;
    };
    
    auto source_frame_to_timeline_frame = [&](int source_id, int64_t source_frame) -> int64_t {
        if (clips.empty()) return 0;
        int64_t t = 0;
        for (size_t i = 0; i < clips.size(); i++) {
            const auto& c = clips[i];
            if (c.source_id != source_id) { t += c.frame_count(); continue; }
            if (source_frame >= c.start_frame && source_frame < c.end_frame)
                return t + (source_frame - c.start_frame);
            if (i == clips.size() - 1 && source_frame == c.end_frame)
                return t + c.frame_count();
            t += c.frame_count();
        }
        return std::min(t, total_timeline_frames);
    };
    
    // Hover playhead (temporary) - follows mouse, does not move real playhead
    if (mouse_in_timeline && !mouse_over_scrollbar) {
        float hover_time = x_to_time(mouse.x);
        hover_time = std::clamp(hover_time, 0.0f, total_duration);
        
        // Snap hover playhead to discrete timeline frames
        int64_t hover_timeline_frame = time_to_frame_local(hover_time);
        hover_timeline_frame = std::clamp<int64_t>(hover_timeline_frame, 0, total_timeline_frames);
        float hover_timeline_pos = frame_to_time(hover_timeline_frame);
        
        float hover_x = time_to_x(hover_timeline_pos);
        hover_x = std::clamp(hover_x, bb_min.x, bb_max.x);
        
        draw_list->AddLine(ImVec2(hover_x, bb_min.y + playhead_head_size), 
                           ImVec2(hover_x, clip_area_bottom), IM_COL32(255, 255, 255, 180), 1.0f);
        ImVec2 head_points[3] = {
            ImVec2(hover_x, bb_min.y + playhead_head_size),
            ImVec2(hover_x - playhead_head_size/2, bb_min.y),
            ImVec2(hover_x + playhead_head_size/2, bb_min.y)
        };
        draw_list->AddTriangleFilled(head_points[0], head_points[1], head_points[2], IM_COL32(255, 255, 255, 180));
    }
    
    // Detect what we clicked on (but not if clicking on scrollbar)
    if (ImGui::IsItemClicked(0) && !mouse_over_scrollbar) {
        float click_x = mouse.x;
        state.dragging = 0;
        state.dragging_clip = -1;
        state.pending_clip = -1;
        state.pending_click = false;
        state.pending_grab_offset_time = 0.0f;
        state.drag_start_mouse_time = 0.0f;
        bool clicked_clip = false;
        
        // Check each clip's handles or body (only if mouse Y is in main track)
        bool mouse_in_main_track = (mouse.y >= main_track_top && mouse.y <= main_track_bottom);
        for (int i = 0; i < (int)clips.size(); i++) {
            if (!mouse_in_main_track) break;  // Skip main clip check if not in main track
            float start_x = clip_positions[i].start_x;
            float end_x = clip_positions[i].end_x;
            
            bool on_left = (click_x >= start_x && click_x <= start_x + handle_w + 6);
            bool on_right = (click_x >= end_x - handle_w - 6 && click_x <= end_x);
            bool on_body = (click_x > start_x + handle_w + 6 && click_x < end_x - handle_w - 6);
            
            if (on_left && on_right) {
                clicked_clip = true;
                state.selected_clip = i;
                if (std::abs(click_x - start_x) < std::abs(click_x - end_x)) {
                    state.dragging = 1;
                } else {
                    state.dragging = 2;
                }
                state.dragging_clip = i;
                state.trim_mouse_start_x = mouse.x;
                state.lead_in_start = state.lead_in_time;
                state.trim_start_frame = clips[i].start_frame;
                {
                    int64_t playhead = *current_timeline_frame;
                    int64_t clip_timeline_start = clip_positions[i].start_frame;
                    int64_t clip_timeline_end = clip_timeline_start + clips[i].frame_count();
                    state.trim_playhead_in_clip =
                        (playhead >= clip_timeline_start && playhead <= clip_timeline_end);
                    state.trim_playhead_source_frame = *current_source_frame;
                    state.trim_playhead_source_id = *current_source_id;
                    // Record original connection positions for left-trim
                    state.left_trim_clip_timeline_start = clip_timeline_start;
                    state.left_trim_original_connections.clear();
                    for (int ci = 0; ci < (int)connected_clips.size(); ci++) {
                        const auto& cc = connected_clips[ci];
                        state.left_trim_original_connections.push_back({ci, cc.connection_frame, cc.connection_offset});
                    }
                }
                break;
            } else if (on_left) {
                clicked_clip = true;
                state.selected_clip = i;
                state.dragging = 1;
                state.dragging_clip = i;
                state.trim_mouse_start_x = mouse.x;
                state.lead_in_start = state.lead_in_time;
                state.trim_start_frame = clips[i].start_frame;
                {
                    int64_t playhead = *current_timeline_frame;
                    int64_t clip_timeline_start = clip_positions[i].start_frame;
                    int64_t clip_timeline_end = clip_timeline_start + clips[i].frame_count();
                    state.trim_playhead_in_clip =
                        (playhead >= clip_timeline_start && playhead <= clip_timeline_end);
                    state.trim_playhead_source_frame = *current_source_frame;
                    state.trim_playhead_source_id = *current_source_id;
                    state.trim_playhead_timeline = playhead;
                    // Record original connection positions for left-trim
                    state.left_trim_clip_timeline_start = clip_timeline_start;
                    state.left_trim_original_connections.clear();
                    for (int ci = 0; ci < (int)connected_clips.size(); ci++) {
                        const auto& cc = connected_clips[ci];
                        state.left_trim_original_connections.push_back({ci, cc.connection_frame, cc.connection_offset});
                    }
                }
                break;
            } else if (on_right) {
                clicked_clip = true;
                state.selected_clip = i;
                state.dragging = 2;
                state.dragging_clip = i;
                // Initialize right-trim state
                state.right_trim_original_end = clips[i].end_frame;
                state.right_trim_clip_timeline_start = clip_positions[i].start_frame;
                state.right_trim_original_connections.clear();
                state.right_trim_temp_gap_clip_idx = -1;
                state.right_trim_cc_on_gap.clear();
                // Record ALL connected clips' original positions including timeline_start
                for (int ci = 0; ci < (int)connected_clips.size(); ci++) {
                    const auto& cc = connected_clips[ci];
                    state.right_trim_original_connections.push_back({ci, cc.connection_frame, cc.connection_offset, cc.timeline_start()});
                }
                break;
            } else if (on_body && !ImGui::GetIO().KeyAlt) {
                clicked_clip = true;
                state.selected_clip = i;
                state.pending_clip = i;
                state.pending_click = true;
                state.pending_click_x = click_x;
                state.pending_mouse_start = mouse;
                // Store grab offset so dragging uses the clip's actual center.
                float grab_offset = (click_x - start_x) / size.x * visible_duration;
                state.pending_grab_offset_time = std::clamp(grab_offset, 0.0f, frame_to_time(clips[i].frame_count()));
                
                // Click moves playhead immediately; drag can take over if it starts
                float timeline_time = x_to_time(click_x);
                timeline_time = std::clamp(timeline_time, 0.0f, total_duration);
                int64_t timeline_frame = time_to_frame_local(timeline_time);
                timeline_frame = std::clamp<int64_t>(timeline_frame, 0, total_timeline_frames);
                *current_timeline_frame = timeline_frame;
                *current_source_frame = timeline_frame_to_source_frame(timeline_frame, current_source_id);
                changed = true;
                break;
            }
        }
        
        // Check connected clips if we didn't hit a main clip
        bool clicked_connected = false;
        if (!clicked_clip) {
            for (int i = 0; i < (int)connected_clips.size(); i++) {
                const ConnectedClip& cc = connected_clips[i];
                float clip_start_time = frame_to_time(cc.timeline_start());
                float clip_dur_time = frame_to_time(cc.frame_count());
                float start_x = time_to_x(clip_start_time);
                float end_x = time_to_x(clip_start_time + clip_dur_time);
                auto [lane_top, lane_bottom] = lane_y_bounds(cc.lane);
                
                // Check if click is in this clip's Y range
                if (mouse.y < lane_top || mouse.y > lane_bottom) continue;
                if (click_x < start_x || click_x > end_x) continue;
                
                bool on_left = (click_x >= start_x && click_x <= start_x + handle_w + 6);
                bool on_right = (click_x >= end_x - handle_w - 6 && click_x <= end_x);
                
                clicked_connected = true;
                state.selected_clip = -1;  // Deselect main clip
                state.selected_connected_clip = i;
                
                if (on_left && on_right) {
                    if (std::abs(click_x - start_x) < std::abs(click_x - end_x)) {
                        state.connected_clip_dragging = 1;
                    } else {
                        state.connected_clip_dragging = 2;
                    }
                } else if (on_left) {
                    state.connected_clip_dragging = 1;
                } else if (on_right) {
                    state.connected_clip_dragging = 2;
                } else {
                    state.connected_clip_dragging = 3;  // Move the clip
                }
                state.dragging_connected_clip = i;
                state.connected_clip_drag_start_connection = cc.connection_frame;
                state.connected_clip_trim_start_frame = cc.start_frame;
                state.trim_mouse_start_x = mouse.x;
                break;
            }
        }
        
        // If didn't hit any handle or clip, move playhead to the clicked position
        if (state.dragging == 0 && !clicked_connected && !ImGui::GetIO().KeyAlt) {
            float timeline_time = x_to_time(click_x);
            timeline_time = std::clamp(timeline_time, 0.0f, total_duration);
            int64_t timeline_frame = time_to_frame_local(timeline_time);
            timeline_frame = std::clamp<int64_t>(timeline_frame, 0, total_timeline_frames);
            *current_timeline_frame = timeline_frame;
            *current_source_frame = timeline_frame_to_source_frame(timeline_frame, current_source_id);
            changed = true;
            if (!clicked_clip && !clicked_connected) {
                state.selected_clip = -1;
                state.selected_connected_clip = -1;
            }
        }
    }
    
    if (!is_active && state.dragging != 4) {  // Don't reset for middle-mouse panning
        // Finalize gap clip on right-trim release
        if (state.dragging == 2 && state.right_trim_temp_gap_clip_idx != -1) {
            int gap_idx = state.right_trim_temp_gap_clip_idx;
            if (gap_idx < (int)clips.size() && clips[gap_idx].is_gap) {
                // Check if any connected clips are still on the gap
                if (state.right_trim_cc_on_gap.empty()) {
                    // No connected clips on gap - remove it
                    clips.erase(clips.begin() + gap_idx);
                }
                // Otherwise keep the gap clip (it's now permanent)
            }
            state.right_trim_temp_gap_clip_idx = -1;
            state.right_trim_cc_on_gap.clear();
        }
        
        state.dragging = 0;
        state.dragging_clip = -1;
        state.pending_clip = -1;
        state.pending_click = false;
        state.pending_grab_offset_time = 0.0f;
        state.drag_grab_offset_time = 0.0f;
        state.drag_start_mouse_time = 0.0f;
    }

    // Promote click to drag if mouse moves past threshold
    if (is_active && state.pending_click && state.pending_clip >= 0) {
        float dx = mouse.x - state.pending_mouse_start.x;
        float dy = mouse.y - state.pending_mouse_start.y;
        float dist2 = dx * dx + dy * dy;
        float thresh = ImGui::GetIO().MouseDragThreshold;
        if (dist2 >= thresh * thresh) {
            state.dragging = 5;
            state.dragging_clip = state.pending_clip;
            state.pending_click = false;
            state.drag_grab_offset_time = state.pending_grab_offset_time;
            state.drag_start_mouse_time = x_to_time(state.pending_mouse_start.x);
        }
    }
    
    if (state.dragging != 0 && state.dragging != 4 && is_active) {
        // Use zoom-aware time conversion
        float timeline_time = x_to_time(mouse.x);
        timeline_time = std::clamp(timeline_time, 0.0f, total_duration);
        
        if (state.dragging == 5 && state.dragging_clip >= 0 && state.dragging_clip < (int)clips.size()) {
            // Dragging a clip to reorder
            const Clip dragged = clips[state.dragging_clip];
            int insert_index = 0;
            float drag_cursor_time = timeline_time;
            for (int i = 0; i < (int)clips.size(); i++) {
                if (i == state.dragging_clip) continue;
                float tpos = clip_positions[i].timeline_start;
                float dur = frame_to_time(clips[i].frame_count());
                // Drop only after crossing the midpoint of the target clip.
                float drop_threshold = tpos + dur * 0.5f;
                if (drag_cursor_time < drop_threshold) break;
                insert_index++;
            }
            
            int old_index = state.dragging_clip;
            if (insert_index != old_index) {
                // Record old clip timeline starts for connected clip adjustment
                std::vector<int64_t> old_starts(clips.size());
                int64_t t = 0;
                for (size_t i = 0; i < clips.size(); i++) {
                    old_starts[i] = t;
                    t += clips[i].frame_count();
                }
                int64_t dragged_old_start = old_starts[old_index];
                int64_t dragged_frames = dragged.frame_count();
                
                clips.erase(clips.begin() + old_index);
                clips.insert(clips.begin() + insert_index, dragged);
                
                // Calculate new clip timeline starts
                std::vector<int64_t> new_starts(clips.size());
                t = 0;
                for (size_t i = 0; i < clips.size(); i++) {
                    new_starts[i] = t;
                    t += clips[i].frame_count();
                }
                int64_t dragged_new_start = new_starts[insert_index];
                
                // Update connected clips: adjust connection_frame based on timeline shift
                for (auto& cc : connected_clips) {
                    int64_t cf = cc.connection_frame;
                    // If connected to the dragged clip, move with it
                    if (cf >= dragged_old_start && cf < dragged_old_start + dragged_frames) {
                        int64_t offset_in_clip = cf - dragged_old_start;
                        cc.connection_frame = dragged_new_start + offset_in_clip;
                    }
                    // If connected to a clip that shifted due to the move
                    else if (old_index < insert_index) {
                        // Dragged clip moved right: clips between old and new shifted left
                        if (cf >= dragged_old_start + dragged_frames && cf < new_starts[insert_index] + dragged_frames) {
                            cc.connection_frame -= dragged_frames;
                        }
                    } else {
                        // Dragged clip moved left: clips between new and old shifted right
                        if (cf >= dragged_new_start + dragged_frames && cf < dragged_old_start + dragged_frames) {
                            cc.connection_frame += dragged_frames;
                        }
                    }
                }
                
                if (state.selected_clip == old_index) {
                    state.selected_clip = insert_index;
                } else if (old_index < insert_index) {
                    if (state.selected_clip > old_index && state.selected_clip <= insert_index) {
                        state.selected_clip--;
                    }
                } else {
                    if (state.selected_clip >= insert_index && state.selected_clip < old_index) {
                        state.selected_clip++;
                    }
                }
                state.dragging_clip = insert_index;
                changed = true;
                state.clips_modified = true;
            }
        } else if (state.dragging_clip >= 0 && state.dragging_clip < (int)clips.size()) {
            Clip& clip = clips[state.dragging_clip];
            
            // Use zoom-aware conversion for handle dragging
            const auto& cp = clip_positions[state.dragging_clip];
            
            if (state.dragging == 1) {
                // Left handle - adjust start_frame (trim beginning)
                float delta_x = mouse.x - state.trim_mouse_start_x;
                float delta_time = (delta_x / size.x) * visible_duration;
                int64_t delta_frames = time_to_frame_local(std::abs(delta_time));
                if (delta_time < 0) delta_frames = -delta_frames;
                int64_t clip_timeline_start = cp.start_frame;
                int64_t min_start = 0;
                int64_t max_start = clip.end_frame - 1;
                if (min_start > max_start) min_start = max_start;
                int64_t new_start = state.trim_start_frame + delta_frames;
                new_start = std::clamp(new_start, min_start, max_start);
                clip.start_frame = new_start;
                // Adjust lead_in based on actual frame change (not floating point delta_time)
                // to avoid jitter from float/int mismatch
                int64_t actual_delta = new_start - state.trim_start_frame;
                state.lead_in_time = std::max(0.0f, state.lead_in_start + frame_to_time(actual_delta));
                
                if (delta_frames != 0) {
                    int64_t orig_timeline = state.trim_playhead_timeline;
                    int64_t new_timeline = orig_timeline;
                    
                    if (state.trim_playhead_in_clip) {
                        int64_t orig_src = state.trim_playhead_source_frame;
                        int orig_sid = state.trim_playhead_source_id;
                        
                        // Playhead is in the clip being trimmed
                        bool clip_start_passed = orig_sid == clip.source_id
                            && new_start > orig_src;
                        
                        if (clip_start_passed) {
                            // Playhead got trimmed past - move to new clip start
                            new_timeline = clip_timeline_start;
                        } else {
                            // Preserve source frame - recalculate timeline within this clip
                            int64_t offset_in_clip = orig_src - new_start;
                            new_timeline = clip_timeline_start + offset_in_clip;
                        }
                    } else if (orig_timeline > clip_timeline_start) {
                        // Playhead is AFTER the clip being trimmed
                        // Calculate from ORIGINAL timeline, subtracting total delta
                        new_timeline = orig_timeline - delta_frames;
                    }
                    // If playhead is BEFORE the clip, no adjustment needed (new_timeline = orig_timeline)
                    
                    // Update connected clips based on left-trim using ORIGINAL positions
                    // actual_delta > 0: clip grows (left edge moves left, showing earlier source frames)
                    // actual_delta < 0: clip shrinks (left edge moves right, hiding earlier source frames)
                    // 
                    // Connection points should stay at the same SOURCE frame in the parent clip.
                    // When source content shifts, connections shift on the timeline to compensate.
                    // Exception: if left edge passes the connection's source position, it sticks to edge.
                    int64_t orig_clip_timeline_start = state.left_trim_clip_timeline_start;
                    int64_t orig_start_frame = state.trim_start_frame;  // Source frame at drag start
                    int64_t orig_clip_end_timeline = orig_clip_timeline_start + (clips[state.dragging_clip].end_frame - orig_start_frame);
                    int64_t new_start_frame = clip.start_frame;  // Source frame after trim
                    
                    for (const auto& [cc_idx, orig_connection, orig_conn_offset] : state.left_trim_original_connections) {
                        if (cc_idx >= (int)connected_clips.size()) continue;
                        auto& cc = connected_clips[cc_idx];
                        
                        // Was this connected clip in the ORIGINAL clip range?
                        if (orig_connection >= orig_clip_timeline_start && 
                            orig_connection < orig_clip_end_timeline) {
                            // Calculate the source frame in the PARENT this connection was originally pointing to
                            int64_t orig_offset_in_parent = orig_connection - orig_clip_timeline_start;
                            int64_t parent_source_frame = orig_start_frame + orig_offset_in_parent;
                            
                            // Check if the new left edge has passed this source frame
                            if (new_start_frame > parent_source_frame) {
                                // Edge passed - connection sticks to parent's left edge
                                // connection_offset increases to point to the next frame in connected clip
                                int64_t frames_past = new_start_frame - parent_source_frame;
                                cc.connection_frame = clip_timeline_start;
                                cc.connection_offset = orig_conn_offset + frames_past;
                                // Clamp to not exceed clip length
                                if (cc.connection_offset >= cc.frame_count()) {
                                    cc.connection_offset = cc.frame_count() - 1;
                                }
                            } else {
                                // Normal case - stay at same source frame, adjust timeline position
                                int64_t new_offset = parent_source_frame - new_start_frame;
                                cc.connection_frame = clip_timeline_start + new_offset;
                                cc.connection_offset = orig_conn_offset;  // Restore original offset
                            }
                        }
                        // Connected clips AFTER the original clip end should shift
                        else if (orig_connection >= orig_clip_end_timeline) {
                            cc.connection_frame = orig_connection - actual_delta;
                            cc.connection_offset = orig_conn_offset;
                        }
                        // Connected clips BEFORE the clip being trimmed stay in place
                        else {
                            cc.connection_frame = orig_connection;
                            cc.connection_offset = orig_conn_offset;
                        }
                    }
                    
                    *current_timeline_frame = new_timeline;
                    changed = true;
                    state.clips_modified = true;
                }
            } else if (state.dragging == 2) {
                // Right handle - adjust end_frame (trim end)
                float delta_x = mouse.x - cp.end_x;
                float delta_time = (delta_x / size.x) * visible_duration;
                int64_t delta_frames = time_to_frame_local(std::abs(delta_time));
                if (delta_time < 0) delta_frames = -delta_frames;
                
                int64_t old_end = clip.end_frame;
                int64_t new_end = clip.end_frame + delta_frames;
                
                int64_t max_end = total_source_frames;
                if (clips_in_source_order && state.dragging_clip < (int)clips.size() - 1) {
                    max_end = clips[state.dragging_clip + 1].start_frame;
                }
                int64_t min_end = clip.start_frame + 1;
                if (max_end < min_end) {
                    max_end = min_end;
                }
                new_end = std::clamp(new_end, min_end, max_end);
                
                // Calculate the actual frame change from ORIGINAL end
                int64_t total_delta = new_end - state.right_trim_original_end;
                int64_t orig_clip_end_timeline = cp.start_frame + (state.right_trim_original_end - clip.start_frame);
                
                clip.end_frame = new_end;
                
                // Update connected clips based on right-trim using ORIGINAL positions
                // total_delta > 0: clip grows (right edge moves right from original)
                // total_delta < 0: clip shrinks (right edge moves left from original)
                int64_t new_clip_end_timeline = cp.start_frame + clip.frame_count();
                int64_t clip_timeline_start = state.right_trim_clip_timeline_start;
                int64_t orig_start_source = clip.start_frame;  // Source start doesn't change during right-trim
                
                // First pass: determine which connected clips need gap clip
                std::vector<int> need_gap_clip;
                
                for (const auto& [cc_idx, orig_connection, orig_conn_offset, orig_tl_start] : state.right_trim_original_connections) {
                    if (cc_idx >= (int)connected_clips.size()) continue;
                    
                    // Was this connected clip in the ORIGINAL clip range?
                    if (orig_connection >= clip_timeline_start && 
                        orig_connection < orig_clip_end_timeline) {
                        // Check if connected clip's original timeline_start is past new parent end
                        // This means the connected clip is "orphaned" - needs gap clip
                        if (orig_tl_start >= new_clip_end_timeline) {
                            need_gap_clip.push_back(cc_idx);
                        }
                    }
                }
                
                // Manage gap clip - ONLY insert if this is the last clip in timeline
                // If there's a next clip, connected clips can hover over it (no gap needed)
                bool is_last_clip = (state.dragging_clip == (int)clips.size() - 1);
                int gap_clip_idx = state.right_trim_temp_gap_clip_idx;
                int64_t gap_timeline_start = new_clip_end_timeline;
                
                if (!need_gap_clip.empty() && gap_clip_idx == -1 && is_last_clip) {
                    // Need to insert a gap clip after the parent clip (only for last clip)
                    // Find where to insert it (after the clip being trimmed)
                    int insert_pos = state.dragging_clip + 1;
                    
                    // Calculate the duration needed - at least covers the furthest connected clip
                    int64_t max_cc_end = 0;
                    for (int cc_idx : need_gap_clip) {
                        const auto& orig = state.right_trim_original_connections[cc_idx];
                        int64_t orig_tl_end = std::get<3>(orig) + connected_clips[cc_idx].frame_count();
                        max_cc_end = std::max(max_cc_end, orig_tl_end);
                    }
                    int64_t gap_duration = max_cc_end - new_clip_end_timeline;
                    if (gap_duration < 1) gap_duration = 1;
                    
                    // Create gap clip - use source_id 0 with virtual frames
                    Clip gap_clip;
                    gap_clip.source_id = clip.source_id;  // Use same source for frame reference
                    gap_clip.start_frame = 0;
                    gap_clip.end_frame = gap_duration;
                    gap_clip.color_id = -1;  // Special color for gap
                    gap_clip.is_gap = true;
                    
                    clips.insert(clips.begin() + insert_pos, gap_clip);
                    state.right_trim_temp_gap_clip_idx = insert_pos;
                    gap_clip_idx = insert_pos;
                    state.right_trim_cc_on_gap.clear();
                } else if (need_gap_clip.empty() && gap_clip_idx != -1) {
                    // No longer need gap clip - remove it
                    if (gap_clip_idx < (int)clips.size() && clips[gap_clip_idx].is_gap) {
                        clips.erase(clips.begin() + gap_clip_idx);
                    }
                    state.right_trim_temp_gap_clip_idx = -1;
                    state.right_trim_cc_on_gap.clear();
                    gap_clip_idx = -1;
                }
                
                // Adjust gap clip duration if it exists
                if (gap_clip_idx != -1 && gap_clip_idx < (int)clips.size()) {
                    int64_t max_cc_end = 0;
                    for (const auto& [cc_idx, orig_connection, orig_conn_offset, orig_tl_start] : state.right_trim_original_connections) {
                        if (orig_tl_start >= new_clip_end_timeline) {
                            int64_t orig_tl_end = orig_tl_start + connected_clips[cc_idx].frame_count();
                            max_cc_end = std::max(max_cc_end, orig_tl_end);
                        }
                    }
                    int64_t gap_duration = max_cc_end - new_clip_end_timeline;
                    if (gap_duration < 1) gap_duration = 1;
                    clips[gap_clip_idx].end_frame = gap_duration;
                }
                
                // Second pass: update connected clips
                state.right_trim_cc_on_gap.clear();
                
                for (const auto& [cc_idx, orig_connection, orig_conn_offset, orig_tl_start] : state.right_trim_original_connections) {
                    if (cc_idx >= (int)connected_clips.size()) continue;
                    auto& cc = connected_clips[cc_idx];
                    
                    // Was this connected clip in the ORIGINAL clip range?
                    if (orig_connection >= clip_timeline_start && 
                        orig_connection < orig_clip_end_timeline) {
                        
                        // Check if connected clip is orphaned (timeline_start past parent end)
                        if (orig_tl_start >= new_clip_end_timeline && gap_clip_idx != -1) {
                            // Move to gap clip - keep at original timeline position
                            cc.connection_frame = orig_connection;
                            cc.connection_offset = orig_conn_offset;
                            state.right_trim_cc_on_gap.push_back(cc_idx);
                        } else {
                            // Calculate the source frame in the PARENT this connection was originally pointing to
                            int64_t orig_offset_in_parent = orig_connection - clip_timeline_start;
                            int64_t parent_source_frame = orig_start_source + orig_offset_in_parent;
                            int64_t new_end_source = clip.end_frame;
                            
                            // Check if the new right edge has passed this source frame
                            if (new_end_source <= parent_source_frame) {
                                // Right edge passed the connection point - stick to right edge
                                // connection_offset becomes negative so clip extends AFTER the anchor
                                int64_t frames_past = parent_source_frame - new_end_source + 1;
                                cc.connection_frame = new_clip_end_timeline - 1;
                                if (cc.connection_frame < clip_timeline_start) {
                                    cc.connection_frame = clip_timeline_start;
                                }
                                // Negative offset means clip extends to the right of the anchor
                                cc.connection_offset = orig_conn_offset - frames_past;
                                // Clamp so anchor doesn't go past the end of the clip
                                if (cc.connection_offset < -(cc.frame_count() - 1)) {
                                    cc.connection_offset = -(cc.frame_count() - 1);
                                }
                            } else {
                                // Stay at the original position (source frame unchanged)
                                cc.connection_frame = orig_connection;
                                cc.connection_offset = orig_conn_offset;
                            }
                        }
                    }
                    // Connected clips AFTER the original clip end should shift
                    else if (orig_connection >= orig_clip_end_timeline) {
                        // Account for gap clip if present
                        int64_t extra_shift = (gap_clip_idx != -1) ? clips[gap_clip_idx].frame_count() : 0;
                        cc.connection_frame = orig_connection + total_delta + extra_shift;
                        cc.connection_offset = orig_conn_offset;
                    }
                    // Connected clips BEFORE the clip being trimmed stay in place
                    else {
                        cc.connection_frame = orig_connection;
                        cc.connection_offset = orig_conn_offset;
                    }
                }
                
                // If playhead was at the old last frame, follow the handle
                int64_t max_timeline = 0;
                for (const auto& c : clips) max_timeline += c.frame_count();
                *current_timeline_frame = std::clamp(*current_timeline_frame, (int64_t)0, max_timeline);
                *current_source_frame = timeline_frame_to_source_frame(*current_timeline_frame, current_source_id);
                changed = true;
                state.clips_modified = true;
            }
            
            // After resizing, ensure playhead stays within timeline bounds.
            // Skip for left-trim: playhead is managed there (move only when clip start passes it).
            if (!clips.empty() && state.dragging != 1) {
                int64_t max_timeline = 0;
                for (const auto& c : clips) max_timeline += c.frame_count();
                *current_timeline_frame = std::clamp(*current_timeline_frame, (int64_t)0, max_timeline);
                *current_source_frame = timeline_frame_to_source_frame(*current_timeline_frame, current_source_id);
            }
        }
    }
    
    // Handle connected clip dragging
    if (is_active && state.connected_clip_dragging != 0 && state.dragging_connected_clip >= 0) {
        int idx = state.dragging_connected_clip;
        if (idx < (int)connected_clips.size()) {
            ConnectedClip& cc = connected_clips[idx];
            float delta_x = mouse.x - state.trim_mouse_start_x;
            float delta_time = (delta_x / size.x) * visible_duration;
            int64_t delta_frames = time_to_frame_local(std::abs(delta_time));
            if (delta_time < 0) delta_frames = -delta_frames;
            
            if (state.connected_clip_dragging == 1) {
                // Left handle - trim start
                int64_t new_start = state.connected_clip_trim_start_frame + delta_frames;
                int64_t min_start = 0;
                int64_t max_start = cc.end_frame - 1;
                new_start = std::clamp(new_start, min_start, max_start);
                cc.start_frame = new_start;
                changed = true;
                state.clips_modified = true;
            } else if (state.connected_clip_dragging == 2) {
                // Right handle - trim end
                int64_t old_end = cc.end_frame;
                int64_t new_end = old_end + delta_frames;
                int64_t min_end = cc.start_frame + 1;
                new_end = std::max(new_end, min_end);
                cc.end_frame = new_end;
                state.trim_mouse_start_x = mouse.x;  // Incremental for right trim
                changed = true;
                state.clips_modified = true;
            } else if (state.connected_clip_dragging == 3) {
                // Move the clip
                int64_t new_connection = state.connected_clip_drag_start_connection + delta_frames;
                new_connection = std::max(new_connection, (int64_t)0);
                cc.connection_frame = new_connection;
                changed = true;
                state.clips_modified = true;
            }
        }
    }
    
    // Reset connected clip dragging when mouse released
    if (!is_active) {
        state.connected_clip_dragging = 0;
        state.dragging_connected_clip = -1;
    }
    
    // Change cursor when hovering handles
    if (is_hovered && state.dragging == 0 && state.connected_clip_dragging == 0) {
        bool cursor_set = false;
        for (int i = 0; i < (int)clip_positions.size() && !cursor_set; i++) {
            float start_x = clip_positions[i].start_x;
            float end_x = clip_positions[i].end_x;
            
            bool over_left = (mouse.x >= start_x && mouse.x <= start_x + handle_w + 6);
            bool over_right = (mouse.x >= end_x - handle_w - 6 && mouse.x <= end_x);
            bool over_body = (mouse.x > start_x + handle_w + 6 && mouse.x < end_x - handle_w - 6);
            if (over_left || over_right) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                cursor_set = true;
            } else if (over_body) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                cursor_set = true;
            }
        }
        // Check connected clips if no main clip cursor set
        for (int i = 0; i < (int)connected_clips.size() && !cursor_set; i++) {
            const ConnectedClip& cc = connected_clips[i];
            float clip_start_time = frame_to_time(cc.timeline_start());
            float clip_dur_time = frame_to_time(cc.frame_count());
            float start_x = time_to_x(clip_start_time);
            float end_x = time_to_x(clip_start_time + clip_dur_time);
            auto [lane_top, lane_bottom] = lane_y_bounds(cc.lane);
            
            if (mouse.y >= lane_top && mouse.y <= lane_bottom &&
                mouse.x >= start_x && mouse.x <= end_x) {
                bool over_left = (mouse.x <= start_x + handle_w + 6);
                bool over_right = (mouse.x >= end_x - handle_w - 6);
                if (over_left || over_right) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    cursor_set = true;
                } else {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    cursor_set = true;
                }
            }
        }
    }
    
    // Draw scrollbar AFTER clips (always visible to prevent layout jumps)
    {
        // Scrollbar track (always visible)
        draw_list->AddRectFilled(ImVec2(bb_min.x + 2, scroll_bar_y), 
                                 ImVec2(bb_max.x - 2, scroll_bar_y + scroll_bar_h), 
                                 IM_COL32(15, 15, 18, 255), 6.0f);
        
        if (scrollbar_active) {
            // Active scrollbar - show thumb
            float scroll_width = std::max(30.0f, size.x / state.zoom);  // Minimum thumb width
            float scroll_range = size.x - scroll_width;
            float scroll_x = bb_min.x + (max_scroll > 0 ? (state.scroll / max_scroll) * scroll_range : 0);
            
            ImVec2 thumb_min(scroll_x, scroll_bar_y);
            ImVec2 thumb_max(scroll_x + scroll_width, scroll_bar_y + scroll_bar_h);
            bool thumb_hovered = (mouse.x >= thumb_min.x && mouse.x <= thumb_max.x &&
                                  mouse.y >= thumb_min.y - 4 && mouse.y <= thumb_max.y + 4);
            
            // Thumb color
            ImU32 thumb_color = IM_COL32(70, 70, 80, 255);
            if (state.dragging == 6) {
                thumb_color = IM_COL32(110, 110, 130, 255);
            } else if (thumb_hovered) {
                thumb_color = IM_COL32(90, 90, 105, 255);
            }
            
            draw_list->AddRectFilled(thumb_min, thumb_max, thumb_color, 6.0f);
            
            // Handle scrollbar click/drag
            if (mouse_over_scrollbar && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && state.dragging == 0) {
                if (thumb_hovered) {
                    state.dragging = 6;
                    state.pan_start_x = mouse.x;
                    state.pan_start_scroll = state.scroll;
                } else {
                    // Click on track - jump to that position
                    float click_ratio = (mouse.x - bb_min.x - scroll_width/2) / scroll_range;
                    state.scroll = click_ratio * max_scroll;
                    state.scroll = std::clamp(state.scroll, 0.0f, max_scroll);
                }
            }
            
            // Cursor feedback
            if (mouse_over_scrollbar && state.dragging == 0) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        } else {
            // Not zoomed - show full-width greyed thumb
            draw_list->AddRectFilled(ImVec2(bb_min.x + 2, scroll_bar_y), 
                                     ImVec2(bb_max.x - 2, scroll_bar_y + scroll_bar_h), 
                                     IM_COL32(40, 40, 45, 255), 6.0f);
        }
    }
    
    // Handle scrollbar dragging (continue even when mouse leaves scrollbar area)
    if (state.dragging == 6) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float delta_x = mouse.x - state.pan_start_x;
            float delta_scroll = (delta_x / size.x) * base_duration;
            state.scroll = state.pan_start_scroll + delta_scroll;
            state.scroll = std::clamp(state.scroll, 0.0f, max_scroll);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else {
            state.dragging = 0;
        }
    }
    
    return changed;
}

bool TrimTimeline(const char* label, float* current, float* trim_start, float* trim_end, float duration, const ImVec2& size, TrimTimelineState& state) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    ImVec2 bb_min = pos;
    ImVec2 bb_max = ImVec2(pos.x + size.x, pos.y + size.y);
    
    // Background (dark, represents untrimmed/excluded areas)
    draw_list->AddRectFilled(bb_min, bb_max, IM_COL32(30, 30, 35, 255), 4.0f);
    
    // Trimmed region (the part that will be kept)
    float start_x = bb_min.x + (*trim_start / duration) * size.x;
    float end_x = bb_min.x + (*trim_end / duration) * size.x;
    draw_list->AddRectFilled(ImVec2(start_x, bb_min.y), ImVec2(end_x, bb_max.y), IM_COL32(70, 130, 180, 200), 4.0f);
    
    // Trim handles - vertical bars at the edges of trim region
    float handle_w = 6.0f;
    draw_list->AddRectFilled(ImVec2(start_x, bb_min.y), ImVec2(start_x + handle_w, bb_max.y), IM_COL32(255, 200, 50, 255));
    draw_list->AddRectFilled(ImVec2(end_x - handle_w, bb_min.y), ImVec2(end_x, bb_max.y), IM_COL32(255, 200, 50, 255));
    
    // Current position (playhead)
    float curr_x = bb_min.x + (*current / duration) * size.x;
    draw_list->AddLine(ImVec2(curr_x, bb_min.y - 2), ImVec2(curr_x, bb_max.y + 2), IM_COL32(255, 255, 255, 255), 3.0f);
    
    // Invisible button for interaction
    ImGui::InvisibleButton(label, size);
    bool is_hovered = ImGui::IsItemHovered();
    bool is_active = ImGui::IsItemActive();
    
    bool changed = false;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    
    // Detect what we clicked on
    if (ImGui::IsItemClicked(0)) {
        float click_x = mouse.x;
        
        // Check if clicking on left handle (within handle_w pixels of start_x, on the inside)
        bool on_left_handle = (click_x >= start_x && click_x <= start_x + handle_w + 4);
        // Check if clicking on right handle (within handle_w pixels of end_x, on the inside)  
        bool on_right_handle = (click_x >= end_x - handle_w - 4 && click_x <= end_x);
        
        // Prioritize: if both handles overlap (very short trim), use the closer one
        if (on_left_handle && on_right_handle) {
            if (std::abs(click_x - start_x) < std::abs(click_x - end_x)) {
                state.dragging = 1;
            } else {
                state.dragging = 2;
            }
        } else if (on_left_handle) {
            state.dragging = 1;
        } else if (on_right_handle) {
            state.dragging = 2;
        } else {
            state.dragging = 3; // Playhead
        }
    }
    
    if (!is_active) {
        state.dragging = 0;
    }
    
    if (state.dragging != 0 && is_active) {
        float rel_x = std::clamp((mouse.x - bb_min.x) / size.x, 0.0f, 1.0f);
        float time = rel_x * duration;
        
        if (state.dragging == 1) {
            float new_start = std::min(time, *trim_end - 0.1f);
            *trim_start = std::max(new_start, 0.0f);
            changed = true;
        } else if (state.dragging == 2) {
            float new_end = std::max(time, *trim_start + 0.1f);
            *trim_end = std::min(new_end, duration);
            changed = true;
        } else if (state.dragging == 3) {
            *current = std::clamp(time, 0.0f, duration);
            changed = true;
        }
    }
    
    // Change cursor when hovering handles
    if (is_hovered && state.dragging == 0) {
        float hover_x = mouse.x;
        bool over_left = (hover_x >= start_x && hover_x <= start_x + handle_w + 4);
        bool over_right = (hover_x >= end_x - handle_w - 4 && hover_x <= end_x);
        if (over_left || over_right) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }
    
    return changed;
}

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    WindowData main_window = create_window("VibeCut", 1600, 1000);
    if (!main_window.window) {
        std::fprintf(stderr, "Failed to create main window\n");
        glfwTerminate();
        return 1;
    }

    WindowData browser_window{};
    WindowData confirm_window{};
    FileBrowser browser;
    std::string project_path;
    bool project_open = false;
    bool project_dirty = false;
    bool show_exit_confirm = false;
    bool exit_after_save = false;
    std::string save_filename = "project.vibecut";
    VideoPlayer player;
    std::string pending_new_project_media;
    std::string pending_project_load;
    std::string pending_project_save;
    int library_selected = -1;
    enum class BrowserMode { None, AddLibrary, OpenProject, SaveProject, NewProjectFromVideo };
    BrowserMode browser_mode = BrowserMode::None;
    ClipsTimelineState timeline_state;
    std::string window_title_cache;
    
    std::atomic<bool> exporting{false};
    std::atomic<float> export_progress{0.0f};
    std::thread export_thread;

    bool should_exit = false;
    while (!should_exit) {
        glfwPollEvents();

        if (glfwWindowShouldClose(main_window.window)) {
            if (project_dirty) {
                show_exit_confirm = true;
                glfwSetWindowShouldClose(main_window.window, GLFW_FALSE);
            } else {
                should_exit = true;
            }
        }

        if (browser_window.window && glfwWindowShouldClose(browser_window.window)) {
            if (browser_mode == BrowserMode::SaveProject) {
                exit_after_save = false;
            }
            destroy_window(browser_window);
        }
        if (confirm_window.window && glfwWindowShouldClose(confirm_window.window)) {
            destroy_window(confirm_window);
            show_exit_confirm = false;
        }

        if (!pending_new_project_media.empty()) {
            glfwMakeContextCurrent(main_window.window);
            player.close();
            if (player.open(pending_new_project_media)) {
                std::printf("Loaded video: %dx%d, %.1f fps, %lld frames, %.1f sec\n", 
                    player.width, player.height, player.fps, (long long)player.total_frames, player.duration);
                project_open = true;
                project_dirty = true;
                project_path.clear();
            }
            pending_new_project_media.clear();
        }
        if (!pending_project_load.empty()) {
            glfwMakeContextCurrent(main_window.window);
            if (load_project_file(pending_project_load, player, timeline_state.zoom)) {
                project_path = pending_project_load;
                project_open = true;
                project_dirty = false;
                timeline_state.trim_playhead_source_frame = -1;
                timeline_state.trim_playhead_source_id = -1;
                if (!player.sources.empty()) {
                    library_selected = 0;
                }
            }
            pending_project_load.clear();
        }
        if (!pending_project_save.empty()) {
            glfwMakeContextCurrent(main_window.window);
            if (save_project_file(pending_project_save, player, timeline_state.zoom)) {
                project_path = pending_project_save;
                project_dirty = false;
                if (exit_after_save) {
                    should_exit = true;
                }
            }
            exit_after_save = false;
            pending_project_save.clear();
        }

        glfwMakeContextCurrent(main_window.window);
        player.update();

        std::string desired_title = "VibeCut";
        if (project_open) {
            if (!project_path.empty()) {
                desired_title = "VibeCut - " + project_path;
            } else {
                desired_title = "VibeCut - (unsaved)";
            }
        }
        if (desired_title != window_title_cache) {
            glfwSetWindowTitle(main_window.window, desired_title.c_str());
            window_title_cache = desired_title;
        }

        bool open_browser = false;
        render_window(main_window, [&]() {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            if (show_exit_confirm) {
                ImGui::BeginDisabled();
            }
            
            if (project_open) {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(viewport->Size);
                ImGui::Begin("##VideoView", nullptr,
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoBackground |
                    ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse);
                
                float ui_scale = main_window.scale;
                float controls_height = 195 * ui_scale;
                float panel_w = 280 * ui_scale;
                float pad = 10 * ui_scale;
                float panel_h = viewport->Size.y - controls_height - 2 * pad;
                
                ImGui::SetCursorPos(ImVec2(pad, pad));
                ImGui::BeginChild("##ProjectPanel", ImVec2(panel_w, panel_h), true);
                ImGui::Text("Project");
                float full_w = ImGui::GetContentRegionAvail().x;
                if (ImGui::Button("Add a Clip...", ImVec2(full_w, 0))) {
                    if (!browser_window.window) {
                        browser_mode = BrowserMode::AddLibrary;
                        open_browser = true;
                    }
                }
                if (ImGui::Button("Save Project", ImVec2(full_w, 0))) {
                    if (!project_path.empty()) {
                        pending_project_save = project_path;
                    } else {
                        if (!browser_window.window) {
                            save_filename = "project.vibecut";
                            browser_mode = BrowserMode::SaveProject;
                            open_browser = true;
                        }
                    }
                }
                if (ImGui::Button("Open Project", ImVec2(full_w, 0))) {
                    if (!browser_window.window) {
                        browser_mode = BrowserMode::OpenProject;
                        open_browser = true;
                    }
                }
                if (ImGui::Button("New Project", ImVec2(full_w, 0))) {
                    player.close();
                    project_open = true;
                    project_dirty = true;
                    project_path.clear();
                    library_selected = -1;
                }
                if (exporting) {
                    ImGui::ProgressBar(export_progress, ImVec2(full_w, 0), "Exporting...");
                } else {
                    bool can_export = player.loaded && !player.clips.empty() && !player.source_path.empty();
                    if (!can_export) ImGui::BeginDisabled();
                    if (ImGui::Button("Export", ImVec2(full_w, 0))) {
                        fs::path base = project_path.empty() ? fs::path(player.source_path) : fs::path(project_path);
                        std::string stem = base.stem().string();
                        if (stem.empty()) stem = "vibecut_project";
                        std::string out_path = (base.parent_path() / (stem + "_export.mp4")).string();
                        
                        exporting = true;
                        export_progress = 0.0f;
                        
                        std::vector<Clip> clips_copy = player.clips;
                        std::vector<ConnectedClip> connected_clips_copy = player.connected_clips;
                        double fps_copy = player.fps;
                        std::vector<std::string> source_paths;
                        source_paths.reserve(player.sources.size());
                        for (const auto& src : player.sources) {
                            source_paths.push_back(src.path);
                        }
                        ExportAudio audio_copy;
                        audio_copy.timeline_pcm = player.audio.timeline_pcm;
                        audio_copy.sample_rate = player.audio.sample_rate;
                        audio_copy.channels = player.audio.channels;
                        audio_copy.has_audio = !audio_copy.timeline_pcm.empty();
                        
                        if (export_thread.joinable()) export_thread.join();
                        export_thread = std::thread([&, out_path, clips_copy, connected_clips_copy, fps_copy, source_paths, audio_copy]() {
                            export_clips(source_paths, out_path, clips_copy, connected_clips_copy, fps_copy, audio_copy, exporting, export_progress);
                        });
                    }
                    if (!can_export) ImGui::EndDisabled();
                }
                ImGui::Separator();
                for (int i = 0; i < (int)player.sources.size(); i++) {
                    const auto& src = player.sources[i];
                    bool selected = (library_selected == i);
                    if (ImGui::Selectable(path_display_name(fs::path(src.path)).c_str(), selected)) {
                        library_selected = i;
                    }
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("LIB_CLIP", &i, sizeof(int));
                        ImGui::Text("%s", path_display_name(fs::path(src.path)).c_str());
                        ImGui::EndDragDropSource();
                    }
                }
                if (library_selected >= 0 && library_selected < (int)player.sources.size()) {
                    ImGui::Separator();
                    float clip_btn_w = ImGui::GetContentRegionAvail().x;
                    if (ImGui::Button("Preview", ImVec2(clip_btn_w, 0))) {
                        player.start_library_preview(library_selected);
                    }
                    if (ImGui::Button("Insert to Timeline", ImVec2(clip_btn_w, 0))) {
                        if (player.insert_clip_at_timeline(library_selected, player.current_timeline_frame)) {
                            player.audio_dirty = true;
                            project_dirty = true;
                        }
                    }
                    if (ImGui::Button("Remove Clip", ImVec2(clip_btn_w, 0))) {
                        if (player.previewing_library) {
                            player.stop_library_preview();
                        }
                        if (player.remove_source(library_selected)) {
                            project_dirty = true;
                            if (player.sources.empty()) {
                                library_selected = -1;
                            } else {
                                library_selected = std::clamp(library_selected, 0, (int)player.sources.size() - 1);
                            }
                        }
                    }
                }
                ImGui::EndChild();
                
                ImGui::SameLine();
                ImGui::SetCursorPos(ImVec2(pad + panel_w + pad, pad));
                ImGui::BeginChild("##PreviewPanel",
                                  ImVec2(viewport->Size.x - panel_w - 3 * pad, panel_h), false);
                ImVec2 preview_size = ImGui::GetContentRegionAvail();
                if (player.loaded) {
                    float scale_x = preview_size.x / player.width;
                    float scale_y = preview_size.y / player.height;
                    float img_scale = std::min(scale_x, scale_y);
                    float img_w = player.width * img_scale;
                    float img_h = player.height * img_scale;
                    ImVec2 img_pos((preview_size.x - img_w) * 0.5f, (preview_size.y - img_h) * 0.5f);
                    ImGui::SetCursorPos(img_pos);
                    
                    // Check if we're on a gap clip with no connected clip covering it
                    bool on_gap = player.is_timeline_frame_on_gap(player.current_timeline_frame);
                    bool has_connected = player.connected_clip_at_timeline_frame(player.current_timeline_frame) >= 0;
                    if (on_gap && !has_connected) {
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        ImVec2 window_pos = ImGui::GetWindowPos();
                        ImVec2 rect_min(window_pos.x + img_pos.x, window_pos.y + img_pos.y);
                        ImVec2 rect_max(window_pos.x + img_pos.x + img_w, window_pos.y + img_pos.y + img_h);
                        draw_list->AddRectFilled(rect_min, rect_max, IM_COL32(0, 0, 0, 255));
                        ImGui::Dummy(ImVec2(img_w, img_h));  // Required after SetCursorPos
                    } else {
                        ImGui::Image((ImTextureID)(intptr_t)player.texture_id, ImVec2(img_w, img_h));
                    }
                    
                    if (player.at_timeline_end()) {
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        ImVec2 window_pos = ImGui::GetWindowPos();
                        
                        float overlay_width = img_w * 0.12f;
                        ImVec2 overlay_min(window_pos.x + img_pos.x + img_w - overlay_width,
                                           window_pos.y + img_pos.y);
                        ImVec2 overlay_max(window_pos.x + img_pos.x + img_w,
                                           window_pos.y + img_pos.y + img_h);
                        draw_list->AddRectFilled(overlay_min, overlay_max, IM_COL32(50, 50, 55, 180));
                        
                        const char* end_text = "END";
                        ImVec2 text_size = ImGui::CalcTextSize(end_text);
                        float text_x = window_pos.x + img_pos.x + img_w - overlay_width / 2 - text_size.x / 2;
                        float text_y = window_pos.y + img_pos.y + img_h / 2 - text_size.y / 2;
                        draw_list->AddText(ImVec2(text_x, text_y), IM_COL32(255, 255, 255, 220), end_text);
                    }
                } else {
                    ImGui::Text("No media loaded. Add a clip to get started.");
                }
                ImGui::EndChild();
                
                // Controls
                ImGui::SetCursorPos(ImVec2(10, viewport->Size.y - controls_height + 5));
                
                if (ImGui::Button(player.playing ? "Pause" : "Play", ImVec2(60 * ui_scale, 0))) {
                    if (player.previewing_library) {
                        player.stop_library_preview();
                    }
                    player.toggle_play();
                }
                
                ImGui::SameLine();
                int64_t total_timeline_frames = player.total_timeline_frames();
                int64_t display_timeline_frame = std::clamp<int64_t>(
                    player.current_timeline_frame, 0, std::max<int64_t>(0, total_timeline_frames - 1));
                int display_source_id = player.active_source;
                int64_t display_source_frame = player.timeline_to_source_frame(display_timeline_frame, &display_source_id);
                ImGui::Text("Frame %lld / %lld (src: %d, clips: %zu, total: %lld fr)", 
                    (long long)display_source_frame,
                    (long long)player.total_frames,
                    display_source_id,
                    player.clips.size(),
                    (long long)total_timeline_frames);
                
                // Spacebar to play/pause
                if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                    if (player.previewing_library) {
                        player.stop_library_preview();
                    } else if (player.playing) {
                        player.pause();
                    } else {
                        player.play();
                    }
                }
                
                // 'B' key to split clip at playhead
                if (ImGui::IsKeyPressed(ImGuiKey_B) && !player.clips.empty()) {
                    player.split_at(player.current_time);
                    player.audio_dirty = true;
                    project_dirty = true;
                }
                
                // 'W' key to insert selected library clip at playhead
                if (ImGui::IsKeyPressed(ImGuiKey_W) && library_selected >= 0) {
                    if (player.insert_clip_at_timeline(library_selected, player.current_timeline_frame)) {
                        player.audio_dirty = true;
                        project_dirty = true;
                    }
                }
                
                // 'Q' key to connect selected library clip at playhead (above timeline)
                if (ImGui::IsKeyPressed(ImGuiKey_Q) && library_selected >= 0) {
                    if (player.connect_clip_at_timeline(library_selected, player.current_timeline_frame, 1)) {
                        project_dirty = true;
                    }
                }
                
                // Backspace to delete selected timeline clip or connected clip
                if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                    int remove_idx = timeline_state.selected_clip;
                    int remove_connected_idx = timeline_state.selected_connected_clip;
                    
                    // Prefer connected clip if selected, otherwise main clip
                    if (remove_connected_idx >= 0 && remove_connected_idx < (int)player.connected_clips.size()) {
                        player.connected_clips.erase(player.connected_clips.begin() + remove_connected_idx);
                        timeline_state.selected_connected_clip = -1;
                        timeline_state.clips_modified = true;
                        project_dirty = true;
                    } else if (remove_idx >= 0 && remove_idx < (int)player.clips.size()) {
                        // Calculate deleted clip's timeline position before removal
                        int64_t deleted_timeline_start = 0;
                        for (int i = 0; i < remove_idx; i++) {
                            deleted_timeline_start += player.clips[i].frame_count();
                        }
                        int64_t deleted_frames = player.clips[remove_idx].frame_count();
                        
                        player.clips.erase(player.clips.begin() + remove_idx);
                        
                        // Shift connected clips that were after the deleted clip
                        for (auto& cc : player.connected_clips) {
                            if (cc.connection_frame >= deleted_timeline_start + deleted_frames) {
                                cc.connection_frame -= deleted_frames;
                            }
                        }
                        
                        if (player.clips.empty()) {
                            timeline_state.selected_clip = -1;
                            player.current_timeline_frame = 0;
                            player.pause();
                        } else {
                            timeline_state.selected_clip = std::clamp(remove_idx, 0, (int)player.clips.size() - 1);
                            int64_t max_timeline = player.total_timeline_frames();
                            player.current_timeline_frame = std::clamp<int64_t>(player.current_timeline_frame, 0, max_timeline);
                            player.pause();
                            player.seek_to_timeline_frame(player.current_timeline_frame);
                        }
                        timeline_state.clips_modified = true;
                        player.audio_dirty = true;
                        project_dirty = true;
                    }
                }
                
                // Frame-based arrow key navigation (simple and precise)
                // Must handle case where current_frame is outside all clips (e.g., after resizing)
                
                // Frame-based arrow key navigation using timeline order (works after reordering)
                if (!player.clips.empty()) {
                    int64_t total_timeline = player.total_timeline_frames();
                    int64_t timeline_frame = std::clamp(player.current_timeline_frame, (int64_t)0, total_timeline);
                    
                    // Left arrow: previous timeline frame
                    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                        player.pause();
                        if (timeline_frame > 0) timeline_frame--;
                        player.seek_to_timeline_frame(timeline_frame);
                    }
                    
                    // Right arrow: next timeline frame (allows end position)
                    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                        player.pause();
                        if (timeline_frame < total_timeline) timeline_frame++;
                        player.seek_to_timeline_frame(timeline_frame);
                    }
                    
                    // Up/Down arrows: navigate between clip boundaries in timeline order
                    std::vector<int64_t> boundaries;
                    boundaries.reserve(player.clips.size() + 2);
                    int64_t tpos = 0;
                    boundaries.push_back(0);
                    for (const auto& c : player.clips) {
                        boundaries.push_back(tpos);
                        tpos += c.frame_count();
                    }
                    boundaries.push_back(total_timeline);
                    std::sort(boundaries.begin(), boundaries.end());
                    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
                    
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !boundaries.empty()) {
                        player.pause();
                        int64_t target = boundaries.front();
                        for (int64_t b : boundaries) {
                            if (b < timeline_frame) {
                                target = b;
                            }
                        }
                        player.seek_to_timeline_frame(target);
                    }
                    
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !boundaries.empty()) {
                        player.pause();
                        int64_t target = boundaries.back();
                        for (int64_t b : boundaries) {
                            if (b > timeline_frame) {
                                target = b;
                                break;
                            }
                        }
                        player.seek_to_timeline_frame(target);
                    }
                }
                
                ImGui::Dummy(ImVec2(0, 2 * ui_scale));
                
                // Timeline with clips
                ImGui::SetCursorPos(ImVec2(10, viewport->Size.y - controls_height + 45 * ui_scale));
                int64_t curr_frame = player.current_frame;
                int curr_source_id = player.active_source;
                int64_t curr_timeline_frame = player.current_timeline_frame;
                int64_t max_source_frames = 0;
                for (const auto& src : player.sources) {
                    max_source_frames = std::max(max_source_frames, src.total_frames);
                }
                
                static int64_t last_seek_frame = -1;
                
                if (ClipsTimeline("##clips_timeline", &curr_frame, &curr_source_id, &curr_timeline_frame, player.clips, player.connected_clips, max_source_frames, player.fps,
                                 ImVec2(viewport->Size.x - 20, 150 * ui_scale), timeline_state)) {
                    if (curr_timeline_frame != last_seek_frame) {
                        last_seek_frame = curr_timeline_frame;
                        player.current_timeline_frame = curr_timeline_frame;
                        player.pause();
                        player.seek_to_timeline_frame(curr_timeline_frame);
                    }
                }
                if (player.previewing_library && (timeline_state.dragging != 0 || timeline_state.pending_click)) {
                    player.stop_library_preview();
                }
                if (timeline_state.pending_library_source >= 0) {
                    if (player.insert_clip_at_timeline(timeline_state.pending_library_source,
                                                      timeline_state.pending_library_timeline)) {
                        player.audio_dirty = true;
                        project_dirty = true;
                    }
                    timeline_state.pending_library_source = -1;
                    timeline_state.pending_library_timeline = -1;
                }
                if (timeline_state.clips_modified) {
                    player.audio_dirty = true;
                    project_dirty = true;
                    timeline_state.clips_modified = false;
                }
                if (player.audio_dirty) {
                    player.rebuild_timeline_audio();
                    player.audio_dirty = false;
                }
                
                // Reset seek target tracking when drag ends
                if (timeline_state.dragging == 0) {
                    last_seek_frame = -1;
                }
                
                ImGui::End();
            } else {
                ImVec2 center = viewport->GetCenter();
                ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(0, 0));

                ImGui::Begin("##MainWindow", nullptr, 
                    ImGuiWindowFlags_NoTitleBar | 
                    ImGuiWindowFlags_NoResize | 
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoBackground |
                    ImGuiWindowFlags_AlwaysAutoResize);

                float scale = main_window.scale;
                if (ImGui::Button("New Empty Project", ImVec2(200 * scale, 40 * scale))) {
                    player.close();
                    project_open = true;
                    project_dirty = true;
                    project_path.clear();
                    library_selected = -1;
                }
                ImGui::SameLine();
                if (ImGui::Button("Open Project", ImVec2(200 * scale, 40 * scale))) {
                    if (!browser_window.window) {
                        browser_mode = BrowserMode::OpenProject;
                        open_browser = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("New Project From Video", ImVec2(240 * scale, 40 * scale))) {
                    if (!browser_window.window) {
                        browser_mode = BrowserMode::NewProjectFromVideo;
                        open_browser = true;
                    }
                }

                ImGui::End();
            }
            if (show_exit_confirm) {
                ImGui::EndDisabled();
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                ImVec2 p = viewport->Pos;
                ImVec2 s = viewport->Size;
                fg->AddRectFilled(p, ImVec2(p.x + s.x, p.y + s.y), IM_COL32(0, 0, 0, 120));
            }

        });

        if (open_browser) {
            int bw = (int)(450 * main_window.scale);
            int bh = (int)(550 * main_window.scale);
            const char* title = "Select File";
            if (browser_mode == BrowserMode::OpenProject) title = "Open Project";
            if (browser_mode == BrowserMode::SaveProject) title = "Save Project";
            if (browser_mode == BrowserMode::AddLibrary) title = "Add To Library";
            if (browser_mode == BrowserMode::NewProjectFromVideo) title = "New Project From Video";
            if (browser_mode == BrowserMode::SaveProject) {
                bh = (int)(620 * main_window.scale);
            }
            browser_window = create_window(title, bw, bh, main_window.window);
            browser.refresh();
        }
        if (show_exit_confirm && !confirm_window.window) {
            int cw = (int)(360 * main_window.scale);
            int ch = (int)(160 * main_window.scale);
            confirm_window = create_window("Unsaved Changes", cw, ch, main_window.window);
        }

        if (browser_window.window) {
            bool should_close = false;
            
            render_window(browser_window, [&]() {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

                ImGui::Begin("##FileBrowser", nullptr,
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse);

                ImGui::Text("Path: %s", browser.current_path.string().c_str());
                ImGui::Separator();

                float button_area_height = 40 * browser_window.scale;
                float input_height = (browser_mode == BrowserMode::SaveProject) ? (36 * browser_window.scale) : 0.0f;
                float pad_y = ImGui::GetStyle().ItemSpacing.y * 2.0f;
                float list_height = ImGui::GetContentRegionAvail().y - button_area_height - input_height - pad_y;
                if (list_height < 100.0f * browser_window.scale) {
                    list_height = 100.0f * browser_window.scale;
                }
                
                ImGui::BeginChild("##file_list", ImVec2(0, list_height), true);
                if (ImGui::BeginListBox("##files", ImVec2(-1, -1))) {
                    bool first_is_parent = !browser.entries.empty() && 
                        browser.entries[0].path() != browser.current_path &&
                        browser.current_path.string().starts_with(browser.entries[0].path().string());
                    
                    for (int i = 0; i < (int)browser.entries.size(); i++) {
                        auto& entry = browser.entries[i];
                        std::string label;
                        
                        try {
                            if (first_is_parent && i == 0) {
                                label = "..";
                            } else if (entry.is_directory()) {
                                label = "[" + path_display_name(entry.path()) + "]";
                            } else {
                                label = path_display_name(entry.path());
                            }
                        } catch (...) {
                            label = "???";
                        }
                        
                        bool is_selected = (browser.selected_index == i);
                        if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                            browser.selected_index = i;
                            
                            if (ImGui::IsMouseDoubleClicked(0)) {
                                try {
                                    if (entry.is_directory()) {
                                        browser.navigate_to(entry.path());
                                    } else {
                                        if (browser_mode == BrowserMode::AddLibrary) {
                                            if (player.add_source(entry.path().string()) >= 0) {
                                                project_dirty = true;
                                            }
                                        } else if (browser_mode == BrowserMode::NewProjectFromVideo) {
                                            pending_new_project_media = entry.path().string();
                                        } else if (browser_mode == BrowserMode::OpenProject) {
                                            pending_project_load = entry.path().string();
                                        } else if (browser_mode == BrowserMode::SaveProject) {
                                            save_filename = entry.path().filename().string();
                                            pending_project_save = entry.path().string();
                                        }
                                        should_close = true;
                                    }
                                } catch (...) {}
                            }
                        }
                    }
                    ImGui::EndListBox();
                }
                ImGui::EndChild();

                if (browser_mode == BrowserMode::SaveProject) {
                    ImGui::Text("File name:");
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    input_text_string("##save_name", save_filename);
                }

                ImGui::Spacing();
                
                bool has_selection = browser.selected_index >= 0 &&
                                    browser.selected_index < (int)browser.entries.size();
                bool can_open = (browser_mode == BrowserMode::SaveProject) ? true : has_selection;
                
                if (!can_open) ImGui::BeginDisabled();
                if (ImGui::Button(browser_mode == BrowserMode::SaveProject ? "Save" : "Open",
                                  ImVec2(80 * browser_window.scale, 0))) {
                    try {
                        if (browser_mode == BrowserMode::SaveProject) {
                            fs::path target_dir = browser.current_path;
                            if (has_selection) {
                                auto& entry = browser.entries[browser.selected_index];
                                if (entry.is_directory()) {
                                    target_dir = entry.path();
                                } else {
                                    target_dir = entry.path().parent_path();
                                    if (save_filename.empty()) {
                                        save_filename = entry.path().filename().string();
                                    }
                                }
                            }
                            std::string filename = save_filename.empty() ? "project.vibecut" : save_filename;
                            pending_project_save = (target_dir / filename).string();
                            should_close = true;
                        } else if (has_selection) {
                            auto& entry = browser.entries[browser.selected_index];
                            if (entry.is_directory()) {
                                browser.navigate_to(entry.path());
                            } else {
                                if (browser_mode == BrowserMode::AddLibrary) {
                                    if (player.add_source(entry.path().string()) >= 0) {
                                        project_dirty = true;
                                    }
                                } else if (browser_mode == BrowserMode::NewProjectFromVideo) {
                                    pending_new_project_media = entry.path().string();
                                } else if (browser_mode == BrowserMode::OpenProject) {
                                    pending_project_load = entry.path().string();
                                }
                                should_close = true;
                            }
                        }
                    } catch (...) {}
                }
                if (!can_open) ImGui::EndDisabled();
                
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(80 * browser_window.scale, 0))) {
                    should_close = true;
                    if (browser_mode == BrowserMode::SaveProject) {
                        exit_after_save = false;
                    }
                }

                ImGui::End();
            });
            
            if (should_close) {
                destroy_window(browser_window);
            }
        }
        if (confirm_window.window) {
            bool close_confirm = false;
            render_window(confirm_window, [&]() {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
                ImGui::Begin("##ConfirmExit", nullptr,
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse);
                
                ImGui::Text("You have unsaved changes.");
                ImGui::Separator();
                float w = ImGui::GetContentRegionAvail().x;
                if (ImGui::Button("Save and Exit", ImVec2(w, 0))) {
                    if (!project_path.empty()) {
                        pending_project_save = project_path;
                        exit_after_save = true;
                    } else if (!browser_window.window) {
                        save_filename = "project.vibecut";
                        browser_mode = BrowserMode::SaveProject;
                        open_browser = true;
                        exit_after_save = true;
                    }
                    show_exit_confirm = false;
                    close_confirm = true;
                }
                if (ImGui::Button("Exit Without Saving", ImVec2(w, 0))) {
                    should_exit = true;
                    show_exit_confirm = false;
                    close_confirm = true;
                }
                if (ImGui::Button("Cancel", ImVec2(w, 0))) {
                    show_exit_confirm = false;
                    close_confirm = true;
                }
                ImGui::End();
            });
            if (close_confirm) {
                destroy_window(confirm_window);
            }
        }
    }

    if (export_thread.joinable()) {
        exporting = false;
        export_thread.join();
    }
    
    player.close();
    if (browser_window.window) {
        destroy_window(browser_window);
    }
    destroy_window(main_window);
    glfwTerminate();

    return 0;
}
