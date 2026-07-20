#pragma once
#include <string>

bool UnzipAll(const std::wstring& zipPath, const std::wstring& destDir);

int ExtractNativeLibs(const std::wstring& jarPath, const std::wstring& destDir);

bool ZipFolder(const std::wstring& srcDir, const std::wstring& zipPath, const std::string& innerPrefix = "");

std::string ZipReadEntry(const std::wstring& zipPath, const std::string& entryName);

bool ZipIsValid(const std::wstring& zipPath);

int ZipExtractByPrefix(const std::wstring& zipPath, const std::string& prefix, const std::wstring& destDir);
