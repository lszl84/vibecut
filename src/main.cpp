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

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include "embedded_font.h"

namespace fs = std::filesystem;

// Represents a segment of video on the timeline
struct Clip {
    double source_start;  // Start time in source video
    double source_end;    // End time in source video
    
    double duration() const { return source_end - source_start; }
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
    double framerate = 30.0;
    double time_base = 0.0;
    int64_t stream_start_pts = 0;  // Stream's start timestamp offset
    double first_keyframe_time = 0.0;  // Time of first keyframe (for seek threshold)
    
    GLuint texture_id = 0;
    double current_time = 0.0;
    double play_start_time = 0.0;
    double play_start_position = 0.0;
    bool playing = false;
    bool loaded = false;
    
    std::vector<Clip> clips;  // Timeline clips
    int active_clip = 0;      // Currently selected/playing clip
    
    std::string source_path;
    
    bool open(const std::string& path) {
        close();
        source_path = path;
        
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
            close();
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
            close();
            return false;
        }
        
        AVStream* stream = format_ctx->streams[video_stream];
        AVCodecParameters* codecpar = stream->codecpar;
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            std::fprintf(stderr, "Codec not found\n");
            close();
            return false;
        }
        
        codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx, codecpar);
        
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::fprintf(stderr, "Could not open codec\n");
            close();
            return false;
        }
        
        width = codec_ctx->width;
        height = codec_ctx->height;
        time_base = av_q2d(stream->time_base);
        duration = format_ctx->duration / (double)AV_TIME_BASE;
        stream_start_pts = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
        
        if (stream->avg_frame_rate.num && stream->avg_frame_rate.den) {
            framerate = av_q2d(stream->avg_frame_rate);
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
        
        glGenTextures(1, &texture_id);
        glBindTexture(GL_TEXTURE_2D, texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        
        loaded = true;
        current_time = 0.0;
        playing = false;
        
        // Initialize with single clip covering entire video
        clips.clear();
        clips.push_back({0.0, duration});
        active_clip = 0;
        
        // Detect first keyframe position by doing an initial decode
        decode_frame();
        first_keyframe_time = current_time;
        
        return true;
    }
    
    void close() {
        playing = false;
        loaded = false;
        
        if (texture_id) { glDeleteTextures(1, &texture_id); texture_id = 0; }
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
        stream_start_pts = 0;
        first_keyframe_time = 0.0;
        clips.clear();
        active_clip = 0;
        source_path.clear();
    }
    
    bool decode_frame() {
        while (av_read_frame(format_ctx, packet) >= 0) {
            if (packet->stream_index == video_stream) {
                if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                    if (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                                  frame_rgb->data, frame_rgb->linesize);
                        
                        // Subtract stream start offset to get time relative to 0
                        current_time = (frame->pts - stream_start_pts) * time_base;
                        
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
    
    void seek(double time) {
        if (!loaded) return;
        
        time = std::clamp(time, 0.0, duration);
        int64_t target_ts = (int64_t)(time / time_base);
        
        // For positions before the first keyframe, seek from start
        // For other positions, use fast keyframe-based seeking
        if (time <= first_keyframe_time + 0.1) {
            // Seek to very beginning, then decode forward
            avformat_seek_file(format_ctx, video_stream, 0, 0, target_ts, 0);
        } else {
            // Seek backward to nearest keyframe before target (fast)
            avformat_seek_file(format_ctx, video_stream, 0, target_ts, target_ts, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(codec_ctx);
        
        // Decode forward to reach the exact target frame
        while (decode_frame()) {
            if (current_time >= time - 0.01) break;
        }
        
        if (playing) {
            play_start_time = glfwGetTime();
            play_start_position = current_time;
        }
    }
    
    // Calculate total timeline duration (sum of all clips)
    double total_duration() const {
        double total = 0.0;
        for (const auto& clip : clips) total += clip.duration();
        return total;
    }
    
    // Find which clip index contains a given source time, returns -1 if not in any clip
    int clip_at_source_time(double source_time) const {
        for (int i = 0; i < (int)clips.size(); i++) {
            if (source_time >= clips[i].source_start && source_time < clips[i].source_end) {
                return i;
            }
        }
        return -1;
    }
    
    // Split the clip at the given source time
    void split_at(double source_time) {
        int idx = clip_at_source_time(source_time);
        if (idx < 0) return;
        
        Clip& clip = clips[idx];
        // Don't split if too close to edges
        if (source_time - clip.source_start < 0.1 || clip.source_end - source_time < 0.1) return;
        
        Clip new_clip = {source_time, clip.source_end};
        clip.source_end = source_time;
        clips.insert(clips.begin() + idx + 1, new_clip);
    }
    
    void play() {
        if (!loaded || clips.empty()) return;
        playing = true;
        play_start_time = glfwGetTime();
        play_start_position = current_time;
    }
    
    void pause() {
        playing = false;
    }
    
    void toggle_play() {
        if (playing) pause();
        else play();
    }
    
    void update() {
        if (!loaded || !playing || clips.empty()) return;
        
        double target_time = play_start_position + (glfwGetTime() - play_start_time);
        
        // Find current clip
        int clip_idx = clip_at_source_time(current_time);
        if (clip_idx < 0) clip_idx = 0;
        
        // Check if we've reached end of current clip
        if (target_time >= clips[clip_idx].source_end) {
            // Move to next clip or stop
            if (clip_idx + 1 < (int)clips.size()) {
                seek(clips[clip_idx + 1].source_start);
                play_start_time = glfwGetTime();
                play_start_position = current_time;
                return;
            } else {
                // End of all clips
                seek(clips[0].source_start);
                pause();
                return;
            }
        }
        
        while (current_time < target_time - 0.5 / framerate) {
            if (!decode_frame()) {
                pause();
                return;
            }
        }
    }
    
    std::string format_time(double t) {
        int minutes = (int)(t / 60);
        int seconds = (int)t % 60;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%02d", minutes, seconds);
        return buf;
    }
    
    double trimmed_duration() const {
        return total_duration();
    }
};

// Export multiple clips concatenated together
bool export_clips(const std::string& input, const std::string& output, const std::vector<Clip>& clips, std::atomic<bool>& exporting, std::atomic<float>& progress) {
    if (clips.empty()) {
        exporting = false;
        return false;
    }
    
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
    
    if (avformat_write_header(out_ctx, nullptr) < 0) {
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        av_free(stream_mapping);
        avformat_free_context(out_ctx);
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    // Calculate total duration for progress
    double total_duration = 0.0;
    for (const auto& clip : clips) total_duration += clip.duration();
    
    AVPacket* pkt = av_packet_alloc();
    double time_offset = 0.0;  // Cumulative offset for output timestamps
    double processed_time = 0.0;
    
    for (const auto& clip : clips) {
        if (!exporting) break;
        
        // Seek to clip start
        int64_t start_ts = (int64_t)(clip.source_start * AV_TIME_BASE);
        av_seek_frame(in_ctx, -1, start_ts, AVSEEK_FLAG_BACKWARD);
        
        // Process packets for this clip
        while (av_read_frame(in_ctx, pkt) >= 0 && exporting) {
            if (stream_mapping[pkt->stream_index] < 0) {
                av_packet_unref(pkt);
                continue;
            }
            
            AVStream* in_stream = in_ctx->streams[pkt->stream_index];
            double pkt_time = pkt->pts * av_q2d(in_stream->time_base);
            
            if (pkt_time < clip.source_start) {
                av_packet_unref(pkt);
                continue;
            }
            
            if (pkt_time > clip.source_end) {
                av_packet_unref(pkt);
                break;
            }
            
            progress = (float)((processed_time + (pkt_time - clip.source_start)) / total_duration);
            
            AVStream* out_stream = out_ctx->streams[stream_mapping[pkt->stream_index]];
            
            // Adjust timestamps: subtract clip start, add cumulative offset
            double adjusted_time = (pkt_time - clip.source_start) + time_offset;
            int64_t new_pts = (int64_t)(adjusted_time / av_q2d(out_stream->time_base));
            
            pkt->pts = new_pts;
            pkt->dts = new_pts;  // For stream copy, dts = pts usually works
            pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
            pkt->stream_index = stream_mapping[pkt->stream_index];
            pkt->pos = -1;
            
            av_interleaved_write_frame(out_ctx, pkt);
            av_packet_unref(pkt);
        }
        
        processed_time += clip.duration();
        time_offset += clip.duration();
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(RobotoMedium_compressed_data_base85, 16.0f * data.scale);
    
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(data.scale);
    
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
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    glfwSwapBuffers(data.window);
}

struct TrimTimelineState {
    int dragging = 0; // 0=none, 1=left, 2=right, 3=playhead
};

struct ClipsTimelineState {
    int dragging = 0;       // 0=none, 1=left handle, 2=right handle, 3=playhead
    int dragging_clip = -1; // Which clip's handle we're dragging
};

// Timeline widget that shows multiple clips with individual trim handles
// Returns true if anything changed
bool ClipsTimeline(const char* label, float* current, std::vector<Clip>& clips, float source_duration, const ImVec2& size, ClipsTimelineState& state) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    ImVec2 bb_min = pos;
    ImVec2 bb_max = ImVec2(pos.x + size.x, pos.y + size.y);
    
    // Background (dark, represents excluded areas)
    draw_list->AddRectFilled(bb_min, bb_max, IM_COL32(30, 30, 35, 255), 4.0f);
    
    float handle_w = 6.0f;
    
    // Draw each clip
    for (int i = 0; i < (int)clips.size(); i++) {
        const Clip& clip = clips[i];
        float start_x = bb_min.x + (clip.source_start / source_duration) * size.x;
        float end_x = bb_min.x + (clip.source_end / source_duration) * size.x;
        
        // Clip region (alternating colors for visibility)
        ImU32 clip_color = (i % 2 == 0) ? IM_COL32(70, 130, 180, 200) : IM_COL32(100, 149, 237, 200);
        draw_list->AddRectFilled(ImVec2(start_x, bb_min.y), ImVec2(end_x, bb_max.y), clip_color, 2.0f);
        
        // Left handle (trim start of this clip)
        draw_list->AddRectFilled(ImVec2(start_x, bb_min.y), ImVec2(start_x + handle_w, bb_max.y), IM_COL32(255, 200, 50, 255));
        
        // Right handle (trim end of this clip)
        draw_list->AddRectFilled(ImVec2(end_x - handle_w, bb_min.y), ImVec2(end_x, bb_max.y), IM_COL32(255, 200, 50, 255));
        
        // Draw split line between clips (except for last clip)
        if (i < (int)clips.size() - 1) {
            draw_list->AddLine(ImVec2(end_x, bb_min.y), ImVec2(end_x, bb_max.y), IM_COL32(255, 255, 255, 100), 2.0f);
        }
    }
    
    // Current position (playhead)
    float curr_x = bb_min.x + (*current / source_duration) * size.x;
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
        state.dragging = 0;
        state.dragging_clip = -1;
        
        // Check each clip's handles
        for (int i = 0; i < (int)clips.size(); i++) {
            const Clip& clip = clips[i];
            float start_x = bb_min.x + (clip.source_start / source_duration) * size.x;
            float end_x = bb_min.x + (clip.source_end / source_duration) * size.x;
            
            bool on_left = (click_x >= start_x && click_x <= start_x + handle_w + 4);
            bool on_right = (click_x >= end_x - handle_w - 4 && click_x <= end_x);
            
            if (on_left && on_right) {
                // Both handles overlapping - use closer one
                if (std::abs(click_x - start_x) < std::abs(click_x - end_x)) {
                    state.dragging = 1;
                } else {
                    state.dragging = 2;
                }
                state.dragging_clip = i;
                break;
            } else if (on_left) {
                state.dragging = 1;
                state.dragging_clip = i;
                break;
            } else if (on_right) {
                state.dragging = 2;
                state.dragging_clip = i;
                break;
            }
        }
        
        // If didn't hit any handle, move playhead
        if (state.dragging == 0) {
            state.dragging = 3;
        }
    }
    
    if (!is_active) {
        state.dragging = 0;
        state.dragging_clip = -1;
    }
    
    if (state.dragging != 0 && is_active) {
        float rel_x = std::clamp((mouse.x - bb_min.x) / size.x, 0.0f, 1.0f);
        float time = rel_x * source_duration;
        
        if (state.dragging == 3) {
            // Moving playhead
            *current = std::clamp(time, 0.0f, source_duration);
            changed = true;
        } else if (state.dragging_clip >= 0 && state.dragging_clip < (int)clips.size()) {
            Clip& clip = clips[state.dragging_clip];
            
            if (state.dragging == 1) {
                // Left handle - adjust source_start
                float min_start = (state.dragging_clip > 0) ? clips[state.dragging_clip - 1].source_end : 0.0f;
                float max_start = clip.source_end - 0.1f;
                clip.source_start = std::clamp((double)time, (double)min_start, (double)max_start);
                changed = true;
            } else if (state.dragging == 2) {
                // Right handle - adjust source_end
                float min_end = clip.source_start + 0.1f;
                float max_end = (state.dragging_clip < (int)clips.size() - 1) ? clips[state.dragging_clip + 1].source_start : source_duration;
                clip.source_end = std::clamp((double)time, (double)min_end, (double)max_end);
                changed = true;
            }
        }
    }
    
    // Change cursor when hovering handles
    if (is_hovered && state.dragging == 0) {
        float hover_x = mouse.x;
        for (const Clip& clip : clips) {
            float start_x = bb_min.x + (clip.source_start / source_duration) * size.x;
            float end_x = bb_min.x + (clip.source_end / source_duration) * size.x;
            
            bool over_left = (hover_x >= start_x && hover_x <= start_x + handle_w + 4);
            bool over_right = (hover_x >= end_x - handle_w - 4 && hover_x <= end_x);
            if (over_left || over_right) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                break;
            }
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

    WindowData main_window = create_window("VibeCut", 1280, 720);
    if (!main_window.window) {
        std::fprintf(stderr, "Failed to create main window\n");
        glfwTerminate();
        return 1;
    }

    WindowData browser_window{};
    FileBrowser browser;
    std::string selected_file;
    VideoPlayer player;
    std::string pending_load;
    ClipsTimelineState timeline_state;
    
    std::atomic<bool> exporting{false};
    std::atomic<float> export_progress{0.0f};
    std::thread export_thread;

    while (!glfwWindowShouldClose(main_window.window)) {
        glfwPollEvents();

        if (browser_window.window && glfwWindowShouldClose(browser_window.window)) {
            destroy_window(browser_window);
        }

        if (!pending_load.empty()) {
            glfwMakeContextCurrent(main_window.window);
            if (player.open(pending_load)) {
                std::printf("Loaded video: %dx%d, %.1f fps, %.1f sec\n", 
                    player.width, player.height, player.framerate, player.duration);
            }
            pending_load.clear();
        }

        glfwMakeContextCurrent(main_window.window);
        player.update();

        bool open_browser = false;
        render_window(main_window, [&]() {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            
            if (player.loaded) {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(viewport->Size);
                ImGui::Begin("##VideoView", nullptr,
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoBackground |
                    ImGuiWindowFlags_NoScrollbar);
                
                float controls_height = 130;
                float scale_x = viewport->Size.x / player.width;
                float scale_y = (viewport->Size.y - controls_height) / player.height;
                float scale = std::min(scale_x, scale_y);
                
                float img_w = player.width * scale;
                float img_h = player.height * scale;
                float img_x = (viewport->Size.x - img_w) / 2;
                float img_y = (viewport->Size.y - controls_height - img_h) / 2;
                
                ImGui::SetCursorPos(ImVec2(img_x, img_y));
                ImGui::Image((ImTextureID)(intptr_t)player.texture_id, ImVec2(img_w, img_h));
                
                // Controls
                ImGui::SetCursorPos(ImVec2(10, viewport->Size.y - controls_height + 5));
                
                if (ImGui::Button(player.playing ? "Pause" : "Play", ImVec2(80, 30))) {
                    player.toggle_play();
                }
                
                ImGui::SameLine();
                ImGui::Text("%s / %s (clips: %zu, total: %s)", 
                    player.format_time(player.current_time).c_str(),
                    player.format_time(player.duration).c_str(),
                    player.clips.size(),
                    player.format_time(player.trimmed_duration()).c_str());
                
                // 'B' key to split clip at playhead
                if (ImGui::IsKeyPressed(ImGuiKey_B) && !player.clips.empty()) {
                    player.split_at(player.current_time);
                }
                
                // Timeline with clips
                ImGui::SetCursorPos(ImVec2(10, viewport->Size.y - controls_height + 45));
                float curr = (float)player.current_time;
                
                static float last_seek_target = -1.0f;
                
                if (ClipsTimeline("##clips_timeline", &curr, player.clips, (float)player.duration, 
                                 ImVec2(viewport->Size.x - 20, 30), timeline_state)) {
                    // Only seek if playhead moved
                    float target_diff = std::abs(curr - last_seek_target);
                    if (target_diff > 0.001f && timeline_state.dragging == 3) {
                        last_seek_target = curr;
                        player.pause();
                        player.seek(curr);
                    }
                }
                
                // Reset seek target tracking when drag ends
                if (timeline_state.dragging == 0) {
                    last_seek_target = -1.0f;
                }
                
                // Bottom row
                ImGui::SetCursorPos(ImVec2(10, viewport->Size.y - 40));
                ImGui::Text("File: %s", path_display_name(fs::path(selected_file)).c_str());
                
                ImGui::SameLine(viewport->Size.x - 240);
                
                if (exporting) {
                    ImGui::ProgressBar(export_progress, ImVec2(120, 0), "Exporting...");
                } else {
                    if (ImGui::Button("Export", ImVec2(80, 0))) {
                        fs::path src(selected_file);
                        std::string out_name = src.stem().string() + "_edited" + src.extension().string();
                        std::string out_path = (src.parent_path() / out_name).string();
                        
                        exporting = true;
                        export_progress = 0.0f;
                        
                        // Copy clips for thread safety
                        std::vector<Clip> clips_copy = player.clips;
                        
                        if (export_thread.joinable()) export_thread.join();
                        export_thread = std::thread([&, out_path, clips_copy]() {
                            export_clips(player.source_path, out_path, clips_copy, exporting, export_progress);
                        });
                    }
                }
                
                ImGui::SameLine();
                if (ImGui::Button("Open File...", ImVec2(110, 0))) {
                    if (!browser_window.window) open_browser = true;
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

                if (ImGui::Button("Open File...", ImVec2(200, 60))) {
                    if (!browser_window.window) open_browser = true;
                }

                ImGui::End();
            }
        });

        if (open_browser) {
            browser_window = create_window("Open File", 600, 500, main_window.window);
            browser.refresh();
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
                    ImGuiWindowFlags_NoCollapse);

                ImGui::Text("Path: %s", browser.current_path.string().c_str());
                ImGui::Separator();

                ImVec2 list_size(ImGui::GetContentRegionAvail().x, 
                                 ImGui::GetContentRegionAvail().y - 40);
                
                if (ImGui::BeginListBox("##files", list_size)) {
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
                                        selected_file = entry.path().string();
                                        pending_load = selected_file;
                                        should_close = true;
                                    }
                                } catch (...) {}
                            }
                        }
                    }
                    ImGui::EndListBox();
                }

                ImGui::Spacing();
                
                bool can_open = browser.selected_index >= 0 && 
                               browser.selected_index < (int)browser.entries.size();
                try {
                    can_open = can_open && !browser.entries[browser.selected_index].is_directory();
                } catch (...) { can_open = false; }
                
                if (!can_open) ImGui::BeginDisabled();
                if (ImGui::Button("Open", ImVec2(100, 0))) {
                    try {
                        selected_file = browser.entries[browser.selected_index].path().string();
                        pending_load = selected_file;
                    } catch (...) {}
                    should_close = true;
                }
                if (!can_open) ImGui::EndDisabled();
                
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                    should_close = true;
                }

                ImGui::End();
            });
            
            if (should_close) {
                destroy_window(browser_window);
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
