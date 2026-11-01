#pragma once

// Plays the alarm beep in its own task; listening for "Hi ESP" pauses while it sounds.

// path: unused (always the built-in beep). volume 0–100. fade_s: seconds from quiet to full volume.
void audio_start(void);  // at boot, while internal RAM is free: makes the sound task
void audio_play(const char* path, int volume, bool loop, int fade_s);
void audio_stop(void);
bool audio_playing(void);
