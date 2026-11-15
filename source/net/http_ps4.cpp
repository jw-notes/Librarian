// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Console HTTP backend: the system's libSceHttp with its TLS (libSceSsl).
// Redirects are turned off here and followed by http.cpp, which needs the
// final address of every page.
#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <vector>

#include "core/log.h"
#include "core/text.h"
#include "net/http.h"
#include "net/url.h"

extern "C" int32_t sceSslInit(size_t poolSize);
extern "C" int32_t sceSslTerm(int32_t id);
// The toolchain declares these without their parameters.
extern "C" int32_t lib_http_set_auto_redirect(int32_t id, int32_t enable) __asm__("sceHttpSetAutoRedirect");
extern "C" int32_t lib_http_set_recv_timeout(int32_t id, uint32_t usec) __asm__("sceHttpSetRecvTimeOut");

namespace lib {

namespace {

int g_pool = -1, g_ssl = -1, g_ctx = -1, g_template = -1;
std::atomic<bool> g_insecure{false};
// Server-certificate checks (SCE_HTTPS_FLAG_SERVER_VERIFY | KNOWN_CA_CHECK | CN_CHECK | dates).
constexpr uint32_t kVerifyFlags = 0x01 | 0x04 | 0x08 | 0x10 | 0x20;

std::string error_text(const char* stage, int code, int request) {
    int os_error = 0;
    char text[160];
    if (request >= 0 && sceHttpGetLastErrno(request, &os_error) >= 0 && os_error)
        snprintf(text, sizeof text, "%s failed (error 0x%08x, network error %d).", stage, (unsigned)code, os_error);
    else
        snprintf(text, sizeof text, "%s failed (error 0x%08x).", stage, (unsigned)code);
    return text;
}

// Pulls one header's value out of the raw header block.
std::string header_value(const char* headers, size_t size, const char* name) {
    const std::string block(headers, size);
    const std::string key = lower(name);
    size_t at = 0;
    while (at < block.size()) {
        size_t end = block.find('\n', at);
        if (end == std::string::npos) end = block.size();
        const std::string line = block.substr(at, end - at);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && lower(trim(line.substr(0, colon))) == key) return trim(line.substr(colon + 1));
        at = end + 1;
    }
    return std::string();
}

// Every header matching `name` (Set-Cookie can appear several times).
std::vector<std::string> header_values(const char* headers, size_t size, const char* name) {
    std::vector<std::string> out;
    const std::string block(headers, size);
    const std::string key = lower(name);
    size_t at = 0;
    while (at < block.size()) {
        size_t end = block.find('\n', at);
        if (end == std::string::npos) end = block.size();
        const std::string line = block.substr(at, end - at);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && lower(trim(line.substr(0, colon))) == key) out.push_back(trim(line.substr(colon + 1)));
        at = end + 1;
    }
    return out;
}

}  // namespace

bool http_init() {
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    sceNetInit();
    sceNetCtlInit();
    OrbisNetCtlInfo info{};
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) >= 0) LOG("network: console IP %s", info.ip_address);
    g_pool = sceNetPoolCreate("Librarian", 4 * 1024 * 1024, 0);
    if (g_pool < 0) { LOG("net pool 0x%08x", g_pool); return false; }
    g_ssl = sceSslInit(4 * 1024 * 1024);
    if (g_ssl < 0) { LOG("ssl init 0x%08x", g_ssl); return false; }
    g_ctx = sceHttpInit(g_pool, g_ssl, 8 * 1024 * 1024);
    if (g_ctx < 0) { LOG("http init 0x%08x", g_ctx); return false; }
    // Direct connections: an explicit server address should not inherit the
    // console browser's proxy settings.
    g_template = sceHttpCreateTemplate(g_ctx, "Librarian-PS4/1.0", 2, 0);
    if (g_template < 0) { LOG("http template 0x%08x", g_template); return false; }
    sceHttpSetConnectTimeOut(g_template, 10 * 1000000);
    sceHttpSetResolveTimeOut(g_template, 10 * 1000000);
    sceHttpSetSendTimeOut(g_template, 30 * 1000000);
    lib_http_set_auto_redirect(g_template, 0);
    return true;
}

void http_shutdown() {
    if (g_template >= 0) sceHttpDeleteTemplate(g_template);
    if (g_ctx >= 0) sceHttpTerm(g_ctx);
    if (g_ssl >= 0) sceSslTerm(g_ssl);
    if (g_pool >= 0) sceNetPoolDestroy(g_pool);
    g_template = g_ctx = g_ssl = g_pool = -1;
}

void http_set_allow_insecure(bool allow) { g_insecure = allow; }

HttpRawResult http_perform(const std::string& method, const std::string& full_url, const std::string& body,
                           const std::vector<std::pair<std::string, std::string>>& headers, int timeout_seconds,
                           const BodySink& sink, const HeadHook& head) {
    HttpRawResult result;
    // The system library sends a "#fragment" to the server as part of the path
    // (Kiwix then answers 404): it is for the page, not the server.
    const std::string url = strip_fragment(full_url);
    if (g_template < 0) {
        result.error = "The network is not available.";
        return result;
    }
    // No keep-alive (sign-in POSTs are known to work that way on the
    // console): each connection is deleted after its one request anyway.
    const int connection = sceHttpCreateConnectionWithURL(g_template, url.c_str(), false);
    if (connection < 0) {
        result.error = error_text("Connecting", connection, -1);
        return result;
    }
    const int request = sceHttpCreateRequestWithURL2(connection, method.c_str(), url.c_str(), body.size());
    if (request < 0) {
        result.error = error_text("Creating the request", request, -1);
        sceHttpDeleteConnection(connection);
        return result;
    }
    lib_http_set_auto_redirect(request, 0);
    lib_http_set_recv_timeout(request, (uint32_t)timeout_seconds * 1000000u);
    if (g_insecure) sceHttpsDisableOption(request, kVerifyFlags);
    for (const auto& [k, v] : headers) sceHttpAddRequestHeader(request, k.c_str(), v.c_str(), 0);

    int rc = sceHttpSendRequest(request, body.empty() ? nullptr : body.data(), body.size());
    if (rc < 0) {
        // Most often a wrong address or a server that is not running: name the
        // address, so a typo is easy to see.
        Url u;
        const std::string where = parse_url(url, u) ? u.origin() : url;
        char code[16];
        snprintf(code, sizeof code, "0x%08x", (unsigned)rc);
        result.error = "No answer from " + where + ". Check the address in Settings and that the server is running (" +
                       code + ").";
        if (starts_with(url, "https"))
            result.error += " If the server uses a self-signed certificate, allow it in Settings.";
        LOG("%s", error_text("Sending the request", rc, request).c_str());
    } else if ((rc = sceHttpGetStatusCode(request, &result.status)) < 0) {
        result.error = error_text("Reading the status", rc, request);
        result.status = -1;
    } else {
        char* raw_headers = nullptr;
        size_t header_size = 0;
        if (sceHttpGetAllResponseHeaders(request, &raw_headers, &header_size) >= 0 && raw_headers) {
            result.location = header_value(raw_headers, header_size, "Location");
            result.content_type = header_value(raw_headers, header_size, "Content-Type");
            result.set_cookies = header_values(raw_headers, header_size, "Set-Cookie");
        }
        int32_t known = 0;
        size_t length = 0;
        if (sceHttpGetResponseContentLength(request, &known, &length) >= 0 && known == 0) result.content_length = length;

        const bool redirect = result.status >= 300 && result.status < 400 && !result.location.empty();
        if (!redirect && method != "HEAD" && (!head || head(result))) {
            std::vector<char> buffer(128 * 1024);
            int n;
            while ((n = sceHttpReadData(request, buffer.data(), (uint32_t)buffer.size())) > 0) {
                if (!sink(buffer.data(), (size_t)n)) break;
            }
            if (n < 0) {
                result.error = error_text("Reading the response", n, request);
                result.status = -1;
            }
        }
    }
    sceHttpDeleteRequest(request);
    sceHttpDeleteConnection(connection);
    return result;
}

}  // namespace lib
