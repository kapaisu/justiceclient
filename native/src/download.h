#pragma once
#include <string>
#include <vector>
#include <functional>

bool DownloadFile(const std::string& url, const std::wstring& dest,
                  std::function<void(double)> onProgress = {},
                  const std::string& expectedSha1 = {});

bool DownloadFileBytes(const std::string& url, const std::wstring& dest,
                       std::function<void(unsigned long long transferred, unsigned long long total)> onBytes);

struct DlTask { std::string url; std::wstring dest; };

int DownloadBatch(const std::vector<DlTask>& tasks, int concurrency,
                  std::function<void(int done, int total)> onProgress = {});
