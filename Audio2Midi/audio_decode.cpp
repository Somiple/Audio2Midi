#include "audio_decode.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static std::wstring FindFfmpeg()
{
    std::wstring exe(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
        if (n < exe.size())
        {
            exe.resize(n);
            break;
        }
        exe.resize(exe.size() * 2);
    }
    const size_t sep = exe.find_last_of(L"\\/");
    const std::wstring local = exe.substr(0, sep + 1) + L"ffmpeg.exe";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;

    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

static bool DecodeFfmpeg(const std::wstring& ffmpeg, const std::wstring& path, int rate,
                         std::vector<float>& out, const std::atomic<bool>& cancel)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 1 << 20)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + ffmpeg + L"\" -nostdin -v error -i \"" + path +
                       L"\" -vn -ac 2 -ar " + std::to_wstring(rate) + L" -f f32le -";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = hWrite;
    si.hStdError = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok)
    {
        CloseHandle(hRead);
        return false;
    }

    //pipe reads can split a float -> carry the leftover bytes
    std::vector<uint8_t> buf((1 << 16) + 4);
    size_t carry = 0;
    for (;;)
    {
        if (cancel.load()) break;
        DWORD n = 0;
        if (!ReadFile(hRead, buf.data() + carry, (DWORD)(buf.size() - 4), &n, nullptr) || n == 0) break;
        const size_t total = carry + n, whole = total / 4;
        const size_t at = out.size();
        out.resize(at + whole);
        std::memcpy(out.data() + at, buf.data(), whole * 4);
        carry = total - whole * 4;
        if (carry) std::memmove(buf.data(), buf.data() + whole * 4, carry);
    }
    CloseHandle(hRead);
    DWORD code = 1;
    if (cancel.load()) TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    out.resize(out.size() & ~(size_t)1);
    return !out.empty() && !cancel.load();
}

static bool DecodeMediaFoundation(const std::wstring& path, int rate, std::vector<float>& out,
                                  const std::atomic<bool>& cancel)
{
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return false;
    }

    IMFAttributes* attr = nullptr;
    IMFSourceReader* rd = nullptr;
    IMFMediaType* mt = nullptr;
    bool ok = false;
    const DWORD as = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    do
    {
        //the reader inserts the decoder and resampler for the type we ask for
        if (FAILED(MFCreateAttributes(&attr, 1))) break;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attr, &rd))) break;
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(rd->SetStreamSelection(as, TRUE))) break;

        if (FAILED(MFCreateMediaType(&mt))) break;
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        mt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        mt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
        mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        mt->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
        mt->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)rate * 8);
        mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (FAILED(rd->SetCurrentMediaType(as, nullptr, mt))) break;

        for (;;)
        {
            if (cancel.load()) break;
            DWORD flags = 0;
            IMFSample* s = nullptr;
            if (FAILED(rd->ReadSample(as, 0, nullptr, &flags, nullptr, &s))) break;
            if (s)
            {
                IMFMediaBuffer* b = nullptr;
                if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)))
                {
                    BYTE* p = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(b->Lock(&p, nullptr, &len)))
                    {
                        const float* f = reinterpret_cast<const float*>(p);
                        out.insert(out.end(), f, f + len / 4);
                        b->Unlock();
                    }
                    b->Release();
                }
                s->Release();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                ok = true;
                break;
            }
            if (flags & MF_SOURCE_READERF_ERROR) break;
        }
    } while (false);

    if (mt) mt->Release();
    if (rd) rd->Release();
    if (attr) attr->Release();
    MFShutdown();
    if (SUCCEEDED(hrCo)) CoUninitialize();

    out.resize(out.size() & ~(size_t)1);
    return ok && !out.empty();
}

bool DecodeAudioStereo(const std::wstring& path, int rate, std::vector<float>& out,
                       const std::atomic<bool>& cancel, std::wstring& err)
{
    out.clear();
    const std::wstring ffmpeg = FindFfmpeg();
    if (!ffmpeg.empty() && DecodeFfmpeg(ffmpeg, path, rate, out, cancel)) return true;
    if (cancel.load())
    {
        err = L"Cancelled.";
        return false;
    }

    out.clear();
    if (DecodeMediaFoundation(path, rate, out, cancel)) return true;
    if (cancel.load())
    {
        err = L"Cancelled.";
        return false;
    }

    err = ffmpeg.empty()
              ? L"Could not decode the audio.\nPut ffmpeg.exe next to Audio2Midi.exe or on PATH "
                L"(needed for opus/ogg and most other formats)."
              : L"Could not decode the audio.\nCheck that the file is a valid audio file.";
    return false;
}
