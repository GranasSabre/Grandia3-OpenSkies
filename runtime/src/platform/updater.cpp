// Updater: at the start the game asks GitHub for the newest release (only GitHub is contacted, nothing
// about the player is sent). When it is newer than this build, a small window with the title screen
// logo offers it: [Update now] downloads the release's PC zip, checks it against GitHub's SHA-256
// digest and the zip's own CRCs, puts the replaced files into update_backup\ and the new ones in place,
// and starts the new version. Settings (grandia3.ini), saves, savestates, memory cards, the player's
// disc data and HD texture pack are never touched (and are not in the zip anyway).
// Setting update_check: -1 = not asked yet (asked once at the first start), 0 = off, 1 = on;
// update_skip = a version the player chose to skip. Test flags: --update-url=URL (a local server that
// mimics the GitHub API), --update-auto (window off screen, clicks "update"), --update-shot=DIR.
#ifdef _WIN32
#include "updater.h"
#include "platform.h"
#include "../version.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>

namespace updater {

using platform::tr;

static const wchar_t* API_URL = L"https://api.github.com/repos/GranasSabre/Grandia3-OpenSkies/releases/latest";
static const wchar_t* PAGE_URL = L"https://github.com/GranasSabre/Grandia3-OpenSkies/releases/latest";

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------------------------------
// JSON (just enough for the GitHub release object)
// ---------------------------------------------------------------------------------------------------
struct Json {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    std::string s;
    double n = 0;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json* get(const char* k) const {
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    std::string str(const char* k) const {
        const Json* j = get(k);
        return j && j->t == STR ? j->s : std::string();
    }
};

struct JsonParser {
    const char* p;
    const char* e;
    bool ok = true;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    static void put_utf8(std::string& s, u32 c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | c >> 6); s += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += (char)(0xE0 | c >> 12); s += (char)(0x80 | (c >> 6 & 63)); s += (char)(0x80 | (c & 63)); }
        else { s += (char)(0xF0 | c >> 18); s += (char)(0x80 | (c >> 12 & 63)); s += (char)(0x80 | (c >> 6 & 63)); s += (char)(0x80 | (c & 63)); }
    }
    u32 hex4() {
        u32 v = 0;
        for (int i = 0; i < 4; i++) {
            if (p >= e) { ok = false; return 0; }
            char c = *p++;
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (ok = false, 0));
        }
        return v;
    }
    std::string string() {
        std::string s;
        p++;  // "
        while (p < e && *p != '"') {
            char c = *p++;
            if (c != '\\') { s += c; continue; }
            if (p >= e) break;
            c = *p++;
            switch (c) {
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u': {
                    u32 cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= e && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        u32 lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(s, cp);
                    break;
                }
                default: s += c;
            }
        }
        if (p < e) p++; else ok = false;
        return s;
    }
    Json value(int depth = 0) {
        Json j;
        ws();
        if (p >= e || depth > 64) { ok = false; return j; }
        if (*p == '{') {
            j.t = Json::OBJ;
            p++;
            ws();
            if (p < e && *p == '}') { p++; return j; }
            while (ok) {
                ws();
                if (p >= e || *p != '"') { ok = false; break; }
                std::string k = string();
                ws();
                if (p >= e || *p != ':') { ok = false; break; }
                p++;
                j.o.emplace_back(k, value(depth + 1));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false;
            }
        } else if (*p == '[') {
            j.t = Json::ARR;
            p++;
            ws();
            if (p < e && *p == ']') { p++; return j; }
            while (ok) {
                j.a.push_back(value(depth + 1));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false;
            }
        } else if (*p == '"') {
            j.t = Json::STR;
            j.s = string();
        } else if (*p == 't' || *p == 'f') {
            j.t = Json::BOOL;
            j.n = *p == 't';
            p += *p == 't' ? 4 : 5;
        } else if (*p == 'n') {
            p += 4;
        } else {
            j.t = Json::NUM;
            char* end;
            j.n = strtod(p, &end);
            if (end == p) ok = false;
            p = end;
        }
        if (p > e) { p = e; ok = false; }
        return j;
    }
};

// ---------------------------------------------------------------------------------------------------
// HTTP (WinHTTP; follows GitHub's redirect to its download server)
// ---------------------------------------------------------------------------------------------------
static bool http_get(const std::wstring& url, std::string& out, const std::function<bool(u64, u64)>& progress,
                     std::wstring& err, int receive_timeout_ms) {
    out.clear();
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = L"URL"; return false; }
    HINTERNET s = WinHttpOpen(L"Grandia3-OpenSkies-Updater/" G3_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"Grandia3-OpenSkies-Updater/" G3_VERSION_W, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = L"WinHttpOpen"; return false; }
    WinHttpSetTimeouts(s, 4000, 4000, 4000, receive_timeout_ms);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        wchar_t clen[32] = L"";
        len = sizeof(clen);
        u64 total = 0;
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &len, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(clen, nullptr, 10);
        if (status != 200) {
            err = L"HTTP " + std::to_wstring(status);
        } else {
            if (total) out.reserve((size_t)total);
            ok = true;
            char buf[65536];
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(r, buf, sizeof(buf), &got)) { ok = false; err = L"read " + std::to_wstring(GetLastError()); break; }
                if (!got) break;
                out.append(buf, got);
                if (progress && !progress(out.size(), total)) { ok = false; err = L"cancelled"; break; }
            }
        }
    } else {
        err = L"network " + std::to_wstring(GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// ---------------------------------------------------------------------------------------------------
// SHA-256, CRC-32, inflate, zip
// ---------------------------------------------------------------------------------------------------
static std::string sha256_hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    u8 digest[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
        BCryptFinishHash(h, digest, 32, 0) == 0) {
        static const char* X = "0123456789abcdef";
        for (u8 b : digest) { hex += X[b >> 4]; hex += X[b & 15]; }
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

static u32 crc32(const u8* p, size_t n) {
    static u32 table[256];
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    u32 c = 0xFFFFFFFFu;
    while (n--) c = table[(c ^ *p++) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// raw deflate (RFC 1951), after zlib's puff.c
struct Inflater {
    const u8* in;
    size_t n, pos = 0;
    u32 bb = 0;
    int bc = 0;
    bool bad = false;
    std::vector<u8>& out;
    Inflater(const u8* p, size_t len, std::vector<u8>& o) : in(p), n(len), out(o) {}
    struct Huff { short count[16]; short sym[320]; };

    int bits(int k) {
        u32 v = bb;
        while (bc < k) {
            if (pos >= n) { bad = true; return 0; }
            v |= (u32)in[pos++] << bc;
            bc += 8;
        }
        bb = v >> k;
        bc -= k;
        return (int)(v & ((1u << k) - 1));
    }
    static int build(Huff& h, const short* len, int cnt) {
        short offs[16];
        memset(h.count, 0, sizeof(h.count));
        for (int s = 0; s < cnt; s++) h.count[len[s]]++;
        if (h.count[0] == cnt) return 0;
        int left = 1;
        for (int l = 1; l < 16; l++) {
            left <<= 1;
            left -= h.count[l];
            if (left < 0) return left;
        }
        offs[1] = 0;
        for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
        for (int s = 0; s < cnt; s++)
            if (len[s]) h.sym[offs[len[s]]++] = (short)s;
        return left;
    }
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; l++) {
            code |= bits(1);
            if (bad) return -1;
            int count = h.count[l];
            if (code - count < first) return h.sym[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -10;
    }
    bool codes(const Huff& lc, const Huff& dc) {
        static const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lc);
            if (sym < 0 || bad) return false;
            if (sym < 256) { out.push_back((u8)sym); continue; }
            if (sym == 256) return true;
            sym -= 257;
            if (sym >= 29) return false;
            int len = LBASE[sym] + bits(LEXT[sym]);
            int ds = decode(dc);
            if (ds < 0 || ds >= 30 || bad) return false;
            size_t dist = (size_t)(DBASE[ds] + bits(DEXT[ds]));
            if (bad || dist > out.size()) return false;
            size_t from = out.size() - dist;
            for (int i = 0; i < len; i++) out.push_back(out[from + i]);
        }
    }
    bool stored() {
        bb = 0;
        bc = 0;
        if (pos + 4 > n) return false;
        unsigned len = in[pos] | in[pos + 1] << 8;
        unsigned nlen = in[pos + 2] | in[pos + 3] << 8;
        pos += 4;
        if (len != (~nlen & 0xFFFF) || pos + len > n) return false;
        out.insert(out.end(), in + pos, in + pos + len);
        pos += len;
        return true;
    }
    bool fixed() {
        static Huff lc, dc;
        static bool init = false;
        if (!init) {
            short l[288];
            int s = 0;
            for (; s < 144; s++) l[s] = 8;
            for (; s < 256; s++) l[s] = 9;
            for (; s < 280; s++) l[s] = 7;
            for (; s < 288; s++) l[s] = 8;
            build(lc, l, 288);
            for (s = 0; s < 30; s++) l[s] = 5;
            build(dc, l, 30);
            init = true;
        }
        return codes(lc, dc);
    }
    bool dynamic() {
        static const short ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short len[320];
        Huff lc, dc;
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (bad || nlen > 286 || ndist > 30) return false;
        int i = 0;
        for (; i < ncode; i++) len[ORDER[i]] = (short)bits(3);
        for (; i < 19; i++) len[ORDER[i]] = 0;
        if (bad || build(lc, len, 19) != 0) return false;
        i = 0;
        while (i < nlen + ndist) {
            int sym = decode(lc);
            if (sym < 0 || bad) return false;
            if (sym < 16) { len[i++] = (short)sym; continue; }
            int l = 0, rep;
            if (sym == 16) {
                if (i == 0) return false;
                l = len[i - 1];
                rep = 3 + bits(2);
            } else if (sym == 17) rep = 3 + bits(3);
            else rep = 11 + bits(7);
            if (bad || i + rep > nlen + ndist) return false;
            while (rep--) len[i++] = (short)l;
        }
        if (len[256] == 0) return false;
        int err = build(lc, len, nlen);
        if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return false;
        err = build(dc, len + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return false;
        return codes(lc, dc);
    }
    bool run() {
        int last;
        do {
            last = bits(1);
            int type = bits(2);
            if (bad) return false;
            bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok) return false;
        } while (!last);
        return true;
    }
};

struct ZipFile {
    std::string name;  // with backslashes
    std::vector<u8> data;
};

static u32 rd16(const u8* p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static bool unzip(const std::string& zip, std::vector<ZipFile>& files, std::wstring& err) {
    const u8* z = (const u8*)zip.data();
    size_t n = zip.size();
    if (n < 22) { err = L"zip too small"; return false; }
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i < 22 + 65536;)
        if (rd32(z + i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) { err = L"zip: no directory"; return false; }
    u32 count = rd16(z + eocd + 10), cd = rd32(z + eocd + 16);
    size_t p = cd;
    for (u32 k = 0; k < count; k++) {
        if (p + 46 > n || rd32(z + p) != 0x02014b50) { err = L"zip: bad directory"; return false; }
        u32 method = rd16(z + p + 10), crc = rd32(z + p + 16), csize = rd32(z + p + 20), usize = rd32(z + p + 24);
        u32 nlen = rd16(z + p + 28), elen = rd16(z + p + 30), clen = rd16(z + p + 32), lho = rd32(z + p + 42);
        if (p + 46 + nlen > n) { err = L"zip: bad name"; return false; }
        std::string name((const char*)z + p + 46, nlen);
        p += 46 + nlen + elen + clen;
        for (char& c : name) if (c == '/') c = '\\';
        if (name.empty() || name.back() == '\\') continue;  // a directory
        if (lho + 30 > n || rd32(z + lho) != 0x04034b50) { err = L"zip: bad entry"; return false; }
        size_t data = lho + 30 + rd16(z + lho + 26) + rd16(z + lho + 28);
        if (data + csize > n || usize > 512u * 1024 * 1024) { err = L"zip: bad size"; return false; }
        ZipFile f;
        f.name = name;
        if (method == 0) {
            f.data.assign(z + data, z + data + csize);
        } else if (method == 8) {
            f.data.reserve(usize);
            Inflater inf(z + data, csize, f.data);
            if (!inf.run()) { err = L"zip: damaged (" + widen(name) + L")"; return false; }
        } else {
            err = L"zip: unknown method";
            return false;
        }
        if (f.data.size() != usize || crc32(f.data.data(), f.data.size()) != crc) {
            err = L"zip: CRC (" + widen(name) + L")";
            return false;
        }
        files.push_back(std::move(f));
    }
    return !files.empty() || (err = L"zip: empty", false);
}

// ---------------------------------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------------------------------
static std::wstring exe_dir_w() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}
static std::wstring exe_name_w() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s(p, n);
    return s.substr(s.find_last_of(L"\\/") + 1);
}

static std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// files the updater never writes (they are not in the release zip either)
static bool protected_path(const std::string& name) {
    std::string l = lower(name);
    if (l.find("..") != std::string::npos || l.find(':') != std::string::npos || l[0] == '\\') return true;
    static const char* DIRS[] = {"savestates\\", "memcards\\", "data\\", "textures\\", "shots\\", "update_backup\\",
                                 "override\\", "shadercache\\", "texdump\\"};
    for (const char* d : DIRS) if (l.rfind(d, 0) == 0) return true;
    if (l == "grandia3.ini" || l == "grandia3.log") return true;
    for (const char* ext : {".iso", ".g3s", ".ps2", ".mcd"})
        if (l.size() > strlen(ext) && l.compare(l.size() - strlen(ext), std::string::npos, ext) == 0) return true;
    return false;
}

static void make_dirs(const std::wstring& file) {
    for (size_t i = 0; (i = file.find(L'\\', i + 1)) != std::wstring::npos;) CreateDirectoryW(file.substr(0, i).c_str(), nullptr);
}

static void delete_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static bool write_file(const std::wstring& path, const std::vector<u8>& d) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, d.data(), (DWORD)d.size(), &w, nullptr) && w == d.size();
    CloseHandle(h);
    return ok;
}

// the previous files go to update_backup\ (only the last version is kept); on any failure everything
// is put back as it was
static bool install(const std::vector<ZipFile>& files, const std::wstring& dir, const std::string& old_version,
                    std::wstring& err, std::wstring& new_exe) {
    std::wstring backup = dir + L"\\update_backup";
    delete_tree(backup);
    if (!CreateDirectoryW(backup.c_str(), nullptr)) {
        err = tr(L"Keine Schreibrechte im Spielordner. Bitte das Update von Hand entpacken oder den Spielordner an einen anderen Ort verschieben (nicht unter \"Programme\").");
        return false;
    }
    struct Done { std::wstring target, saved; };
    std::vector<Done> done;
    auto rollback = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            DeleteFileW(it->target.c_str());
            if (!it->saved.empty()) MoveFileExW(it->saved.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    };
    std::wstring exe = exe_name_w();
    for (const ZipFile& f : files) {
        if (protected_path(f.name)) continue;
        std::wstring rel = widen(f.name);
        // the program file keeps the name it has here (a renamed Grandia3.exe stays renamed)
        bool is_exe = lower(f.name) == "grandia3.exe";
        if (is_exe) rel = exe;
        std::wstring target = dir + L"\\" + rel, saved;
        make_dirs(target);
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            saved = backup + L"\\" + rel;
            make_dirs(saved);
            if (!MoveFileExW(target.c_str(), saved.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                err = tr(L"Eine Datei des Spiels ist gesperrt oder schreibgeschützt: ") + rel;
                rollback();
                return false;
            }
        }
        done.push_back({target, saved});
        if (!write_file(target, f.data)) {
            err = tr(L"Schreiben fehlgeschlagen (voller Datenträger?): ") + rel;
            rollback();
            return false;
        }
        if (is_exe) new_exe = target;
    }
    FILE* fv = _wfopen((backup + L"\\version.txt").c_str(), L"w");
    if (fv) {
        fprintf(fv, "%s\n", old_version.c_str());
        fclose(fv);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------
// release info
// ---------------------------------------------------------------------------------------------------
struct Release {
    std::string version;  // "1.0.9"
    std::string tag;
    std::wstring notes;   // short summary for the window
    std::wstring page;
    std::wstring url;     // the PC zip
    u64 size = 0;
    std::string sha256;   // from GitHub's asset digest ("" if none)
};

static std::vector<int> version_parts(const std::string& v) {
    std::vector<int> r;
    const char* p = v.c_str();
    while (*p && !isdigit((unsigned char)*p)) p++;
    while (*p) {
        r.push_back(atoi(p));
        while (isdigit((unsigned char)*p)) p++;
        if (*p != '.') break;
        p++;
    }
    return r;
}
static bool newer(const std::string& a, const std::string& b) {
    std::vector<int> x = version_parts(a), y = version_parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p > q;
    }
    return false;
}

// the first paragraph of the release text, markdown removed; a German one can be given as
// <!-- de: ... --> in the release text
static std::wstring summary(const std::string& body) {
    std::string b = body;
    size_t de = b.find("<!-- de:");
    if (platform::ui_german() && de != std::string::npos) {
        size_t e = b.find("-->", de);
        b = b.substr(de + 8, e == std::string::npos ? std::string::npos : e - de - 8);
    } else if (de != std::string::npos) {
        size_t e = b.find("-->", de);
        b.erase(de, e == std::string::npos ? std::string::npos : e + 3 - de);
    }
    std::string out;
    size_t i = 0;
    while (i < b.size() && (b[i] == '\r' || b[i] == '\n' || b[i] == ' ')) i++;
    for (; i < b.size(); i++) {
        char c = b[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (i + 1 < b.size() && (b[i + 1] == '\n' || b[i + 1] == '\r')) break;
            out += ' ';
            continue;
        }
        if (c == '*' || c == '`' || c == '#' || c == '_') continue;
        if (c == '[') continue;
        if (c == ']' && i + 1 < b.size() && b[i + 1] == '(') {  // [text](url): keep the text
            size_t e = b.find(')', i);
            if (e != std::string::npos) { i = e; continue; }
        }
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    std::wstring w = widen(out);
    if (w.size() > 260) {
        size_t cut = w.rfind(L' ', 255);
        w = w.substr(0, cut == std::wstring::npos ? 255 : cut) + L" …";
    }
    return w;
}

static bool fetch_release(const std::wstring& api, Release& rel, std::wstring& err) {
    std::string text;
    if (!http_get(api, text, nullptr, err, 5000)) return false;
    JsonParser jp{text.data(), text.data() + text.size()};
    Json j = jp.value();
    if (!jp.ok || j.t != Json::OBJ) { err = L"JSON"; return false; }
    rel.tag = j.str("tag_name");
    rel.version = rel.tag;
    while (!rel.version.empty() && !isdigit((unsigned char)rel.version[0])) rel.version.erase(0, 1);
    rel.page = widen(j.str("html_url"));
    if (rel.page.empty()) rel.page = PAGE_URL;
    rel.notes = summary(j.str("body"));
    const Json* assets = j.get("assets");
    if (assets && assets->t == Json::ARR)
        for (const Json& a : assets->a) {
            std::string n = lower(a.str("name"));
            if (n.size() < 4 || n.compare(n.size() - 4, 4, ".zip") != 0) continue;
            if (n.find("pc") == std::string::npos || n.find("hd") != std::string::npos) continue;
            rel.url = widen(a.str("browser_download_url"));
            const Json* sz = a.get("size");
            rel.size = sz && sz->t == Json::NUM ? (u64)sz->n : 0;
            std::string d = a.str("digest");
            if (d.rfind("sha256:", 0) == 0) rel.sha256 = lower(d.substr(7));
            break;
        }
    if (rel.version.empty()) { err = L"no tag"; return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------------
// the window
// ---------------------------------------------------------------------------------------------------
enum State { OFFER, DOWNLOADING, INSTALLING, DONE, FAILED, CHECKING, LATEST, OFFLINE };
enum Id { NONE, BTN_UPDATE, BTN_LATER, BTN_SKIP, BTN_PAGE, BTN_CANCEL, LINK_NOTES, CHECKBOX, BTN_PLAY, CHECK_AUTO };
static const int LW = 380, LH = 740;  // logical client size
static const COLORREF BG = RGB(12, 22, 52);

struct Ui {
    Release rel;
    State state = OFFER;
    std::wstring error;
    std::atomic<u64> got{0}, total{0};
    std::atomic<bool> cancel{false};
    std::atomic<int> worker_state{-1};  // set by the worker: DONE / FAILED
    std::wstring worker_error, new_exe;
    std::wstring api;                 // where the release is asked for
    std::string skip;                 // update_skip
    std::atomic<bool> fetching{false};
    bool fetch_ok = false;
    int countdown = 0;                // "Play (3)": seconds until the game starts on its own
    std::string old_version = G3_VERSION;
    int hover = NONE, press = NONE;
    bool check = true;
    bool autoplay = false;           // update_autoplay: when up to date, the game starts after 3 s on its own
    int result = 0;  // 0 = play on, 1 = updated (restart)
    float s = 1;     // DPI scale
    int w = 0, h = 0;
    std::vector<u32> bg;  // background picture, BGRA, w x h
    struct Hit { RECT r; int id; };
    std::vector<Hit> hits;
    HWND hwnd = nullptr;
    bool done = false;
};

static int S(const Ui& u, float v) { return (int)(v * u.s + 0.5f); }

// background picture from the resource (JPG), scaled to the client size
static void load_background(Ui& u) {
    u.bg.assign((size_t)u.w * u.h, 0xFF000000u | (GetRValue(BG) << 16) | (GetGValue(BG) << 8) | GetBValue(BG));
    HRSRC rs = FindResourceW(nullptr, L"UPDATER_ART", MAKEINTRESOURCEW(10));
    if (!rs) return;
    HGLOBAL hg = LoadResource(nullptr, rs);
    const void* data = LockResource(hg);
    DWORD size = SizeofResource(nullptr, rs);
    IWICImagingFactory* f = nullptr;
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        IWICStream* st = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* fr = nullptr;
        IWICBitmapScaler* sc = nullptr;
        IWICFormatConverter* cv = nullptr;
        if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)data, size)) &&
            SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
            SUCCEEDED(sc->Initialize(fr, u.w, u.h, (WICBitmapInterpolationMode)4 /* high quality cubic */)) &&
            SUCCEEDED(f->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            cv->CopyPixels(nullptr, u.w * 4, (UINT)(u.bg.size() * 4), (BYTE*)u.bg.data());
        for (IUnknown* p : {(IUnknown*)cv, (IUnknown*)sc, (IUnknown*)fr, (IUnknown*)dec, (IUnknown*)st}) if (p) p->Release();
        f->Release();
    }
    if (com) CoUninitialize();
}

// anti-aliased rounded rectangle (fill, optional border), straight into the BGRA buffer
static void round_rect(u32* px, int W, int H, float x, float y, float w, float h, float r, u32 rgb, float alpha,
                       u32 border = 0, float balpha = 0, float bw = 0) {
    int x0 = std::max(0, (int)x - 1), x1 = std::min(W, (int)(x + w) + 2);
    int y0 = std::max(0, (int)y - 1), y1 = std::min(H, (int)(y + h) + 2);
    float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - r, hy = h / 2 - r;
    auto blend = [](u32& d, u32 c, float a) {
        if (a <= 0) return;
        if (a > 1) a = 1;
        u32 r = (u32)(((d >> 16) & 255) * (1 - a) + ((c >> 16) & 255) * a);
        u32 g = (u32)(((d >> 8) & 255) * (1 - a) + ((c >> 8) & 255) * a);
        u32 b = (u32)((d & 255) * (1 - a) + (c & 255) * a);
        d = 0xFF000000u | r << 16 | g << 8 | b;
    };
    for (int py = y0; py < y1; py++)
        for (int pxx = x0; pxx < x1; pxx++) {
            float dx = fabsf(pxx + 0.5f - cx) - hx, dy = fabsf(py + 0.5f - cy) - hy;
            float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
            float dist = sqrtf(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - r;  // signed distance
            u32& d = px[py * W + pxx];
            blend(d, rgb, alpha * std::min(1.0f, std::max(0.0f, 0.5f - dist)));
            if (bw > 0) {
                float inner = std::min(1.0f, std::max(0.0f, 0.5f - dist)) - std::min(1.0f, std::max(0.0f, 0.5f - (dist + bw)));
                blend(d, border, balpha * inner);
            }
        }
}

static HFONT font(const Ui& u, float px, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-S(u, px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static std::wstring mb(u64 b) {
    wchar_t t[32];
    swprintf(t, 32, platform::ui_german() ? L"%.1f MB" : L"%.1f MB", b / 1048576.0);
    std::wstring s = t;
    if (platform::ui_german())
        for (auto& c : s) if (c == L'.') c = L',';
    return s;
}

static void paint(Ui& u, HDC target) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), u.w, -u.h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldbmp = SelectObject(dc, bmp);
    u32* px = (u32*)bits;
    memcpy(px, u.bg.data(), u.bg.size() * 4);
    u.hits.clear();

    const u32 WHITE = 0xFFFFFF, GOLD = 0xECB240, GOLD_HI = 0xF6C862;
    auto R = [&](float x, float y, float w, float h) { return RECT{S(u, x), S(u, y), S(u, x + w), S(u, y + h)}; };
    struct Text { std::wstring t; RECT r; HFONT f; COLORREF c; UINT fmt; };
    std::vector<Text> texts;
    HFONT f_title = font(u, 21, FW_SEMIBOLD), f_body = font(u, 13.5f, FW_NORMAL), f_small = font(u, 12.5f, FW_NORMAL),
          f_btn = font(u, 15, FW_SEMIBOLD), f_btn2 = font(u, 14, FW_NORMAL), f_check = font(u, 13, FW_NORMAL, L"Segoe UI Symbol");

    auto button = [&](int id, float y, float h, const std::wstring& label, bool primary) {
        float x = 32, w = LW - 64;
        bool hot = u.hover == id;
        if (primary) round_rect(px, u.w, u.h, x * u.s, y * u.s, w * u.s, h * u.s, 9 * u.s, hot ? GOLD_HI : GOLD, 1.0f);
        else round_rect(px, u.w, u.h, x * u.s, y * u.s, w * u.s, h * u.s, 9 * u.s, WHITE, hot ? 0.16f : 0.08f, WHITE, 0.28f, 1.0f * u.s);
        RECT r = R(x, y, w, h);
        texts.push_back({label, r, primary ? f_btn : f_btn2, primary ? RGB(22, 24, 40) : RGB(236, 240, 252), DT_CENTER | DT_VCENTER | DT_SINGLELINE});
        u.hits.push_back({r, id});
    };
    auto link = [&](int id, float y, const std::wstring& label, UINT align = DT_CENTER) {
        RECT r = R(32, y, LW - 64, 20);
        texts.push_back({label, r, f_small, u.hover == id ? RGB(255, 214, 120) : RGB(150, 170, 215), align | DT_VCENTER | DT_SINGLELINE});
        u.hits.push_back({r, id});
    };

    const wchar_t* title = u.state == FAILED ? tr(L"Update fehlgeschlagen")
                           : u.state == DONE ? tr(L"Update installiert")
                           : u.state == CHECKING ? tr(L"Suche nach Updates …")
                           : u.state == LATEST ? tr(L"Alles aktuell")
                           : u.state == OFFLINE ? tr(L"Keine Verbindung zu GitHub")
                                                : tr(L"Neue Version verfügbar");
    texts.push_back({title, R(20, 380, LW - 40, 30), f_title, RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE | DT_VCENTER});
    bool offer_like = u.state == OFFER || u.state == DOWNLOADING || u.state == INSTALLING || u.state == DONE || u.state == FAILED;
    std::wstring ver = offer_like ? L"v" + widen(u.rel.version) + L"   ·   " + tr(L"installiert: ") + L"v" + G3_VERSION_W
                                  : tr(L"installiert: ") + std::wstring(L"v") + G3_VERSION_W;
    texts.push_back({ver, R(20, 410, LW - 40, 20), f_small, RGB(160, 178, 220), DT_CENTER | DT_SINGLELINE | DT_VCENTER});

    // the panel: what's new / the error
    round_rect(px, u.w, u.h, 24 * u.s, 440 * u.s, (LW - 48) * u.s, 112 * u.s, 10 * u.s, WHITE, 0.06f, WHITE, 0.10f, 1.0f * u.s);
    std::wstring body = u.state == FAILED ? u.error : u.rel.notes;
    if (u.state == CHECKING) body = tr(L"Einen Moment, GitHub wird gefragt, ob es eine neue Version gibt.");
    if (u.state == LATEST) body = tr(L"Du hast die neueste Version von Grandia III: Open Skies. Viel Spaß beim Spielen!");
    if (u.state == OFFLINE) body = tr(L"GitHub ist gerade nicht erreichbar (keine Internetverbindung?). Das Spiel startet ohne Update-Prüfung.");
    if (body.empty()) body = tr(L"Eine neue Version von Grandia III: Open Skies ist erschienen.");
    texts.push_back({body, R(38, 450, LW - 76, 94), f_body, u.state == FAILED ? RGB(255, 190, 170) : RGB(222, 228, 245),
                     DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL});
    link(LINK_NOTES, 556, tr(L"Alle Änderungen auf GitHub ansehen  ›"));

    if (u.state == OFFER) {
        button(BTN_UPDATE, 588, 44, tr(L"Jetzt aktualisieren"), true);
        button(BTN_LATER, 640, 38, tr(L"Später"), false);
        link(BTN_SKIP, 682, tr(L"Diese Version überspringen"));
        RECT cr = R(32, 710, LW - 64, 20);
        std::wstring cl = std::wstring(u.check ? L"☑  " : L"☐  ") + tr(L"Beim Start nach Updates suchen");
        texts.push_back({cl, cr, f_check, u.hover == CHECKBOX ? RGB(236, 240, 252) : RGB(150, 170, 215), DT_CENTER | DT_VCENTER | DT_SINGLELINE});
        u.hits.push_back({cr, CHECKBOX});
    } else if (u.state == DOWNLOADING || u.state == INSTALLING || u.state == DONE) {
        u64 got = u.got, total = u.total ? u.total.load() : u.rel.size;
        float frac = u.state == DOWNLOADING ? (total ? (float)got / total : 0) : 1.0f;
        std::wstring label = u.state == DOWNLOADING ? tr(L"Wird heruntergeladen …") + std::wstring(L"   ") + mb(got) + L" / " + mb(total)
                             : u.state == INSTALLING ? std::wstring(tr(L"Wird installiert …"))
                                                     : std::wstring(tr(L"Fertig! Das Spiel startet neu …"));
        texts.push_back({label, R(32, 588, LW - 64, 22), f_body, RGB(236, 240, 252), DT_CENTER | DT_SINGLELINE | DT_VCENTER});
        round_rect(px, u.w, u.h, 32 * u.s, 618 * u.s, (LW - 64) * u.s, 12 * u.s, 6 * u.s, WHITE, 0.12f);
        if (frac > 0) round_rect(px, u.w, u.h, 32 * u.s, 618 * u.s, std::max(12.0f, (LW - 64) * frac) * u.s, 12 * u.s, 6 * u.s, GOLD, 1.0f);
        if (u.state == DOWNLOADING) button(BTN_CANCEL, 644, 38, tr(L"Abbrechen"), false);
    } else if (u.state == FAILED) {
        button(BTN_PAGE, 588, 44, tr(L"Download-Seite öffnen"), true);
        button(BTN_LATER, 640, 38, tr(L"Ohne Update weiterspielen"), false);
    } else if (u.state == LATEST || u.state == OFFLINE || u.state == CHECKING) {
        std::wstring play = tr(L"Spielen");
        if (u.countdown > 0) play += L"  (" + std::to_wstring(u.countdown) + L")";
        button(BTN_PLAY, 588, 44, play, true);
        RECT ar = R(32, 682, LW - 64, 20);
        std::wstring al = std::wstring(u.autoplay ? L"☑  " : L"☐  ") + tr(L"Ohne Klick starten, wenn alles aktuell ist");
        texts.push_back({al, ar, f_check, u.hover == CHECK_AUTO ? RGB(236, 240, 252) : RGB(150, 170, 215), DT_CENTER | DT_VCENTER | DT_SINGLELINE});
        u.hits.push_back({ar, CHECK_AUTO});
        RECT cr = R(32, 710, LW - 64, 20);
        std::wstring cl = std::wstring(u.check ? L"☑  " : L"☐  ") + tr(L"Beim Start nach Updates suchen");
        texts.push_back({cl, cr, f_check, u.hover == CHECKBOX ? RGB(236, 240, 252) : RGB(150, 170, 215), DT_CENTER | DT_VCENTER | DT_SINGLELINE});
        u.hits.push_back({cr, CHECKBOX});
    }

    SetBkMode(dc, TRANSPARENT);
    for (Text& t : texts) {
        SelectObject(dc, t.f);
        SetTextColor(dc, t.c);
        DrawTextW(dc, t.t.c_str(), -1, &t.r, t.fmt | DT_NOPREFIX);
    }
    if (target) BitBlt(target, 0, 0, u.w, u.h, dc, 0, 0, SRCCOPY);
    else {  // shot: the pixels stay in u.bg's copy for render_shots
        GdiFlush();
        u.bg.assign(px, px + (size_t)u.w * u.h);
    }
    for (HFONT f : {f_title, f_body, f_small, f_btn, f_btn2, f_check}) DeleteObject(f);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static Ui* g_ui = nullptr;
static bool g_auto = false;  // --update-auto: also a failure closes the window

static void start_download(Ui& u) {
    u.state = DOWNLOADING;
    u.got = 0;
    u.total = u.rel.size;
    u.cancel = false;
    HWND hwnd = u.hwnd;
    std::thread([&u, hwnd] {
        std::string zip;
        std::wstring err;
        DWORD last = 0;
        bool ok = http_get(u.rel.url, zip, [&](u64 got, u64 total) {
            u.got = got;
            if (total) u.total = total;
            DWORD now = GetTickCount();
            if (now - last > 50) { last = now; PostMessageW(hwnd, WM_APP, 0, 0); }
            return !u.cancel.load();
        }, err, 30000);
        if (u.cancel) return;
        if (ok && u.rel.size && zip.size() != u.rel.size) { ok = false; err = L"size"; }
        if (ok && !u.rel.sha256.empty() && sha256_hex(zip) != u.rel.sha256) {
            ok = false;
            err = tr(L"Die heruntergeladene Datei ist beschädigt (Prüfsumme stimmt nicht).");
        }
        if (!ok) {
            if (err.find(L' ') == std::wstring::npos || err.rfind(L"HTTP", 0) == 0 || err.rfind(L"network", 0) == 0 || err.rfind(L"read", 0) == 0)
                err = tr(L"Download fehlgeschlagen. Bitte die Internetverbindung prüfen.") + std::wstring(L" (") + err + L")";
            u.worker_error = err;
            u.worker_state = FAILED;
            PostMessageW(hwnd, WM_APP, 1, 0);
            return;
        }
        PostMessageW(hwnd, WM_APP, 2, 0);  // installing
        std::vector<ZipFile> files;
        if (!unzip(zip, files, err)) {
            u.worker_error = tr(L"Das Update-Archiv ist beschädigt: ") + err;
            u.worker_state = FAILED;
        } else if (!install(files, exe_dir_w(), u.old_version, err, u.new_exe)) {
            u.worker_error = err;
            u.worker_state = FAILED;
        } else {
            u.worker_state = DONE;
        }
        PostMessageW(hwnd, WM_APP, 1, 0);
    }).detach();
}

static void click(Ui& u, int id) {
    switch (id) {
        case BTN_UPDATE: start_download(u); break;
        case BTN_LATER:
        case BTN_PLAY: u.done = true; break;
        case BTN_SKIP:
            platform::config().update_skip = u.rel.version;
            platform::save_config();
            u.done = true;
            break;
        case BTN_PAGE:
        case LINK_NOTES:
            ShellExecuteW(nullptr, L"open", u.rel.page.empty() || u.state == CHECKING ? PAGE_URL : u.rel.page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case BTN_CANCEL: u.cancel = true; u.state = OFFER; break;
        case CHECKBOX:
            u.check = !u.check;
            platform::config().update_check = u.check ? 1 : 0;
            platform::save_config();
            break;
        case CHECK_AUTO:
            u.autoplay = !u.autoplay;
            platform::config().update_autoplay = u.autoplay;
            platform::save_config();
            if (u.hwnd && (u.state == LATEST || u.state == OFFLINE)) {
                if (u.autoplay) { u.countdown = 3; SetTimer(u.hwnd, 2, 1000, nullptr); }  // ticked: off it goes
                else { u.countdown = 0; KillTimer(u.hwnd, 2); }
            }
            break;
    }
    if (u.hwnd) InvalidateRect(u.hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Ui* u = g_ui;
    if (!u) return DefWindowProcW(h, m, wp, lp);
    auto hit = [&](LPARAM l) {
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        for (auto& x : u->hits) if (PtInRect(&x.r, p)) return x.id;
        return (int)NONE;
    };
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            paint(*u, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            int id = hit(lp);
            if (id != u->hover) { u->hover = id; InvalidateRect(h, nullptr, FALSE); }
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE: u->hover = NONE; InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: u->press = hit(lp); SetCapture(h); return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int id = hit(lp);
            if (id != NONE && id == u->press) click(*u, id);
            u->press = NONE;
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && u->hover != NONE) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
            break;
        case WM_KEYDOWN:
            if (wp == VK_RETURN) click(*u, u->state == OFFER ? BTN_UPDATE : u->state == FAILED ? BTN_PAGE : u->state == DOWNLOADING || u->state == INSTALLING || u->state == DONE ? NONE : BTN_PLAY);
            if (wp == VK_ESCAPE) click(*u, u->state == DOWNLOADING ? BTN_CANCEL : u->state == OFFER || u->state == FAILED ? BTN_LATER : u->state == INSTALLING || u->state == DONE ? NONE : BTN_PLAY);
            return 0;
        case WM_APP + 1:  // the release check is done
            if (!u->fetch_ok) u->state = OFFLINE;
            else if (newer(u->rel.version, G3_VERSION) && !u->rel.url.empty() && (u->skip != u->rel.version || g_auto)) u->state = OFFER;
            else u->state = LATEST;
            if (g_auto) {
                if (u->state == OFFER) start_download(*u);
                else u->done = true;
            } else if (u->state != OFFER && u->autoplay) {
                u->countdown = 3;  // the game starts on its own (only when the player ticked it)
                SetTimer(h, 2, 1000, nullptr);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_APP:
            if (wp == 2) u->state = INSTALLING;
            if (wp == 1 && u->worker_state >= 0) {
                if (u->worker_state == DONE) {
                    u->state = DONE;
                    SetTimer(h, 1, 900, nullptr);
                } else {
                    u->state = FAILED;
                    u->error = u->worker_error;
                    log_write(LOG_MAIN, "updater: failed: %s", narrow(u->error).c_str());
                    if (g_auto) u->done = true;
                }
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wp == 2) {
                if (u->state != LATEST && u->state != OFFLINE) { KillTimer(h, 2); return 0; }
                if (--u->countdown <= 0) { KillTimer(h, 2); u->done = true; }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            KillTimer(h, 1);
            u->result = 1;
            u->done = true;
            return 0;
        case WM_CLOSE:
            if (u->state == INSTALLING || u->state == DONE) return 0;  // never half-way
            u->cancel = true;
            u->done = true;
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int show_window(Ui& u, bool offscreen, bool auto_click) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"Grandia3Updater";
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    u.s = dpi / 96.0f;
    u.w = S(u, LW);
    u.h = S(u, LH);
    load_background(u);
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, u.w, u.h};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = offscreen ? -ww - 4000 : wa.left + (wa.right - wa.left - ww) / 2;
    int y = offscreen ? 0 : wa.top + std::max(0L, (wa.bottom - wa.top - wh) / 2);
    g_ui = &u;
    std::wstring title = std::wstring(L"Grandia III: Open Skies – ") + tr(L"Update");
    u.hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), style, x, y, ww, wh, nullptr, nullptr, hi, nullptr);
    if (!u.hwnd) { g_ui = nullptr; return 0; }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(u.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF cap = BG;
    DwmSetWindowAttribute(u.hwnd, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &cap, sizeof(cap));
    ShowWindow(u.hwnd, offscreen ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (!offscreen) SetForegroundWindow(u.hwnd);
    // ask GitHub in the background while the window says so
    u.state = CHECKING;
    u.fetching = true;
    HWND hwnd = u.hwnd;
    std::thread([&u, hwnd] {
        std::wstring err;
        double t0 = host_time_sec();
        u.fetch_ok = fetch_release(u.api, u.rel, err);
        if (u.fetch_ok)
            log_write(LOG_MAIN, "updater: newest %s (this %s), zip %s (%llu bytes, sha256 %s), %.1f s", u.rel.version.c_str(), G3_VERSION,
                      narrow(u.rel.url).c_str(), (unsigned long long)u.rel.size, u.rel.sha256.empty() ? "-" : u.rel.sha256.c_str(), host_time_sec() - t0);
        else
            log_write(LOG_MAIN, "updater: no answer (%s, %.1f s)", narrow(err).c_str(), host_time_sec() - t0);
        PostMessageW(hwnd, WM_APP + 1, 0, 0);
        u.fetching = false;
    }).detach();
    (void)auto_click;
    MSG msg;
    while (!u.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(u.hwnd);
    u.hwnd = nullptr;
    // let a cancelled download thread notice before this Ui goes away
    for (int i = 0; i < 100 && u.state == DOWNLOADING && u.worker_state < 0; i++) Sleep(20);
    for (int i = 0; i < 1000 && u.fetching; i++) Sleep(20);  // the check thread uses this Ui
    g_ui = nullptr;
    return u.result;
}

// ---------------------------------------------------------------------------------------------------
static bool run_inner(int argc, char** argv);

bool run_at_start(int argc, char** argv) {
    bool only = false;  // --update-only (tests): quit after the updater instead of starting the game
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--update-only")) only = true;
    return run_inner(argc, argv) || only;
}

static bool run_inner(int argc, char** argv) {
    platform::Config& cfg = platform::config();
    std::wstring api = API_URL;
    bool auto_click = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--update-only") continue;
        if (a.rfind("--updated-from=", 0) == 0) {
            // just updated: no check this time
            platform::show_notice(tr(L"Update installiert"), L"Grandia III: Open Skies v" G3_VERSION_W, 8.0);
            log_write(LOG_MAIN, "updater: updated from %s to %s", a.c_str() + 15, G3_VERSION);
            return false;
        }
        if (a.rfind("--update-url=", 0) == 0) api = widen(a.substr(13));
        else if (a == "--update-auto") auto_click = g_auto = true;
        else if (a.rfind("--update-shot=", 0) == 0) { render_shots(a.substr(14)); ExitProcess(0); }
        else if (a.rfind("--", 0) == 0) {
            // test and diagnosis runs never check (only the options a player might start with do)
            static const char* PLAYER[] = {"--steamdeck", "--console", "--widescreen", "--language=", "--disc=", "--aspect=",
                                           "--iso=", "--no-letterbox", "--force60", "--log=", "--scale=", "--renderer="};
            bool player = false;
            for (const char* p : PLAYER) if (a.rfind(p, 0) == 0) player = true;
            if (!player) return false;
        }
    }
    if (cfg.update_check < 0) {
        int r = MessageBoxW(nullptr, tr(L"Soll Grandia III: Open Skies beim Start nach Updates suchen?\n\nDabei wird nur GitHub gefragt, ob es eine neue Version gibt; es werden keine Daten über dich gesendet. Du kannst das jederzeit im F1-Menü ändern."),
                            L"Grandia III: Open Skies", MB_YESNO | MB_ICONQUESTION);
        cfg.update_check = r == IDYES ? 1 : 0;
        platform::save_config();
    }
    if (cfg.update_check != 1) return false;
    // the window shows every start: checking, then "up to date" (starts the game after 3 s), no
    // connection, or the offer
    Ui u;
    u.api = api;
    u.skip = cfg.update_skip;
    u.check = cfg.update_check == 1;
    u.autoplay = cfg.update_autoplay;
    if (show_window(u, auto_click, auto_click) != 1) return false;
    // start the new version with the same arguments
    std::wstring exe = u.new_exe.empty() ? exe_dir_w() + L"\\" + exe_name_w() : u.new_exe;
    std::wstring cmd = L"\"" + exe + L"\"";
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 1; i < wargc; i++) {
        std::wstring a = wargv[i];
        if (a == L"--update-auto") continue;
        cmd += L" \"" + a + L"\"";
    }
    LocalFree(wargv);
    cmd += L" --updated-from=" G3_VERSION_W;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring dir = exe_dir_w();
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        platform::message(tr(L"Das Update ist installiert. Bitte das Spiel neu starten."));
        return true;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log_write(LOG_MAIN, "updater: installed %s, restarted", u.rel.version.c_str());
    return true;
}

// ---------------------------------------------------------------------------------------------------
static bool save_png(const std::wstring& path, const u32* px, int w, int h) {
    IWICImagingFactory* f = nullptr;
    bool ok = false;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h)) &&
        SUCCEEDED(fr->SetPixelFormat(&fmt)) && SUCCEEDED(fr->WritePixels(h, w * 4, w * h * 4, (BYTE*)px)) &&
        SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
        ok = true;
    for (IUnknown* p : {(IUnknown*)fr, (IUnknown*)enc, (IUnknown*)st}) if (p) p->Release();
    f->Release();
    return ok;
}

void render_shots(const std::string& dir) {
    struct Case { const char* name; State st; int hover; float frac; const wchar_t* err; };
    const Case cases[] = {
        {"offer", OFFER, NONE, 0, nullptr},
        {"offer_hover", OFFER, BTN_UPDATE, 0, nullptr},
        {"download", DOWNLOADING, NONE, 0.42f, nullptr},
        {"done", DONE, NONE, 1, nullptr},
        {"failed", FAILED, NONE, 0, L"Download fehlgeschlagen. Bitte die Internetverbindung prüfen. (HTTP 404)"},
        {"latest", LATEST, NONE, 0, nullptr},
        {"checking", CHECKING, NONE, 0, nullptr},
    };
    for (float scale : {1.0f, 1.5f}) {
        for (const Case& c : cases) {
            Ui u;
            u.rel.version = "1.0.9";
            u.rel.notes = summary("An undub with the original Japanese voices, German and French subtitles, an automatic savestate after every map change, and a larger settings menu.\n\n**No game data**");
            u.rel.size = 19702306;
            u.s = scale;
            u.w = S(u, LW);
            u.h = S(u, LH);
            load_background(u);
            u.state = c.st;
            u.hover = c.hover;
            u.total = u.rel.size;
            u.got = (u64)(u.rel.size * c.frac);
            if (c.err) u.error = tr(c.err);
            paint(u, nullptr);
            std::wstring p = widen(dir) + L"\\updater_" + widen(c.name) + (scale > 1 ? L"_150" : L"") + L".png";
            for (u32& v : u.bg) v |= 0xFF000000u;  // GDI text leaves alpha 0
            save_png(p, u.bg.data(), u.w, u.h);
        }
    }
}

}  // namespace updater
#endif  // _WIN32
