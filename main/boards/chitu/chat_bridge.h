#pragma once
// Makes Chitu say something through the AI (the voice comes from the server's text to
// speech, so this needs Wi-Fi). Requests are queued and delivered when Chitu is free:
// a running answer is stopped first, a half-open conversation is closed first, so a
// request is never silently dropped (which happened before when Chitu was listening).
#include <string>

namespace chat_bridge {

enum class Kind {
    kReadAloud,   // read the text out word for word, then react briefly
    kPrompt,      // an instruction for the AI ("say something playful")
};

// interrupt = stop what Chitu is saying right now (otherwise wait until it is done)
// Returns false if there is no internet connection (nothing will be said).
bool Say(Kind kind, const std::string& text, bool interrupt);

// Stops speech / music / sounds right now (does not queue anything)
void StopTalking();

// True while a request is waiting to be delivered
bool Pending();

}  // namespace chat_bridge
