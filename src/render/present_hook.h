#pragma once
// Hooks IDXGISwapChain::Present / Present1 / ResizeBuffers. The vtable is taken from a throwaway D3D11 device +
// swapchain created on a hidden window (DXGI swapchains share one implementation, so hooking that vtable's
// functions catches the game's swapchain). The frame callback runs on the game's render thread once per frame.
struct IDXGISwapChain;

namespace mc::render {

using FrameCallback = void (*)(float dt);
using ResizeCallback = void (*)(unsigned width, unsigned height);
using BeforeResizeCallback = void (*)();  // release backbuffer references here
using DeviceLostCallback = void (*)();    // Present reported DXGI_ERROR_DEVICE_REMOVED/RESET: drop every resource

bool InstallPresentHook(FrameCallback onFrame, ResizeCallback onResize = nullptr,
                        BeforeResizeCallback onBeforeResize = nullptr, DeviceLostCallback onDeviceLost = nullptr);
void RemovePresentHook();

IDXGISwapChain* GameSwapChain();  // last swapchain seen in Present (not AddRef'd), nullptr before the first frame

} // namespace mc::render
