#include "Interceptor.h"
#include "Ui.h"
#include "Model.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <system_error>
#include <iostream>
namespace
{
constexpr UINT Update = WM_APP + 1;
struct App
{
    HWND window = nullptr, start = nullptr, stop = nullptr, setup = nullptr, reset = nullptr, html = nullptr,
         csv = nullptr, device = nullptr, job = nullptr;
    std::thread worker;
    HANDLE stopEvent = nullptr;
    std::atomic<bool> running{false}, clear{false};
    std::mutex mutex;
    std::map<std::string, Bench::Device> devices;
    std::vector<std::string> keys;
    std::wstring status = L"Ready. Start a test only when you are ready to observe and forward inputs.";
    bool preview = false;
    int selected = 0;
    ~App()
    {
        Stop();
        if (stopEvent)
            CloseHandle(stopEvent);
    }
    void Stop()
    {
        if (stopEvent)
            SetEvent(stopEvent);
        if (worker.joinable())
            worker.join();
        running = false;
    }
    void State(const std::wstring &text)
    {
        std::lock_guard<std::mutex> lock(mutex);
        status = text;
    }
    static std::string Family(Interceptor::Device_Type type)
    {
        switch (type)
        {
        case Interceptor::Device_Type::Controller_Hid:
            return "HID";
        case Interceptor::Device_Type::Controller_Xinput:
            return "XUSB / XInput";
        case Interceptor::Device_Type::Controller_XinputHid:
            return "XInput HID";
        case Interceptor::Device_Type::Controller_GIP:
            return "GIP";
        default:
            return "Unsupported";
        }
    }
    static std::string Key(const Interceptor::Input_Event &e)
    {
        if (e.device_info && !e.device_info->token.empty())
            return e.device_info->token;
        return std::to_string(static_cast<unsigned>(e.type)) + ":" + std::to_string(e.device);
    }
    template <class T> void Hid(Bench::Device &d, const T &input, bool neutral)
    {
        if (input.decode_error || !input.decode_complete)
            ++d.invalid_decode;
        auto values = [&](const auto &list, bool center) {
            for (const auto &v : list)
            {
                std::ostringstream key;
                key << "Page " << std::hex << v.usage_page << " / usage " << v.usage << " / collection "
                    << v.link_collection << " / report " << static_cast<unsigned>(v.report_id) << " / index "
                    << std::dec << v.data_index << " / element " << v.element;
                if (v.normalized_valid)
                    d.Value(key.str(), v.normalized, neutral && center);
                else
                    d.Value(key.str() + " (raw)", static_cast<double>(v.value), false);
            }
        };
        values(input.state.axes, true);
        values(input.state.triggers, false);
        values(input.state.hats, false);
        for (const auto &b : input.state.buttons)
        {
            std::ostringstream key;
            key << b.usage_page << ":" << b.usage << ":" << b.link_collection << ":"
                << static_cast<unsigned>(b.report_id) << ":" << b.data_index;
            d.Button(key.str(), b.pressed);
        }
    }
    static void Native(Bench::Device &d, const IC_NATIVE_GAMEPAD_STATE &s, bool neutral, bool gip)
    {
        auto axis = [](short value) {
            return value < 0 ? static_cast<double>(value) / 32768.0 : static_cast<double>(value) / 32767.0;
        };
        d.Value("Left X", axis(s.left_x), neutral);
        d.Value("Left Y", axis(s.left_y), neutral);
        d.Value("Right X", axis(s.right_x), neutral);
        d.Value("Right Y", axis(s.right_y), neutral);
        const double max = gip ? 1023.0 : 255.0;
        d.Value("Left trigger", s.left_trigger / max, false);
        d.Value("Right trigger", s.right_trigger / max, false);
        for (unsigned i = 0; i < 16; ++i)
            d.Button("Protocol button bit " + std::to_string(i), (s.buttons & (1u << i)) != 0);
        for (unsigned i = 0; i < 8; ++i)
            d.Button("Extra button bit " + std::to_string(i), (s.extra_buttons & (1u << i)) != 0);
    }
    void Run()
    {
        try
        {
            LARGE_INTEGER freq{};
            if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0)
                throw std::runtime_error("A valid QPC frequency is required for sampling");
            // Initialize the automatic capture before starting the fixed sample
            // epoch. A late first report must never open its own neutral window.
            (void)Interceptor::Get_Devices();
            const auto qpcNow = [] {
                LARGE_INTEGER value{};
                if (!QueryPerformanceCounter(&value) || value.QuadPart < 0)
                    throw std::runtime_error("QPC sampling failed");
                return static_cast<std::uint64_t>(value.QuadPart);
            };
            Bench::NeutralWindow neutralWindow;
            {
                std::lock_guard<std::mutex> lock(mutex);
                devices.clear();
                clear = false;
                neutralWindow.Reset(qpcNow(), static_cast<std::uint64_t>(freq.QuadPart));
                status = L"Sampling neutral for three seconds. Keep sticks untouched; wait for sample complete.";
            }
            bool neutralFinished = false;
            while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0)
            {
                auto batch = Interceptor::Get_Input_With_Timeout(100, stopEvent);
                const auto sent = Interceptor::Send_Input(batch);
                if (sent.error || sent.accepted != batch.size())
                    throw std::system_error(static_cast<int>(sent.error ? sent.error : ERROR_WRITE_FAULT),
                                            std::system_category(), "Forwarding input failed");
                std::lock_guard<std::mutex> lock(mutex);
                if (clear.exchange(false))
                {
                    devices.clear();
                    neutralWindow.Reset(qpcNow(), static_cast<std::uint64_t>(freq.QuadPart));
                    neutralFinished = false;
                    status = L"Reset acknowledged: sampling neutral for three seconds. Keep sticks untouched.";
                }
                if (!neutralFinished && neutralWindow.Finished(qpcNow()))
                {
                    neutralFinished = true;
                    status = L"Neutral sample complete. Exercise every control. Fields first observed now are not sampled as neutral.";
                }
                for (const auto &e : batch)
                {
                    const auto key = Key(e);
                    if (devices.size() >= 64 && devices.find(key) == devices.end())
                        continue;
                    auto &d = devices[key];
                    if (d.label.empty())
                    {
                        d.family = Family(e.type);
                        d.label = d.family + " / source " + std::to_string(e.device);
                        if (e.device_info)
                        {
                            const auto *raw = e.device_info->name;
                            std::wstring name;
                            for (size_t n = 0; n < 128 && raw[n]; ++n)
                                name += static_cast<wchar_t>(raw[n]);
                            if (!name.empty())
                                d.label = Ui::Utf8(name) + " / " + d.family;
                            std::ostringstream id;
                            id << " [" << std::hex << std::setw(4) << std::setfill('0') << e.device_info->vid
                               << ":" << std::setw(4) << e.device_info->pid << "]";
                            d.label += id.str();
                        }
                    }
                    d.Report(e.timestamp, static_cast<double>(freq.QuadPart));
                    const bool neutral = neutralWindow.Contains(e.timestamp);
                    if (auto p = e.Get<Interceptor::Controller_Hid_Input>())
                        Hid(d, *p, neutral);
                    else if (auto hid = e.Get<Interceptor::Controller_XinputHid_Input>())
                        Hid(d, *hid, neutral);
                    else if (auto xusb = e.Get<Interceptor::Controller_Xinput_Input>())
                        Native(d, xusb->state, neutral, false);
                    else if (auto gip = e.Get<Interceptor::Controller_GIP_Input>())
                        Native(d, gip->state, neutral, true);
                }
            }
            State(L"Test stopped. Captured input was forwarded unchanged; normal passthrough is restored.");
        }
        catch (const std::exception &error)
        {
            State(L"Test stopped: " + Ui::Wide(error.what()) +
                  L". Capture released; input returns to passthrough.");
        }
        try
        {
            Interceptor::Destroy_Context();
        }
        catch (...)
        {
        }
        running = false;
        PostMessageW(window, Update, 0, 0);
    }
    void Start()
    {
        if (running || preview)
            return;
        if (!Ui::StartElevated(window))
            return;
        Stop();
        ResetEvent(stopEvent);
        clear = true;
        running = true;
        try
        {
            worker = std::thread([this] { Run(); });
        }
        catch (const std::exception &error)
        {
            running = false;
            State(Ui::Wide(error.what()));
        }
        InvalidateRect(window, nullptr, FALSE);
    }
    void Layout()
    {
        RECT r{};
        GetClientRect(window, &r);
        const int w = Ui::Logical(window, r.right);
        Ui::Fonts(window);
        Ui::Move(start, 24, 94, 105, 38, TRUE);
        Ui::Move(stop, 139, 94, 105, 38, TRUE);
        Ui::Move(reset, 254, 94, 180, 38, TRUE);
        Ui::Move(setup, w - 180, 28, 156, 38, TRUE);
        Ui::Move(device, 24, 157, std::max(300, w - 48), 300, TRUE);
        Ui::Move(job, 120, 209, std::max(170, w - 498), 32, TRUE);
        Ui::Move(html, w - 350, 206, 156, 38, TRUE);
        Ui::Move(csv, w - 184, 206, 160, 38, TRUE);
    }
    void Paint(HDC dc, int width, int height)
    {
        Ui::Canvas scale(dc, window);
        width = Ui::Logical(window, width);
        height = Ui::Logical(window, height);
        Ui::Box(dc, RECT{0, 0, width, height}, Ui::Background, 0);
        Ui::TextAt(dc, 24, 22, L"Controller Repair Bench", width - 230, Ui::Text, 29, FW_SEMIBOLD);
        Ui::TextAt(dc, 24, 62, L"Source reports and repair evidence | Inputs are always forwarded unchanged",
                   width - 50, Ui::Muted, 16);
        Ui::TextAt(dc, 24, 214, L"Job / serial", 95, Ui::Muted, 16);
        std::unique_lock<std::mutex> lock(mutex);
        Ui::Box(dc, RECT{24, 265, width - 24, 337});
        Ui::TextAt(dc, 40, 279, status, width - 80, Ui::Accent, 16);
        if (devices.empty())
        {
            Ui::TextAt(dc, 36, 375,
                       L"No reports received yet. Start a test and keep sticks untouched while Sampling neutral.\n"
                       L"Wait for Neutral sample complete before moving controls; silence is not a failure.\n"
                       L"Late fields are not sampled as neutral. Reset starts a new fixed sample window.",
                       width - 80, Ui::Muted, 19);
            return;
        }
        if (selected >= static_cast<int>(keys.size()))
            selected = 0;
        auto found = keys.empty() ? devices.begin() : devices.find(keys[static_cast<size_t>(selected)]);
        if (found == devices.end())
            return;
        const auto d = found->second;
        lock.unlock();
        Ui::TextAt(dc, 28, 355,
                   L"REPORTS  " + std::to_wstring(d.reports) + L"       COVERED BUTTON FIELDS  " +
                       std::to_wstring(d.covered.size()) + L" / " + std::to_wstring(d.known_buttons.size()),
                   width - 60, Ui::Text, 19, FW_SEMIBOLD);
        std::wostringstream metric;
        metric << std::fixed << std::setprecision(3) << L"Mean source interval: " << d.cadence.mean
               << L" ms | Interval sample SD: " << d.cadence.Noise() << L" ms | Incomplete HID reports: "
               << d.invalid_decode;
        Ui::TextAt(dc, 28, 387, metric.str(), width - 60, Ui::Muted, 16);
        Ui::TextAt(dc, 28, 427, L"FIELD / CURRENT", width / 2, Ui::Muted, 15, FW_SEMIBOLD);
        Ui::TextAt(dc, width / 2, 427, L"OBSERVED RANGE   |   NEUTRAL MEAN / NOISE", width / 2 - 40, Ui::Muted, 15,
                   FW_SEMIBOLD);
        int y = 462;
        for (const auto &pair : d.axes)
        {
            if (y > height - 85)
            {
                Ui::TextAt(dc, 28, y, L"More fields are included in exported reports.", width - 60, Ui::Muted, 15);
                break;
            }
            const auto &a = pair.second;
            std::wostringstream left, right;
            left << Ui::Wide(pair.first) << L"  " << std::fixed << std::setprecision(3) << a.current;
            right << std::fixed << std::setprecision(3) << a.range.minimum << L" .. " << a.range.maximum;
            if (a.neutral.n)
                right << L"   |   " << a.neutral.mean << L" / " << a.neutral.Noise();
            else
                right << L"   |   not sampled";
            Ui::Box(dc, RECT{24, y - 5, width - 24, y + 33});
            Ui::TextAt(dc, 36, y, left.str(), width / 2 - 50, Ui::Text, 15);
            Ui::TextAt(dc, width / 2, y, right.str(), width / 2 - 40, Ui::Accent, 16);
            y += 43;
        }
        Ui::TextAt(
            dc, 28, height - 36,
            L"Measured report intervals are not physical input-to-game latency. Ctrl+Alt+F12 stops capture.",
            width - 60, Ui::Muted, 14);
    }
    void Refresh()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (keys.size() != devices.size())
        {
            std::string chosen;
            if (selected >= 0 && selected < static_cast<int>(keys.size()))
                chosen = keys[static_cast<size_t>(selected)];
            keys.clear();
            SendMessageW(device, CB_RESETCONTENT, 0, 0);
            int index = 0;
            for (const auto &pair : devices)
            {
                keys.push_back(pair.first);
                SendMessageW(device, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(Ui::Wide(pair.second.label).c_str()));
                if (pair.first == chosen)
                    selected = index;
                ++index;
            }
            if (selected >= static_cast<int>(keys.size()))
                selected = 0;
            SendMessageW(device, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        }
        InvalidateRect(window, nullptr, FALSE);
    }
    void Export(bool isHtml)
    {
        std::filesystem::path path;
        if (!Ui::Save(window, isHtml ? L"html" : L"csv", path))
            return;
        std::map<std::string, Bench::Device> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = devices;
        }
        const auto report = Bench::Report(snapshot, Ui::Utf8(Ui::GetText(job)), isHtml);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(report.data(), static_cast<std::streamsize>(report.size()));
        file.close();
        if (!file)
            MessageBoxW(window, L"Report could not be written. Choose a writable folder.", L"Export failed",
                        MB_OK | MB_ICONERROR);
        else
            State(L"Report saved: " + path.wstring());
    }
};
int Tests()
{
    int count = 0;
    auto check = [&](bool good) {
        ++count;
        if (!good)
            throw std::runtime_error("Controller Bench fixture failed: " + std::to_string(count));
    };
    Bench::NeutralWindow neutralWindow;
    check(!neutralWindow.Contains(1000));
    neutralWindow.Reset(1000, 1000);
    check(!neutralWindow.Contains(999)); // Queued report from before the epoch.
    check(neutralWindow.Contains(1000));
    check(neutralWindow.Contains(3999));
    check(!neutralWindow.Contains(4000) && neutralWindow.Finished(4000));
    Bench::Device late;
    late.label = "Late first report";
    late.Report(5000, 1000);
    late.Value("X", 0.75, neutralWindow.Contains(5000));
    check(late.axes["X"].neutral.n == 0 && late.axes["X"].range.n == 1);
    late.Report(5010, 1000);
    check(late.cadence.n == 1 && late.cadence.mean == 10); // Cadence stays independent.
    auto lateHtml = Bench::Report({{"late", late}}, "late fixture", true);
    check(lateHtml.find("not sampled") != std::string::npos);
    neutralWindow.Reset(8000, 1000);
    check(!neutralWindow.Contains(7999) && !neutralWindow.Finished(7999));
    check(neutralWindow.Contains(8000) && !neutralWindow.Contains(11000));
    Bench::Device resetFixture;
    resetFixture.Value("X", 0.25, neutralWindow.Contains(7999));
    resetFixture.Value("X", 0.05, neutralWindow.Contains(8000));
    check(resetFixture.axes["X"].neutral.n == 1 && resetFixture.axes["X"].neutral.mean == 0.05);
    neutralWindow.Reset(0, 0);
    check(!neutralWindow.Contains(1) && !neutralWindow.Finished(1));
    Bench::Moments m;
    m.Add(-1);
    m.Add(0);
    m.Add(1);
    check(m.n == 3 && m.mean == 0 && m.Noise() == 1);
    m.Add(NAN);
    check(m.n == 3);
    Bench::Device d;
    d.label = "<script>";
    d.family = "HID";
    d.Report(1000, 1000);
    d.Report(1002, 1000);
    d.Report(1001, 1000);
    check(d.reports == 3 && d.cadence.mean == 2 && d.out_of_order == 1);
    d.Value("X", 0.01, true);
    d.Value("X", 1, false);
    check(d.axes["X"].neutral.n == 1 && d.axes["X"].range.maximum == 1);
    d.Button("button", true);
    d.Button("button", false);
    check(d.covered.size() == 1 && d.pressed.empty());
    std::map<std::string, Bench::Device> devices{{"one", d}};
    auto html = Bench::Report(devices, "<job>", true);
    check(html.find("&lt;script&gt;") != std::string::npos && html.find("&lt;job&gt;") != std::string::npos);
    check(Bench::Csv("a\"b") == "\"a\"\"b\"");
    check(Bench::CsvSafe("=cmd") == "\"'=cmd\"");
    check(Bench::CsvSafe("  =cmd") == "\"'  =cmd\"");
    Bench::Device native;
    IC_NATIVE_GAMEPAD_STATE wire{};
    wire.left_x = SHRT_MIN;
    wire.right_x = SHRT_MAX;
    wire.left_trigger = 255;
    wire.buttons = 1;
    App::Native(native, wire, true, false);
    check(native.axes["Left X"].current == -1 && native.axes["Right X"].current == 1 &&
          native.axes["Left trigger"].current == 1);
    wire.left_trigger = 1023;
    App::Native(native, wire, false, true);
    check(native.axes["Left trigger"].current == 1);
    App fixture;
    Bench::Device decoded;
    Interceptor::Controller_Hid_Input hid;
    hid.decode_complete = true;
    Interceptor::Hid_Value first;
    first.usage_page = 1;
    first.usage = 0x30;
    first.normalized_valid = true;
    first.normalized = 0.25;
    first.data_index = 1;
    auto second = first;
    second.data_index = 2;
    second.normalized = -0.5;
    hid.state.axes = {first, second};
    fixture.Hid(decoded, hid, true);
    check(decoded.axes.size() == 2 && decoded.invalid_decode == 0);
    Bench::Moments stress;
    for (int i = 0; i < 100000; ++i)
        stress.Add(static_cast<double>(i % 2) * 2 - 1);
    check(stress.n == 100000 && std::abs(stress.mean) < 1e-12 &&
          std::abs(stress.Noise() - std::sqrt(100000.0 / 99999.0)) < 1e-12);
    for (int i = 0; i < 400; ++i)
        d.Value(std::to_string(i), 0, false);
    check(d.axes.size() == 256);
    for (int i = 0; i < 700; ++i)
        d.Button(std::to_string(i), true);
    check(d.known_buttons.size() == 512);
    std::cout << "Controller Repair Bench: " << count << " offline assertions passed. No driver opened.\n";
    return 0;
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(window, message, w, l);
    switch (message)
    {
    case WM_CREATE:
        app->start = Ui::Button(window, L"Start test", 1);
        app->stop = Ui::Button(window, L"Stop", 2);
        app->reset = Ui::Button(window, L"Reset / new test", 3);
        app->setup = Ui::Button(window, L"Driver setup", 4);
        app->html = Ui::Button(window, L"Export HTML", 5);
        app->csv = Ui::Button(window, L"Export CSV", 6);
        app->device = Ui::Control(window, L"COMBOBOX", L"", 7, CBS_DROPDOWNLIST | WS_VSCROLL);
        app->job = Ui::Control(window, L"EDIT", L"", 8, ES_AUTOHSCROLL | WS_BORDER);
        SendMessageW(app->job, EM_SETLIMITTEXT, 128, 0);
        for (auto h : {app->start, app->stop, app->reset, app->setup, app->html, app->csv, app->device})
            Ui::Hand(h);
        RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12);
        SetTimer(window, 1, 100, nullptr);
        app->Layout();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w))
        {
        case 1:
            app->Start();
            break;
        case 2:
            app->Stop();
            app->State(L"Stopped. Normal controller passthrough is restored.");
            break;
        case 3:
            if (app->running)
                app->clear = true;
            else
            {
                std::lock_guard<std::mutex> lock(app->mutex);
                app->devices.clear();
            }
            app->keys.clear();
            SendMessageW(app->device, CB_RESETCONTENT, 0, 0);
            app->State(L"Reset requested. Wait for the worker to acknowledge its fixed three-second neutral sample.");
            break;
        case 4:
            Ui::Setup(window);
            break;
        case 5:
            app->Export(true);
            break;
        case 6:
            app->Export(false);
            break;
        case 7:
            if (HIWORD(w) == CBN_SELCHANGE)
                app->selected = static_cast<int>(SendMessageW(app->device, CB_GETCURSEL, 0, 0));
            break;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_HOTKEY:
        app->Stop();
        app->State(L"Emergency stop. Controller input returned to passthrough.");
        return 0;
    case WM_SIZE:
        app->Layout();
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO *>(l)->ptMinTrackSize = {Ui::Pixel(window, 950), Ui::Pixel(window, 720)};
        return 0;
    case WM_TIMER:
    case Update:
        app->Refresh();
        return 0;
    case WM_DRAWITEM:
        Ui::DrawButton(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(w), Ui::Text);
        SetBkColor(reinterpret_cast<HDC>(w), Ui::Card);
        {
            static auto b = CreateSolidBrush(Ui::Card);
            return reinterpret_cast<LRESULT>(b);
        }
    case WM_PRINTCLIENT: {
        RECT r{};
        GetClientRect(window, &r);
        app->Paint(reinterpret_cast<HDC>(w), r.right, r.bottom);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window, &ps);
        RECT r{};
        GetClientRect(window, &r);
        auto memory = CreateCompatibleDC(dc);
        auto bmp = CreateCompatibleBitmap(dc, std::max<LONG>(1, r.right), std::max<LONG>(1, r.bottom));
        auto old = SelectObject(memory, bmp);
        app->Paint(memory, r.right, r.bottom);
        BitBlt(dc, 0, 0, r.right, r.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bmp);
        DeleteDC(memory);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        auto *r = reinterpret_cast<RECT *>(l);
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_CLOSE:
        app->Stop();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(window, 1);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w, l);
    }
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    if (Ui::Command(L"--self-test") || Ui::Command(L"--offline"))
    {
        try
        {
            return Tests();
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    App app;
    app.preview = Ui::Command(L"--preview") || Ui::Command(L"--render-preview");
    app.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!app.stopEvent)
        return 2;
    if (app.preview)
    {
        Bench::Device d;
        d.label = "PREVIEW DATA - Virtual inspection fixture";
        d.family = "HID";
        for (int i = 0; i < 120; ++i)
        {
            d.Report(1000 + static_cast<std::uint64_t>(i) * 4, 1000);
            d.Value("Left X", std::sin(i * 0.02) * 0.015, true);
            d.Value("Left Y", std::cos(i * 0.02) * 0.01, true);
        }
        app.devices.emplace("preview", d);
        app.status = L"PREVIEW DATA. No driver opened, no capture and no input sent.";
    }
    else
    {
        try
        {
            const auto driver = Interceptor::Get_Status();
            app.status = driver.version == IC_VERSION ? L"Driver responds. Click Start test to begin observation."
                                                      : L"Driver ABI does not match. Install the matching driver.";
        }
        catch (const std::exception &e)
        {
            app.status = L"Matching driver unavailable: " + Ui::Wide(e.what()) + L". Use Driver setup.";
        }
    }
    WNDCLASSEXW c{sizeof(c)};
    c.lpfnWndProc = Procedure;
    c.hInstance = instance;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.lpszClassName = L"TitanControllerRepairBench";
    RegisterClassExW(&c);
    HWND window =
        CreateWindowExW(0, c.lpszClassName, L"Controller Repair Bench", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 860, nullptr, nullptr, instance, &app);
    if (!window)
        return 3;
    if (Ui::Command(L"--render-preview"))
    {
        app.Refresh();
        const bool ok = Ui::Capture(window, Ui::ExeDirectory() / L"Preview.bmp");
        DestroyWindow(window);
        return ok ? 0 : 4;
    }
    ShowWindow(window, show);
    if (Ui::Command(L"--start") && !app.preview)
        PostMessageW(window, WM_COMMAND, 1, 0);
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return static_cast<int>(m.wParam);
}
