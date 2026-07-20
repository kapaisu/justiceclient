#include "bridge.h"
#include <windows.h>
#include <atomic>
#include <memory>

std::string Bridge::ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Bridge::ToUtf16(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

void Bridge::HandleMessage(const wchar_t* jsonW) {
    Json j;
    try {
        j = Json::parse(ToUtf8(jsonW));
    } catch (...) {
        return;
    }

    const std::string type    = j.value("__type", std::string());
    const std::string channel = j.value("channel", std::string());
    const Json args = j.contains("args") && j["args"].is_array() ? j["args"] : Json::array();

    if (type == "invoke") {
        const long long id = j.value("id", 0LL);

        auto ait = asyncHandlers_.find(channel);
        if (ait != asyncHandlers_.end()) {
            auto once = std::make_shared<std::atomic<bool>>(false);
            ReplyFn reply = [this, id, once](const Json& result, const std::string& error) {
                if (once->exchange(true)) return;
                auto deliver = [this, id, result, error]() {
                    Json rep;
                    rep["__type"] = "reply";
                    rep["id"] = id;
                    if (!error.empty()) rep["error"] = error; else rep["result"] = result;
                    PostJson(rep);
                };
                if (uiDispatch_) uiDispatch_(deliver); else deliver();
            };
            try {
                ait->second(args, reply);
            } catch (const std::exception& e) {
                reply(nullptr, e.what());
            } catch (...) {
                reply(nullptr, "unknown native error");
            }
            return;
        }

        auto it = handlers_.find(channel);
        Handler h = (it != handlers_.end()) ? it->second : defaultHandler_;
        Json reply;
        reply["__type"] = "reply";
        reply["id"] = id;
        try {
            reply["result"] = h ? h(args) : Json(nullptr);
        } catch (const std::exception& e) {
            reply["error"] = e.what();
        } catch (...) {
            reply["error"] = "unknown native error";
        }
        PostJson(reply);
    } else {
        auto it = handlers_.find(channel);
        Handler h = (it != handlers_.end()) ? it->second : defaultHandler_;
        try {
            if (h) h(args);
        } catch (...) {
        }
    }
}

void Bridge::Emit(const std::string& channel, const Json& args) {
    Json m;
    m["__type"]  = "event";
    m["channel"] = channel;
    m["args"]    = args.is_array() ? args : Json::array({ args });
    PostJson(m);
}

void Bridge::EmitAsync(const std::string& channel, const Json& args) {
    if (uiDispatch_) {
        uiDispatch_([this, channel, args]() { Emit(channel, args); });
    } else {
        Emit(channel, args);
    }
}

void Bridge::PostJson(const Json& j) {
    if (post_) post_(ToUtf16(j.dump()));
}

const std::string& Bridge::ShimJs() {
    static const std::string js = R"JS(
(function () {
  if (window.__justiceBridge) return;
  window.__justiceBridge = true;

  var pending   = new Map();
  var listeners = new Map();
  var seq = 0;

  function post(msg) { window.chrome.webview.postMessage(msg); }

  window.chrome.webview.addEventListener('message', function (e) {
    var m = e.data;
    if (!m || typeof m !== 'object') return;
    if (m.__type === 'reply') {
      var p = pending.get(m.id);
      if (!p) return;
      pending.delete(m.id);
      if (Object.prototype.hasOwnProperty.call(m, 'error')) p.reject(new Error(m.error));
      else p.resolve(m.result);
    } else if (m.__type === 'event') {
      var fns = listeners.get(m.channel);
      if (!fns) return;
      var a = m.args || [];
      fns.slice().forEach(function (fn) {
        try { fn.apply(null, [{}].concat(a)); } catch (err) { console.error(err); }
      });
    }
  });

  var ipcRenderer = {
    invoke: function (channel) {
      var args = Array.prototype.slice.call(arguments, 1);
      var id = ++seq;
      return new Promise(function (resolve, reject) {
        pending.set(id, { resolve: resolve, reject: reject });
        post({ __type: 'invoke', id: id, channel: channel, args: args });
      });
    },
    send: function (channel) {
      var args = Array.prototype.slice.call(arguments, 1);
      post({ __type: 'send', channel: channel, args: args });
    },
    on: function (channel, fn) {
      if (!listeners.has(channel)) listeners.set(channel, []);
      listeners.get(channel).push(fn);
      return ipcRenderer;
    },
    once: function (channel, fn) {
      var wrap = function () { ipcRenderer.removeListener(channel, wrap); fn.apply(null, arguments); };
      return ipcRenderer.on(channel, wrap);
    },
    removeListener: function (channel, fn) {
      var a = listeners.get(channel);
      if (a) { var i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); }
      return ipcRenderer;
    },
    removeAllListeners: function (channel) {
      if (channel) listeners.delete(channel); else listeners.clear();
      return ipcRenderer;
    }
  };

  var electron = { ipcRenderer: ipcRenderer };
  var realRequire = window.require;
  window.require = function (mod) {
    if (mod === 'electron') return electron;
    if (typeof realRequire === 'function') return realRequire(mod);
    throw new Error('require("' + mod + '") is not available in the WebView2 host');
  };

  if (typeof window.process === 'undefined') window.process = {};
  if (typeof window.process.platform === 'undefined') window.process.platform = 'win32';
  window.process.versions = window.process.versions || {};

  if (typeof window.Buffer === 'undefined') {
    window.Buffer = {
      from: function (data, enc) {
        if (enc === 'base64') {
          var bin = atob(data), arr = new Uint8Array(bin.length);
          for (var i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i);
          return arr;
        }
        return new TextEncoder().encode(String(data));
      }
    };
  }

  function regionOf(el) {
    try {
      var v = getComputedStyle(el).getPropertyValue('-webkit-app-region');
      return v ? v.trim() : '';
    } catch (e) { return ''; }
  }
  function shouldDrag(target) {
    for (var n = target; n && n.nodeType === 1; n = n.parentElement) {
      var r = regionOf(n);
      if (r === 'no-drag') return false;
      if (r === 'drag') return true;
    }
    if (!target.closest) return false;
    var host = target.closest('#titlebar, #topnav');
    if (!host) return false;
    var noDrag = target.closest('button, a, input, select, textarea, [role="button"], .win-ctrl, .tb-admin-tag, .tl-version');
    if (noDrag && host.contains(noDrag)) return false;
    return true;
  }
  document.addEventListener('mousedown', function (e) {
    if (e.button !== 0) return;
    if (shouldDrag(e.target)) post({ __type: 'send', channel: '__drag-window', args: [] });
  }, true);
  document.addEventListener('dblclick', function (e) {
    if (e.button !== 0) return;
    if (shouldDrag(e.target)) post({ __type: 'send', channel: 'window-maximize', args: [] });
  }, true);
})();
)JS";
    return js;
}
