// vft-papertracker-bridge: streams the HTC VIVE Facial Tracker into PaperTracker without SRanipal.
//
// The tracker is activated directly over its UVC extension unit, captured with Media Foundation,
// and forwarded to PaperTracker as if it were a PaperTracker wireless face tracker (UDP, localhost).
#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include "jpeg_wic.h"
#include "log.h"
#include "pt_udp.h"
#include "vft_camera.h"

namespace {
using Clock = std::chrono::steady_clock;

constexpr const char* kVersion = "1.0.0";

struct Options {
    bool list = false;
    int dumpFrames = 0;
    std::wstring dumpDir = L".";
    bool right = false;  // which stereo view to forward
    int size = 400;      // output is size x size
    int rotate = 0;      // 0/90/180/270 clockwise
    bool flipH = false, flipV = false;
    double gamma = 1.0;
    int quality = 90;
    double fps = 60.0;
    int ir = 255;
    std::string host = "127.0.0.1";
    int port = 45454;
    std::wstring logFile;
    bool verbose = false;
    double runFor = 0;  // seconds; 0 = until stopped
    bool stopRunning = false;
};

constexpr const wchar_t* kInstanceMutex = L"Local\\VftPaperTrackerBridge";
constexpr const wchar_t* kStopEvent = L"Local\\VftPaperTrackerBridge.Stop";
constexpr const wchar_t* kConfigName = L"vft-papertracker-bridge.cfg";

std::atomic<bool> g_stop{false};
HANDLE g_doneEvent = nullptr;

BOOL WINAPI onConsoleCtrl(DWORD type) {
    g_stop = true;
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        // Returning lets Windows kill us; wait so the tracker gets switched off cleanly first.
        WaitForSingleObject(g_doneEvent, 4500);
    }
    return TRUE;
}

void usage() {
    printf(
        "vft-papertracker-bridge %s - HTC VIVE Facial Tracker -> PaperTracker, no SRanipal\n\n"
        "Usage: vft-papertracker-bridge [options]\n"
        "  --view left|right   stereo view to forward (default left)\n"
        "  --size N            output image size NxN, 64..400 (default 400)\n"
        "  --rotate DEG        rotate clockwise 0|90|180|270 (default 0)\n"
        "  --flip-h, --flip-v  mirror horizontally / vertically\n"
        "  --gamma G           brightness gamma, >1 brightens (default 1.0)\n"
        "  --ir N              IR illuminator level 0..255 (default 255)\n"
        "  --quality N         JPEG quality 1..100 (default 90)\n"
        "  --fps N             max frames per second sent (default 60)\n"
        "  --host IP --port N  PaperTracker address (default 127.0.0.1:45454)\n"
        "  --log FILE          also append log to FILE\n"
        "  --verbose           extra diagnostics\n"
        "  --run-for SECONDS   stop cleanly after this long (testing)\n"
        "  --list              list cameras and exit\n"
        "  --dump N [DIR]      capture N frames to DIR (raw + PGM) and exit\n"
        "  --stop              ask a running bridge (e.g. the background one) to stop cleanly\n\n"
        "Options are also read from %ls next to the executable (command line wins).\n",
        kVersion, kConfigName);
}

bool parseArgs(int argc, wchar_t** argv, Options& o) {
    auto num = [&](int& i, double lo, double hi, double& out) -> bool {
        if (i + 1 >= argc) return false;
        wchar_t* end = nullptr;
        out = wcstod(argv[++i], &end);
        return end && *end == 0 && out >= lo && out <= hi;
    };
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        double v = 0;
        if (a == L"--help" || a == L"-h" || a == L"/?") return false;
        else if (a == L"--list") o.list = true;
        else if (a == L"--stop") o.stopRunning = true;
        else if (a == L"--verbose") o.verbose = true;
        else if (a == L"--flip-h") o.flipH = true;
        else if (a == L"--flip-v") o.flipV = true;
        else if (a == L"--view" && i + 1 < argc) {
            std::wstring s = argv[++i];
            if (s == L"left") o.right = false;
            else if (s == L"right") o.right = true;
            else return false;
        } else if (a == L"--size") { if (!num(i, 64, 400, v)) return false; o.size = int(v); }
        else if (a == L"--rotate") {
            if (!num(i, 0, 270, v) || int(v) % 90 != 0) return false;
            o.rotate = int(v);
        } else if (a == L"--gamma") { if (!num(i, 0.1, 5, v)) return false; o.gamma = v; }
        else if (a == L"--ir") { if (!num(i, 0, 255, v)) return false; o.ir = int(v); }
        else if (a == L"--quality") { if (!num(i, 1, 100, v)) return false; o.quality = int(v); }
        else if (a == L"--fps") { if (!num(i, 1, 120, v)) return false; o.fps = v; }
        else if (a == L"--port") { if (!num(i, 1, 65535, v)) return false; o.port = int(v); }
        else if (a == L"--run-for") { if (!num(i, 1, 1e7, v)) return false; o.runFor = v; }
        else if (a == L"--host" && i + 1 < argc) {
            o.host.clear();
            for (const wchar_t* p = argv[++i]; *p; ++p) o.host.push_back(static_cast<char>(*p & 0x7f));
        } else if (a == L"--log" && i + 1 < argc) o.logFile = argv[++i];
        else if (a == L"--dump") {
            if (!num(i, 1, 10000, v)) return false;
            o.dumpFrames = int(v);
            if (i + 1 < argc && argv[i + 1][0] != L'-') o.dumpDir = argv[++i];
        } else return false;
    }
    return true;
}

// Crops one 400x400 view out of the 800x400 stereo frame and applies orientation, gamma and scaling.
class FrameProcessor {
public:
    explicit FrameProcessor(const Options& o) : o_(o) {
        for (int i = 0; i < 256; ++i) {
            lut_[i] = static_cast<uint8_t>(std::clamp(std::lround(255.0 * std::pow(i / 255.0, 1.0 / o.gamma)), 0L, 255L));
        }
    }

    // Returns a size x size image (row-major, stride == size).
    const std::vector<uint8_t>& process(const std::vector<uint8_t>& raw) {
        constexpr int N = 400;
        const int x0 = o_.right ? N : 0;
        view_.resize(N * N);
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                // Source coordinate for output (x, y) after flip + clockwise rotation.
                int sx = x, sy = y;
                switch (o_.rotate) {
                    case 90: sx = y; sy = N - 1 - x; break;
                    case 180: sx = N - 1 - x; sy = N - 1 - y; break;
                    case 270: sx = N - 1 - y; sy = x; break;
                    default: break;
                }
                if (o_.flipH) sx = N - 1 - sx;
                if (o_.flipV) sy = N - 1 - sy;
                view_[size_t(y) * N + x] = lut_[raw[size_t(sy) * VftCamera::kRawWidth + x0 + sx]];
            }
        }
        if (o_.size == N) return view_;
        // Area-averaging downscale (box filter) keeps the IR image smooth.
        const int S = o_.size;
        out_.resize(size_t(S) * S);
        const double scale = double(N) / S;
        for (int y = 0; y < S; ++y) {
            const int ya = int(y * scale), yb = std::max(ya + 1, int((y + 1) * scale));
            for (int x = 0; x < S; ++x) {
                const int xa = int(x * scale), xb = std::max(xa + 1, int((x + 1) * scale));
                unsigned sum = 0;
                for (int yy = ya; yy < yb; ++yy)
                    for (int xx = xa; xx < xb; ++xx) sum += view_[size_t(yy) * N + xx];
                out_[size_t(y) * S + x] = static_cast<uint8_t>(sum / unsigned((yb - ya) * (xb - xa)));
            }
        }
        return out_;
    }

private:
    const Options& o_;
    uint8_t lut_[256];
    std::vector<uint8_t> view_, out_;
};

const VideoDevice* findTracker(const std::vector<VideoDevice>& devs) {
    for (const auto& d : devs)
        if (d.isTracker) return &d;
    return nullptr;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

int runList() {
    auto devs = VftCamera::enumerate();
    printf("%zu video capture device(s):\n", devs.size());
    for (const auto& d : devs)
        printf("  %s %s\n      %s\n", d.isTracker ? "[VFT]" : "     ", narrow(d.name).c_str(), narrow(d.symlink).c_str());
    return findTracker(devs) ? 0 : 2;
}

bool writeFile(const std::wstring& path, const void* data, size_t len, const char* header = nullptr) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    if (header) fputs(header, f);
    fwrite(data, 1, len, f);
    fclose(f);
    return true;
}

int runDump(const Options& o) {
    auto devs = VftCamera::enumerate();
    const VideoDevice* dev = findTracker(devs);
    if (!dev) {
        logError("VIVE Facial Tracker (VID 0BB4 / PID 0321) not found");
        return 2;
    }
    CreateDirectoryW(o.dumpDir.c_str(), nullptr);
    VftCamera cam;
    std::string err;
    if (!cam.open(*dev, static_cast<uint8_t>(o.ir), err)) {
        logError("%s", err.c_str());
        return 3;
    }
    FrameProcessor proc(o);
    JpegEncoder jpeg;
    jpeg.init();
    RawFrame f;
    uint64_t last = 0;
    int saved = 0;
    const auto start = Clock::now();
    while (saved < o.dumpFrames && !g_stop) {
        if (!cam.waitFrame(f, last, 3000)) {
            logError("no frame within 3 s (stream error 0x%08lX)", static_cast<unsigned long>(cam.streamError()));
            break;
        }
        last = f.sequence;
        if (f.sequence < 30) continue;  // let auto-exposure settle
        wchar_t name[64];
        swprintf(name, 64, L"\\frame_%03d", saved);
        writeFile(o.dumpDir + name + L".pgm", f.data.data(), f.data.size(), "P5\n800 400\n255\n");
        const auto& img = proc.process(f.data);
        std::vector<uint8_t> enc;
        if (jpeg.encodeGray(img.data(), o.size, o.size, o.size, o.quality / 100.0f, enc))
            writeFile(o.dumpDir + name + L"_out.jpg", enc.data(), enc.size());
        ++saved;
    }
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    logInfo("dumped %d frame(s); received %llu frames in %.1f s (%.1f fps)", saved,
            static_cast<unsigned long long>(cam.framesReceived()), secs, cam.framesReceived() / secs);
    cam.close();
    return saved == o.dumpFrames ? 0 : 4;
}

int runBridge(const Options& o) {
    PaperTrackerSender sender;
    std::string err;
    if (!sender.open(o.host, static_cast<uint16_t>(o.port), err)) {
        logError("%s", err.c_str());
        return 3;
    }
    logInfo("Forwarding to PaperTracker at %s:%d (view=%s size=%d rotate=%d flip-h=%d flip-v=%d gamma=%.2f)",
            o.host.c_str(), o.port, o.right ? "right" : "left", o.size, o.rotate, o.flipH, o.flipV, o.gamma);

    FrameProcessor proc(o);
    JpegEncoder jpeg;
    if (!jpeg.init()) {
        logError("could not create WIC JPEG encoder");
        return 3;
    }
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / o.fps));
    // Frames arrive with jitter; accept one a little early so 60 fps output keeps up with a 60 fps camera.
    const auto minGap = interval * 3 / 4;
    auto sleepUnlessStopped = [](int ms) {
        for (int i = 0; i < ms / 100 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    bool waitingTrackerLogged = false;
    int ptState = -1;  // -1 unknown, 0 not running, 1 running (for transition logging)

    while (!g_stop) {
        // Only light up the tracker while PaperTracker is actually running.
        if (!sender.probe(300)) {
            if (ptState != 0) logInfo("Waiting for PaperTracker to start...");
            ptState = 0;
            sleepUnlessStopped(2000);
            continue;
        }
        if (ptState != 1) logInfo("PaperTracker is running");
        ptState = 1;

        auto devs = VftCamera::enumerate();
        const VideoDevice* devp = findTracker(devs);
        if (!devp) {
            if (!waitingTrackerLogged) logInfo("Waiting for the VIVE Facial Tracker to be connected...");
            waitingTrackerLogged = true;
            sleepUnlessStopped(2000);
            continue;
        }
        waitingTrackerLogged = false;
        const VideoDevice dev = *devp;

        VftCamera cam;
        if (!cam.open(dev, static_cast<uint8_t>(o.ir), err)) {
            logError("%s; retrying in 5 s", err.c_str());
            sleepUnlessStopped(5000);
            continue;
        }

        RawFrame f;
        uint64_t lastSeq = 0, sent = 0, sentBytes = 0, lastSent = 0, lastRecv = 0;
        int nudges = 0, missedProbes = 0;
        auto lastFrameAt = Clock::now(), lastSendAt = Clock::now() - interval, statsAt = Clock::now(),
             probeAt = Clock::now();
        std::vector<uint8_t> enc;
        while (!g_stop) {
            if (Clock::now() - probeAt > std::chrono::seconds(3)) {
                probeAt = Clock::now();
                missedProbes = sender.probe(200) ? 0 : missedProbes + 1;
                if (missedProbes >= 3) {
                    logInfo("PaperTracker closed - releasing the tracker");
                    ptState = 0;
                    break;
                }
            }
            if (!cam.waitFrame(f, lastSeq, 250)) {
                if (FAILED(cam.streamError())) {
                    logWarn("capture stream ended (0x%08lX) - device unplugged?",
                            static_cast<unsigned long>(cam.streamError()));
                    break;
                }
                const auto quiet = Clock::now() - lastFrameAt;
                // Some units only start once stream-on is repeated while video is already running.
                if (cam.framesReceived() == 0 && nudges < 5 && quiet > std::chrono::milliseconds(1000 * (nudges + 1))) {
                    ++nudges;
                    logInfo("no frames yet, re-sending stream-on (%d/5)", nudges);
                    std::string e;
                    if (!cam.xu().streamOn(e)) logWarn("%s", e.c_str());
                }
                if (quiet > std::chrono::seconds(6)) {
                    logWarn("no frames for 6 s, restarting capture");
                    break;
                }
                continue;
            }
            lastSeq = f.sequence;
            lastFrameAt = Clock::now();
            if (lastFrameAt - lastSendAt < minGap) continue;
            // Advance on a fixed schedule so jitter does not accumulate; resync after stalls.
            lastSendAt = (lastFrameAt - lastSendAt > interval * 2) ? lastFrameAt : lastSendAt + interval;

            const auto& img = proc.process(f.data);
            if (!jpeg.encodeGray(img.data(), o.size, o.size, o.size, o.quality / 100.0f, enc)) {
                logWarn("JPEG encode failed");
                continue;
            }
            sender.sendFrame(enc);
            ++sent;
            sentBytes += enc.size();

            if (Clock::now() - statsAt > std::chrono::seconds(10)) {
                const double secs = std::chrono::duration<double>(Clock::now() - statsAt).count();
                const uint64_t recv = cam.framesReceived();
                const auto logStats = o.verbose ? logInfo : logVerbose;
                logStats("camera %.1f fps, sent %.1f fps, avg JPEG %llu B", (recv - lastRecv) / secs,
                         (sent - lastSent) / secs,
                         static_cast<unsigned long long>(sent > lastSent ? sentBytes / (sent - lastSent) : 0));
                lastRecv = recv;
                lastSent = sent;
                sentBytes = 0;
                statsAt = Clock::now();
            }
        }
        cam.close();
        logInfo("Tracker released (IR off)");
        if (!g_stop) sleepUnlessStopped(1000);
    }
    return 0;
}
std::wstring exeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

// Tokens from the optional config file (whitespace separated, '#' comments, "quoted strings").
std::vector<std::wstring> readConfigArgs(const std::wstring& path) {
    std::vector<std::wstring> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt, ccs=UTF-8") != 0 || !f) return out;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring cur;
        bool quoted = false, have = false;
        for (const wchar_t* p = line; *p; ++p) {
            const wchar_t c = *p;
            if (c == L'"') { quoted = !quoted; have = true; }
            else if (!quoted && c == L'#') break;
            else if (!quoted && iswspace(c)) {
                if (have) out.push_back(cur);
                cur.clear();
                have = false;
            } else { cur.push_back(c); have = true; }
        }
        if (have) out.push_back(cur);
    }
    fclose(f);
    return out;
}

int stopRunningInstance() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEvent);
    HANDLE mtx = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutex);
    if (!ev || !mtx) {
        logInfo("no running bridge found");
        if (ev) CloseHandle(ev);
        if (mtx) CloseHandle(mtx);
        return 0;
    }
    SetEvent(ev);
    // The running instance holds the mutex until it has switched the tracker off and exited.
    const DWORD w = WaitForSingleObject(mtx, 10000);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) ReleaseMutex(mtx);
    CloseHandle(ev);
    CloseHandle(mtx);
    if (w == WAIT_TIMEOUT) {
        logError("the running bridge did not stop within 10 s");
        return 6;
    }
    logInfo("running bridge stopped");
    return 0;
}

LRESULT CALLBACK sessionWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_QUERYENDSESSION) return TRUE;
    if (msg == WM_ENDSESSION && wp) {
        // Logoff/shutdown: switch the tracker off before Windows terminates us.
        g_stop = true;
        WaitForSingleObject(g_doneEvent, 4500);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Hidden top-level window so the windowless build receives logoff/shutdown notifications.
void startSessionWatcher() {
    std::thread([] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = sessionWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VftPaperTrackerBridgeSession";
        RegisterClassW(&wc);
        HWND h = CreateWindowExW(0, wc.lpszClassName, L"VFT PaperTracker Bridge", WS_OVERLAPPED, 0, 0, 0, 0,
                                 nullptr, nullptr, wc.hInstance, nullptr);
        if (!h) return;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    }).detach();
}

std::wstring defaultBackgroundLog() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir = std::wstring(buf) + L"\\vft-papertracker-bridge";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = dir + L"\\bridge.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 2u * 1024 * 1024) {
        MoveFileExW(file.c_str(), (dir + L"\\bridge.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return file;
}

int bridgeMain(int argc, wchar_t** argv, bool background) {
    // Config-file options first so command-line options override them.
    std::vector<std::wstring> cfg = readConfigArgs(exeDir() + kConfigName);
    std::vector<wchar_t*> args{argv[0]};
    for (auto& s : cfg) args.push_back(s.data());
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    logSetConsole(!background);
    Options o;
    const bool argsOk = parseArgs(static_cast<int>(args.size()), args.data(), o);
    if (background && o.logFile.empty()) o.logFile = defaultBackgroundLog();
    if (!o.logFile.empty()) logInit(o.logFile.c_str());
    if (!argsOk) {
        if (background) logError("invalid options (check %ls)", kConfigName);
        else usage();
        return 1;
    }
    logSetVerbose(o.verbose);
    if (o.stopRunning) return stopRunningInstance();

    HANDLE single = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!o.list && GetLastError() == ERROR_ALREADY_EXISTS) {
        logError("another instance of the bridge is already running (stop it with --stop)");
        return 5;
    }

    g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (background) startSessionWatcher();
    else SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
    if (!o.list) {
        HANDLE stopEv = CreateEventW(nullptr, TRUE, FALSE, kStopEvent);
        std::thread([stopEv] {
            WaitForSingleObject(stopEv, INFINITE);
            logInfo("stop requested");
            g_stop = true;
        }).detach();
    }
    logInfo("vft-papertracker-bridge %s started%s", kVersion, background ? " (background)" : "");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    if (o.runFor > 0) {
        std::thread([secs = o.runFor] {
            std::this_thread::sleep_for(std::chrono::duration<double>(secs));
            g_stop = true;
        }).detach();
    }

    int rc;
    if (o.list) rc = runList();
    else if (o.dumpFrames > 0) rc = runDump(o);
    else rc = runBridge(o);

    MFShutdown();
    CoUninitialize();
    WSACleanup();
    logInfo("exit (%d)", rc);
    if (single) {
        ReleaseMutex(single);
        CloseHandle(single);
    }
    SetEvent(g_doneEvent);
    return rc;
}
}  // namespace

#ifdef BRIDGE_BACKGROUND
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return bridgeMain(__argc, __wargv, true); }
#else
int wmain(int argc, wchar_t** argv) { return bridgeMain(argc, argv, false); }
#endif
