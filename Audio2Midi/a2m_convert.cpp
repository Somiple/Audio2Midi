#include "a2m_convert.h"
#include "audio_decode.h"
#include "pcm_font.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

//1 tick per sample - PPQN is half the rate at 120 BPM
constexpr int kRate = 48000;
constexpr int kSides = 2;

//PCM v2 wave layout - the sf2 and the a2m-pcm 2 marker depend on it
constexpr int kPcmWaveLevels = 19;
constexpr int kPcmWaveCenterL = 40; //E2 - left half 21 to 59
constexpr int kPcmWaveCenterR = 84; //C6 - right half 60 to 108

//spectrogram layer - drawn by the player and never heard
constexpr int kSpecCh = 14;
constexpr int kSpecLevels = 32;
constexpr double kSpecRangeDb = 45.0;
constexpr double kSpecHiPct = 0.999;

//matplotlib inferno sampled at each level
static const uint32_t kInferno32[kSpecLevels] = {
    0x040312, 0x0B0724, 0x150B37, 0x210C4A, 0x2F0A5B, 0x3D0965, 0x4A0C6B, 0x57106E,
    0x64156E, 0x71196E, 0x7D1E6D, 0x8A226A, 0x972766, 0xA32C61, 0xB0315B, 0xBC3754,
    0xC73E4C, 0xD24644, 0xDB503B, 0xE45A31, 0xEB6628, 0xF1731D, 0xF68013, 0xF98E09,
    0xFB9D07, 0xFCAC11, 0xFBBC21, 0xF9CB35, 0xF5DB4C, 0xF2EA69, 0xF3F68A, 0xFCFFA4
};

namespace
{
    constexpr double kTwoPi = 6.283185307179586476925;

    //key -> amplitude - 0 for keys outside the wave layout
    std::vector<double> PcmWaveKeyTable(double peak)
    {
        std::vector<double> t(128, 0.0);
        if (!(peak > 0.0)) return t;
        for (int k = 0; k <= kPcmWaveLevels; ++k)
        {
            double a = (k + 0.5) / kPcmWaveLevels * peak;
            if (a > 1.0) a = 1.0;
            t[(size_t)(kPcmWaveCenterL + k)] = a;
            t[(size_t)(kPcmWaveCenterL - k)] = a;
            t[(size_t)(kPcmWaveCenterR + k)] = a;
            t[(size_t)(kPcmWaveCenterR - k)] = a;
        }
        return t;
    }

    //pastel -> deep -> magenta for L or cyan for R by wave level
    void WaveLevelRgb(int side, int level, uint8_t out[3])
    {
        static const double kStops[2][3][3] = {
            { { 255, 179, 209 }, { 255, 60, 150 }, { 235, 30, 255 } },
            { { 167, 199, 255 }, { 40, 140, 255 }, { 0, 235, 255 } }
        };
        constexpr double kMid = 0.6;
        const double t = (double)level / kPcmWaveLevels;
        const double* a = (t <= kMid) ? kStops[side][0] : kStops[side][1];
        const double* b = (t <= kMid) ? kStops[side][1] : kStops[side][2];
        const double u = (t <= kMid) ? (t / kMid) : ((t - kMid) / (1.0 - kMid));
        for (int c = 0; c < 3; ++c) out[c] = (uint8_t)lround(a[c] + (b[c] - a[c]) * u);
    }

    void PutVar(std::vector<uint8_t>& v, uint32_t n)
    {
        uint8_t b[5];
        int i = 0;
        b[i++] = (uint8_t)(n & 0x7F);
        while ((n >>= 7) > 0) b[i++] = (uint8_t)((n & 0x7F) | 0x80);
        while (i) v.push_back(b[--i]);
    }

    void PutColor(std::vector<uint8_t>& t, uint8_t r, uint8_t g, uint8_t b)
    {
        PutVar(t, 0);
        t.push_back(0xFF);
        t.push_back(0x0A);
        t.push_back(8);
        t.push_back(0x00);
        t.push_back(0x0F);
        t.push_back(0x7F);
        t.push_back(0x00);
        t.push_back(r);
        t.push_back(g);
        t.push_back(b);
        t.push_back(0xFF);
    }

    void PutText(std::vector<uint8_t>& t, const char* s, int n)
    {
        PutVar(t, 0);
        t.push_back(0xFF);
        t.push_back(0x01);
        t.push_back((uint8_t)n);
        t.insert(t.end(), s, s + n);
    }

    void PutEnd(std::vector<uint8_t>& t)
    {
        PutVar(t, 0);
        t.push_back(0xFF);
        t.push_back(0x2F);
        t.push_back(0x00);
    }

    //reports only when the value changes
    struct Progress
    {
        const A2MProgressFn& fn;
        int last = -1;
        void operator()(int p)
        {
            if (p == last || !fn) return;
            last = p;
            fn(p);
        }
    };

    struct SpecFft
    {
        int n = 0;
        std::vector<uint32_t> rev;
        std::vector<double> wr, wi;
        void Init(int N)
        {
            n = N;
            rev.assign((size_t)N, 0);
            wr.assign((size_t)N / 2, 0.0);
            wi.assign((size_t)N / 2, 0.0);
            int lg = 0;
            while ((1 << lg) < N) ++lg;
            for (int i = 0; i < N; ++i)
            {
                uint32_t r = 0;
                for (int b = 0; b < lg; ++b)
                    if (i & (1 << b)) r |= 1u << (lg - 1 - b);
                rev[(size_t)i] = r;
            }
            for (int k = 0; k < N / 2; ++k)
            {
                wr[(size_t)k] = cos(kTwoPi * k / N);
                wi[(size_t)k] = -sin(kTwoPi * k / N);
            }
        }
        void Run(double* re, double* im) const
        {
            for (int i = 0; i < n; ++i)
            {
                const int j = (int)rev[(size_t)i];
                if (j > i)
                {
                    std::swap(re[i], re[j]);
                    std::swap(im[i], im[j]);
                }
            }
            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len >> 1, step = n / len;
                for (int i = 0; i < n; i += len)
                {
                    for (int k = 0; k < half; ++k)
                    {
                        const double cr = wr[(size_t)(k * step)], ci = wi[(size_t)(k * step)];
                        const int a = i + k, b = a + half;
                        const double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                        re[b] = re[a] - xr;
                        im[b] = im[a] - xi;
                        re[a] += xr;
                        im[a] += xi;
                    }
                }
            }
        }
    };

    //one window length and the keys that use it
    struct SpecRes
    {
        int N = 0;
        std::vector<double> win;
        double norm = 0.0;
        struct Band
        {
            int key = 0;
            int j0 = 0;
            double invBw = 0.0;
            std::vector<double> w;
        };
        std::vector<Band> bands;
        SpecFft fft;
    };

    //level per frame and key 0 to 32 - 0 means no note - multi resolution hann stft - brightness is power per Hz
    //progress 60 to 95
    bool AnalyzeSpectrogram(const std::vector<float>& pcm, size_t nFrames, int rate,
                            const std::atomic<bool>& cancel, const A2MProgressFn& progress,
                            std::vector<uint8_t>& level, size_t& nSpec, int& hop)
    {
        hop = (rate / 100 > 0) ? rate / 100 : 1;
        nSpec = (nFrames + (size_t)hop - 1) / (size_t)hop;
        level.assign(nSpec * 128, 0);
        if (nSpec == 0) return true;

        int nMin = 1;
        while (nMin < 2 * hop) nMin <<= 1;
        int nMax = 1 << (int)lround(log2((double)rate * 16384.0 / 48000.0));
        if (nMax < nMin) nMax = nMin;

        //shortest window whose bin spacing is under half a semitone - overlap weights per bin
        std::vector<SpecRes> res;
        std::vector<char> valid(128, 0);
        for (int k = 0; k < 128; ++k)
        {
            const double fc = 440.0 * pow(2.0, (k - 69) / 12.0);
            const double lo = fc * pow(2.0, -1.0 / 24.0), hi = fc * pow(2.0, 1.0 / 24.0);
            if (hi >= 0.5 * rate) continue;
            int N = nMin;
            while (N < nMax && (double)rate / N > 0.5 * (hi - lo)) N <<= 1;
            const double d = (double)rate / N;
            SpecRes::Band b;
            b.key = k;
            b.invBw = 1.0 / (hi - lo);
            int j0 = (int)floor(lo / d + 0.5), j1 = (int)floor(hi / d + 0.5);
            if (j0 < 1) j0 = 1;
            if (j1 > N / 2 - 1) j1 = N / 2 - 1;
            b.j0 = j0;
            for (int j = j0; j <= j1; ++j)
            {
                const double a = (std::max)(lo, (j - 0.5) * d), c = (std::min)(hi, (j + 0.5) * d);
                b.w.push_back(c > a ? (c - a) / d : 0.0);
            }
            if (b.w.empty()) continue;
            size_t ri = 0;
            while (ri < res.size() && res[ri].N != N) ++ri;
            if (ri == res.size())
            {
                res.emplace_back();
                res.back().N = N;
            }
            res[ri].bands.push_back(std::move(b));
            valid[(size_t)k] = 1;
        }
        for (auto& r : res)
        {
            r.win.resize((size_t)r.N);
            double s2 = 0.0;
            for (int n = 0; n < r.N; ++n)
            {
                const double w = 0.5 - 0.5 * cos(kTwoPi * n / r.N);
                r.win[(size_t)n] = w;
                s2 += w * w;
            }
            r.norm = 2.0 / ((double)r.N * s2);
            r.fft.Init(r.N);
        }

        //two frames per fft - even frame in re and odd frame in im
        std::vector<float> db(nSpec * 128, -300.0f);
        const size_t nPair = (nSpec + 1) / 2;
        std::atomic<size_t> next{ 0 }, done{ 0 };
        auto mono = [&](long long i) -> double {
            return (i < 0 || i >= (long long)nFrames) ? 0.0
                                                      : 0.5 * ((double)pcm[(size_t)i * 2] + (double)pcm[(size_t)i * 2 + 1]);
        };
        auto work = [&](bool reporter) {
            std::vector<double> re((size_t)nMax), im((size_t)nMax);
            for (;;)
            {
                if (cancel.load()) return;
                const size_t pi = next.fetch_add(1);
                if (pi >= nPair) return;
                const size_t fa = pi * 2, fb = fa + 1;
                const bool hasB = fb < nSpec;
                for (const auto& r : res)
                {
                    const int N = r.N;
                    const long long sa = (long long)fa * hop + hop / 2 - N / 2;
                    const long long sb = (long long)fb * hop + hop / 2 - N / 2;
                    for (int n = 0; n < N; ++n)
                    {
                        re[(size_t)n] = r.win[(size_t)n] * mono(sa + n);
                        im[(size_t)n] = hasB ? r.win[(size_t)n] * mono(sb + n) : 0.0;
                    }
                    r.fft.Run(re.data(), im.data());
                    for (const auto& b : r.bands)
                    {
                        double pa = 0.0, pb = 0.0;
                        for (size_t q = 0; q < b.w.size(); ++q)
                        {
                            const size_t j = (size_t)b.j0 + q;
                            const double xr = re[j], xi = im[j], yr = re[(size_t)N - j], yi = im[(size_t)N - j];
                            pa += b.w[q] * ((xr + yr) * (xr + yr) + (xi - yi) * (xi - yi));
                            pb += b.w[q] * ((xr - yr) * (xr - yr) + (xi + yi) * (xi + yi));
                        }
                        db[fa * 128 + (size_t)b.key] = (float)(10.0 * log10(0.25 * pa * r.norm * b.invBw + 1e-30));
                        if (hasB) db[fb * 128 + (size_t)b.key] = (float)(10.0 * log10(0.25 * pb * r.norm * b.invBw + 1e-30));
                    }
                }
                const size_t dn = done.fetch_add(1) + 1;
                if (reporter && progress && (dn % 64) == 0) progress((int)(60 + dn * 35 / nPair));
            }
        };
        unsigned nt = std::thread::hardware_concurrency();
        if (nt < 1) nt = 1;
        if (nt > 16) nt = 16;
        std::vector<std::thread> th;
        for (unsigned t = 1; t < nt; ++t) th.emplace_back(work, false);
        work(true);
        for (auto& t : th) t.join();
        if (cancel.load()) return false;

        //hi is the 99.9 percentile - lo is 45 dB below - 32 steps between
        std::vector<float> all;
        all.reserve(nSpec * 128);
        for (size_t f = 0; f < nSpec; ++f)
            for (int k = 0; k < 128; ++k)
                if (valid[(size_t)k]) all.push_back(db[f * 128 + (size_t)k]);
        if (all.empty()) return true;
        const size_t at = (size_t)(kSpecHiPct * (double)(all.size() - 1));
        std::nth_element(all.begin(), all.begin() + (ptrdiff_t)at, all.end());
        const double hiDb = all[at];
        const double loDb = hiDb - kSpecRangeDb;
        if (!(hiDb > -200.0)) return true;
        const double step = kSpecRangeDb / kSpecLevels;
        for (size_t f = 0; f < nSpec; ++f)
            for (int k = 0; k < 128; ++k)
            {
                if (!valid[(size_t)k]) continue;
                const double d = db[f * 128 + (size_t)k];
                if (d < loDb) continue;
                int lv = 1 + (int)floor((d - loDb) / step);
                if (lv > kSpecLevels) lv = kSpecLevels;
                level[f * 128 + (size_t)k] = (uint8_t)lv;
            }
        return true;
    }

    //CC16 env and CC17 hit on channel 16 - screen fx sources - silent
    std::vector<uint8_t> BuildFxTrack(const std::vector<float>& pcm, size_t nFrames, int rate, uint32_t& fxLast)
    {
        std::vector<uint8_t> fx;
        const size_t win = (size_t)((rate / 100 > 0) ? rate / 100 : 1);
        const size_t nWin = (nFrames + win - 1) / win;
        std::vector<double> pw(nWin), hfDb(nWin);
        double pl = 0.0, pr = 0.0;
        for (size_t w = 0; w < nWin; ++w)
        {
            const size_t a = w * win, b = (std::min)(nFrames, a + win);
            double s = 0.0, hf = 0.0;
            for (size_t i = a; i < b; ++i)
            {
                const double l = pcm[i * 2], r = pcm[i * 2 + 1];
                s += l * l + r * r;
                hf += (l - pl) * (l - pl) + (r - pr) * (r - pr);
                pl = l;
                pr = r;
            }
            pw[w] = s / (double)(2 * (b - a));
            hfDb[w] = 10.0 * log10(hf / (double)(2 * (b - a)) + 1e-20);
        }
        std::vector<double> rise(nWin, 0.0);
        for (size_t w = 3; w < nWin; ++w) rise[w] = hfDb[w] - (hfDb[w - 1] + hfDb[w - 2] + hfDb[w - 3]) / 3.0;
        const double relK = exp(-10.0 / 150.0);
        std::vector<double> envDb(nWin);
        double e = 0.0;
        for (size_t w = 0; w < nWin; ++w)
        {
            e = (pw[w] > e * relK) ? pw[w] : e * relK;
            envDb[w] = 10.0 * log10(e + 1e-20);
        }
        std::vector<double> snd;
        for (double d : envDb)
            if (d > -90.0) snd.push_back(d);
        double med = -30.0;
        if (!snd.empty())
        {
            std::nth_element(snd.begin(), snd.begin() + snd.size() / 2, snd.end());
            med = snd[snd.size() / 2];
        }

        long long lastHitW = -1000;
        uint32_t last = 0;
        int lastEnv = -1;
        for (size_t w = 0; w < nWin; ++w)
        {
            const uint32_t tick = (uint32_t)(w * win);
            double dev = (envDb[w] - med) / 12.0;
            dev = (dev < -1.0) ? -1.0 : ((dev > 1.0) ? 1.0 : dev);
            const int ev = (int)lround(64.0 + 63.0 * dev);
            if (ev != lastEnv)
            {
                PutVar(fx, tick - last);
                fx.push_back(0xBF);
                fx.push_back(16);
                fx.push_back((uint8_t)ev);
                last = tick;
                lastEnv = ev;
            }
            if (w >= 3 && rise[w] >= 4.0 && (long long)w - lastHitW >= 10 && envDb[w] > med - 24.0)
            {
                bool peak = true;
                for (size_t q = (w >= 6 ? w - 3 : 3); q <= (std::min)(nWin - 1, w + 3) && peak; ++q)
                    if (rise[q] > rise[w]) peak = false;
                if (peak)
                {
                    double st = (rise[w] - 4.0) / 6.0;
                    st = (st < 0.0) ? 0.0 : ((st > 1.0) ? 1.0 : st);
                    PutVar(fx, tick - last);
                    fx.push_back(0xBF);
                    fx.push_back(17);
                    fx.push_back((uint8_t)(48 + lround(79.0 * st)));
                    last = tick;
                    lastHitW = (long long)w;
                }
            }
        }
        PutEnd(fx);
        fxLast = last;
        return fx;
    }
} //namespace

std::wstring A2MOutputPathFor(const std::wstring& inPath)
{
    const std::filesystem::path p(inPath);
    const std::wstring name = p.stem().wstring() + L"_midiver";
    std::filesystem::path dir = p.parent_path();
    //mid sf2 and a2mfreq are one set -> keep them in one folder unless already inside it
    if (dir.filename() != name) dir /= name;
    return (dir / (name + L".mid")).wstring();
}

//key -> amplitude table next to the mid - line number is the key
static bool WriteKeyTable(const std::wstring& path, const std::vector<double>& keyAmp)
{
    std::string txt = "# a2m PCM-direct waveform layout key->amplitude (index = key, 0 = unused)\n# pcm 2\n";
    char line[64];
    for (double a : keyAmp)
    {
        sprintf_s(line, "%.6f\n", a);
        txt += line;
    }
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    f.write(txt.data(), (std::streamsize)txt.size());
    return (bool)f;
}

bool A2MConvert(const std::wstring& inPath, const std::wstring& outPath,
                const std::atomic<bool>& cancel, const A2MProgressFn& progressFn,
                A2MResult& res, std::wstring& err)
{
    Progress progress{ progressFn };
    const int rate = kRate;
    progress(0);

    std::vector<float> pcm;
    if (!DecodeAudioStereo(inPath, rate, pcm, cancel, err)) return false;
    const size_t nFrames = pcm.size() / (size_t)kSides;
    if (nFrames == 0)
    {
        err = L"No samples were decoded.";
        return false;
    }
    if (nFrames >= 0xFFFFFFF0ull)
    {
        err = L"The audio is too long.";
        return false;
    }
    res.seconds = (double)nFrames / rate;
    progress(10);

    //float sources can exceed 1 -> clip first then measure the peak
    for (float& v : pcm) v = (v > 1.0f) ? 1.0f : ((v < -1.0f) ? -1.0f : v);
    double peak = 0.0;
    for (float v : pcm)
    {
        const double a = fabs((double)v);
        if (a > peak) peak = a;
    }
    if (!(peak > 0.0))
    {
        err = L"The audio is silent - nothing to convert.";
        return false;
    }
    const std::vector<double> keyAmp = PcmWaveKeyTable(peak);

    //track -> channel by side and sign then wave level - even channel positive - odd channel negative
    constexpr int kLv = kPcmWaveLevels + 1;
    const int nNoteTracks = kSides * 2 * kLv;
    std::vector<std::vector<uint8_t>> trk((size_t)nNoteTracks);
    std::vector<uint32_t> lastTick((size_t)nNoteTracks, 0);

    for (int ti = 0; ti < nNoteTracks; ++ti)
    {
        auto& t = trk[(size_t)ti];
        const int c = ti / kLv, lv = ti % kLv, side = c / 2;
        uint8_t rgb[3];
        WaveLevelRgb(side, lv, rgb);
        PutVar(t, 0);
        t.push_back((uint8_t)(0xC0 | c));
        t.push_back((uint8_t)(c & 1));
        PutVar(t, 0);
        t.push_back((uint8_t)(0xB0 | c));
        t.push_back(10);
        t.push_back((uint8_t)(side == 0 ? 0 : 127));
        PutColor(t, rgb[0], rgb[1], rgb[2]);
    }

    //one 1 tick note per sample - key is wave position - velocity is mantissa with amp following v squared
    long long noteCount = 0;
    const size_t progStep = (nFrames / 100) ? (nFrames / 100) : 1;
    for (size_t i = 0; i < nFrames; ++i)
    {
        for (int s = 0; s < kSides; ++s)
        {
            const double x = (double)pcm[i * (size_t)kSides + (size_t)s];
            const int k = (int)lround(x * kPcmWaveLevels / peak);
            const int key = (s == 0 ? kPcmWaveCenterL : kPcmWaveCenterR) + k;
            int v = (int)lround(127.0 * sqrt(fabs(x) / keyAmp[(size_t)key]));
            if (v < 1) continue;
            if (v > 127) v = 127;

            const int c = s * 2 + ((x < 0.0) ? 1 : 0);
            const int ti = c * kLv + ((k < 0) ? -k : k);
            auto& t = trk[(size_t)ti];
            PutVar(t, (uint32_t)i - lastTick[(size_t)ti]);
            t.push_back((uint8_t)(0x90 | c));
            t.push_back((uint8_t)key);
            t.push_back((uint8_t)v);
            PutVar(t, 1);
            t.push_back((uint8_t)(0x80 | c));
            t.push_back((uint8_t)key);
            t.push_back(0);
            lastTick[(size_t)ti] = (uint32_t)i + 1;
            ++noteCount;
        }
        if ((i % progStep) == 0)
        {
            if (cancel.load())
            {
                err = L"Cancelled.";
                return false;
            }
            progress((int)(10 + i * 50 / nFrames));
        }
    }
    for (auto& t : trk) PutEnd(t);

    uint32_t fxLast = 0;
    const std::vector<uint8_t> fxTrk = BuildFxTrack(pcm, nFrames, rate, fxLast);

    //spectrogram tracks in level order - later track draws on top
    std::vector<std::vector<uint8_t>> specTrk((size_t)kSpecLevels);
    long long specNotes = 0;
    {
        std::vector<uint8_t> lvl;
        size_t nSpec = 0;
        int hop = 1;
        if (!AnalyzeSpectrogram(pcm, nFrames, rate, cancel, progressFn, lvl, nSpec, hop))
        {
            err = L"Cancelled.";
            return false;
        }
        //picture notes never run past the sound layer
        uint32_t cap = fxLast;
        for (uint32_t t : lastTick) cap = (std::max)(cap, t);

        struct SpecEv
        {
            uint32_t tick;
            uint8_t on, key, vel;
        };
        std::vector<std::vector<SpecEv>> ev((size_t)kSpecLevels);
        for (int k = 0; k < 128; ++k)
        {
            int cur = 0;
            size_t f0 = 0;
            for (size_t f = 0; f <= nSpec; ++f)
            {
                const int lv = (f < nSpec) ? lvl[f * 128 + (size_t)k] : 0;
                if (lv == cur) continue;
                if (cur > 0)
                {
                    const uint32_t s = (uint32_t)(f0 * (size_t)hop);
                    uint32_t e = (uint32_t)(std::min)((size_t)cap, f * (size_t)hop);
                    //end 1 tick early so off and the next on in another track never share a tick
                    if (e > 0) --e;
                    if (e > s)
                    {
                        const uint8_t v = (uint8_t)lround(cur * 127.0 / kSpecLevels);
                        ev[(size_t)cur - 1].push_back({ s, 1, (uint8_t)k, v });
                        ev[(size_t)cur - 1].push_back({ e, 0, (uint8_t)k, 0 });
                        ++specNotes;
                    }
                }
                cur = lv;
                f0 = f;
            }
        }
        for (int l = 0; l < kSpecLevels; ++l)
        {
            auto& e = ev[(size_t)l];
            std::sort(e.begin(), e.end(), [](const SpecEv& a, const SpecEv& b) {
                if (a.tick != b.tick) return a.tick < b.tick;
                if (a.on != b.on) return a.on < b.on;
                return a.key < b.key;
            });
            auto& t = specTrk[(size_t)l];
            const uint8_t sc = (uint8_t)kSpecCh;
            //silent preset with CC7 0 and CC11 0 -> no sound with any soundfont
            PutVar(t, 0);
            t.push_back((uint8_t)(0xC0 | sc));
            t.push_back((uint8_t)kPcmSilentPreset);
            PutVar(t, 0);
            t.push_back((uint8_t)(0xB0 | sc));
            t.push_back(7);
            t.push_back(0);
            PutVar(t, 0);
            t.push_back((uint8_t)(0xB0 | sc));
            t.push_back(11);
            t.push_back(0);
            const uint32_t rgb = kInferno32[l];
            PutColor(t, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
            uint32_t last = 0;
            for (const SpecEv& x : e)
            {
                PutVar(t, x.tick - last);
                last = x.tick;
                t.push_back((uint8_t)((x.on ? 0x90 : 0x80) | sc));
                t.push_back(x.key);
                t.push_back(x.vel);
            }
            PutEnd(t);
        }
    }
    progress(95);

    //tempo track - 500000 us per quarter - format markers so the file stands alone
    const int ppq = rate / 2;
    std::vector<uint8_t> t0;
    PutVar(t0, 0);
    t0.push_back(0xFF);
    t0.push_back(0x51);
    t0.push_back(0x03);
    t0.push_back(0x07);
    t0.push_back(0xA1);
    t0.push_back(0x20);
    {
        char mk[96];
        const int n = sprintf_s(mk, "a2m-pcm 2 lv=%d lc=%d rc=%d peak=%.9g",
                                kPcmWaveLevels, kPcmWaveCenterL, kPcmWaveCenterR, peak);
        PutText(t0, mk, n);
    }
    {
        char mk[64];
        const int n = sprintf_s(mk, "a2m-spec 1 ch=%d levels=%d", kSpecCh, kSpecLevels);
        PutText(t0, mk, n);
    }
    PutEnd(t0);

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(outPath).parent_path(), ec);
    if (ec)
    {
        err = L"Could not create the output folder.";
        return false;
    }

    //write to a part file then swap in -> a failed export never leaves a broken mid
    const std::wstring tmpPath = outPath + L".part";
    {
        std::ofstream f(std::filesystem::path(tmpPath), std::ios::binary | std::ios::trunc);
        if (!f)
        {
            err = L"Could not create the output file.";
            return false;
        }
        auto put32 = [&](uint32_t n) {
            const char b[4] = { (char)(n >> 24), (char)(n >> 16), (char)(n >> 8), (char)n };
            f.write(b, 4);
        };
        auto chunk = [&](const char* id, const std::vector<uint8_t>& body) {
            f.write(id, 4);
            put32((uint32_t)body.size());
            if (!body.empty()) f.write((const char*)body.data(), (std::streamsize)body.size());
        };
        const uint16_t ntrk = (uint16_t)(1 + nNoteTracks + 1 + kSpecLevels);
        const std::vector<uint8_t> mthd = { 0, 1, (uint8_t)(ntrk >> 8), (uint8_t)ntrk,
                                            (uint8_t)(ppq >> 8), (uint8_t)ppq };
        chunk("MThd", mthd);
        chunk("MTrk", t0);
        for (auto& t : trk) chunk("MTrk", t);
        chunk("MTrk", fxTrk);
        for (auto& t : specTrk) chunk("MTrk", t);
        f.flush();
        if (!f)
        {
            f.close();
            DeleteFileW(tmpPath.c_str());
            err = L"Writing the output file failed (disk full?).";
            return false;
        }
    }
    if (!MoveFileExW(tmpPath.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmpPath.c_str());
        err = L"Could not replace the output file (is it open in another program?).";
        return false;
    }

    //impulse sf2 -> any sf2 player can play the mid - a2mfreq keeps the key table next to it
    std::filesystem::path base(outPath);
    if (!WriteKeyTable(base.replace_extension(L".a2mfreq").wstring(), keyAmp))
    {
        err = L"Could not write the a2mfreq table.";
        return false;
    }
    if (!WritePcmImpulseFont(base.replace_extension(L".sf2").wstring(), keyAmp, rate, err)) return false;
    res.fontPath = base.wstring();

    res.soundNotes = noteCount;
    res.specNotes = specNotes;
    progress(100);
    return true;
}
