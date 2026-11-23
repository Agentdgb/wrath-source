

#include "aimassist_debug.h"

#include "platform/Win32Process.h"
#include "util/Log.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <windows.h>

namespace vanta::modules::aimassist::debug {

namespace {

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_sessionActive{false};
std::atomic<const char*> g_workerStage{"not observed"};
std::atomic<const char*> g_renderStage{"not observed"};
std::atomic<long long> g_workerHeartbeat{0};
long long g_lastFrameStatus = 0; // protected by g_mx
std::mutex        g_mx;
FILE*             g_file = nullptr;
std::string       g_path;
std::wstring      g_wpath;

struct TickTracker {
    bool    havePrev = false;
    bool    haveBucket = false;
    long long bucketStartMs = 0;
    float   bucketYaw = 0.f, bucketPitch = 0.f;
    float   prevYaw = 0.f, prevPitch = 0.f;
    int     frames = 0;

    static float wrap(float d) {
        d = std::fmod(d + 180.f, 360.f);
        if (d < 0.f) d += 360.f;
        return d - 180.f;
    }

    bool sample(long long nowMs, float liveYaw, float livePitch,
                float& outYaw, float& outPitch, int& outFrames)
    {
        if (!havePrev) {
            prevYaw = liveYaw; prevPitch = livePitch;
            havePrev = true;
            return false;
        }
        if (!haveBucket) {
            bucketStartMs = nowMs;
            bucketYaw = liveYaw; bucketPitch = livePitch;
            frames = 0;
            haveBucket = true;
        }
        ++frames;
        const bool boundary = (nowMs - bucketStartMs) >= 50;
        bool out = false;
        if (boundary) {
            outYaw    = wrap(liveYaw - bucketYaw);
            outPitch  = livePitch - bucketPitch;
            outFrames = frames;
            bucketStartMs = nowMs;
            bucketYaw = liveYaw; bucketPitch = livePitch;
            frames = 0;
            out = true;
        }
        prevYaw = liveYaw; prevPitch = livePitch;
        return out;
    }
};

TickTracker g_tick;
float g_quantum = 0.15f;

long long g_switchUntil = 0;
float    g_lastDiagTgtYaw = 0.f;
bool     g_haveDiagTgtYaw = false;
int      g_burstSwitch = 0, g_burstSteady = 0;
double   g_backlogAccum = 0.0; int g_backlogN = 0;
int      g_stepsFrame[64]; int g_stepsFrameN = 0;
int      g_pFrame[64];
int      g_pBursts = 0;
double   g_absStepsAccum = 0.0; int g_absStepsN = 0;
float    g_eyeMin = 1e9f, g_eyeMax = -1e9f;
float    g_tPitchMin = 1e9f, g_tPitchMax = -1e9f;

long long g_lastBurstMs = 0;
int      g_gapsTotal = 0, g_gapsTick = 0, g_gapsHunt = 0;

std::atomic<int>   g_optsMode{0};
std::atomic<int>   g_optsVert{0};
std::atomic<int>   g_optsMouseDown{0};
std::atomic<float> g_optsH{5.f};
std::atomic<float> g_optsV{5.f};
long long g_optsStart = 0;

constexpr int kHist = 40;
float g_histYaw[kHist] = {0}, g_histPitch[kHist] = {0};
int   g_histN = 0, g_histHead = 0;

void pushHist(float dy, float dp)
{
    g_histYaw[g_histHead] = dy;
    g_histPitch[g_histHead] = dp;
    g_histHead = (g_histHead + 1) % kHist;
    if (g_histN < kHist) ++g_histN;
}

bool onGrid(float v)
{
    const float m = std::round(v / g_quantum);
    return std::fabs(v - m * g_quantum) < 3e-4f;
}

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void writeLineLocked(const char* s)
{
    if (!g_file) return;
    std::fprintf(g_file, "%s\n", s);
}

void analyzeLocked(long long now)
{
    static long long last = 0;
    if (last == 0) { last = now; return; }
    if (now - last < 2000) return;
    last = now;
    if (g_histN < 6) return;

    for (int axis = 0; axis < 2; ++axis) {
        const float* h = axis == 0 ? g_histYaw : g_histPitch;
        const char* name = axis == 0 ? "yaw" : "pitch";

        float seq[kHist];
        int n = g_histN;
        for (int i = 0; i < n; ++i)
            seq[i] = h[(g_histHead - n + i + kHist * 2) % kHist];

        double sum = 0, sum2 = 0;
        float mn = seq[0], mx = seq[0];
        int gridOk = 0;
        for (int i = 0; i < n; ++i) {
            sum  += seq[i];
            sum2 += (double)seq[i] * (double)seq[i];
            const float v = seq[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            if (onGrid(v)) ++gridOk;
        }
        const double mean = sum / n;
        const double var  = sum2 / n - mean * mean;
        const double std_ = var > 0 ? std::sqrt(var) : 0.0;

        int bestRun = 1, run = 1;
        for (int i = 1; i < n; ++i) {
            if (std::fabs(seq[i] - seq[i - 1]) < 0.02f) { ++run; if (run > bestRun) bestRun = run; }
            else run = 1;
        }

        double lrSum = 0, lrSum2 = 0; int lrN = 0;
        int signFlips = 0;
        for (int i = 1; i < n; ++i) {
            const float a = std::fabs(seq[i - 1]), b = std::fabs(seq[i]);
            if (a > 0.02f && b > 0.02f) {
                const double lr = std::log((double)b / (double)a);
                lrSum += lr; lrSum2 += lr * lr; ++lrN;
            }
            if (seq[i] * seq[i - 1] < 0.f) ++signFlips;
        }
        const double lrMean = lrN ? lrSum / lrN : 0.0;
        const double lrStd  = lrN ? std::sqrt(std::max(0.0, lrSum2 / lrN - lrMean * lrMean)) : 0.0;

        char line[512];
        std::snprintf(line, sizeof(line),
            "ANALYZE %s n=%d mean=%.3f std=%.3f min=%.3f max=%.3f "
            "eqRun=%d ratioStd=%.3f grid=%d%% flips=%d%s%s",
            name, n, mean, std_, mn, mx, bestRun, lrStd,
            100 * gridOk / n, signFlips,
            bestRun >= 4 ? " WARN_CONSTANT" : "",
            (lrN >= 5 && lrStd < 0.10) ? " WARN_LINEAR" : "");
        writeLineLocked(line);
    }

    {

        int tmp[64];
        const int m = g_stepsFrameN < 64 ? g_stepsFrameN : 64;
        for (int i = 0; i < m; ++i) tmp[i] = g_stepsFrame[i];
        for (int i = 1; i < m; ++i) {
            const int v = tmp[i]; int j = i - 1;
            while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
            tmp[j + 1] = v;
        }
        const int p95 = m ? tmp[(m * 95) / 100 > 0 ? (m * 95) / 100 - 1 : 0] : 0;
        const int mx  = m ? tmp[m - 1] : 0;
        const double avgBacklog = g_backlogN ? g_backlogAccum / g_backlogN : 0.0;
        const double avgAbs     = g_absStepsN ? g_absStepsAccum / g_absStepsN : 0.0;

        int ptmp[64];
        for (int i = 0; i < 64; ++i) ptmp[i] = g_pFrame[i];
        for (int i = 1; i < 64; ++i) {
            const int v = ptmp[i]; int j = i - 1;
            while (j >= 0 && ptmp[j] > v) { ptmp[j + 1] = ptmp[j]; --j; }
            ptmp[j + 1] = v;
        }
        const int pP95_t = ptmp[63 - (64 * 5) / 100];
        const int pMax = ptmp[63];

        char diag[256];
        std::snprintf(diag, sizeof(diag),
            "DIAG frames=%d avgAbs=%.1f p95=%d max=%d backlog=%.1f "
            "burstsSwitch=%d burstsSteady=%d | pitch p95=%d max=%d bursts=%d",
            g_backlogN, avgAbs, p95, mx, avgBacklog, g_burstSwitch, g_burstSteady,
            pP95_t, pMax, g_pBursts);
        writeLineLocked(diag);

        const float eyeRange    = (g_eyeMax > g_eyeMin) ? g_eyeMax - g_eyeMin : 0.f;
        const float tPitchRange = (g_tPitchMax > g_tPitchMin) ? g_tPitchMax - g_tPitchMin : 0.f;

        const int tickLock = g_gapsTotal >= 5
                           ? 100 * g_gapsTick / g_gapsTotal : 0;
        const int huntLock = g_gapsTotal >= 5
                           ? 100 * g_gapsHunt / g_gapsTotal : 0;
        char diag2[224];
        std::snprintf(diag2, sizeof(diag2),
            "DIAG2 eyeRange=%.2f tPitchRange=%.1f tickLock=%d%% huntLock=%d%% (bursts=%d)",
            eyeRange, tPitchRange, tickLock, huntLock, g_gapsTotal);
        writeLineLocked(diag2);

        const char* advice = nullptr;
        if (g_backlogN < 60) {
            advice = "ADVICE window too short — fight longer";
        } else if (g_pBursts >= 6 && pP95_t >= 12) {
            advice = "ADVICE PITCH_WAVES: el eje vertical sigue oscilando — pega el "
                     "archivo (lead slew activo; si persiste, es el pVel)";
        } else if (huntLock >= 40 && g_gapsTotal >= 8) {
            advice = "ADVICE HUNT: ciclo limite ~5Hz en el lazo del controller — "
                     "pega el archivo, es un termino concreto que ajustar";
        } else if (tickLock >= 60 && g_gapsTotal >= 5) {
            advice = "ADVICE TICK_STAIRCASE: ráfagas clavadas al tick de 20Hz — "
                     "el filtro de reconstrucción debería haberlo matado; si ves "
                     "esto, pégame el archivo";
        } else if (eyeRange > 0.35f && tPitchRange > 25.f) {
            advice = "ADVICE JUMP_WHIPSAW: saltando con vertical ON mueve el angulo "
                     "+-20 por salto — enciende Jump compensate (o salta menos, o "
                     "baja vSpeed)";
        } else if (avgBacklog > 25.0) {
            advice = "ADVICE SMOOTHER_STARVING: backlog alto — BAJA Emission smoothing "
                     "2 puntos (el drain no alcanza al incoming)";
        } else if (g_burstSteady >= 4) {
            advice = "ADVICE LIMIT_CYCLE: bursts con target estable (chatter de "
                     "yawVelocity) — SUBE Emission smoothing a 7-8; si persiste, "
                     "baja Horizontal speed 2 puntos";
        } else if (g_burstSwitch >= 2) {
            advice = "ADVICE SWITCH_BURSTS: catch-up al cambiar target (normal) — "
                     "SUBE Emission smoothing a 8 para repartir el swing";
        } else if (p95 <= 6 && avgAbs <= 3.0) {
            advice = "ADVICE CLEAN: stream suave, sin accion";
        } else {
            advice = "ADVICE OK: bursts aislados, sin firma de jitter";
        }
        writeLineLocked(advice);
    }
    g_burstSwitch = 0; g_burstSteady = 0;
    g_backlogAccum = 0.0; g_backlogN = 0;
    g_absStepsAccum = 0.0; g_absStepsN = 0;
    g_stepsFrameN = 0;
    g_eyeMin = 1e9f; g_eyeMax = -1e9f;
    g_tPitchMin = 1e9f; g_tPitchMax = -1e9f;
    g_lastBurstMs = 0;
    g_gapsTotal = 0; g_gapsTick = 0; g_gapsHunt = 0;
    g_pBursts = 0;
    for (int i = 0; i < 64; ++i) g_pFrame[i] = 0;

    std::fflush(g_file);
}

}

void setEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool active() { return g_sessionActive.load(std::memory_order_acquire); }

void workerStage(const char* stage)
{
    if (active()) g_workerStage.store(stage, std::memory_order_relaxed);
}

void renderStage(const char* stage)
{
    if (active()) g_renderStage.store(stage, std::memory_order_relaxed);
}

void frameStatus(const char* stage, bool workerRunning, bool moduleEnabled)
{
    if (!active()) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_file) return;
    const auto now = nowMs();
    if (g_lastFrameStatus && now - g_lastFrameStatus < 1000) return;
    g_lastFrameStatus = now;
    const auto heartbeat = g_workerHeartbeat.load(std::memory_order_relaxed);
    char line[320];
    std::snprintf(line, sizeof(line),
        "STATUS t=%lld render=%s worker=%s running=%d enabled=%d workerAgeMs=%lld",
        now, stage ? stage : g_renderStage.load(std::memory_order_relaxed),
        g_workerStage.load(std::memory_order_relaxed),
        static_cast<int>(workerRunning), static_cast<int>(moduleEnabled),
        heartbeat ? now - heartbeat : -1LL);
    writeLineLocked(line);
    std::fflush(g_file);
}

void sessionOpen(int mode, float hSpeed, float vSpeed, bool aimVertically,
                 bool requireMouseDown, float sensitivity)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_sessionActive.store(false, std::memory_order_release);
    if (g_file) std::fclose(g_file), g_file = nullptr;
    g_lastFrameStatus = 0;
    g_workerStage.store("not observed", std::memory_order_relaxed);
    g_renderStage.store("not observed", std::memory_order_relaxed);

    /* Beside the client's own log rather than on the Desktop. A module
       does not get to drop files where the user keeps their things. */
    const std::wstring dir = vanta::platform::logDir();
    g_wpath = dir + L"\\vanta_aim_debug.txt";
    g_path.assign(dir.begin(), dir.end());
    g_path += "\\vanta_aim_debug.txt";
    _wfopen_s(&g_file, g_wpath.c_str(), L"w");

    g_tick = TickTracker{};
    g_histN = 0; g_histHead = 0;
    g_optsStart = 0;
    g_switchUntil = 0;
    g_lastDiagTgtYaw = 0.f; g_haveDiagTgtYaw = false;
    g_burstSwitch = 0; g_burstSteady = 0;
    g_backlogAccum = 0.0; g_backlogN = 0;
    g_absStepsAccum = 0.0; g_absStepsN = 0;
    g_stepsFrameN = 0;
    g_eyeMin = 1e9f; g_eyeMax = -1e9f;
    g_tPitchMin = 1e9f; g_tPitchMax = -1e9f;
    const float base = sensitivity * 0.6f + 0.2f;
    g_quantum = base * base * base * 8.0f * 0.15f;

    if (!g_file) {
        VLOG_WARN("aimassist", "debug file could not be created at {}", g_path);
        return;
    }
    std::setvbuf(g_file, nullptr, _IOFBF, 64 * 1024);

    const float f3 = base * base * base * 8.0f;
    char line[512];
    std::snprintf(line, sizeof(line),
        "SESSION t=%lld mode=%s hSpeed=%.1f vSpeed=%.1f aimVertically=%d "
        "requireMouseDown=%d sens=%.4f f3=%.5f quantum=%.5f deg",
        nowMs(), mode == 0 ? "Simple" : "Adaptive", hSpeed, vSpeed,
        (int)aimVertically, (int)requireMouseDown, sensitivity, f3, g_quantum);
    writeLineLocked(line);
    VLOG_INFO("aimassist", "debug file open: {}", g_path);
    g_sessionActive.store(true, std::memory_order_release);
}

void sessionClose()
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_sessionActive.store(false, std::memory_order_release);
    if (g_file) { std::fflush(g_file); std::fclose(g_file); g_file = nullptr; }
}

void resolveLine(const char* text)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_file) return;
    char line[512];
    std::snprintf(line, sizeof(line), "RESOLVE %s", text);
    writeLineLocked(line);
    std::fflush(g_file);
}

void workerTick(double realIntervalMs)
{
    g_workerHeartbeat.store(nowMs(), std::memory_order_relaxed);
    static long long n = 0;
    static double acc = 0.0;
    static long long lastLog = 0;
    ++n; acc += realIntervalMs;
    const long long now = nowMs();
    if (lastLog == 0) lastLog = now;
    if (now - lastLog >= 1000) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_file) {
            char line[160];
            std::snprintf(line, sizeof(line), "CADENCE t=%lld ticks=%lld avg=%.3f ms",
                          now, n, n ? acc / n : 0.0);
            writeLineLocked(line);
        }
        n = 0; acc = 0.0; lastLog = now;
    }
}

void emit(float yawSteps, float pitchSteps, float pendYaw, float pendPitch,
          float liveYaw, float livePitch, float gapYaw, float gapPitch)
{
    float dYaw = 0.f, dPitch = 0.f;
    int frames = 0;
    const bool boundary =
        g_tick.sample(nowMs(), liveYaw, livePitch, dYaw, dPitch, frames);
    if (boundary) pushHist(dYaw, dPitch);

    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_file) return;

    char line[320];
    std::snprintf(line, sizeof(line),
        "EMIT t=%lld ySteps=%+.0f pSteps=%+.0f pendY=%+.3f pendP=%+.3f "
        "yaw=%.3f pitch=%.3f gapY=%+.3f gapP=%+.3f",
        nowMs(), yawSteps, pitchSteps, pendYaw, pendPitch,
        liveYaw, livePitch, gapYaw, gapPitch);
    writeLineLocked(line);

    {
        const float ay = std::fabs(yawSteps);
        if (ay >= 15.f) {
            const long long bn = nowMs();
            if (nowMs() < g_switchUntil) ++g_burstSwitch;
            else                         ++g_burstSteady;
            if (g_lastBurstMs != 0) {
                const long long gap = bn - g_lastBurstMs;
                ++g_gapsTotal;
                if (gap >= 40 && gap <= 62) ++g_gapsTick;
                if (gap >= 130 && gap <= 260) ++g_gapsHunt;
            }
            g_lastBurstMs = bn;
        }
        g_backlogAccum += std::fabs(pendYaw);
        ++g_backlogN;
        g_absStepsAccum += ay;
        ++g_absStepsN;
        if (g_stepsFrameN < 64) g_stepsFrame[g_stepsFrameN++] = static_cast<int>(ay);
        else {
            for (int i = 1; i < 64; ++i) g_stepsFrame[i - 1] = g_stepsFrame[i];
            g_stepsFrame[63] = static_cast<int>(ay);
        }
        {
            const int ap = static_cast<int>(std::fabs(pitchSteps));
            if (ap >= 15) ++g_pBursts;
            for (int i = 1; i < 64; ++i) g_pFrame[i - 1] = g_pFrame[i];
            g_pFrame[63] = ap;
        }
    }

    if (boundary) {
        char tline[192];
        std::snprintf(tline, sizeof(tline),
            "TICK t=%lld dyaw=%+.4f dpitch=%+.4f frames=%d qY=%c qP=%c",
            nowMs(), dYaw, dPitch, frames,
            onGrid(dYaw) ? 'Y' : 'N', onGrid(dPitch) ? 'Y' : 'N');
        writeLineLocked(tline);
    }

    analyzeLocked(nowMs());
    static int flushCounter = 0;
    if ((++flushCounter & 15) == 0) std::fflush(g_file);
}

void ctrlAdaptive(const AdaptiveSnap& s)
{
    static long long last = 0;
    const long long now = nowMs();

    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_haveDiagTgtYaw) {
            const float d = g_lastDiagTgtYaw - s.targetYaw;
            if (std::fabs(d) > 15.f) g_switchUntil = now + 150;
        }
        g_lastDiagTgtYaw = s.targetYaw;
        g_haveDiagTgtYaw = true;
    }

    if (now - last < 100) return;
    last = now;

    {

        std::lock_guard<std::mutex> lk(g_mx);
        if (s.playerEyeY < g_eyeMin) g_eyeMin = s.playerEyeY;
        if (s.playerEyeY > g_eyeMax) g_eyeMax = s.playerEyeY;
        if (s.targetPitch < g_tPitchMin) g_tPitchMin = s.targetPitch;
        if (s.targetPitch > g_tPitchMax) g_tPitchMax = s.targetPitch;
    }

    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_file) return;
    char line[640];
    std::snprintf(line, sizeof(line),
        "CTRL t=%lld dt=%.4f str=%.3f tYaw=%.2f tPitch=%.2f yErr=%+.3f pErr=%+.3f "
        "yRate=%+.1f pRate=%+.1f pendY=%+.2f pendP=%+.2f yBias=%+.2f pBias=%+.2f "
        "yVel=%+.2f pVel=%+.2f leadY=%+.3f vLead=%+.3f aimY=%.2f adjY=%.2f eyeY=%.2f "
        "driftP=%+.2f driftV=%+.2f dist=%.2f snap=%d%d aEy=%.2f "
        "cmdY=%+.1f cmdP=%+.1f pRY=%+.1f pRP=%+.1f",
        now, s.dt, s.aimStrength, s.targetYaw, s.targetPitch, s.yawError, s.pitchError,
        s.yawRate, s.pitchRate, s.pendingYaw, s.pendingPitch, s.yawBias, s.pitchBias,
        s.yawVelocity, s.pitchVelocity, s.leadY, s.verticalLead, s.aimY, s.adjustedAimY,
        s.playerEyeY, s.driftPos, s.driftVelocity, s.targetDistance,
        (int)s.snapYaw, (int)s.snapPitch, s.aimEyeY,
        s.cmdRateYaw, s.cmdRatePitch, s.playerYawRate, s.playerPitchRate);
    writeLineLocked(line);
}

[[maybe_unused]] static void ctrlSimple_unused(const SimpleSnap& s)
{
    static long long last = 0;
    const long long now = nowMs();
    if (now - last < 100) return;
    last = now;

    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_file) return;
    char line[512];
    std::snprintf(line, sizeof(line),
        "SCTRL t=%lld angH=%.2f angV=%d accH=%+.2f accV=%+.2f velH=%+.3f velV=%+.3f "
        "bufH=%+.3f bufV=%+.3f fH=%.2f fV=%.2f motX=%+.3f motZ=%+.3f strafe=%+.2f dist=%.2f",
        now, s.angDiffH, (int)s.vertAngDiff, s.accH, s.accV, s.velH, s.velV,
        s.bufH, s.bufV, s.forceH, s.forceV, s.targetMotionX, s.targetMotionZ,
        s.strafe, s.distance);
    writeLineLocked(line);
}

void opts(int mode, float hSpeed, float vSpeed, bool aimVertically, bool requireMouseDown)
{
    g_optsMode.store(mode, std::memory_order_relaxed);
    g_optsH.store(hSpeed, std::memory_order_relaxed);
    g_optsV.store(vSpeed, std::memory_order_relaxed);
    g_optsVert.store(aimVertically ? 1 : 0, std::memory_order_relaxed);
    g_optsMouseDown.store(requireMouseDown ? 1 : 0, std::memory_order_relaxed);
}

void setQuantum(float quantum)
{
    if (quantum > 0.001f && quantum < 2.f) g_quantum = quantum;
}

void optsTick()
{
    if (!active()) return;
    const long long now = nowMs();
    if (g_optsStart == 0) { g_optsStart = now; return; }
    if (now - g_optsStart < 1000) return;
    g_optsStart = now;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_file) return;
    char line[192];
    std::snprintf(line, sizeof(line),
        "OPTS t=%lld mode=%s hSpeed=%.1f vSpeed=%.1f aimVertically=%d requireMouseDown=%d",
        now, g_optsMode.load(std::memory_order_relaxed) == 0 ? "Simple" : "Adaptive",
        g_optsH.load(std::memory_order_relaxed),
        g_optsV.load(std::memory_order_relaxed),
        g_optsVert.load(std::memory_order_relaxed),
        g_optsMouseDown.load(std::memory_order_relaxed));
    writeLineLocked(line);
}

}
