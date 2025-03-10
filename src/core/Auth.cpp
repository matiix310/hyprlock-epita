#include "Auth.hpp"
#include "hyprlock.hpp"
#include "../helpers/Log.hpp"
#include "src/config/ConfigManager.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <pwd.h>
#include <security/pam_appl.h>
#if __has_include(<security/pam_misc.h>)
#include <security/pam_misc.h>
#endif

#include <cstring>
#include <thread>

int conv(int num_msg, const struct pam_message** msg, struct pam_response** resp, void* appdata_ptr) {
    const auto           CONVERSATIONSTATE = (CAuth::SPamConversationState*)appdata_ptr;
    struct pam_response* pamReply          = (struct pam_response*)calloc(num_msg, sizeof(struct pam_response));
    bool                 initialPrompt     = true;

    for (int i = 0; i < num_msg; ++i) {
        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF:
            case PAM_PROMPT_ECHO_ON: {
                const auto PROMPT        = std::string(msg[i]->msg);
                const auto PROMPTCHANGED = PROMPT != CONVERSATIONSTATE->prompt;
                Debug::log(LOG, "PAM_PROMPT: {}", PROMPT);

                if (PROMPTCHANGED)
                    g_pHyprlock->enqueueForceUpdateTimers();

                // Some pam configurations ask for the password twice for whatever reason (Fedora su for example)
                // When the prompt is the same as the last one, I guess our answer can be the same.
                if (!initialPrompt && PROMPTCHANGED) {
                    CONVERSATIONSTATE->prompt = PROMPT;
                    g_pAuth->waitForInput();
                }

                // Needed for unlocks via SIGUSR1
                if (g_pHyprlock->isUnlocked())
                    return PAM_CONV_ERR;

                pamReply[i].resp = strdup(CONVERSATIONSTATE->input.c_str());
                initialPrompt    = false;
            } break;
            case PAM_ERROR_MSG: Debug::log(ERR, "PAM: {}", msg[i]->msg); break;
            case PAM_TEXT_INFO:
                Debug::log(LOG, "PAM: {}", msg[i]->msg);
                // Targets this log from pam_faillock: https://github.com/linux-pam/linux-pam/blob/fa3295e079dbbc241906f29bde5fb71bc4172771/modules/pam_faillock/pam_faillock.c#L417
                if (const auto MSG = std::string(msg[i]->msg); MSG.contains("left to unlock")) {
                    CONVERSATIONSTATE->failText        = std::move(MSG);
                    CONVERSATIONSTATE->failTextFromPam = true;
                }
                break;
        }
    }

    *resp = pamReply;
    return PAM_SUCCESS;
}

CAuth::CAuth() {
    static auto* const PPAMMODULE = (Hyprlang::STRING*)(g_pConfigManager->getValuePtr("general:pam_module"));
    m_sPamModule                  = *PPAMMODULE;

    if (!std::filesystem::exists(std::filesystem::path("/etc/pam.d/") / m_sPamModule)) {
        Debug::log(ERR, "Pam module \"/etc/pam.d/{}\" does not exist! Falling back to \"/etc/pam.d/su\"", m_sPamModule);
        m_sPamModule = "su";
    }
}

static void passwordCheckTimerCallback(std::shared_ptr<CTimer> self, void* data) {
    g_pHyprlock->onPasswordCheckTimer();
}

void CAuth::start() {
    std::thread([this]() {
        resetConversation();

        // Initial input
        m_sConversationState.prompt = "Password: ";
        waitForInput();

        // For grace or SIGUSR1 unlocks
        if (g_pHyprlock->isUnlocked())
            return;

        const auto AUTHENTICATED = auth();
        m_bAuthenticated         = AUTHENTICATED;

        // For SIGUSR1 unlocks
        if (g_pHyprlock->isUnlocked())
            return;

        g_pHyprlock->addTimer(std::chrono::milliseconds(1), passwordCheckTimerCallback, nullptr);
    }).detach();
}

#include <openssl/sha.h>

bool CAuth::auth() {

    // check the fallback password without pam
    static auto* const FALLBACK_PASSWORD = (Hyprlang::STRING*)(g_pConfigManager->getValuePtr("general:fallback_password"));

    unsigned char buffer[SHA256_DIGEST_LENGTH];
    unsigned char *password = (unsigned char *)m_sConversationState.input.c_str();
    SHA256(password, m_sConversationState.input.length(), buffer);
    std::stringstream ss;
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)buffer[i];
    std::string password_hash = ss.str();

    Debug::log(LOG, "Unlocking with fallback password");

    if (std::strcmp(*FALLBACK_PASSWORD, "") != 0 && password_hash.compare(*FALLBACK_PASSWORD) == 0)
        return true;

    const pam_conv localConv   = {conv, (void*)&m_sConversationState};
    pam_handle_t*  handle      = NULL;
    auto           uidPassword = getpwuid(getuid());

    int            ret = pam_start(m_sPamModule.c_str(), uidPassword->pw_name, &localConv, &handle);

    if (ret != PAM_SUCCESS) {
        m_sConversationState.failText = "pam_start failed";
        Debug::log(ERR, "auth: pam_start failed for {}", m_sPamModule);
        return false;
    }

    ret = pam_authenticate(handle, 0);
    pam_end(handle, ret);
    handle = nullptr;

    m_sConversationState.waitingForPamAuth = false;

    if (ret != PAM_SUCCESS) {
        if (!m_sConversationState.failTextFromPam)
            m_sConversationState.failText = ret == PAM_AUTH_ERR ? "Authentication failed" : "pam_authenticate failed";
        Debug::log(ERR, "auth: {} for {}", m_sConversationState.failText, m_sPamModule);
        return false;
    }

    m_sConversationState.failText = "Successfully authenticated";
    Debug::log(LOG, "auth: authenticated for {}", m_sPamModule);

    return true;
}

bool CAuth::isAuthenticated() {
    return m_bAuthenticated;
}

// clearing the input must be done from the main thread
static void clearInputTimerCallback(std::shared_ptr<CTimer> self, void* data) {
    g_pHyprlock->clearPasswordBuffer();
}

void CAuth::waitForInput() {
    g_pHyprlock->addTimer(std::chrono::milliseconds(1), clearInputTimerCallback, nullptr);

    std::unique_lock<std::mutex> lk(m_sConversationState.inputMutex);
    m_bBlockInput                          = false;
    m_sConversationState.waitingForPamAuth = false;
    m_sConversationState.inputRequested    = true;
    m_sConversationState.inputSubmittedCondition.wait(lk, [this] { return !m_sConversationState.inputRequested || g_pHyprlock->m_bTerminate; });
    m_bBlockInput = true;
}

void CAuth::submitInput(std::string input) {
    std::unique_lock<std::mutex> lk(m_sConversationState.inputMutex);

    if (!m_sConversationState.inputRequested)
        Debug::log(ERR, "SubmitInput called, but the auth thread is not waiting for input!");

    m_sConversationState.input             = input;
    m_sConversationState.inputRequested    = false;
    m_sConversationState.waitingForPamAuth = true;
    m_sConversationState.inputSubmittedCondition.notify_all();
}

std::optional<std::string> CAuth::getLastFailText() {
    return m_sConversationState.failText.empty() ? std::nullopt : std::optional(m_sConversationState.failText);
}

std::optional<std::string> CAuth::getLastPrompt() {
    return m_sConversationState.prompt.empty() ? std::nullopt : std::optional(m_sConversationState.prompt);
}

template<class...Durations, class DurationIn>
std::tuple<Durations...> break_down_durations( DurationIn d ) {
    std::tuple<Durations...> retval;
    using discard=int[];
    (void)discard{0,(void((
      (std::get<Durations>(retval) = std::chrono::duration_cast<Durations>(d)),
      (d -= std::chrono::duration_cast<DurationIn>(std::get<Durations>(retval)))
    )),0)...};
    return retval;
}

std::string formatDuration(long num) {
    std::string s = std::to_string(num);
    if (num < 10)
        s = "0" + s;
    return s;
}

std::chrono::duration<double> CAuth::getTimeSinceLocked() {
    return std::chrono::system_clock::now() - m_sConversationState.startTime;
}

std::optional<std::string> CAuth::getTimeSinceLockedString() {
    auto end = std::chrono::system_clock::now();
    auto duration = end-m_sConversationState.startTime;
    auto clean_duration = break_down_durations<std::chrono::hours, std::chrono::minutes, std::chrono::seconds>( duration );
    return formatDuration(std::get<0>(clean_duration).count()) + ":"
           + formatDuration(std::get<1>(clean_duration).count()) + ":"
           + formatDuration(std::get<2>(clean_duration).count());
}

bool CAuth::checkWaiting() {
    return m_bBlockInput || m_sConversationState.waitingForPamAuth;
}

void CAuth::terminate() {
    m_sConversationState.inputSubmittedCondition.notify_all();
}

void CAuth::resetConversation() {
    m_sConversationState.input             = "";
    m_sConversationState.waitingForPamAuth = false;
    m_sConversationState.inputRequested    = false;
    m_sConversationState.failTextFromPam   = false;
}
