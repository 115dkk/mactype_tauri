#include "../../../renderer/activation_repaint.h"
#include "../../../renderer/arrival_evidence.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(window, message, wParam, lParam);
}

void PumpPendingMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

int main()
{
    const wchar_t className[] = L"MacTypeActivationRepaintTests";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    Require(RegisterClassW(&windowClass) != 0, "the private window class must register");

    HWND shown = CreateWindowExW(
        0, className, L"shown", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 200,
        nullptr, nullptr, windowClass.hInstance, nullptr);
    HWND hidden = CreateWindowExW(
        0, className, L"hidden", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 200,
        nullptr, nullptr, windowClass.hInstance, nullptr);
    Require(shown != nullptr && hidden != nullptr, "both test windows must be created");

    ShowWindow(shown, SW_SHOWNOACTIVATE);
    Require(UpdateWindow(shown) != FALSE, "the shown window must complete its first paint");
    PumpPendingMessages();

    renderer::arrival_evidence::ResetForTests();
    renderer::arrival_evidence::Record(
        renderer::arrival_evidence::ProcessSamplers());
    Require(!renderer::arrival_evidence::Recorded()->visibleTopLevelWindowExisted,
        "the visible-window sampler ran while arrival was recorded");
    renderer::arrival_evidence::CompleteDeferredSamples();
    Require(renderer::arrival_evidence::Recorded()->visibleTopLevelWindowExisted,
        "the deferred sampler missed the shown top-level window");
    Require(GetUpdateRect(shown, nullptr, FALSE) == FALSE,
            "the shown window must begin with a validated client area");

    Require(renderer::RepaintOwnedWindows(4) == 0,
            "a foreign process id must not invalidate the test windows");
    const unsigned int invalidated = renderer::RepaintOwnedWindows(GetCurrentProcessId());
    Require(invalidated >= 1, "at least one visible owned window must be invalidated");
    Require(GetUpdateRect(shown, nullptr, FALSE) != FALSE,
            "the shown window must have a pending repaint");
    Require(GetUpdateRect(hidden, nullptr, FALSE) == FALSE,
            "the hidden window must remain validated");

    DestroyWindow(hidden);
    DestroyWindow(shown);
    Require(UnregisterClassW(className, windowClass.hInstance) != FALSE,
            "the private window class must unregister");

    std::cout << "Activation repaint tests passed.\n";
    return 0;
}
