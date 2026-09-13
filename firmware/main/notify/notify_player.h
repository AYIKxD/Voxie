#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Notify Player — handles async cloud-pushed voice notifications.
 *
 * When the cloud sends a "notify" message to an idle device:
 * 1. Device transitions to NOTIFYING state
 * 2. Plays notification audio (Opus via HTTP URL or inline)
 * 3. Shows notification text on display
 * 4. Returns to IDLE_LISTENING when done
 * 5. Wake word or button press cancels notification
 */

/// Initialize the notification player
void notify_player_init(void);

/// Play a notification. text is shown on display, audio_url is fetched and played.
/// Non-blocking — spawns a task to handle download and playback.
void notify_player_play(const char *text, const char *audio_url);

/// Cancel current notification playback (e.g. on wake word barge-in)
void notify_player_cancel(void);

/// Check if a notification is currently playing
bool notify_player_is_playing(void);
