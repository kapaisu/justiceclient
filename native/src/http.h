#pragma once
#include <string>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>

struct HttpResponse {
    long status = 0;
    std::string body;
    bool ok = false;
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

HttpResponse HttpSend(const std::string& method,
                      const std::string& url,
                      const HttpHeaders& headers = {},
                      const std::string& body = {});

nlohmann::json HttpJson(const std::string& method,
                        const std::string& url,
                        const HttpHeaders& headers = {},
                        const std::string& body = {});
