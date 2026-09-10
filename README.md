# Libra for the HiBy R1

An alternative firmware for the Hiby R1.

The aim is to create a completely open soure firmware for the R1 from the kernel upwards that is faster and includes QoL features.

Please note: Libra is entirely written by LLM with extensive human testing. I don't know C, but I do have strong opinions on QA and how the firmware should work.

## Features

🎵**Music features** you would expect - WAV, FLAC, MP3, OGG, M4A, M4B, shuffle, repeat, queues, playlists.

🎙️**Podcast player** - The original reason I got into this rabbit hole 😅 download and play podcasts on the device with syncing, browsing subscribed podcasts, custom playback speed. More features coming.

📖**Audiobook player** - Mostly feature complete. Chapter selection, playback speed, doubled-up chapter and book seekbars.

📻**Radio** - Internet Radio - everything works apart from HLS streams.




💿 **Automatic cover art download** - From Spotify and LastFM. Artist pictures and Bios as well.

🔈 **Parametric EQ** - With support for 'EqualizerAPO ParametricEq' profiles from https://autoeq.app! Just pop them on the SD card under 'EQ Profiles'. Includes automatic profile switching for bluetooth headphones.

🔈 **MSEB** - a workalike replacement for the built-in Hiby MSEB system.

🛑 **Hold mode** - Double tap the power button to lock the screen and hardware buttons

🌓 **Automatic theme selection** - There are three themes - light, dark and grey. 'Auto' switches between the three depending on your latitude, date and the timezone.

🏃 **Resume from shutdown** - Starting from an auto shutdown puts you back where you were (music, book, podcast etc.), connects to bluetooth and optionally restarts playback

💽 **Miniplayer** - While you are going through menus, a miniplayer will appear at the bottom of the screen (similar to lots of android apps)

🏄 **Waveform seekbar** - After the first playthrough, subsequent playthroughs of music files will replace the standard seekbar with a waveform seekbar.

🚚 **USB Transport Mode** - Using USB-out for audio automatically disables bluetooth, PEQ, MSEB and locks volume.

🐧 **Custom Kernel** - Currently this means booting is slightly faster - more work being done here.

## Requirements

- A HiBy R1 with SD Card
- Patience and willingness to write some bug reports :-)

## Install

1 - Download the r1.upt file from Releases
2 - Copy the file to the SD Card
3 - Shutdown your R1
4 - Boot and immediately hold down **Power** and **Vol+**
5 - Device will restart into Libra!

## Using it

**After installation**; add your WiFi credentials, Radio stations, Spotify and LastFM credentials and podcast RSS feeds to **/your SD Card/settings.txt** (example provided)

**Important note:** Yes, this is putting credentials in plain text on an unencrypted device. No, this is not proper security. If your Spotify and LastFM credentials are otherwise used for something important, don't do this. If your WiFi credentials shouldn't be in plain text then you shouldn't put them in stock either (where they are also in plain text).

Everything that is configurable is in **Settings**

**Scan Library** in Settings will build a database of your files.

**Gestures** are not immediately obvious, but you can swipe from the left edge to go back, and up from the Hiby logo to go to home.

## Building

TODO

## Known limitations

- Podcasts plays **MP3 only** — no AAC, M4A/M4B, Opus or FLAC episodes.
- English UI only; German and Swedish coming

## Bugs

- Very much so :-) Yes. Lots. This is a WIP with lots of testing ongoing. PRs and Bug reports are very, very welcome.


## Licence

MIT — see [LICENSE](LICENSE). Credits and vendored code in
[THIRD_PARTY.md](THIRD_PARTY.md). No HiBy resources are redistributed here.
