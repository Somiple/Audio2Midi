#include "pcm_font.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    enum
    {
        GEN_INSTRUMENT = 41,
        GEN_KEYRANGE = 43,
        GEN_SAMPLEID = 53,
        GEN_SAMPLEMODES = 54,
        GEN_ROOTKEY = 58,
    };

    void PutU8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }

    void PutU16(std::vector<uint8_t>& v, uint16_t x)
    {
        v.push_back((uint8_t)(x & 0xFF));
        v.push_back((uint8_t)(x >> 8));
    }

    void PutU32(std::vector<uint8_t>& v, uint32_t x)
    {
        for (int i = 0; i < 4; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
    }

    //20 bytes - null padded
    void PutName(std::vector<uint8_t>& v, const char* s)
    {
        char b[20] = {};
        for (int i = 0; i < 19 && s[i]; ++i) b[i] = s[i];
        v.insert(v.end(), b, b + 20);
    }

    //null terminated - even length
    void PutZstr(std::vector<uint8_t>& v, const char* s)
    {
        const size_t n = strlen(s) + 1;
        v.insert(v.end(), s, s + n);
        if (n & 1) v.push_back(0);
    }

    void PutChunk(std::vector<uint8_t>& v, const char id[4], const std::vector<uint8_t>& body)
    {
        v.insert(v.end(), id, id + 4);
        PutU32(v, (uint32_t)body.size());
        v.insert(v.end(), body.begin(), body.end());
        if (body.size() & 1) v.push_back(0);
    }

    void PutGen(std::vector<uint8_t>& v, uint16_t oper, uint16_t amount)
    {
        PutU16(v, oper);
        PutU16(v, amount);
    }

    struct Zone
    {
        int key;
        double amp;
    };

    struct Shdr
    {
        uint32_t start, end, loopS, loopE;
        uint8_t rootKey;
        char name[24];
    };
}

bool WritePcmImpulseFont(const std::wstring& path, const std::vector<double>& keyAmp, int sampleRate, std::wstring& err)
{
    std::vector<Zone> zones;
    for (size_t k = 0; k < keyAmp.size() && k < 128; ++k)
    {
        if (!(keyAmp[k] > 0.0)) continue;
        zones.push_back({ (int)k, keyAmp[k] < 1.0 ? keyAmp[k] : 1.0 });
    }
    const int K = (int)zones.size();
    if (K <= 0)
    {
        err = L"The impulse font has no keys.";
        return false;
    }
    if (sampleRate <= 0)
    {
        err = L"Invalid sample rate for the impulse font.";
        return false;
    }

    //impulse sits 192 samples in -> after the 4 ms voice stop ramp of BASSMIDI - a 1 tick note would cut it otherwise
    constexpr int kLead = 192;
    constexpr int kTail = 1;
    constexpr int kGap = 46;
    constexpr int kSilentLen = 8;

    std::vector<int16_t> pcm;
    pcm.reserve((size_t)K * 2 * (kLead + 1 + kTail + kGap) + kSilentLen + kGap);
    std::vector<Shdr> shdrs;
    shdrs.reserve((size_t)K * 2 + 1);

    for (int sign = 0; sign < 2; ++sign)
    {
        for (int e = 0; e < K; ++e)
        {
            const uint32_t start = (uint32_t)pcm.size();
            pcm.insert(pcm.end(), (size_t)kLead, (int16_t)0);
            const double v = 32767.0 * zones[(size_t)e].amp;
            const int s = (int)lround(sign ? -v : v);
            pcm.push_back((int16_t)(std::clamp)(s, -32767, 32767));
            pcm.insert(pcm.end(), (size_t)kTail, (int16_t)0);
            Shdr sh{};
            sh.start = start;
            sh.end = start + (uint32_t)kLead + 1 + (uint32_t)kTail;
            sh.loopS = start;
            sh.loopE = sh.end - 1;
            sh.rootKey = (uint8_t)zones[(size_t)e].key;
            sprintf_s(sh.name, "%sk%d", sign ? "neg" : "pos", zones[(size_t)e].key);
            shdrs.push_back(sh);
            pcm.insert(pcm.end(), (size_t)kGap, (int16_t)0);
        }
    }
    {
        //silent sample for the spectrogram preset
        const uint32_t start = (uint32_t)pcm.size();
        pcm.insert(pcm.end(), (size_t)kSilentLen, (int16_t)0);
        Shdr sh{};
        sh.start = start;
        sh.end = start + (uint32_t)kSilentLen;
        sh.loopS = start;
        sh.loopE = sh.end - 1;
        sh.rootKey = 60;
        sprintf_s(sh.name, "silent");
        shdrs.push_back(sh);
        pcm.insert(pcm.end(), (size_t)kGap, (int16_t)0);
    }
    constexpr int nPre = 3;

    std::vector<uint8_t> phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;

    for (int p = 0; p < nPre; ++p)
    {
        PutName(phdr, p == kPcmSilentPreset ? "pcm silent" : (p ? "pcm negative" : "pcm positive"));
        PutU16(phdr, (uint16_t)p);
        PutU16(phdr, 0);
        PutU16(phdr, (uint16_t)p);
        PutU32(phdr, 0);
        PutU32(phdr, 0);
        PutU32(phdr, 0);
        PutU16(pbag, (uint16_t)p);
        PutU16(pbag, 0);
        PutGen(pgen, GEN_INSTRUMENT, (uint16_t)p);
    }
    PutName(phdr, "EOP");
    PutU16(phdr, 0);
    PutU16(phdr, 0);
    PutU16(phdr, (uint16_t)nPre);
    PutU32(phdr, 0);
    PutU32(phdr, 0);
    PutU32(phdr, 0);
    PutU16(pbag, (uint16_t)nPre);
    PutU16(pbag, 0);
    PutGen(pgen, 0, 0);
    for (int i = 0; i < 10; ++i) PutU8(pmod, 0);

    //keyrange rootkey samplemodes sampleid - no envelope and never an exclusive class
    constexpr int kGensPerZone = 4;
    int zoneNdx = 0;
    for (int p = 0; p < 2; ++p)
    {
        PutName(inst, p ? "pcm neg inst" : "pcm pos inst");
        PutU16(inst, (uint16_t)zoneNdx);
        for (int e = 0; e < K; ++e)
        {
            PutU16(ibag, (uint16_t)(zoneNdx * kGensPerZone));
            PutU16(ibag, 0);
            ++zoneNdx;
            const uint8_t key = (uint8_t)zones[(size_t)e].key;
            PutGen(igen, GEN_KEYRANGE, (uint16_t)(key | ((uint16_t)key << 8)));
            PutGen(igen, GEN_ROOTKEY, (uint16_t)key);
            PutGen(igen, GEN_SAMPLEMODES, 0);
            PutGen(igen, GEN_SAMPLEID, (uint16_t)(p * K + e));
        }
    }
    PutName(inst, "pcm silent inst");
    PutU16(inst, (uint16_t)zoneNdx);
    PutU16(ibag, (uint16_t)(zoneNdx * kGensPerZone));
    PutU16(ibag, 0);
    ++zoneNdx;
    PutGen(igen, GEN_KEYRANGE, (uint16_t)(0 | (127u << 8)));
    PutGen(igen, GEN_ROOTKEY, 60);
    PutGen(igen, GEN_SAMPLEMODES, 0);
    PutGen(igen, GEN_SAMPLEID, (uint16_t)(2 * K));
    PutName(inst, "EOI");
    PutU16(inst, (uint16_t)zoneNdx);
    PutU16(ibag, (uint16_t)(zoneNdx * kGensPerZone));
    PutU16(ibag, 0);
    PutGen(igen, 0, 0);
    for (int i = 0; i < 10; ++i) PutU8(imod, 0);

    for (const Shdr& sh : shdrs)
    {
        PutName(shdr, sh.name);
        PutU32(shdr, sh.start);
        PutU32(shdr, sh.end);
        PutU32(shdr, sh.loopS);
        PutU32(shdr, sh.loopE);
        PutU32(shdr, (uint32_t)sampleRate);
        PutU8(shdr, sh.rootKey);
        PutU8(shdr, 0);
        PutU16(shdr, 0);
        PutU16(shdr, 1);
    }
    PutName(shdr, "EOS");
    for (int i = 0; i < 5; ++i) PutU32(shdr, 0);
    PutU8(shdr, 0);
    PutU8(shdr, 0);
    PutU16(shdr, 0);
    PutU16(shdr, 0);

    std::vector<uint8_t> info;
    info.insert(info.end(), { 'I', 'N', 'F', 'O' });
    {
        std::vector<uint8_t> b;
        PutU16(b, 2);
        PutU16(b, 1);
        PutChunk(info, "ifil", b);
    }
    {
        std::vector<uint8_t> b;
        PutZstr(b, "EMU8000");
        PutChunk(info, "isng", b);
    }
    {
        std::vector<uint8_t> b;
        PutZstr(b, "Audio2Midi PCM impulse");
        PutChunk(info, "INAM", b);
    }
    {
        std::vector<uint8_t> b;
        PutZstr(b, "Audio2Midi");
        PutChunk(info, "ISFT", b);
    }

    std::vector<uint8_t> sdta;
    sdta.insert(sdta.end(), { 's', 'd', 't', 'a' });
    {
        std::vector<uint8_t> b((size_t)pcm.size() * 2);
        memcpy(b.data(), pcm.data(), b.size());
        PutChunk(sdta, "smpl", b);
    }

    std::vector<uint8_t> pdta;
    pdta.insert(pdta.end(), { 'p', 'd', 't', 'a' });
    PutChunk(pdta, "phdr", phdr);
    PutChunk(pdta, "pbag", pbag);
    PutChunk(pdta, "pmod", pmod);
    PutChunk(pdta, "pgen", pgen);
    PutChunk(pdta, "inst", inst);
    PutChunk(pdta, "ibag", ibag);
    PutChunk(pdta, "imod", imod);
    PutChunk(pdta, "igen", igen);
    PutChunk(pdta, "shdr", shdr);

    std::vector<uint8_t> body;
    body.insert(body.end(), { 's', 'f', 'b', 'k' });
    PutChunk(body, "LIST", info);
    PutChunk(body, "LIST", sdta);
    PutChunk(body, "LIST", pdta);
    std::vector<uint8_t> file;
    PutChunk(file, "RIFF", body);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp)
    {
        err = L"Could not create the sf2 file.";
        return false;
    }
    const size_t wrote = fwrite(file.data(), 1, file.size(), fp);
    fclose(fp);
    if (wrote != file.size())
    {
        err = L"Writing the sf2 file failed.";
        return false;
    }
    return true;
}
