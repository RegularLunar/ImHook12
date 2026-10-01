#include <Windows.h>
#include "menu.h"
#include "../lib/ImGui/imgui.h"

static bool g_open = false;                
static RECT g_oldClip = {};
static bool g_hadClip = false;

static void OnOpen()
{
    g_hadClip = GetClipCursor(&g_oldClip) != 0;        
    ClipCursor(nullptr);                              
    ImGui::GetIO().MouseDrawCursor = true;             
}

static void OnClose()
{
    ImGui::GetIO().MouseDrawCursor = false;
    if (g_hadClip) ClipCursor(&g_oldClip);               
}

void Menu::Toggle()
{
    g_open = !g_open;
    g_open ? OnOpen() : OnClose();
}

bool Menu::IsOpen() { return g_open; }

void Menu::Render()
{
    if (!g_open) return;

    ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);

    ImGui::Begin("Debug");
    ImGui::Text("hello");
    ImGui::End();
}