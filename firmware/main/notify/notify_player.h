#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Notify Player — handles async cloud-pushed voice notifications.
 *
 * When the cloud pushes an unsolicited notification to an idle device:
 *   1. Server sends {"type":"notify","state":"start","text":"..."}
 *      -> device enters NOTIFYING and shows the text
 *   2. Server streams the spoken text as Opus binary frames on the existing
 *      TTS audio path (stream_service decodes + plays them)
 *   3. Server sends {"type":"notify","state":"end"}
 *      -> device returns to IDLE_LISTENING
 *
 * A wake word, button press, or server disconnect cancels the notification.
 */

/// Initialize the notification player
void notify_player_init(void);

/// Begin a notification: shows `text` and enters the NOTIFYING state.
/// Ignored unless the device is idle. Audio frames that follow are played
/// by the shared TTS pipeline.
void notify_player_start(const char *text);

/// End the current notification and return to idle.
void notify_player_end(void);

/// Cancel current notification playback (e.g. on wake word barge-in)
void notify_player_cancel(void);

/// Check if a notification is currently active
bool notify_player_is_playing(void);
