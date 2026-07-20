#pragma once
#include <string>
#include <cstdint>

namespace util {

std::wstring HomeDir();
std::wstring GameDir();
std::wstring ExeDir();
std::wstring InstanceDir(const std::string& versionId);
std::wstring ModsDir(const std::string& versionId);

std::wstring Utf8ToWide(const std::string&);
std::string  WideToUtf8(const std::wstring&);

std::string  ReadFile(const std::wstring& path);
bool         WriteFile(const std::wstring& path, const std::string& data);
bool         Exists(const std::wstring& path);
bool         IsDir(const std::wstring& path);
void         EnsureDir(const std::wstring& path);

std::string  Base64Encode(const std::string& bytes);
std::string  Base64Decode(const std::string& b64);

uint64_t     LastWriteMs(const std::wstring& path);
std::string  CreationIso8601(const std::wstring& path);
uint64_t     FileSize(const std::wstring& path);
uint64_t     DirSize(const std::wstring& path);

std::string  UrlEncode(const std::string&);

std::string  Sha1Hex(const std::string& bytes);
std::string  Sha1HexOfFile(const std::wstring& path);
std::string  Md5Raw(const std::string& data);

}
