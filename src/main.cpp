#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <cstdio>
#include <vector>

#include "embedded_font.h"

struct WindowData {
    GLFWwindow* window = nullptr;
    ImGuiContext* imgui_ctx = nullptr;
    float scale = 1.0f;
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

    std::vector<WindowData> child_windows;
    int child_count = 0;

    while (!glfwWindowShouldClose(main_window.window)) {
        glfwPollEvents();

        // Clean up closed child windows
        for (auto& child : child_windows) {
            if (child.window && glfwWindowShouldClose(child.window)) {
                destroy_window(child);
            }
        }

        // Render main window
        bool open_new_window = false;
        render_window(main_window, [&]() {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();

            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(0, 0));

            ImGui::Begin("##MainWindow", nullptr, 
                ImGuiWindowFlags_NoTitleBar | 
                ImGuiWindowFlags_NoResize | 
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoBackground |
                ImGuiWindowFlags_AlwaysAutoResize);

            if (ImGui::Button("Click Me!", ImVec2(200, 60))) {
                open_new_window = true;
            }

            ImGui::End();
        });

        // Create new child window if button was clicked
        if (open_new_window) {
            child_count++;
            char title[64];
            std::snprintf(title, sizeof(title), "Child Window %d", child_count);
            
            WindowData child = create_window(title, 400, 300, main_window.window);
            if (child.window) {
                child_windows.push_back(child);
            }
        }

        // Render child windows
        for (auto& child : child_windows) {
            if (!child.window) continue;
            
            render_window(child, [&]() {
                ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImVec2 center = viewport->GetCenter();

                ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(0, 0));

                ImGui::Begin("##About", nullptr,
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoBackground |
                    ImGuiWindowFlags_AlwaysAutoResize);

                ImGui::Text("About VibeCut");
                ImGui::Separator();
                ImGui::Text("A vibe-coded video editor");

                ImGui::End();
            });
        }
    }

    for (auto& child : child_windows) {
        destroy_window(child);
    }
    destroy_window(main_window);
    glfwTerminate();

    return 0;
}
