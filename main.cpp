// ObliQ - Professional Audio Processor
// WASAPI + Win32 GDI

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>
#include <comdef.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static const int   SAMPLE_RATE  = 48000;
static const int   CHANNELS     = 2;
static const int   BLOCK_SIZE   = 512;
static const int   FFT_SIZE     = 2048;
static const int   RING_FRAMES  = 65536;
static const float PI           = 3.14159265358979323846f;

struct RingBuffer {
    float  data[RING_FRAMES * 2];
    std::atomic<int> wpos{0};
    std::atomic<int> rpos{0};

    void reset() { wpos.store(0); rpos.store(0); }

    int available_read() const {
        int w = wpos.load(std::memory_order_acquire);
        int r = rpos.load(std::memory_order_relaxed);
        return (w - r + RING_FRAMES) & (RING_FRAMES - 1);
    }

    int write(const float* src, int frames) {
        int w = wpos.load(std::memory_order_relaxed);
        int r = rpos.load(std::memory_order_acquire);
        int avail = (RING_FRAMES - 1 - ((w - r + RING_FRAMES) & (RING_FRAMES - 1)));
        if (frames > avail) frames = avail;
        for (int i = 0; i < frames; i++) {
            int idx = ((w + i) & (RING_FRAMES - 1)) * 2;
            data[idx]   = src[i * 2];
            data[idx+1] = src[i * 2 + 1];
        }
        wpos.store((w + frames) & (RING_FRAMES - 1), std::memory_order_release);
        return frames;
    }

    int read(float* dst, int frames) {
        int r = rpos.load(std::memory_order_relaxed);
        int w = wpos.load(std::memory_order_acquire);
        int avail = (w - r + RING_FRAMES) & (RING_FRAMES - 1);
        if (frames > avail) { memset(dst, 0, frames * 2 * sizeof(float)); return 0; }
        for (int i = 0; i < frames; i++) {
            int idx = ((r + i) & (RING_FRAMES - 1)) * 2;
            dst[i * 2]   = data[idx];
            dst[i * 2+1] = data[idx+1];
        }
        rpos.store((r + frames) & (RING_FRAMES - 1), std::memory_order_release);
        return frames;
    }
};

struct Biquad {
    double b0=1,b1=0,b2=0,a1=0,a2=0;
    double z1L=0,z2L=0,z1R=0,z2R=0;

    void reset_state() { z1L=z2L=z1R=z2R=0; }

    void lowshelf(double fc, double gain_db, double S=1.0) {
        double A  = pow(10.0, gain_db / 40.0);
        double w0 = 2*PI*fc/SAMPLE_RATE;
        double cosw = cos(w0), sinw = sin(w0);
        double alpha = sinw/2.0 * sqrt((A+1/A)*(1/S-1)+2);
        double ap1 = A+1, am1 = A-1;
        double b0_ = A*((ap1 - am1*cosw) + 2*sqrt(A)*alpha);
        double b1_ = 2*A*(am1 - ap1*cosw);
        double b2_ = A*((ap1 - am1*cosw) - 2*sqrt(A)*alpha);
        double a0_ =    (ap1 + am1*cosw) + 2*sqrt(A)*alpha;
        double a1_ =-2*   (am1 + ap1*cosw);
        double a2_ =       (ap1 + am1*cosw) - 2*sqrt(A)*alpha;
        b0=b0_/a0_; b1=b1_/a0_; b2=b2_/a0_; a1=a1_/a0_; a2=a2_/a0_;
    }

    void highshelf(double fc, double gain_db, double S=1.0) {
        double A  = pow(10.0, gain_db / 40.0);
        double w0 = 2*PI*fc/SAMPLE_RATE;
        double cosw = cos(w0), sinw = sin(w0);
        double alpha = sinw/2.0 * sqrt((A+1/A)*(1/S-1)+2);
        double ap1 = A+1, am1 = A-1;
        double b0_ = A*((ap1 + am1*cosw) + 2*sqrt(A)*alpha);
        double b1_ =-2*A*(am1 + ap1*cosw);
        double b2_ = A*((ap1 + am1*cosw) - 2*sqrt(A)*alpha);
        double a0_ =    (ap1 - am1*cosw) + 2*sqrt(A)*alpha;
        double a1_ = 2*    (am1 - ap1*cosw);
        double a2_ =       (ap1 - am1*cosw) - 2*sqrt(A)*alpha;
        b0=b0_/a0_; b1=b1_/a0_; b2=b2_/a0_; a1=a1_/a0_; a2=a2_/a0_;
    }

    void peaking(double fc, double gain_db, double Q=1.0) {
        double A  = pow(10.0, gain_db / 40.0);
        double w0 = 2*PI*fc/SAMPLE_RATE;
        double alpha = sin(w0)/(2*Q);
        b0 = 1 + alpha*A;
        b1 = -2*cos(w0);
        b2 = 1 - alpha*A;
        double a0 = 1 + alpha/A;
        a1 = -2*cos(w0)/a0;
        a2 = (1 - alpha/A)/a0;
        b0/=a0; b1/=a0; b2/=a0;
    }

    void highpass(double fc, double Q=0.707) {
        double w0 = 2*PI*fc/SAMPLE_RATE;
        double alpha = sin(w0)/(2*Q);
        double cosw = cos(w0);
        double a0 = 1+alpha;
        b0 = (1+cosw)/2/a0;
        b1 = -(1+cosw)/a0;
        b2 = (1+cosw)/2/a0;
        a1 = -2*cosw/a0;
        a2 = (1-alpha)/a0;
    }

    void lowpass(double fc, double Q=0.707) {
        double w0 = 2*PI*fc/SAMPLE_RATE;
        double alpha = sin(w0)/(2*Q);
        double cosw = cos(w0);
        double a0 = 1+alpha;
        b0 = (1-cosw)/2/a0;
        b1 = (1-cosw)/a0;
        b2 = (1-cosw)/2/a0;
        a1 = -2*cosw/a0;
        a2 = (1-alpha)/a0;
    }

    inline float processSampleL(float x) {
        double y = b0*x + z1L;
        z1L = b1*x - a1*y + z2L;
        z2L = b2*x - a2*y;
        return (float)y;
    }
    inline float processSampleR(float x) {
        double y = b0*x + z1R;
        z1R = b1*x - a1*y + z2R;
        z2R = b2*x - a2*y;
        return (float)y;
    }
};

struct Compressor {
    float env   = 0.f;
    float att   = 0.f;
    float rel   = 0.f;
    float thr   = 1.f;
    float ratio = 4.f;
    float makeup= 1.f;

    void configure(float thr_lin, float ratio_, float att_ms, float rel_ms, float makeup_lin) {
        thr   = thr_lin;
        ratio = ratio_;
        makeup= makeup_lin;
        att = expf(-1.f / (SAMPLE_RATE * att_ms / 1000.f));
        rel = expf(-1.f / (SAMPLE_RATE * rel_ms / 1000.f));
    }

    inline float processSample(float x) {
        float absX = fabsf(x);
        float coeff = (absX > env) ? att : rel;
        env = coeff * env + (1.f - coeff) * absX;
        float gain = makeup;
        if (env > thr) {
            gain *= powf(env / thr, 1.f / ratio - 1.f);
        }
        return x * gain;
    }
};

inline float hardclip(float x, float ceiling = 0.98f) {
    if (x >  ceiling) return  ceiling;
    if (x < -ceiling) return -ceiling;
    return x;
}

inline float softclip(float x) {
    float x2 = x*x;
    return x*(27.f+x2)/(27.f+9.f*x2);
}

static const int REV_TAPS = 7;
static const int REV_BUF  = 65536;

struct Reverb {
    float bufL[REV_BUF] = {};
    float bufR[REV_BUF] = {};
    int   head = 0;

    float tap_ms[REV_TAPS]   = {23.4f, 41.7f, 63.1f, 84.3f, 107.2f, 131.8f, 158.4f};
    float tap_gainL[REV_TAPS]= {0.7f, 0.55f, 0.45f, 0.38f, 0.30f, 0.22f, 0.15f};
    float tap_gainR[REV_TAPS]= {0.6f, 0.60f, 0.40f, 0.42f, 0.25f, 0.28f, 0.12f};

    float lpf_c = 0.6f;
    float lpf_l = 0.f, lpf_r = 0.f;

    void configure(float wall_cutoff_norm, float decay) {
        lpf_c = wall_cutoff_norm;
        float base_l[] = {0.7f, 0.55f, 0.45f, 0.38f, 0.30f, 0.22f, 0.15f};
        float base_r[] = {0.6f, 0.60f, 0.40f, 0.42f, 0.25f, 0.28f, 0.12f};
        for (int t = 0; t < REV_TAPS; t++) {
            tap_gainL[t] = base_l[t] * decay;
            tap_gainR[t] = base_r[t] * decay;
        }
    }

    inline void processSample(float inL, float inR, float& outL, float& outR) {
        float accL = 0.f, accR = 0.f;
        for (int t = 0; t < REV_TAPS; t++) {
            int delay_smp = (int)(tap_ms[t] * SAMPLE_RATE / 1000.f);
            int idx = (head - delay_smp) & (REV_BUF - 1);
            accL += bufL[idx] * tap_gainL[t];
            accR += bufR[idx] * tap_gainR[t];
        }
        lpf_l = lpf_c * lpf_l + (1.f - lpf_c) * accL;
        lpf_r = lpf_c * lpf_r + (1.f - lpf_c) * accR;
        outL = lpf_l;
        outR = lpf_r;
        bufL[head] = inL + lpf_l * 0.3f;
        bufR[head] = inR + lpf_r * 0.3f;
        head = (head + 1) & (REV_BUF - 1);
    }
};

struct Panner8D {
    float phase   = 0.f;
    float rate_hz = 0.25f;
    float depth   = 1.0f;

    void configure(float rate, float dep) {
        rate_hz = rate;
        depth   = dep;
    }

    inline void processSample(float inL, float inR, float& outL, float& outR) {
        float pan = sinf(phase) * depth;
        phase += 2.f * PI * rate_hz / SAMPLE_RATE;
        if (phase >= 2.f * PI) phase -= 2.f * PI;
        float panR = (pan + 1.f) * 0.5f;
        float panL = 1.f - panR;
        float gainL = sqrtf(panL);
        float gainR = sqrtf(panR);
        float mono = (inL + inR) * 0.5f;
        outL = mono * gainL;
        outR = mono * gainR;
    }
};

struct LoudnessMax {
    float gain     = 1.f;
    float att      = 0.f;
    float rel      = 0.f;
    float target   = 0.95f;
    float max_gain = 80.f;

    void init() {
        att = expf(-1.f / (SAMPLE_RATE * 0.008f));
        rel = expf(-1.f / (SAMPLE_RATE * 0.8f));
    }

    void processBlock(float* buf, int frames) {
        float sum = 0.f;
        for (int i = 0; i < frames; i++) {
            float m = (buf[i*2] + buf[i*2+1]) * 0.5f;
            sum += m * m;
        }
        float rms = sqrtf(sum / (float)frames + 1e-12f);
        float want = target / rms;
        if (want > max_gain) want = max_gain;
        if (want < 0.1f)     want = 0.1f;
        float coeff = (want < gain) ? att : rel;
        gain = coeff * gain + (1.f - coeff) * want;

        for (int i = 0; i < frames; i++) {
            buf[i*2]   = hardclip(buf[i*2]   * gain);
            buf[i*2+1] = hardclip(buf[i*2+1] * gain);
        }
    }
};

struct Complex { float re, im; };

static void fft(Complex* x, int N) {
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { Complex tmp = x[i]; x[i] = x[j]; x[j] = tmp; }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.f * PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float cr = 1.f, ci = 0.f;
            for (int j = 0; j < len/2; j++) {
                Complex u = x[i+j];
                Complex v = { x[i+j+len/2].re*cr - x[i+j+len/2].im*ci,
                               x[i+j+len/2].re*ci + x[i+j+len/2].im*cr };
                x[i+j]         = { u.re+v.re, u.im+v.im };
                x[i+j+len/2]   = { u.re-v.re, u.im-v.im };
                float nr = cr*wr - ci*wi;
                float ni = cr*wi + ci*wr;
                cr = nr; ci = ni;
            }
        }
    }
}

static const int SPEC_BINS = FFT_SIZE / 2;

struct Spectrum {
    float  capture[FFT_SIZE * 2] = {};
    int    cap_head = 0;
    float  window[FFT_SIZE];
    Complex fft_buf[FFT_SIZE];
    float  mag_back[SPEC_BINS]  = {};
    std::atomic<bool> ready{false};
    float  mag_front[SPEC_BINS] = {};

    void init() {
        for (int i = 0; i < FFT_SIZE; i++)
            window[i] = 0.5f * (1.f - cosf(2.f*PI*i/(FFT_SIZE-1)));
    }

    void push(const float* stereo, int frames) {
        for (int i = 0; i < frames; i++) {
            capture[cap_head * 2]   = stereo[i * 2];
            capture[cap_head * 2+1] = stereo[i * 2 + 1];
            cap_head = (cap_head + 1) % FFT_SIZE;
        }
        for (int i = 0; i < FFT_SIZE; i++) {
            float mono = (capture[((cap_head+i)%FFT_SIZE)*2] +
                          capture[((cap_head+i)%FFT_SIZE)*2+1]) * 0.5f;
            fft_buf[i] = { mono * window[i], 0.f };
        }
        fft(fft_buf, FFT_SIZE);
        for (int i = 0; i < SPEC_BINS; i++) {
            float m = sqrtf(fft_buf[i].re*fft_buf[i].re + fft_buf[i].im*fft_buf[i].im) / (FFT_SIZE*0.5f);
            mag_back[i] = mag_back[i] * 0.8f + m * 0.2f;
        }
        memcpy(mag_front, mag_back, sizeof(mag_front));
        ready.store(true, std::memory_order_release);
    }
};

enum class Mode { Normal, Room, Hard, Night, Pan8D };

struct DSPState {
    Biquad eq_lowcut;
    Biquad eq_bass;
    Biquad eq_mid;
    Biquad eq_presence;
    Biquad eq_air;

    Compressor comp;
    Compressor limiter;

    Reverb      reverb;
    Panner8D    panner;
    LoudnessMax loudmax;

    Biquad room_lpf;
    Biquad room_lpf2;
    Biquad room_bass;
    Biquad room_thump;

    Mode mode = Mode::Normal;

    float lim_env = 0.f;
    float lim_rel = 0.f;

    std::atomic<float> gain_db{0.f};
    std::atomic<float> volume{1.f};
    std::atomic<float> bass_db{0.f};
    std::atomic<float> mid_db{0.f};
    std::atomic<float> presence_db{0.f};
    std::atomic<float> air_db{0.f};
    std::atomic<float> comp_thresh{0.5f};
    std::atomic<float> comp_ratio{4.f};
    std::atomic<float> reverb_mix{0.f};
    std::atomic<float> reverb_decay{0.5f};
    std::atomic<float> reverb_wall{0.4f};
    std::atomic<float> pan8d_rate{0.25f};
    std::atomic<float> pan8d_depth{1.f};
    std::atomic<int>   mode_i{0};
    std::atomic<bool>  monitor{true};
    std::atomic<bool>  eq_enabled{true};

    void init() {
        eq_lowcut.highpass(80.0, 0.707);
        eq_bass.lowshelf(100.0, 0.0);
        eq_mid.peaking(1000.0, 0.0, 1.0);
        eq_presence.peaking(5000.0, 0.0, 1.0);
        eq_air.highshelf(12000.0, 0.0);

        comp.configure(0.3f, 6.f, 5.f, 80.f, 1.0f);
        limiter.configure(0.90f, 100.f, 0.05f, 30.f, 1.0f);
        lim_rel = expf(-1.f / (SAMPLE_RATE * 0.08f));
        reverb.configure(0.4f, 0.5f);
        panner.configure(0.25f, 1.0f);
        loudmax.init();

        room_lpf.lowpass(850.0, 0.6);
        room_lpf2.lowpass(850.0, 0.6);
        room_bass.lowshelf(120.0, 2.5);
        room_thump.peaking(95.0, 1.5, 1.2);
    }

    void update_from_params() {
        float bd = bass_db.load(std::memory_order_relaxed);
        float md = mid_db.load(std::memory_order_relaxed);
        float pd = presence_db.load(std::memory_order_relaxed);
        float ad = air_db.load(std::memory_order_relaxed);
        eq_bass.lowshelf(100.0, bd);
        eq_mid.peaking(1000.0, md, 1.0);
        eq_presence.peaking(5000.0, pd, 1.0);
        eq_air.highshelf(12000.0, ad);

        float ct = comp_thresh.load(std::memory_order_relaxed);
        float cr = comp_ratio.load(std::memory_order_relaxed);
        comp.configure(ct, cr, 10.f, 100.f, 1.0f);

        reverb.configure(
            reverb_wall.load(std::memory_order_relaxed),
            reverb_decay.load(std::memory_order_relaxed));

        panner.configure(
            pan8d_rate.load(std::memory_order_relaxed),
            pan8d_depth.load(std::memory_order_relaxed));

        mode = static_cast<Mode>(mode_i.load(std::memory_order_relaxed));
    }

    void processSample(float& L, float& R) {
        float gain_lin = powf(10.f, gain_db.load(std::memory_order_relaxed) / 20.f);
        L *= gain_lin;
        R *= gain_lin;

        if (eq_enabled.load(std::memory_order_relaxed)) {
            L = eq_lowcut.processSampleL(L);
            R = eq_lowcut.processSampleR(R);
            L = eq_bass.processSampleL(L);
            R = eq_bass.processSampleR(R);
            L = eq_mid.processSampleL(L);
            R = eq_mid.processSampleR(R);
            L = eq_presence.processSampleL(L);
            R = eq_presence.processSampleR(R);
            L = eq_air.processSampleL(L);
            R = eq_air.processSampleR(R);
        }

        L = comp.processSample(L);
        R = comp.processSample(R);

        Mode m = mode;
        if (m == Mode::Hard) {
            L = softclip(L * 50.f);
            R = softclip(R * 50.f);
            L = softclip(L * 15.f);
            R = softclip(R * 15.f);
            L = softclip(L * 6.f) * 1.1f;
            R = softclip(R * 6.f) * 1.1f;
        } else if (m == Mode::Room) {
            L *= 0.65f;
            R *= 0.65f;
            L = room_lpf.processSampleL(L);
            R = room_lpf.processSampleR(R);
            L = room_lpf2.processSampleL(L);
            R = room_lpf2.processSampleR(R);
            L = room_bass.processSampleL(L);
            R = room_bass.processSampleR(R);
            L = room_thump.processSampleL(L);
            R = room_thump.processSampleR(R);
            L = softclip(L);
            R = softclip(R);
            float mid  = (L + R) * 0.5f;
            float side = (L - R) * 0.5f * 0.30f;
            L = mid + side;
            R = mid - side;
            float wetL, wetR;
            reverb.processSample(L, R, wetL, wetR);
            float rmix = reverb_mix.load(std::memory_order_relaxed);
            L = L * (1.f - rmix) + wetL * rmix;
            R = R * (1.f - rmix) + wetR * rmix;
        } else if (m == Mode::Night) {
            L = comp.processSample(L);
            R = comp.processSample(R);
            L = softclip(L * 1.4f) * 0.71f;
            R = softclip(R * 1.4f) * 0.71f;
        } else if (m == Mode::Pan8D) {
            float pL, pR;
            panner.processSample(L, R, pL, pR);
            float wetL, wetR;
            reverb.processSample(pL, pR, wetL, wetR);
            L = pL * 0.7f + wetL * 0.3f;
            R = pR * 0.7f + wetR * 0.3f;
        }

        float vol = volume.load(std::memory_order_relaxed);
        L *= vol;
        R *= vol;
        if (m == Mode::Hard) {
            if (L >  1.f) L =  1.f; else if (L < -1.f) L = -1.f;
            if (R >  1.f) R =  1.f; else if (R < -1.f) R = -1.f;
        } else {
            L = limiter.processSample(L);
            R = limiter.processSample(R);
            L = hardclip(L);
            R = hardclip(R);
        }
    }
};

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    EDataFlow    flow;
};

static std::vector<DeviceInfo> EnumerateDevices(EDataFlow flow) {
    std::vector<DeviceInfo> result;
    IMMDeviceEnumerator* pEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (!pEnum) return result;

    IMMDeviceCollection* pColl = nullptr;
    pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pColl);
    if (pColl) {
        UINT count = 0;
        pColl->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* pDev = nullptr;
            pColl->Item(i, &pDev);
            if (!pDev) continue;

            LPWSTR id = nullptr;
            pDev->GetId(&id);

            IPropertyStore* props = nullptr;
            pDev->OpenPropertyStore(STGM_READ, &props);
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (props) props->GetValue(PKEY_Device_FriendlyName, &pv);

            DeviceInfo di;
            if (id) { di.id = id; CoTaskMemFree(id); }
            if (pv.vt == VT_LPWSTR) di.name = pv.pwszVal;
            di.flow = flow;
            result.push_back(di);

            PropVariantClear(&pv);
            if (props) props->Release();
            pDev->Release();
        }
        pColl->Release();
    }
    pEnum->Release();
    return result;
}

static RingBuffer g_ring;
static RingBuffer g_ring_virt;
static DSPState   g_dsp;
static Spectrum   g_spec;
static std::atomic<bool> g_running{false};
static std::thread g_capture_thread;
static std::thread g_render_thread;
static std::thread g_virt_thread;

static float g_proc_buf[BLOCK_SIZE * 2];

static void CaptureThread(std::wstring captureDevId) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&pEnum);

    IMMDevice* pDev = nullptr;
    if (captureDevId.empty()) {
        pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pDev);
    } else {
        pEnum->GetDevice(captureDevId.c_str(), &pDev);
    }

    IAudioClient* pClient = nullptr;
    if (pDev) pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pClient);

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = (WORD)CHANNELS;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (pClient) {
        HRESULT hr = pClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            2000000, 0, &wfx, nullptr);

        if (FAILED(hr)) {
            WAVEFORMATEX* pFmt = nullptr;
            pClient->GetMixFormat(&pFmt);
            if (pFmt) {
                pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    2000000, 0, pFmt, nullptr);
                CoTaskMemFree(pFmt);
            }
        }
    }

    IAudioCaptureClient* pCapture = nullptr;
    if (pClient) {
        pClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCapture);
        pClient->Start();
    }

    float convert_buf[BLOCK_SIZE * 2];

    while (g_running.load(std::memory_order_relaxed)) {
        if (!pCapture) { Sleep(10); continue; }

        UINT32 pktFrames = 0;
        pCapture->GetNextPacketSize(&pktFrames);
        if (pktFrames == 0) { Sleep(1); continue; }

        BYTE*  pData  = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        pCapture->GetBuffer(&pData, &frames, &flags, nullptr, nullptr);

        if (pData && !(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames > 0) {
            int to_write = (int)(frames < (UINT32)BLOCK_SIZE ? frames : (UINT32)BLOCK_SIZE);
            memcpy(convert_buf, pData, to_write * CHANNELS * sizeof(float));
            g_ring.write(convert_buf, to_write);
        }

        pCapture->ReleaseBuffer(frames);
    }

    if (pClient)  { pClient->Stop(); pClient->Release(); }
    if (pCapture) pCapture->Release();
    if (pDev)     pDev->Release();
    if (pEnum)    pEnum->Release();
    CoUninitialize();
}

static void RenderThread(std::wstring renderDevId) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    IMMDeviceEnumerator* pEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&pEnum);

    IMMDevice* pDev = nullptr;
    if (renderDevId.empty()) {
        pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
    } else {
        pEnum->GetDevice(renderDevId.c_str(), &pDev);
    }

    IAudioClient* pClient = nullptr;
    if (pDev) pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pClient);

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = (WORD)CHANNELS;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (pClient) {
        HRESULT hr = pClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            2000000, 0, &wfx, nullptr);
        if (FAILED(hr)) {
            WAVEFORMATEX* pFmt = nullptr;
            pClient->GetMixFormat(&pFmt);
            if (pFmt) {
                pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    2000000, 0, pFmt, nullptr);
                CoTaskMemFree(pFmt);
            }
        }
    }

    UINT32 bufSize = 0;
    IAudioRenderClient* pRender = nullptr;
    if (pClient) {
        pClient->GetBufferSize(&bufSize);
        pClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRender);
        pClient->Start();
    }

    static int param_update_counter = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        if (!pRender) { Sleep(10); continue; }

        UINT32 padding = 0;
        pClient->GetCurrentPadding(&padding);
        UINT32 avail = bufSize - padding;
        if (avail < (UINT32)BLOCK_SIZE) { Sleep(1); continue; }

        if (++param_update_counter >= 32) {
            param_update_counter = 0;
            g_dsp.update_from_params();
        }

        int got = g_ring.read(g_proc_buf, BLOCK_SIZE);
        bool mon = g_dsp.monitor.load(std::memory_order_relaxed);

        if (got > 0) {
            for (int i = 0; i < BLOCK_SIZE; i++) {
                float L = g_proc_buf[i*2];
                float R = g_proc_buf[i*2+1];
                g_dsp.processSample(L, R);
                g_proc_buf[i*2]   = L;
                g_proc_buf[i*2+1] = R;
            }
            g_dsp.loudmax.processBlock(g_proc_buf, BLOCK_SIZE);
            g_spec.push(g_proc_buf, BLOCK_SIZE);
            g_ring_virt.write(g_proc_buf, BLOCK_SIZE);
            if (!mon) memset(g_proc_buf, 0, BLOCK_SIZE * CHANNELS * sizeof(float));
        } else {
            memset(g_proc_buf, 0, BLOCK_SIZE * CHANNELS * sizeof(float));
        }

        BYTE* pData = nullptr;
        pRender->GetBuffer(BLOCK_SIZE, &pData);
        if (pData) {
            memcpy(pData, g_proc_buf, BLOCK_SIZE * CHANNELS * sizeof(float));
            pRender->ReleaseBuffer(BLOCK_SIZE, 0);
        }
    }

    if (pClient)  { pClient->Stop(); pClient->Release(); }
    if (pRender)  pRender->Release();
    if (pDev)     pDev->Release();
    if (pEnum)    pEnum->Release();
    CoUninitialize();
}

static void VirtualRenderThread(std::wstring virtDevId) {
    if (virtDevId.empty()) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&pEnum);

    IMMDevice* pDev = nullptr;
    pEnum->GetDevice(virtDevId.c_str(), &pDev);

    IAudioClient* pClient = nullptr;
    if (pDev) pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pClient);

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = 8;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * 8;

    if (pClient) {
        HRESULT hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            2000000, 0, &wfx, nullptr);
        if (FAILED(hr)) {
            WAVEFORMATEX* pFmt = nullptr;
            pClient->GetMixFormat(&pFmt);
            if (pFmt) {
                pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    2000000, 0, pFmt, nullptr);
                CoTaskMemFree(pFmt);
            }
        }
    }

    UINT32 bufSize = 0;
    IAudioRenderClient* pRender = nullptr;
    if (pClient) {
        pClient->GetBufferSize(&bufSize);
        pClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRender);
        pClient->Start();
    }

    float vbuf[BLOCK_SIZE * 2];

    while (g_running.load(std::memory_order_relaxed)) {
        if (!pRender) { Sleep(10); continue; }
        UINT32 padding = 0;
        pClient->GetCurrentPadding(&padding);
        UINT32 avail = bufSize - padding;
        if (avail < (UINT32)BLOCK_SIZE) { Sleep(1); continue; }

        int got = g_ring_virt.read(vbuf, BLOCK_SIZE);
        if (got == 0) memset(vbuf, 0, sizeof(float) * BLOCK_SIZE * 2);

        BYTE* pData = nullptr;
        pRender->GetBuffer(BLOCK_SIZE, &pData);
        if (pData) {
            memcpy(pData, vbuf, BLOCK_SIZE * 2 * sizeof(float));
            pRender->ReleaseBuffer(BLOCK_SIZE, 0);
        }
    }

    if (pClient)  { pClient->Stop(); pClient->Release(); }
    if (pRender)  pRender->Release();
    if (pDev)     pDev->Release();
    if (pEnum)    pEnum->Release();
    CoUninitialize();
}

static std::wstring g_virt_dev_id;

static void StartAudio(const std::wstring& capId, const std::wstring& renId) {
    g_running.store(false);
    if (g_virt_thread.joinable())    g_virt_thread.join();
    if (g_render_thread.joinable())  g_render_thread.join();
    if (g_capture_thread.joinable()) g_capture_thread.join();
    g_ring.reset();
    g_ring_virt.reset();
    g_running.store(true);
    g_capture_thread = std::thread(CaptureThread, capId);
    g_render_thread  = std::thread(RenderThread,  renId);
    g_virt_thread    = std::thread(VirtualRenderThread, g_virt_dev_id);
}

static void StopAudio() {
    g_running.store(false);
    if (g_virt_thread.joinable())    g_virt_thread.join();
    if (g_render_thread.joinable())  g_render_thread.join();
    if (g_capture_thread.joinable()) g_capture_thread.join();
}

#define IDC_BTN_START    1001
#define IDC_BTN_STOP     1002
#define IDC_BTN_MONITOR  1003
#define IDC_CMB_CAPTURE  1004
#define IDC_CMB_RENDER   1005
#define IDC_CMB_VIRTUAL  1006
#define IDC_SLD_GAIN     1010
#define IDC_SLD_VOLUME   1011
#define IDC_SLD_BASS     1012
#define IDC_SLD_MID      1013
#define IDC_SLD_PRESENCE 1014
#define IDC_SLD_AIR      1015
#define IDC_SLD_COMP_THR 1016
#define IDC_SLD_COMP_RAT 1017
#define IDC_SLD_REV_MIX  1018
#define IDC_SLD_REV_DEC  1019
#define IDC_SLD_REV_WALL 1020
#define IDC_SLD_PAN_RATE 1021
#define IDC_SLD_PAN_DEP  1022
#define IDC_BTN_MODE0    1030
#define IDC_BTN_MODE1    1031
#define IDC_BTN_MODE2    1032
#define IDC_BTN_MODE3    1033
#define IDC_BTN_MODE4    1034
#define IDC_TIMER_SPEC   2001

#define WIN_W 1200
#define WIN_H 620
#define SPEC_H 160

#define COL_BG       RGB(12,12,12)
#define COL_PANEL    RGB(24,24,24)
#define COL_BORDER   RGB(60,60,60)
#define COL_WHITE    RGB(255,255,255)
#define COL_GRAY     RGB(140,140,140)
#define COL_DARK     RGB(36,36,36)
#define COL_ACTIVE   RGB(220,220,220)
#define COL_INACTIVE RGB(70,70,70)

static HWND g_hwnd = nullptr;
static std::vector<DeviceInfo> g_cap_devs, g_ren_devs;
static bool g_audio_started = false;
static int  g_active_mode   = 0;

static void GetIniPath(wchar_t* buf, int bufLen) {
    GetModuleFileNameW(nullptr, buf, bufLen);
    wchar_t* p = wcsrchr(buf, L'\\');
    if (p) *(p+1) = L'\0';
    wcscat_s(buf, bufLen, L"AudioBeast.ini");
}

static void SaveSettings(HWND hwnd) {
    wchar_t ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);

    auto saveDevice = [&](int comboId, const std::vector<DeviceInfo>& devs, int virtOffset, const wchar_t* key) {
        int sel = (int)SendDlgItemMessageW(hwnd, comboId, CB_GETCURSEL, 0, 0);
        std::wstring id;
        if (virtOffset) {
            if (sel > 0 && sel - 1 < (int)devs.size()) id = devs[sel - 1].id;
        } else {
            if (sel >= 0 && sel < (int)devs.size()) id = devs[sel].id;
        }
        WritePrivateProfileStringW(L"Devices", key, id.c_str(), ini);
    };
    saveDevice(IDC_CMB_CAPTURE, g_cap_devs, 0, L"CaptureId");
    saveDevice(IDC_CMB_RENDER,  g_ren_devs, 0, L"RenderId");
    saveDevice(IDC_CMB_VIRTUAL, g_ren_devs, 1, L"VirtualId");

    // Save slider positions
    auto saveSlider = [&](int id, const wchar_t* key) {
        int v = (int)SendDlgItemMessageW(hwnd, id, TBM_GETPOS, 0, 0);
        wchar_t buf[32]; swprintf_s(buf, L"%d", v);
        WritePrivateProfileStringW(L"Sliders", key, buf, ini);
    };
    saveSlider(IDC_SLD_GAIN,     L"Gain");
    saveSlider(IDC_SLD_VOLUME,   L"Volume");
    saveSlider(IDC_SLD_BASS,     L"Bass");
    saveSlider(IDC_SLD_MID,      L"Mid");
    saveSlider(IDC_SLD_PRESENCE, L"Presence");
    saveSlider(IDC_SLD_AIR,      L"Air");
    saveSlider(IDC_SLD_COMP_THR, L"CompThr");
    saveSlider(IDC_SLD_COMP_RAT, L"CompRat");
    saveSlider(IDC_SLD_PAN_RATE, L"PanRate");
    saveSlider(IDC_SLD_PAN_DEP,  L"PanDep");
    saveSlider(IDC_SLD_REV_MIX,  L"RevMix");
    saveSlider(IDC_SLD_REV_DEC,  L"RevDec");
    saveSlider(IDC_SLD_REV_WALL, L"RevWall");

    // Save mode and monitor
    wchar_t buf[32];
    swprintf_s(buf, L"%d", g_active_mode);
    WritePrivateProfileStringW(L"State", L"Mode", buf, ini);
    swprintf_s(buf, L"%d", g_dsp.monitor.load() ? 1 : 0);
    WritePrivateProfileStringW(L"State", L"Monitor", buf, ini);
}

static void LoadSettings(HWND hwnd) {
    wchar_t ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) return; // no ini yet

    // Restore device combos by matching saved ID
    auto loadDevice = [&](int comboId, const std::vector<DeviceInfo>& devs, int virtOffset, const wchar_t* key) {
        wchar_t savedId[512] = {};
        GetPrivateProfileStringW(L"Devices", key, L"", savedId, 512, ini);
        if (!savedId[0]) return;
        if (virtOffset) {
            for (int i = 0; i < (int)devs.size(); i++) {
                if (devs[i].id == savedId) {
                    SendDlgItemMessageW(hwnd, comboId, CB_SETCURSEL, i + 1, 0);
                    return;
                }
            }
        } else {
            for (int i = 0; i < (int)devs.size(); i++) {
                if (devs[i].id == savedId) {
                    SendDlgItemMessageW(hwnd, comboId, CB_SETCURSEL, i, 0);
                    return;
                }
            }
        }
    };
    loadDevice(IDC_CMB_CAPTURE, g_cap_devs, 0, L"CaptureId");
    loadDevice(IDC_CMB_RENDER,  g_ren_devs, 0, L"RenderId");
    loadDevice(IDC_CMB_VIRTUAL, g_ren_devs, 1, L"VirtualId");

    // Restore slider positions
    auto loadSlider = [&](int id, const wchar_t* key, int def) {
        wchar_t buf[32] = {};
        GetPrivateProfileStringW(L"Sliders", key, L"", buf, 32, ini);
        if (!buf[0]) return;
        int v = _wtoi(buf);
        SendDlgItemMessageW(hwnd, id, TBM_SETPOS, TRUE, v);
    };
    loadSlider(IDC_SLD_GAIN,     L"Gain",     200);
    loadSlider(IDC_SLD_VOLUME,   L"Volume",   100);
    loadSlider(IDC_SLD_BASS,     L"Bass",     120);
    loadSlider(IDC_SLD_MID,      L"Mid",      120);
    loadSlider(IDC_SLD_PRESENCE, L"Presence", 120);
    loadSlider(IDC_SLD_AIR,      L"Air",      120);
    loadSlider(IDC_SLD_COMP_THR, L"CompThr",   70);
    loadSlider(IDC_SLD_COMP_RAT, L"CompRat",   40);
    loadSlider(IDC_SLD_PAN_RATE, L"PanRate",   12);
    loadSlider(IDC_SLD_PAN_DEP,  L"PanDep",    80);
    loadSlider(IDC_SLD_REV_MIX,  L"RevMix",    50);
    loadSlider(IDC_SLD_REV_DEC,  L"RevDec",    60);
    loadSlider(IDC_SLD_REV_WALL, L"RevWall",   40);

    // Restore mode
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"State", L"Mode", L"0", buf, 32, ini);
    g_active_mode = _wtoi(buf);
    g_dsp.mode_i.store(g_active_mode);

    // Restore monitor
    GetPrivateProfileStringW(L"State", L"Monitor", L"1", buf, 32, ini);
    g_dsp.monitor.store(_wtoi(buf) != 0);
}

static HBRUSH g_br_bg    = nullptr;
static HBRUSH g_br_panel = nullptr;
static HFONT  g_font_sm  = nullptr;
static HFONT  g_font_md  = nullptr;
static HFONT  g_font_lg  = nullptr;
static HICON  g_icon     = nullptr;

// Spectrum display rect  (y=290 to y=610, height 320)
static RECT g_spec_rect = {10, 290, WIN_W-10, 610};

static HWND CreateSlider(HWND parent, int id, int x, int y, int w, int h,
                          int min_v, int max_v, int init_v) {
    HWND hw = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, GetModuleHandle(nullptr), nullptr);
    SendMessage(hw, TBM_SETRANGEMIN, FALSE, min_v);
    SendMessage(hw, TBM_SETRANGEMAX, FALSE, max_v);
    SendMessage(hw, TBM_SETPOS,      TRUE,  init_v);
    return hw;
}

static HWND CreateBtn(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id,
        GetModuleHandle(nullptr), nullptr);
}

static HWND CreateODBtn(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id,
        GetModuleHandle(nullptr), nullptr);
}

static void DrawLabel(HDC hdc, const wchar_t* text, int x, int y, int w, int h,
                       HFONT font, COLORREF col, UINT align = DT_CENTER | DT_VCENTER | DT_SINGLELINE) {
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, font);
    RECT r = {x, y, x+w, y+h};
    DrawTextW(hdc, text, -1, &r, align);
}

static void DrawSpectrumPanel(HDC hdc) {
    RECT& sr = g_spec_rect;
    HBRUSH brDark = CreateSolidBrush(COL_PANEL);
    FillRect(hdc, &sr, brDark);
    DeleteObject(brDark);

    HPEN penBorder = CreatePen(PS_SOLID, 1, COL_BORDER);
    SelectObject(hdc, penBorder);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, sr.left, sr.top, sr.right, sr.bottom);
    DeleteObject(penBorder);

    if (!g_spec.ready.load(std::memory_order_acquire)) return;

    int pw = sr.right - sr.left - 4;
    int ph = sr.bottom - sr.top - 4;
    int bx = sr.left + 2;
    int by = sr.top  + 2;

    static const int DISP_BINS = 128;
    int bar_w = pw / DISP_BINS;
    if (bar_w < 1) bar_w = 1;

    HPEN penWhite = CreatePen(PS_SOLID, bar_w > 1 ? bar_w-1 : 1, COL_WHITE);
    HPEN penGray  = CreatePen(PS_SOLID, 1, COL_GRAY);
    SelectObject(hdc, penWhite);

    for (int i = 0; i < DISP_BINS; i++) {
        float log_pos = (float)i / DISP_BINS;
        float freq_hz = 20.f * powf(22000.f / 20.f, log_pos);
        int bin = (int)(freq_hz * FFT_SIZE / SAMPLE_RATE);
        if (bin >= SPEC_BINS) bin = SPEC_BINS - 1;

        float mag = g_spec.mag_front[bin];
        float db  = 20.f * log10f(mag + 1e-6f);
        float norm = (db + 80.f) / 80.f;
        if (norm < 0.f) norm = 0.f;
        if (norm > 1.f) norm = 1.f;

        int bar_h = (int)(norm * ph);
        int x0 = bx + i * bar_w;
        int y0 = by + ph - bar_h;
        int y1 = by + ph;

        if (norm > 0.6f) SelectObject(hdc, penWhite);
        else SelectObject(hdc, penGray);

        MoveToEx(hdc, x0, y1, nullptr);
        LineTo(hdc, x0, y0);
    }

    DeleteObject(penWhite);
    DeleteObject(penGray);

    const wchar_t* flabs[] = {L"20", L"50", L"100", L"200", L"500", L"1k", L"2k", L"5k", L"10k", L"20k"};
    const float    fhz[]   = {20,    50,    100,    200,    500,    1000, 2000, 5000, 10000, 20000};
    SetTextColor(hdc, COL_GRAY);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_font_sm);
    for (int i = 0; i < 10; i++) {
        float log_pos = log10f(fhz[i]/20.f) / log10f(22000.f/20.f);
        int x0 = bx + (int)(log_pos * pw);
        RECT lr = {x0-15, by+ph-14, x0+15, by+ph};
        DrawTextW(hdc, flabs[i], -1, &lr, DT_CENTER | DT_SINGLELINE);
    }
}

static void DrawModeButton(HDC hdc, int id, const wchar_t* label, int x, int y, int w, int h, bool active) {
    COLORREF bg  = active ? COL_WHITE    : COL_DARK;
    COLORREF fg  = active ? RGB(0,0,0)   : COL_GRAY;
    COLORREF brd = active ? COL_WHITE    : COL_BORDER;

    HBRUSH br  = CreateSolidBrush(bg);
    HPEN   pen = CreatePen(PS_SOLID, 1, brd);
    SelectObject(hdc, br);
    SelectObject(hdc, pen);
    RECT r = {x, y, x+w, y+h};
    FillRect(hdc, &r, br);
    Rectangle(hdc, x, y, x+w, y+h);
    DeleteObject(br);
    DeleteObject(pen);

    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_font_md);
    DrawTextW(hdc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT client;
    GetClientRect(hwnd, &client);
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    FillRect(memDC, &client, g_br_bg);


    if (g_icon) DrawIconEx(memDC, 8, 4, g_icon, 38, 38, 0, nullptr, DI_NORMAL);
    DrawLabel(memDC, L"OBLIQ by bzow", 52, 4, 320, 26, g_font_lg, COL_WHITE, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"Professional Audio Processor  \u2014  Dev by UHQ for UHQ user", 52, 30, 580, 14, g_font_sm, COL_GRAY, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const wchar_t* status = g_audio_started ? L"[ RUNNING ]" : L"[ STOPPED ]";
    COLORREF stcol = g_audio_started ? COL_WHITE : COL_GRAY;
    DrawLabel(memDC, status, WIN_W-155, 6, 145, 20, g_font_md, stcol, DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

    DrawLabel(memDC, L"CAPTURE DEVICE", 10,  50, 230, 14, g_font_sm, COL_GRAY, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"OUTPUT DEVICE",  245, 50, 230, 14, g_font_sm, COL_GRAY, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"DISCORD OUT",    480, 50, 230, 14, g_font_sm, COL_GRAY, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"-- MODE --",     928, 50, 262, 14, g_font_sm, COL_GRAY, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

    DrawLabel(memDC, L"INPUT GAIN", 10,  112, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"VOLUME",     145, 112, 80,  14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"-- EQUALIZER --", 238, 112, 520, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);
    const wchar_t* eq_labels[] = {L"BASS", L"MID", L"PRESENCE", L"AIR"};
    for (int i = 0; i < 4; i++)
        DrawLabel(memDC, eq_labels[i], 238 + i*130, 128, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);

    {
        HPEN pv = CreatePen(PS_SOLID, 1, COL_BORDER);
        SelectObject(memDC, pv);
        MoveToEx(memDC, 233, 108, nullptr);
        LineTo(memDC, 233, 172);
        DeleteObject(pv);
    }

    DrawLabel(memDC, L"-- COMPRESSOR --", 10,  190, 260, 14, g_font_sm, COL_GRAY, DT_LEFT|DT_SINGLELINE);
    DrawLabel(memDC, L"THRESHOLD",        10,  206, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"RATIO",            140, 206, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);

    DrawLabel(memDC, L"-- 8D PAN --",     280, 190, 260, 14, g_font_sm, COL_GRAY, DT_LEFT|DT_SINGLELINE);
    DrawLabel(memDC, L"RATE",             280, 206, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"DEPTH",            410, 206, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);

    DrawLabel(memDC, L"-- REVERB --",     550, 190, 380, 14, g_font_sm, COL_GRAY, DT_LEFT|DT_SINGLELINE);
    DrawLabel(memDC, L"MIX",              550, 206, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"DECAY",            680, 206, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);
    DrawLabel(memDC, L"WALL",             810, 206, 120, 14, g_font_sm, COL_GRAY, DT_CENTER|DT_SINGLELINE);

    {
        HPEN pv = CreatePen(PS_SOLID, 1, COL_BORDER);
        SelectObject(memDC, pv);
        MoveToEx(memDC, 275, 186, nullptr); LineTo(memDC, 275, 282);
        MoveToEx(memDC, 545, 186, nullptr); LineTo(memDC, 545, 282);
        DeleteObject(pv);
    }

    HPEN penSep = CreatePen(PS_SOLID, 1, COL_BORDER);
    SelectObject(memDC, penSep);
    MoveToEx(memDC, 10, 285, nullptr);
    LineTo(memDC, WIN_W-10, 285);
    DeleteObject(penSep);

    DrawSpectrumPanel(memDC);
    BitBlt(hdc, 0, 0, client.right, client.bottom, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

static void UpdateSliderValues() {
    if (!g_hwnd) return;
    HWND hGain    = GetDlgItem(g_hwnd, IDC_SLD_GAIN);
    HWND hVol     = GetDlgItem(g_hwnd, IDC_SLD_VOLUME);
    HWND hBass    = GetDlgItem(g_hwnd, IDC_SLD_BASS);
    HWND hMid     = GetDlgItem(g_hwnd, IDC_SLD_MID);
    HWND hPres    = GetDlgItem(g_hwnd, IDC_SLD_PRESENCE);
    HWND hAir     = GetDlgItem(g_hwnd, IDC_SLD_AIR);
    HWND hCmpT    = GetDlgItem(g_hwnd, IDC_SLD_COMP_THR);
    HWND hCmpR    = GetDlgItem(g_hwnd, IDC_SLD_COMP_RAT);
    HWND hRevM    = GetDlgItem(g_hwnd, IDC_SLD_REV_MIX);
    HWND hRevD    = GetDlgItem(g_hwnd, IDC_SLD_REV_DEC);
    HWND hRevW    = GetDlgItem(g_hwnd, IDC_SLD_REV_WALL);
    HWND hPanR    = GetDlgItem(g_hwnd, IDC_SLD_PAN_RATE);
    HWND hPanD    = GetDlgItem(g_hwnd, IDC_SLD_PAN_DEP);

    auto geti = [](HWND h) { return (int)SendMessage(h, TBM_GETPOS, 0, 0); };

    if (hGain)  g_dsp.gain_db.store((geti(hGain) - 200) / 10.f);
    if (hVol)   g_dsp.volume.store(geti(hVol) / 100.f);
    if (hBass)  g_dsp.bass_db.store((geti(hBass)  - 120) / 10.f);
    if (hMid)   g_dsp.mid_db.store((geti(hMid)   - 120) / 10.f);
    if (hPres)  g_dsp.presence_db.store((geti(hPres) - 120) / 10.f);
    if (hAir)   g_dsp.air_db.store((geti(hAir)  - 120) / 10.f);
    if (hCmpT)  g_dsp.comp_thresh.store(geti(hCmpT) / 100.f);
    if (hCmpR)  g_dsp.comp_ratio.store(geti(hCmpR) / 10.f);
    if (hRevM)  g_dsp.reverb_mix.store(geti(hRevM) / 100.f);
    if (hRevD)  g_dsp.reverb_decay.store(geti(hRevD) / 100.f);
    if (hRevW)  g_dsp.reverb_wall.store(geti(hRevW) / 101.f);
    if (hPanR)  g_dsp.pan8d_rate.store(geti(hPanR) / 50.f);
    if (hPanD)  g_dsp.pan8d_depth.store(geti(hPanD) / 100.f);
}

static void OnStartAudio(HWND hwnd) {
    HWND hCap  = GetDlgItem(hwnd, IDC_CMB_CAPTURE);
    HWND hRen  = GetDlgItem(hwnd, IDC_CMB_RENDER);
    HWND hVirt = GetDlgItem(hwnd, IDC_CMB_VIRTUAL);
    int  ci    = (int)SendMessage(hCap,  CB_GETCURSEL, 0, 0);
    int  ri    = (int)SendMessage(hRen,  CB_GETCURSEL, 0, 0);
    int  vi    = (int)SendMessage(hVirt, CB_GETCURSEL, 0, 0);

    std::wstring capId, renId;
    if (ci >= 0 && ci < (int)g_cap_devs.size()) capId = g_cap_devs[ci].id;
    if (ri >= 0 && ri < (int)g_ren_devs.size()) renId = g_ren_devs[ri].id;

    g_virt_dev_id.clear();
    if (vi > 0 && vi - 1 < (int)g_ren_devs.size())
        g_virt_dev_id = g_ren_devs[vi - 1].id;

    StartAudio(capId, renId);
    g_audio_started = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void OnStopAudio(HWND hwnd) {
    StopAudio();
    g_audio_started = false;
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        InitCommonControls();

        g_font_sm = CreateFontW(12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_md = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_lg = CreateFontW(22, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        g_cap_devs = EnumerateDevices(eCapture);
        g_ren_devs = EnumerateDevices(eRender);

        HWND hCap = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            10, 66, 230, 200, hwnd, (HMENU)IDC_CMB_CAPTURE,
            GetModuleHandle(nullptr), nullptr);
        for (auto& d : g_cap_devs)
            SendMessageW(hCap, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
        if (!g_cap_devs.empty()) SendMessage(hCap, CB_SETCURSEL, 0, 0);
        SendMessage(hCap, WM_SETFONT, (WPARAM)g_font_sm, TRUE);

        HWND hRen = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            245, 66, 230, 200, hwnd, (HMENU)IDC_CMB_RENDER,
            GetModuleHandle(nullptr), nullptr);
        for (auto& d : g_ren_devs)
            SendMessageW(hRen, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
        if (!g_ren_devs.empty()) SendMessage(hRen, CB_SETCURSEL, 0, 0);
        SendMessage(hRen, WM_SETFONT, (WPARAM)g_font_sm, TRUE);

        HWND hVirt = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            480, 66, 230, 200, hwnd, (HMENU)IDC_CMB_VIRTUAL,
            GetModuleHandle(nullptr), nullptr);
        SendMessageW(hVirt, CB_ADDSTRING, 0, (LPARAM)L"-- None --");
        for (auto& d : g_ren_devs)
            SendMessageW(hVirt, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
        SendMessage(hVirt, CB_SETCURSEL, 0, 0);
        SendMessage(hVirt, WM_SETFONT, (WPARAM)g_font_sm, TRUE);

        HWND hStart = CreateBtn(hwnd, IDC_BTN_START,   L"START",   717, 66, 52, 26);
        HWND hStop  = CreateBtn(hwnd, IDC_BTN_STOP,    L"STOP",    773, 66, 52, 26);
        CreateODBtn(hwnd, IDC_BTN_MONITOR, L"", 829, 66, 92, 26);
        SendMessage(hStart, WM_SETFONT, (WPARAM)g_font_sm, TRUE);
        SendMessage(hStop,  WM_SETFONT, (WPARAM)g_font_sm, TRUE);

        static const wchar_t* mode_btn_labels[] = {L"NORMAL", L"ROOM", L"HARD", L"NIGHT", L"8D"};
        for (int i = 0; i < 5; i++)
            CreateODBtn(hwnd, IDC_BTN_MODE0+i, mode_btn_labels[i], 928 + i*52, 66, 48, 26);

        CreateSlider(hwnd, IDC_SLD_GAIN,    10,  145, 120, 22, 0, 1000, 600);
        CreateSlider(hwnd, IDC_SLD_VOLUME, 145,  145,  80, 22, 0, 800, 400);
        CreateSlider(hwnd, IDC_SLD_BASS,     238,       145, 120, 22, 0, 240, 120);
        CreateSlider(hwnd, IDC_SLD_MID,      238+130,   145, 120, 22, 0, 240, 120);
        CreateSlider(hwnd, IDC_SLD_PRESENCE, 238+260,   145, 120, 22, 0, 240, 120);
        CreateSlider(hwnd, IDC_SLD_AIR,      238+390,   145, 120, 22, 0, 240, 120);

        CreateSlider(hwnd, IDC_SLD_COMP_THR, 10,  222, 120, 22,  1, 100, 70);
        CreateSlider(hwnd, IDC_SLD_COMP_RAT, 140, 222, 120, 22, 10, 200, 40);
        CreateSlider(hwnd, IDC_SLD_PAN_RATE, 280, 222, 120, 22,  1, 100, 12);
        CreateSlider(hwnd, IDC_SLD_PAN_DEP,  410, 222, 120, 22,  0, 100, 80);
        CreateSlider(hwnd, IDC_SLD_REV_MIX,  550, 222, 120, 22,  0, 100, 50);
        CreateSlider(hwnd, IDC_SLD_REV_DEC,  680, 222, 120, 22,  0, 100, 60);
        CreateSlider(hwnd, IDC_SLD_REV_WALL, 810, 222, 120, 22,  0, 100, 40);

        g_dsp.init();
        g_spec.init();
        SetTimer(hwnd, IDC_TIMER_SPEC, 33, nullptr);
        LoadSettings(hwnd);
        UpdateSliderValues();
        break;
    }

    case WM_TIMER:
        if (wParam == IDC_TIMER_SPEC) {
            InvalidateRect(hwnd, &g_spec_rect, FALSE);
        }
        break;

    case WM_HSCROLL:
        UpdateSliderValues();
        break;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_START)   { OnStartAudio(hwnd); break; }
        if (id == IDC_BTN_STOP)    { OnStopAudio(hwnd);  break; }
        if (id == IDC_BTN_MONITOR) {
            bool cur = g_dsp.monitor.load();
            g_dsp.monitor.store(!cur);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        if (id >= IDC_BTN_MODE0 && id <= IDC_BTN_MODE4) {
            g_active_mode = id - IDC_BTN_MODE0;
            g_dsp.mode_i.store(g_active_mode);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        break;
    }

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lParam;
        int id = (int)di->CtlID;

        if (id == IDC_BTN_MONITOR) {
            bool mon     = g_dsp.monitor.load(std::memory_order_relaxed);
            bool pressed = (di->itemState & ODS_SELECTED) != 0;
            COLORREF bg  = mon ? COL_WHITE : COL_DARK;
            COLORREF fg  = mon ? RGB(0,0,0) : COL_GRAY;
            COLORREF brd = mon ? COL_WHITE  : COL_BORDER;
            if (pressed) { bg = COL_GRAY; fg = RGB(0,0,0); }

            HBRUSH br = CreateSolidBrush(bg);
            FillRect(di->hDC, &di->rcItem, br);
            DeleteObject(br);
            HPEN pen = CreatePen(PS_SOLID, 1, brd);
            HPEN old = (HPEN)SelectObject(di->hDC, pen);
            SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(di->hDC, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom);
            SelectObject(di->hDC, old); DeleteObject(pen);

            SetTextColor(di->hDC, fg);
            SetBkMode(di->hDC, TRANSPARENT);
            SelectObject(di->hDC, g_font_md);
            const wchar_t* lbl = mon ? L"MONITOR ON" : L"MONITOR OFF";
            DrawTextW(di->hDC, lbl, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        if (id >= IDC_BTN_MODE0 && id <= IDC_BTN_MODE4) {
            int idx = id - IDC_BTN_MODE0;
            bool active  = (g_active_mode == idx);
            bool pressed = (di->itemState & ODS_SELECTED) != 0;

            COLORREF bg  = active ? COL_WHITE : (pressed ? COL_GRAY : COL_DARK);
            COLORREF fg  = active ? RGB(0,0,0) : (pressed ? RGB(0,0,0) : COL_GRAY);
            COLORREF brd = active ? COL_WHITE : COL_BORDER;

            HBRUSH br = CreateSolidBrush(bg);
            FillRect(di->hDC, &di->rcItem, br);
            DeleteObject(br);

            HPEN pen = CreatePen(PS_SOLID, 1, brd);
            HPEN old = (HPEN)SelectObject(di->hDC, pen);
            SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(di->hDC, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom);
            SelectObject(di->hDC, old);
            DeleteObject(pen);

            SetTextColor(di->hDC, fg);
            SetBkMode(di->hDC, TRANSPARENT);
            SelectObject(di->hDC, g_font_md);
            wchar_t txt[32] = {};
            GetWindowTextW(di->hwndItem, txt, 31);
            DrawTextW(di->hDC, txt, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wParam, COL_WHITE);
        SetBkColor((HDC)wParam, COL_PANEL);
        return (LRESULT)g_br_panel;

    case WM_CTLCOLORBTN:
        return (LRESULT)g_br_panel;

    case WM_CLOSE:
        SaveSettings(hwnd);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        StopAudio();
        KillTimer(hwnd, IDC_TIMER_SPEC);
        DeleteObject(g_font_sm);
        DeleteObject(g_font_md);
        DeleteObject(g_font_lg);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    g_br_bg    = CreateSolidBrush(COL_BG);
    g_br_panel = CreateSolidBrush(COL_PANEL);
    g_icon     = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!g_icon) {
        wchar_t icoPath[MAX_PATH];
        GetModuleFileNameW(hInst, icoPath, MAX_PATH);
        wchar_t* p = wcsrchr(icoPath, L'\\'); if (p) *(p+1) = L'\0';
        wcscat_s(icoPath, MAX_PATH, L"icon.ico");
        g_icon = (HICON)LoadImageW(nullptr, icoPath, IMAGE_ICON, 38, 38, LR_LOADFROMFILE);
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = g_br_bg;
    wc.lpszClassName = L"AudioBeastClass";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = g_icon;
    wc.hIconSm       = g_icon;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"AudioBeastClass",
        L"OBLIQ by bzow",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WIN_W, WIN_H,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g_br_bg);
    DeleteObject(g_br_panel);
    CoUninitialize();
    return (int)msg.wParam;
}
