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

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include "embedded_font.h"

namespace fs = std::filesystem;

struct VideoFrame {
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    
    void destroy() {
        if (texture_id) {
            glDeleteTextures(1, &texture_id);
            texture_id = 0;
        }
        width = height = 0;
    }
};

VideoFrame load_first_frame(const std::string& path) {
    VideoFrame result;
    
    AVFormatContext* format_ctx = nullptr;
    if (avformat_open_input(&format_ctx, path.c_str(), nullptr, nullptr) < 0) {
        std::fprintf(stderr, "Could not open file: %s\n", path.c_str());
        return result;
    }
    
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        std::fprintf(stderr, "Could not find stream info\n");
        avformat_close_input(&format_ctx);
        return result;
    }
    
    int video_stream = -1;
    for (unsigned i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = i;
            break;
        }
    }
    
    if (video_stream < 0) {
        std::fprintf(stderr, "No video stream found\n");
        avformat_close_input(&format_ctx);
        return result;
    }
    
    AVCodecParameters* codecpar = format_ctx->streams[video_stream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::fprintf(stderr, "Codec not found\n");
        avformat_close_input(&format_ctx);
        return result;
    }
    
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);
    
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::fprintf(stderr, "Could not open codec\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return result;
    }
    
    AVFrame* frame = av_frame_alloc();
    AVFrame* frame_rgb = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    
    int width = codec_ctx->width;
    int height = codec_ctx->height;
    
    uint8_t* buffer = (uint8_t*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1));
    av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, buffer, AV_PIX_FMT_RGB24, width, height, 1);
    
    SwsContext* sws_ctx = sws_getContext(
        width, height, codec_ctx->pix_fmt,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    bool got_frame = false;
    while (av_read_frame(format_ctx, packet) >= 0 && !got_frame) {
        if (packet->stream_index == video_stream) {
            if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                if (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                              frame_rgb->data, frame_rgb->linesize);
                    got_frame = true;
                }
            }
        }
        av_packet_unref(packet);
    }
    
    if (got_frame) {
        glGenTextures(1, &result.texture_id);
        glBindTexture(GL_TEXTURE_2D, result.texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame_rgb->linesize[0] / 3);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, frame_rgb->data[0]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        
        result.width = width;
        result.height = height;
    }
    
    av_free(buffer);
    sws_freeContext(sws_ctx);
    av_packet_free(&packet);
    av_frame_free(&frame_rgb);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    
    return result;
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
    VideoFrame video_frame;
    std::string pending_load;

    while (!glfwWindowShouldClose(main_window.window)) {
        glfwPollEvents();

        if (browser_window.window && glfwWindowShouldClose(browser_window.window)) {
            destroy_window(browser_window);
        }

        // Load video after browser closes (need main window's GL context)
        if (!pending_load.empty()) {
            glfwMakeContextCurrent(main_window.window);
            video_frame.destroy();
            video_frame = load_first_frame(pending_load);
            if (video_frame.texture_id) {
                std::printf("Loaded video: %dx%d\n", video_frame.width, video_frame.height);
            }
            pending_load.clear();
        }

        bool open_browser = false;
        render_window(main_window, [&]() {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            
            if (video_frame.texture_id) {
                // Show video frame centered, scaled to fit
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(viewport->Size);
                ImGui::Begin("##VideoView", nullptr,
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoBackground);
                
                float scale_x = viewport->Size.x / video_frame.width;
                float scale_y = (viewport->Size.y - 80) / video_frame.height;
                float scale = std::min(scale_x, scale_y);
                
                float img_w = video_frame.width * scale;
                float img_h = video_frame.height * scale;
                float img_x = (viewport->Size.x - img_w) / 2;
                float img_y = (viewport->Size.y - 80 - img_h) / 2;
                
                ImGui::SetCursorPos(ImVec2(img_x, img_y));
                ImGui::Image((ImTextureID)(intptr_t)video_frame.texture_id, ImVec2(img_w, img_h));
                
                // Controls at bottom
                ImGui::SetCursorPos(ImVec2(0, viewport->Size.y - 70));
                ImGui::Separator();
                ImGui::Text("File: %s", selected_file.c_str());
                ImGui::SameLine(viewport->Size.x - 120);
                if (ImGui::Button("Open File...", ImVec2(110, 0))) {
                    if (!browser_window.window) open_browser = true;
                }
                
                ImGui::End();
            } else {
                // No video loaded - show centered button
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

    video_frame.destroy();
    if (browser_window.window) {
        destroy_window(browser_window);
    }
    destroy_window(main_window);
    glfwTerminate();

    return 0;
}
