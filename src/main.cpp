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
        
        // Detect first keyframe position by doing an initial decode
        decode_frame();
        first_keyframe_time = current_time;
        
        // Adjust duration to account for first frame offset (some videos have frames starting later)
        double actual_duration = duration - first_keyframe_time;
        if (actual_duration > 0.1) {
            duration = actual_duration;
            std::printf("Adjusted duration from %.3f to %.3f (first frame offset: %.3f)\n", 
                        duration + first_keyframe_time, duration, first_keyframe_time);
        }
        
        // Initialize with single clip covering entire video
        clips.clear();
        clips.push_back({0.0, duration});
        active_clip = 0;
        
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
        
        // Find which clip we're starting in
        active_clip = clip_at_source_time(current_time);
        if (active_clip < 0) {
            // Not in any clip - find the next clip after current position
            active_clip = 0;
            for (int i = 0; i < (int)clips.size(); i++) {
                if (clips[i].source_start >= current_time) {
                    active_clip = i;
                    seek(clips[i].source_start);
                    break;
                }
            }
        }
        
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
        if (active_clip < 0 || active_clip >= (int)clips.size()) active_clip = 0;
        
        double elapsed = glfwGetTime() - play_start_time;
        double target_time = play_start_position + elapsed;
        
        // Check if we've reached end of current clip
        const Clip& clip = clips[active_clip];
        if (current_time >= clip.source_end - 0.01 || target_time >= clip.source_end) {
            // Move to next clip or stop
            if (active_clip + 1 < (int)clips.size()) {
                active_clip++;
                seek(clips[active_clip].source_start);
                play_start_time = glfwGetTime();
                play_start_position = clips[active_clip].source_start;
                return;
            } else {
                // End of all clips - go back to start
                active_clip = 0;
                seek(clips[0].source_start);
                pause();
                return;
            }
        }
        
        // Decode frames to catch up to target time
        while (current_time < target_time - 0.5 / framerate) {
            if (!decode_frame()) {
                pause();
                return;
            }
            // Safety: don't decode past clip end
            if (current_time >= clip.source_end) break;
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

// Export multiple clips with re-encoding for clean transitions
bool export_clips(const std::string& input, const std::string& output, const std::vector<Clip>& clips, std::atomic<bool>& exporting, std::atomic<float>& progress) {
    if (clips.empty()) {
        exporting = false;
        return false;
    }
    
    AVFormatContext* in_ctx = nullptr;
    AVFormatContext* out_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    AVCodecContext* enc_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    int out_video_idx = -1;
    int out_audio_idx = -1;
    
    // Open input with ignore_editlist to prevent skipping initial frames
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
    
    // Find video and audio streams
    for (unsigned i = 0; i < in_ctx->nb_streams; i++) {
        if (in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0) {
            video_stream_idx = i;
        } else if (in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx < 0) {
            audio_stream_idx = i;
        }
    }
    
    if (video_stream_idx < 0) {
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    // Set up video decoder
    AVStream* in_video = in_ctx->streams[video_stream_idx];
    const AVCodec* decoder = avcodec_find_decoder(in_video->codecpar->codec_id);
    dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, in_video->codecpar);
    avcodec_open2(dec_ctx, decoder, nullptr);
    
    // Create output context
    if (avformat_alloc_output_context2(&out_ctx, nullptr, nullptr, output.c_str()) < 0) {
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    // Set up video encoder (H.264) - preserve source framerate
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) encoder = avcodec_find_encoder(in_video->codecpar->codec_id);
    
    AVStream* out_video = avformat_new_stream(out_ctx, nullptr);
    out_video_idx = out_video->index;
    
    // Get source framerate
    AVRational src_fps = av_guess_frame_rate(in_ctx, in_video, nullptr);
    if (src_fps.num == 0 || src_fps.den == 0) {
        src_fps = AVRational{30, 1};  // Fallback to 30fps
    }
    
    enc_ctx = avcodec_alloc_context3(encoder);
    enc_ctx->width = dec_ctx->width;
    enc_ctx->height = dec_ctx->height;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = AVRational{1, 1000};  // Millisecond precision
    enc_ctx->framerate = src_fps;
    enc_ctx->bit_rate = 8000000;  // 8 Mbps for better quality
    enc_ctx->gop_size = 12;  // Keyframe every 12 frames
    enc_ctx->max_b_frames = 0;  // Disable B-frames for simpler encoding
    
    if (out_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    
    AVDictionary* enc_opts = nullptr;
    av_dict_set(&enc_opts, "preset", "medium", 0);
    av_dict_set(&enc_opts, "crf", "18", 0);  // High quality
    avcodec_open2(enc_ctx, encoder, &enc_opts);
    av_dict_free(&enc_opts);
    
    avcodec_parameters_from_context(out_video->codecpar, enc_ctx);
    out_video->time_base = enc_ctx->time_base;
    
    // Copy audio stream (stream copy for audio is fine)
    if (audio_stream_idx >= 0) {
        AVStream* in_audio = in_ctx->streams[audio_stream_idx];
        AVStream* out_audio = avformat_new_stream(out_ctx, nullptr);
        out_audio_idx = out_audio->index;
        avcodec_parameters_copy(out_audio->codecpar, in_audio->codecpar);
        out_audio->codecpar->codec_tag = 0;
        out_audio->time_base = in_audio->time_base;
    }
    
    // Set up pixel format converter if needed
    sws_ctx = sws_getContext(
        dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    // Open output file
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_ctx->pb, output.c_str(), AVIO_FLAG_WRITE) < 0) {
            sws_freeContext(sws_ctx);
            avcodec_free_context(&enc_ctx);
            avcodec_free_context(&dec_ctx);
            avformat_free_context(out_ctx);
            avformat_close_input(&in_ctx);
            exporting = false;
            return false;
        }
    }
    
    if (avformat_write_header(out_ctx, nullptr) < 0) {
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        sws_freeContext(sws_ctx);
        avcodec_free_context(&enc_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_free_context(out_ctx);
        avformat_close_input(&in_ctx);
        exporting = false;
        return false;
    }
    
    // Calculate total duration
    double total_duration = 0.0;
    for (const auto& clip : clips) total_duration += clip.duration();
    
    AVPacket* pkt = av_packet_alloc();
    AVPacket* enc_pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* enc_frame = av_frame_alloc();
    
    enc_frame->format = enc_ctx->pix_fmt;
    enc_frame->width = enc_ctx->width;
    enc_frame->height = enc_ctx->height;
    av_frame_get_buffer(enc_frame, 0);
    
    double time_offset = 0.0;  // Accumulated output time from previous clips
    double processed_time = 0.0;
    int64_t video_pts = 0;  // Running PTS in milliseconds
    int64_t audio_pts = 0;  // Running audio PTS
    bool first_clip = true;
    
    for (const auto& clip : clips) {
        if (!exporting) break;
        
        std::printf("EXPORT: Processing clip [%.3f - %.3f], duration=%.3f\n", 
                    clip.source_start, clip.source_end, clip.duration());
        
        // Seek to clip start (skip seeking for first clip starting at 0 to preserve all frames)
        if (first_clip && clip.source_start < 0.1) {
            std::printf("EXPORT: Starting from beginning (no seek)\n");
        } else {
            avcodec_flush_buffers(dec_ctx);
            int64_t start_ts = (int64_t)(clip.source_start * AV_TIME_BASE);
            avformat_seek_file(in_ctx, -1, INT64_MIN, start_ts, start_ts, 0);
            avcodec_flush_buffers(dec_ctx);
            std::printf("EXPORT: Seeking to timestamp %.3f\n", clip.source_start);
        }
        first_clip = false;
        
        bool reached_clip = false;
        int frames_skipped = 0;
        int frames_encoded = 0;
        double first_frame_time = -1.0;  // Will be set when we get the first frame
        
        while (av_read_frame(in_ctx, pkt) >= 0 && exporting) {
            AVStream* in_stream = in_ctx->streams[pkt->stream_index];
            double pkt_time = pkt->pts * av_q2d(in_stream->time_base);
            
            if (pkt->stream_index == video_stream_idx) {
                // Decode frame
                avcodec_send_packet(dec_ctx, pkt);
                while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
                    double frame_time = frame->pts * av_q2d(in_stream->time_base);
                    
                    // Track the first frame we receive after seeking
                    if (first_frame_time < 0) {
                        first_frame_time = frame_time;
                        std::printf("EXPORT: First frame received at %.3f\n", first_frame_time);
                    }
                    
                    // Calculate relative time: how far into the clip this frame is
                    double relative_time = frame_time - first_frame_time;
                    
                    // Check if we've exceeded the clip duration
                    if (relative_time >= clip.duration()) {
                        std::printf("EXPORT: Frame at relative %.3f past clip duration %.3f, stopping\n", relative_time, clip.duration());
                        av_packet_unref(pkt);
                        goto done_with_clip;
                    }
                    
                    reached_clip = true;
                    frames_encoded++;
                    
                    // Calculate output time: relative time within clip + offset from previous clips
                    double output_time = relative_time + time_offset;
                    progress = (float)(output_time / total_duration);
                    
                    // Convert pixel format
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
                              enc_frame->data, enc_frame->linesize);
                    
                    // Use simple incrementing PTS in milliseconds (based on frame rate)
                    double frame_duration_ms = 1000.0 * enc_ctx->framerate.den / enc_ctx->framerate.num;
                    enc_frame->pts = video_pts;
                    video_pts += (int64_t)frame_duration_ms;
                    
                    // Encode frame
                    avcodec_send_frame(enc_ctx, enc_frame);
                    while (avcodec_receive_packet(enc_ctx, enc_pkt) >= 0) {
                        enc_pkt->stream_index = out_video_idx;
                        av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_ctx->streams[out_video_idx]->time_base);
                        av_interleaved_write_frame(out_ctx, enc_pkt);
                        av_packet_unref(enc_pkt);
                    }
                }
            } else if (pkt->stream_index == audio_stream_idx && out_audio_idx >= 0 && first_frame_time >= 0) {
                // Stream copy audio - use relative times based on first video frame
                double audio_relative_time = pkt_time - first_frame_time;
                
                if (audio_relative_time < -0.1 || audio_relative_time > clip.duration() + 0.1) {
                    av_packet_unref(pkt);
                    continue;
                }
                
                AVStream* out_audio = out_ctx->streams[out_audio_idx];
                AVStream* in_audio = in_ctx->streams[audio_stream_idx];
                
                // Keep original duration
                int64_t orig_duration = pkt->duration;
                
                // Calculate output PTS based on relative time + accumulated offset
                double audio_output_time = std::max(0.0, audio_relative_time) + time_offset;
                pkt->pts = (int64_t)(audio_output_time / av_q2d(out_audio->time_base));
                pkt->dts = pkt->pts;
                pkt->duration = av_rescale_q(orig_duration, in_audio->time_base, out_audio->time_base);
                pkt->stream_index = out_audio_idx;
                pkt->pos = -1;
                
                av_interleaved_write_frame(out_ctx, pkt);
            }
            
            av_packet_unref(pkt);
        }
        done_with_clip:
        
        std::printf("EXPORT: Clip done - skipped %d frames, encoded %d frames\n", frames_skipped, frames_encoded);
        processed_time += clip.duration();
        time_offset += clip.duration();
    }
    
    // Flush encoder
    avcodec_send_frame(enc_ctx, nullptr);
    while (avcodec_receive_packet(enc_ctx, enc_pkt) >= 0) {
        enc_pkt->stream_index = out_video_idx;
        av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_ctx->streams[out_video_idx]->time_base);
        av_interleaved_write_frame(out_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }
    
    av_write_trailer(out_ctx);
    
    // Cleanup
    av_frame_free(&enc_frame);
    av_frame_free(&frame);
    av_packet_free(&enc_pkt);
    av_packet_free(&pkt);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&enc_ctx);
    avcodec_free_context(&dec_ctx);
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
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
    int dragging = 0;       // 0=none, 1=left handle, 2=right handle, 3=playhead, 4=panning
    int dragging_clip = -1; // Which clip's handle we're dragging
    float zoom = 1.0f;      // Zoom level (1.0 = fit all, higher = zoom in)
    float scroll = 0.0f;    // Scroll position (0.0 to 1.0, normalized)
    float pan_start_x = 0.0f; // For panning
    float pan_start_scroll = 0.0f;
};

// Modern Final Cut-style magnetic timeline widget with zoom/scroll
// Clips are always contiguous - no gaps allowed
// Returns true if anything changed
bool ClipsTimeline(const char* label, float* current, std::vector<Clip>& clips, float source_duration, float frame_duration, const ImVec2& size, ClipsTimelineState& state) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    ImVec2 bb_min = pos;
    ImVec2 bb_max = ImVec2(pos.x + size.x, pos.y + size.y);
    
    float rounding = 12.0f;
    float handle_w = 10.0f;
    float clip_margin = 2.0f;
    float playhead_head_size = 10.0f;
    
    // Calculate total timeline duration (sum of all clip durations)
    float total_duration = 0.0f;
    for (const auto& c : clips) total_duration += (float)c.duration();
    if (total_duration < 0.01f) total_duration = source_duration;
    
    // Zoom and scroll calculations - based on SOURCE duration for consistent pixel scaling
    // This means 1 second of video = same pixel width regardless of trimming
    float base_duration = source_duration;
    
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
    
    // Helper to convert timeline time to screen X
    auto time_to_x = [&](float t) -> float {
        return bb_min.x + ((t - view_start) / visible_duration) * size.x;
    };
    
    // Helper to convert screen X to timeline time
    auto x_to_time = [&](float x) -> float {
        return view_start + ((x - bb_min.x) / size.x) * visible_duration;
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
    
    // Track if mouse is over scrollbar area (for click priority)
    bool mouse_over_scrollbar = (mouse.y >= scroll_bar_y - 2 && mouse.y <= bb_max.y &&
                                  mouse.x >= bb_min.x && mouse.x <= bb_max.x);
    
    // Check if mouse is in timeline area
    bool mouse_in_timeline = (mouse.x >= bb_min.x && mouse.x <= bb_max.x && 
                              mouse.y >= bb_min.y && mouse.y <= bb_max.y);
    
    // Draw each clip - positioned sequentially (magnetic/ripple style)
    float timeline_pos = 0.0f;  // Current position on timeline
    for (int i = 0; i < (int)clips.size(); i++) {
        const Clip& clip = clips[i];
        float clip_duration = (float)clip.duration();
        
        // Skip clips outside visible range
        if (timeline_pos + clip_duration < view_start || timeline_pos > view_end) {
            timeline_pos += clip_duration;
            continue;
        }
        
        // Calculate screen positions using zoom/scroll-aware conversion
        float start_x = time_to_x(timeline_pos);
        float end_x = time_to_x(timeline_pos + clip_duration);
        
        // Clip bounds with margin (leave room for scrollbar at bottom)
        ImVec2 clip_min(start_x + clip_margin, bb_min.y + clip_margin);
        ImVec2 clip_max(end_x - clip_margin, clip_area_bottom - clip_margin);
        
        if (clip_max.x <= clip_min.x) continue;  // Skip if too small
        
        // Check if mouse is over this clip
        bool hover_clip = mouse_in_timeline && mouse.x >= start_x && mouse.x <= end_x;
        bool hover_left = hover_clip && mouse.x <= start_x + handle_w + 4;
        bool hover_right = hover_clip && mouse.x >= end_x - handle_w - 4;
        
        // Clip colors - purple/blue tones like Final Cut
        ImU32 clip_color = (i % 2 == 0) ? IM_COL32(85, 65, 125, 255) : IM_COL32(65, 85, 125, 255);
        
        // Draw clip with rounded corners
        draw_list->AddRectFilled(clip_min, clip_max, clip_color, rounding);
        
        // Subtle highlight at top edge for depth
        draw_list->AddLine(ImVec2(clip_min.x + rounding, clip_min.y + 1), 
                          ImVec2(clip_max.x - rounding, clip_min.y + 1), 
                          IM_COL32(255, 255, 255, 40), 1.0f);
        
        // Handle zones - subtle darker/lighter areas at edges
        float handle_inner = handle_w;
        
        if (clip_max.x - clip_min.x > handle_w * 3) {
            // Left handle zone
            ImU32 left_handle_col = hover_left ? IM_COL32(255, 255, 255, 60) : IM_COL32(0, 0, 0, 30);
            draw_list->AddRectFilled(clip_min, ImVec2(clip_min.x + handle_inner, clip_max.y), 
                                     left_handle_col, rounding, ImDrawFlags_RoundCornersLeft);
            
            // Right handle zone
            ImU32 right_handle_col = hover_right ? IM_COL32(255, 255, 255, 60) : IM_COL32(0, 0, 0, 30);
            draw_list->AddRectFilled(ImVec2(clip_max.x - handle_inner, clip_min.y), clip_max, 
                                     right_handle_col, rounding, ImDrawFlags_RoundCornersRight);
        }
        
        // Clip duration text (if clip is wide enough)
        float clip_width = clip_max.x - clip_min.x;
        if (clip_width > 60) {
            char time_str[32];
            int secs = (int)clip.duration();
            int frames = (int)((clip.duration() - secs) * 24);
            snprintf(time_str, sizeof(time_str), "%d:%02d", secs, frames);
            
            ImVec2 text_size = ImGui::CalcTextSize(time_str);
            ImVec2 text_pos((clip_min.x + clip_max.x - text_size.x) / 2, 
                           (clip_min.y + clip_max.y - text_size.y) / 2);
            draw_list->AddText(text_pos, IM_COL32(255, 255, 255, 180), time_str);
        }
        
        timeline_pos += clip_duration;
    }
    
    // Convert current time (in source coordinates) to timeline position
    float playhead_timeline_pos = 0.0f;
    float remaining = *current;
    for (const auto& c : clips) {
        if (remaining <= c.source_start) break;
        if (remaining < c.source_end) {
            playhead_timeline_pos += (remaining - (float)c.source_start);
            break;
        }
        playhead_timeline_pos += (float)c.duration();
    }
    
    // Playhead - red line with triangular head (only if visible)
    float curr_x = time_to_x(playhead_timeline_pos);
    bool playhead_visible = (playhead_timeline_pos >= view_start - 0.1f && playhead_timeline_pos <= view_end + 0.1f);
    
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
    struct ClipScreenPos { float start_x, end_x; int clip_idx; float timeline_start; };
    std::vector<ClipScreenPos> clip_positions;
    {
        float tpos = 0.0f;
        for (int i = 0; i < (int)clips.size(); i++) {
            float dur = (float)clips[i].duration();
            float sx = time_to_x(tpos);
            float ex = time_to_x(tpos + dur);
            clip_positions.push_back({sx, ex, i, tpos});
            tpos += dur;
        }
    }
    
    // Detect what we clicked on (but not if clicking on scrollbar)
    if (ImGui::IsItemClicked(0) && !mouse_over_scrollbar) {
        float click_x = mouse.x;
        state.dragging = 0;
        state.dragging_clip = -1;
        
        // Check each clip's handles
        for (int i = 0; i < (int)clips.size(); i++) {
            float start_x = clip_positions[i].start_x;
            float end_x = clip_positions[i].end_x;
            
            bool on_left = (click_x >= start_x && click_x <= start_x + handle_w + 6);
            bool on_right = (click_x >= end_x - handle_w - 6 && click_x <= end_x);
            
            if (on_left && on_right) {
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
    
    if (!is_active && state.dragging != 4) {  // Don't reset for middle-mouse panning
        state.dragging = 0;
        state.dragging_clip = -1;
    }
    
    if (state.dragging != 0 && state.dragging != 4 && is_active) {
        // Use zoom-aware time conversion
        float timeline_time = x_to_time(mouse.x);
        timeline_time = std::clamp(timeline_time, 0.0f, total_duration);
        
        if (state.dragging == 3) {
            // Moving playhead - convert timeline position to source position
            float tpos = 0.0f;
            float source_time = 0.0f;
            for (const auto& c : clips) {
                float dur = (float)c.duration();
                if (timeline_time < tpos + dur) {
                    source_time = (float)c.source_start + (timeline_time - tpos);
                    break;
                }
                tpos += dur;
                source_time = (float)c.source_end;
            }
            *current = std::clamp(source_time, 0.0f, source_duration);
            changed = true;
        } else if (state.dragging_clip >= 0 && state.dragging_clip < (int)clips.size()) {
            Clip& clip = clips[state.dragging_clip];
            
            // Use zoom-aware conversion for handle dragging
            const auto& cp = clip_positions[state.dragging_clip];
            
            if (state.dragging == 1) {
                // Left handle - adjust source_start (trim beginning)
                float delta_x = mouse.x - cp.start_x;
                float delta_time = (delta_x / size.x) * visible_duration;
                
                double new_start = clip.source_start + delta_time;
                new_start = std::clamp(new_start, 0.0, clip.source_end - (double)frame_duration);
                clip.source_start = new_start;
                changed = true;
            } else if (state.dragging == 2) {
                // Right handle - adjust source_end (trim end)
                float delta_x = mouse.x - cp.end_x;
                float delta_time = (delta_x / size.x) * visible_duration;
                
                double new_end = clip.source_end + delta_time;
                new_end = std::clamp(new_end, clip.source_start + (double)frame_duration, (double)source_duration);
                clip.source_end = new_end;
                changed = true;
            }
        }
    }
    
    // Change cursor when hovering handles
    if (is_hovered && state.dragging == 0) {
        for (int i = 0; i < (int)clip_positions.size(); i++) {
            float start_x = clip_positions[i].start_x;
            float end_x = clip_positions[i].end_x;
            
            bool over_left = (mouse.x >= start_x && mouse.x <= start_x + handle_w + 6);
            bool over_right = (mouse.x >= end_x - handle_w - 6 && mouse.x <= end_x);
            if (over_left || over_right) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                break;
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
            if (state.dragging == 5) {
                thumb_color = IM_COL32(110, 110, 130, 255);
            } else if (thumb_hovered) {
                thumb_color = IM_COL32(90, 90, 105, 255);
            }
            
            draw_list->AddRectFilled(thumb_min, thumb_max, thumb_color, 6.0f);
            
            // Handle scrollbar click/drag
            if (mouse_over_scrollbar && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && state.dragging == 0) {
                if (thumb_hovered) {
                    state.dragging = 5;
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
    if (state.dragging == 5) {
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

    WindowData main_window = create_window("VibeCut", 1400, 1000);
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
                    ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse);
                
                float ui_scale = main_window.scale;
                float controls_height = 130 * ui_scale;
                float scale_x = viewport->Size.x / player.width;
                float scale_y = (viewport->Size.y - controls_height) / player.height;
                float img_scale = std::min(scale_x, scale_y);
                
                float img_w = player.width * img_scale;
                float img_h = player.height * img_scale;
                float img_x = (viewport->Size.x - img_w) / 2;
                float img_y = (viewport->Size.y - controls_height - img_h) / 2;
                
                ImGui::SetCursorPos(ImVec2(img_x, img_y));
                ImGui::Image((ImTextureID)(intptr_t)player.texture_id, ImVec2(img_w, img_h));
                
                // Controls
                ImGui::SetCursorPos(ImVec2(10, viewport->Size.y - controls_height + 5));
                
                if (ImGui::Button(player.playing ? "Pause" : "Play", ImVec2(60 * ui_scale, 0))) {
                    player.toggle_play();
                }
                
                ImGui::SameLine();
                ImGui::Text("%s / %s (clips: %zu, total: %s)", 
                    player.format_time(player.current_time).c_str(),
                    player.format_time(player.duration).c_str(),
                    player.clips.size(),
                    player.format_time(player.trimmed_duration()).c_str());
                
                // Spacebar to play/pause
                if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                    if (player.playing) {
                        player.pause();
                    } else {
                        player.play();
                    }
                }
                
                // 'B' key to split clip at playhead
                if (ImGui::IsKeyPressed(ImGuiKey_B) && !player.clips.empty()) {
                    player.split_at(player.current_time);
                }
                
                // Arrow key navigation
                double nav_frame_dur = 1.0 / player.framerate;
                
                // Left/Right arrows: move by one frame in EDITED timeline
                // (skips over trimmed-out sections between clips)
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !player.clips.empty()) {
                    player.pause();
                    double new_time = player.current_time - nav_frame_dur;
                    
                    // Check if new_time is inside any clip
                    bool in_clip = false;
                    for (const auto& c : player.clips) {
                        if (new_time >= c.source_start && new_time < c.source_end) {
                            in_clip = true;
                            break;
                        }
                    }
                    
                    if (!in_clip) {
                        // Find the end of the previous clip
                        double best = 0.0;
                        for (const auto& c : player.clips) {
                            if (c.source_end <= player.current_time + nav_frame_dur * 0.5) {
                                best = c.source_end - nav_frame_dur * 0.1;
                            }
                        }
                        new_time = best;
                    }
                    
                    player.seek(std::max(0.0, new_time));
                }
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !player.clips.empty()) {
                    player.pause();
                    double new_time = player.current_time + nav_frame_dur;
                    
                    // Check if new_time is inside any clip
                    bool in_clip = false;
                    for (const auto& c : player.clips) {
                        if (new_time >= c.source_start && new_time < c.source_end) {
                            in_clip = true;
                            break;
                        }
                    }
                    
                    if (!in_clip) {
                        // Find the start of the next clip
                        for (const auto& c : player.clips) {
                            if (c.source_start > player.current_time - nav_frame_dur * 0.5) {
                                new_time = c.source_start;
                                break;
                            }
                        }
                    }
                    
                    player.seek(std::min(player.duration, new_time));
                }
                
                // Up arrow: go to previous clip boundary
                // Works like: start of clip 2 -> end of clip 1 -> start of clip 1 -> end of clip 0...
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !player.clips.empty()) {
                    player.pause();
                    double target = 0.0;
                    
                    // Find the previous clip boundary before current position
                    for (int i = (int)player.clips.size() - 1; i >= 0; i--) {
                        const Clip& c = player.clips[i];
                        // Check if clip end is behind us
                        if (c.source_end < player.current_time - nav_frame_dur * 0.5) {
                            target = c.source_end - nav_frame_dur * 0.1;
                            break;
                        }
                        // Check if clip start is behind us  
                        if (c.source_start < player.current_time - nav_frame_dur * 0.5) {
                            target = c.source_start;
                            break;
                        }
                    }
                    
                    player.seek(target);
                }
                
                // Down arrow: go to next clip boundary
                // Works like: end of clip 1 -> start of clip 2 -> end of clip 2 -> start of clip 3...
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !player.clips.empty()) {
                    player.pause();
                    double target = player.current_time;
                    
                    // Find the next clip boundary after current position
                    for (int i = 0; i < (int)player.clips.size(); i++) {
                        const Clip& c = player.clips[i];
                        // Check if clip start is ahead of us
                        if (c.source_start > player.current_time + nav_frame_dur * 0.5) {
                            target = c.source_start;
                            break;
                        }
                        // Check if clip end is ahead of us
                        if (c.source_end > player.current_time + nav_frame_dur * 0.5) {
                            // Go to end (minus tiny bit to stay in clip)
                            target = c.source_end - nav_frame_dur * 0.1;
                            break;
                        }
                    }
                    
                    player.seek(target);
                }
                
                // Timeline with clips
                ImGui::SetCursorPos(ImVec2(10, viewport->Size.y - controls_height + 35 * ui_scale));
                float curr = (float)player.current_time;
                
                static float last_seek_target = -1.0f;
                
                float frame_dur = 1.0f / (float)player.framerate;
                if (ClipsTimeline("##clips_timeline", &curr, player.clips, (float)player.duration, frame_dur,
                                 ImVec2(viewport->Size.x - 20, 50 * ui_scale), timeline_state)) {
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
                ImGui::SetCursorPos(ImVec2(10, viewport->Size.y - 30 * ui_scale));
                ImGui::Text("File: %s", path_display_name(fs::path(selected_file)).c_str());
                
                ImGui::SameLine(viewport->Size.x - 180 * ui_scale);
                
                if (exporting) {
                    ImGui::ProgressBar(export_progress, ImVec2(80 * ui_scale, 0), "Exporting...");
                } else {
                    if (ImGui::Button("Export", ImVec2(0, 0))) {
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
                if (ImGui::Button("Open File...", ImVec2(0, 0))) {
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

                float scale = main_window.scale;
                if (ImGui::Button("Open File...", ImVec2(160 * scale, 40 * scale))) {
                    if (!browser_window.window) open_browser = true;
                }

                ImGui::End();
            }
        });

        if (open_browser) {
            int bw = (int)(450 * main_window.scale);
            int bh = (int)(550 * main_window.scale);
            browser_window = create_window("Open File", bw, bh, main_window.window);
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
                    ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse);

                ImGui::Text("Path: %s", browser.current_path.string().c_str());
                ImGui::Separator();

                float button_area_height = 35 * browser_window.scale;
                ImVec2 list_size(ImGui::GetContentRegionAvail().x, 
                                 ImGui::GetContentRegionAvail().y - button_area_height);
                
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
                if (ImGui::Button("Open", ImVec2(80 * browser_window.scale, 0))) {
                    try {
                        selected_file = browser.entries[browser.selected_index].path().string();
                        pending_load = selected_file;
                    } catch (...) {}
                    should_close = true;
                }
                if (!can_open) ImGui::EndDisabled();
                
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(80 * browser_window.scale, 0))) {
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
