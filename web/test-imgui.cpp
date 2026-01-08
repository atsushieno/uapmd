#include "imgui.h"

static void main_loop() {
    ImGui::NewFrame();
    
    ImGui::Begin("uapmd-app - WebAssembly");
    ImGui::Text("uapmd-app is running in WebAssembly!");
    ImGui::Text("ImGui test successful!");
    ImGui::End();
    
    ImGui::Render();
}

int main(int argc, char** argv) {
    return 0;
}
