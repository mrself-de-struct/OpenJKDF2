#include "stdJSON.h"

#include <iostream>
#include <fstream>
#ifdef _WIN32
#include <process.h>
#define STDJSON_GETPID() _getpid() // Added: stdJSON_WriteToFile's temporary file
#else
#include <unistd.h>
#define STDJSON_GETPID() getpid()
#endif
#include <vector>
#include <cstring>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// Whyyyyyyy are you like this KallistiOS toolchain
#ifdef TARGET_DREAMCAST
#include <cstdio>
#include <cstdlib>

namespace std {
    using ::snprintf;
    using ::strtoll;
    using ::strtoull;
    using ::strtold;
}
#endif

#include <nlohmann/json.hpp>
#include <filesystem>
#include <unordered_map>
#include <locale> 
#include <codecvt> 
#include "jk.h"
#include "stdPlatform.h"

namespace fs = std::filesystem;

// string (utf8) -> u16string
static std::u16string utf8_to_utf16(const std::string& utf8)
{
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert; 
    std::u16string utf16 = convert.from_bytes(utf8);
    return utf16;
}
// u16string -> string (utf8)
static std::string utf16_to_utf8(const std::u16string& utf16) {
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert; 
    std::string utf8 = convert.to_bytes(utf16);
    return utf8;
}

extern "C"
{

#define CHECK_COMMON(pFpath, pKey) \
    if (!pFpath) { \
        stdJSON_PrintNullWarning();  \
        return 0;  \
    }  \
    if (!pKey) {  \
        stdJSON_PrintNullKeyWarning();  \
        return 0;  \
    } \
    ; \

#define CHECK_COMMON_GET(pFpath, pKey, val) \
    if (!pFpath) { \
        stdJSON_PrintNullWarning();  \
        return val;  \
    }  \
    if (!pKey) {  \
        stdJSON_PrintNullKeyWarning();  \
        return val;  \
    } \
    ; \

#define CHECK_ARGPTR(pPtr) \
    if (!pPtr) { \
        stdJSON_PrintNullPtrWarning();  \
        return 0;  \
    }  \
    ; \

static inline void stdJSON_PrintNullWarning() {
    stdPlatform_Printf("WARN: stdJSON was passed a NULL pFpath.\n");
}

static inline void stdJSON_PrintNullKeyWarning() {
    stdPlatform_Printf("WARN: stdJSON was passed a NULL key.\n");
}

static inline void stdJSON_PrintNullPtrWarning() {
    stdPlatform_Printf("WARN: stdJSON was passed a NULL ptr value.\n");
}

static nlohmann::json stdJSON_OpenAndReadFile(const char* pFpath)
{
    fs::path json_path = {pFpath};
    nlohmann::json json_file(nlohmann::json::value_t::object);
    if (!fs::exists(json_path)) {
        return json_file;
    }

    std::ifstream i(json_path);
    // Altered: a file that doesn't parse (empty, cut short) threw out of here and ended the game.
    // It's kept as <file>.bad and read as empty.
    try {
        i >> json_file;
    }
    catch (const nlohmann::json::exception& e) {
        i.close();
        stdPlatform_Printf("ERROR: `%s` can't be read (%s), kept as `%s.bad`\n", pFpath, e.what(), pFpath);
        std::error_code ec;
        fs::rename(json_path, fs::path(std::string(pFpath) + ".bad"), ec);
        return nlohmann::json(nlohmann::json::value_t::object);
    }
    i.close();

    return json_file;
}

#if 0
static int stdJSON_WriteToFile(const char* pFpath, nlohmann::json& json_file)
{
    if (!pFpath)
    {
        stdPlatform_Printf("ERROR: Failed to open `(NULL)`!\n");
        return 0;
    }

    printf("asdf %s\n", pFpath);

    FILE* f = fopen("sd:/test_idk.json", "w");
    if (!f) {
        stdPlatform_Printf("ERROR: Failed to open `%s`!\n", pFpath);
        return 0;
    }

    std::string s = json_file.dump(4, ' ', true);
    size_t sz_expect = strlen(s.c_str());
    size_t sz = fwrite(s.c_str(), 1, sz_expect, f);
    if (sz != sz_expect)
    {
        stdPlatform_Printf("ERROR: Failed to write `%s`!\n", pFpath);
        return 0;
    }
    return 1;
}
#endif

static int stdJSON_WriteToFile(const char* pFpath, nlohmann::json& json_file)
{
    if (!pFpath)
    {
        stdPlatform_Printf("ERROR: Failed to open `(NULL)`!\n");
        return 0;
    }

    // Altered: written to a temporary file that then replaces the old one, so that another
    // game reading it at the same moment (two dedicated servers started from one folder)
    // never finds it empty or half written.
    fs::path json_path = {pFpath};
    fs::path tmp_path = {std::string(pFpath) + ".tmp" + std::to_string((unsigned long)STDJSON_GETPID())};
    {
        std::ofstream o(tmp_path);
        if (!o)
        {
            stdPlatform_Printf("ERROR: Failed to open `%s`!\n", pFpath);
            return 0;
        }
        o << json_file.dump(4, ' ', true);
        o.close();
        if (!o)
        {
            stdPlatform_Printf("ERROR: Failed to write `%s`!\n", pFpath);
            std::error_code ec;
            fs::remove(tmp_path, ec);
            return 0;
        }
    }
    std::error_code ec;
    fs::rename(tmp_path, json_path, ec);
    if (ec)
    {
        stdPlatform_Printf("ERROR: Failed to write `%s`!\n", pFpath);
        fs::remove(tmp_path, ec);
        return 0;
    }
    return 1;
}

int stdJSON_SaveInt(const char* pFpath, const char* pKey, int val)
{
    CHECK_COMMON(pFpath, pKey);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    json_file[pKey] = val;
    return stdJSON_WriteToFile(pFpath, json_file);
}

int stdJSON_SaveFloat(const char* pFpath, const char* pKey, flex_t val)
{
    CHECK_COMMON(pFpath, pKey);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    json_file[pKey] = (double)val; // FLEXTODO
    return stdJSON_WriteToFile(pFpath, json_file);
}

int stdJSON_GetInt(const char* pFpath, const char* pKey, int valDefault)
{
    CHECK_COMMON_GET(pFpath, pKey, valDefault);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    if (!json_file.contains(pKey)) {
        stdJSON_SaveInt(pFpath, pKey, valDefault);
        return valDefault;
    }

    auto ret = json_file[pKey];
    if (ret.is_boolean()) {
        return ret.get<bool>() ? 1 : 0;
    }

    if (!ret.is_number()) {
        stdJSON_SaveInt(pFpath, pKey, valDefault);
        return valDefault;
    }

    return ret.get<int>();
}

flex_t stdJSON_GetFloat(const char* pFpath, const char* pKey, flex_t valDefault)
{
    CHECK_COMMON_GET(pFpath, pKey, valDefault);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);

    if (!json_file.contains(pKey)) {
        stdJSON_SaveFloat(pFpath, pKey, valDefault);
        return valDefault;
    }

    auto ret = json_file[pKey];
    if (!ret.is_number()) {
        stdJSON_SaveFloat(pFpath, pKey, valDefault);
        return valDefault;
    }

    return ret.get<double>(); // FLEXTODO
}

int stdJSON_SaveBool(const char* pFpath, const char* pKey, int bVal)
{
    CHECK_COMMON(pFpath, pKey);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    json_file[pKey] = !!bVal;
    return stdJSON_WriteToFile(pFpath, json_file);
}

int stdJSON_GetBool(const char* pFpath, const char* pKey, int bValDefault)
{
    CHECK_COMMON_GET(pFpath, pKey, bValDefault);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    if (!json_file.contains(pKey)) {
        stdJSON_SaveBool(pFpath, pKey, !!bValDefault);
        return bValDefault;
    }

    auto ret = json_file[pKey];
    if (ret.is_number()) {
        return ret.get<int>() != 0 ? 1 : 0;
    }

    if (!ret.is_boolean()) {
        stdJSON_SaveBool(pFpath, pKey, !!bValDefault);
        return bValDefault;
    }

    return ret.get<bool>() ? 1 : 0;
}

int stdJSON_SaveBytes(const char* pFpath, const char* pKey, uint8_t *pData, uint32_t dataLen)
{
    CHECK_COMMON(pFpath, pKey);
    CHECK_ARGPTR(pData);

    std::vector<uint8_t> data(pData, pData+dataLen);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    json_file[pKey] = data;
    return stdJSON_WriteToFile(pFpath, json_file);
}

int stdJSON_GetBytes(const char* pFpath, const char* pKey, uint8_t* pData, uint32_t dataLen)
{
    CHECK_COMMON(pFpath, pKey);
    CHECK_ARGPTR(pData);

    std::vector<uint8_t> out(pData, pData+dataLen);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    auto ret = json_file[pKey];
    if (!ret.is_array()) {
        stdJSON_SaveBytes(pFpath, pKey, out.data(), dataLen);
    }
    else {
        out = json_file[pKey].get<std::vector<uint8_t>>();
    }

    std::copy_n(out.begin(), dataLen, pData);

    return 1;
}

int stdJSON_SetString(const char* pFpath, const char* pKey, const char *pVal)
{
    CHECK_COMMON(pFpath, pKey);
    CHECK_ARGPTR(pVal);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    json_file[pKey] = std::string(pVal);
    return stdJSON_WriteToFile(pFpath, json_file);
}

int stdJSON_GetString(const char* pFpath, const char* pKey, char* pOut, int outSize, const char *pValDefault)
{
    CHECK_COMMON(pFpath, pKey);
    CHECK_ARGPTR(pOut);
    CHECK_ARGPTR(pValDefault);

    std::string out = "";

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    out = json_file.value(std::string(pKey), std::string(pValDefault));
    if (out == std::string(pValDefault)) {
        stdJSON_SetString(pFpath, pKey, pValDefault);
    }
    
    // Fix: _strncpy does NOT NUL-terminate when strlen(src) >= n. The old code
    // clamped n down to strlen(src) then _strncpy'd exactly that many bytes, so
    // pOut was left unterminated and callers read trailing stack garbage after the
    // value (e.g. playerShortName "Max" produced player/ dirs like "Max\x01\x01").
    // Copy min(strlen, outSize-1) bytes and always write the terminator.
    if (outSize > 0) {
        size_t readSize = strlen(out.c_str());
        if (readSize > (size_t)(outSize - 1)) {
            readSize = (size_t)(outSize - 1);
        }
        _strncpy(pOut, out.c_str(), readSize);
        pOut[readSize] = 0;
    }

    return 1;
}

/*
static std::string utf16_to_utf8( std::u16string&& utf16_string )
{
   std::wstring_convert<std::codecvt_utf8_utf16<int16_t>, int16_t> convert;
   auto p = reinterpret_cast<const int16_t *>( utf16_string.data() );
   return convert.to_bytes( p, p + utf16_string.size() );
}

static std::u16string utf8_to_utf16( std::string&& utf8_string )
{
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert; 
    std::u16string dest = convert.from_bytes(utf8_string);
}
*/

int stdJSON_SetWString(const char* pFpath, const char* pKey, const char16_t *pVal)
{
    CHECK_COMMON(pFpath, pKey);
    CHECK_ARGPTR(pVal);

    std::string val = utf16_to_utf8(std::u16string(pVal));

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    json_file[pKey] = val;
    return stdJSON_WriteToFile(pFpath, json_file);
}

int stdJSON_GetWString(const char* pFpath, const char* pKey, char16_t* pOut, int outSize, const char16_t *pValDefault)
{
    CHECK_COMMON(pFpath, pKey);
    CHECK_ARGPTR(pOut);
    CHECK_ARGPTR(pValDefault);

    std::u16string out = std::u16string(pValDefault);
    std::string out_u8 = "";

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    auto ret = json_file[pKey];
    if (!ret.is_string()) {
        stdJSON_SetWString(pFpath, pKey, pValDefault);
    }
    else {
        out_u8 = ret.get<std::string>();
        out = utf8_to_utf16(out_u8);
    }
    
    // Fix: same non-termination bug as stdJSON_GetString (see there). _wcsncpy does
    // not NUL-terminate when wcslen(src) >= n. Copy min(wcslen, outSize-1) and always
    // write the wide terminator so callers don't read trailing garbage.
    if (outSize > 0) {
        size_t readSize = _wcslen(out.data());
        if (readSize > (size_t)(outSize - 1)) {
            readSize = (size_t)(outSize - 1);
        }
        _wcsncpy(pOut, out.data(), readSize);
        pOut[readSize] = 0;
    }

    return 1;
}

int stdJSON_IterateKeys(const char* pFpath, stdJSONCallback_t pCallbackFn, void* pCtx)
{
    CHECK_ARGPTR(pFpath);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);

    for (auto& el : json_file.items())
    {
        std::string out = json_file.value(std::string(el.key().c_str()), std::string(""));
        if (out == std::string("")) {
            stdJSON_SetString(pFpath, el.key().c_str(), "");
        }

        if (pCallbackFn) {
            pCallbackFn(el.key().c_str(), out.c_str(), pCtx);
        }
    }

    //[pKey] = std::string(pVal);
    //return stdJSON_WriteToFile(pFpath, json_file);
    return 1;
}

// Added: a list of strings (a JSON array, or one string with the items separated
// by commas or spaces) into maxRows rows of rowLen chars. Returns how many. A
// missing key is written as an empty array, so it shows up in the file.
int stdJSON_GetStringList(const char* pFpath, const char* pKey, char* pOut, int maxRows, int rowLen)
{
    CHECK_COMMON(pFpath, pKey);
    CHECK_ARGPTR(pOut);

    std::vector<std::string> items;
    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    if (!json_file.contains(pKey))
    {
        json_file[pKey] = nlohmann::json::array();
        stdJSON_WriteToFile(pFpath, json_file);
        return 0;
    }
    const nlohmann::json& v = json_file[pKey];
    if (v.is_array())
    {
        for (const auto& el : v)
            if (el.is_string()) items.push_back(el.get<std::string>());
    }
    else if (v.is_string())
    {
        std::string cur;
        for (char c : v.get<std::string>() + ",")
        {
            if (c == ',' || c == ' ' || c == '\t') { if (!cur.empty()) items.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    int n = 0;
    for (const auto& item : items)
    {
        if (n >= maxRows || item.empty()) continue;
        strncpy(pOut + (size_t)n * rowLen, item.c_str(), rowLen - 1);
        pOut[(size_t)n * rowLen + rowLen - 1] = 0;
        n++;
    }
    return n;
}

// Added: writes a list of strings (given as one, items separated by commas) as a JSON array.
int stdJSON_SetStringList(const char* pFpath, const char* pKey, const char* pCommaList)
{
    CHECK_COMMON(pFpath, pKey);
    CHECK_ARGPTR(pCommaList);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    nlohmann::json arr = nlohmann::json::array();
    std::string cur;
    for (const char* p = pCommaList; ; p++)
    {
        if (*p == ',' || !*p) { if (!cur.empty()) arr.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    json_file[pKey] = arr;
    return stdJSON_WriteToFile(pFpath, json_file);
}

// Added: whether pKey is in the file (any type).
int stdJSON_HasKey(const char* pFpath, const char* pKey)
{
    CHECK_COMMON(pFpath, pKey);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    return json_file.contains(pKey) ? 1 : 0;
}

// Added: moves pKey and its value (any type) from one file to another. 0 if it
// wasn't in pFrom.
int stdJSON_MoveKey(const char* pFrom, const char* pTo, const char* pKey)
{
    CHECK_COMMON(pFrom, pKey);
    CHECK_ARGPTR(pTo);

    nlohmann::json from = stdJSON_OpenAndReadFile(pFrom);
    if (!from.contains(pKey))
        return 0;
    nlohmann::json to = stdJSON_OpenAndReadFile(pTo);
    to[pKey] = from[pKey];
    if (!stdJSON_WriteToFile(pTo, to))
        return 0;
    from.erase(pKey);
    return stdJSON_WriteToFile(pFrom, from);
}

int stdJSON_EraseKey(const char* pFpath, const char* pKey)
{
    CHECK_COMMON(pFpath, pKey);

    nlohmann::json json_file = stdJSON_OpenAndReadFile(pFpath);
    json_file.erase(pKey);
    return stdJSON_WriteToFile(pFpath, json_file);
}

int stdJSON_EraseAll(const char* pFpath)
{
    CHECK_ARGPTR(pFpath);
    
    nlohmann::json json_file(nlohmann::json::value_t::object);
    return stdJSON_WriteToFile(pFpath, json_file);
}
}