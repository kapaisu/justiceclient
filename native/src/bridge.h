#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>

class Bridge {
public:
    using Json        = nlohmann::json;
    using Handler     = std::function<Json(const Json& args)>;
    using ReplyFn     = std::function<void(const Json& result, const std::string& error)>;
    using AsyncHandler = std::function<void(const Json& args, ReplyFn reply)>;

    void SetPostCallback(std::function<void(const std::wstring&)> post) { post_ = std::move(post); }

    void SetUiDispatcher(std::function<void(std::function<void()>)> d) { uiDispatch_ = std::move(d); }

    void Register(const std::string& channel, Handler h) { handlers_[channel] = std::move(h); }
    void RegisterAsync(const std::string& channel, AsyncHandler h) { asyncHandlers_[channel] = std::move(h); }
    void RegisterDefault(Handler h) { defaultHandler_ = std::move(h); }

    void HandleMessage(const wchar_t* json);
    void Emit(const std::string& channel, const Json& args);
    void EmitAsync(const std::string& channel, const Json& args);

    static const std::string& ShimJs();
    static std::string  ToUtf8(const std::wstring&);
    static std::wstring ToUtf16(const std::string&);

private:
    void PostJson(const Json& j);

    std::unordered_map<std::string, Handler>      handlers_;
    std::unordered_map<std::string, AsyncHandler> asyncHandlers_;
    Handler                                       defaultHandler_;
    std::function<void(const std::wstring&)>      post_;
    std::function<void(std::function<void()>)>    uiDispatch_;
};

void RegisterCoreHandlers(Bridge& b);
void RegisterAuthHandlers(Bridge& b);
void RegisterGameHandlers(Bridge& b);
void RegisterModHandlers(Bridge& b);
void RegisterExtraHandlers(Bridge& b);
void RegisterDiscordHandlers(Bridge& b);
void RegisterOverlayHandlers(Bridge& b);
void RegisterUpdaterHandlers(Bridge& b, void* mainHwnd);

nlohmann::json AuthGetValidAuth();

int GameSelfTest(const std::wstring& mode, const std::wstring& arg1);
