/*
========================================================================

                           D O O M  R e t r o
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright © 1993-2012 id Software LLC, a ZeniMax Media company.
  Copyright © 2013-2017 Brad Harding.

  DOOM Retro is a fork of Chocolate DOOM.
  For a list of credits, see <http://wiki.doomretro.com/credits>.

  This file is part of DOOM Retro.

  DOOM Retro is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the License, or (at your
  option) any later version.

  DOOM Retro is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM Retro. If not, see <https://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM Retro is in no way affiliated with nor endorsed by
  id Software.

========================================================================
*/

#include "c_console.h"
#include "i_midirpc.h"
#include "i_system.h"
#include "m_config.h"
#include "m_misc.h"
#include "mmus2mid.h"
#include "s_sound.h"
#include "SDL_mixer.h"
#include "version.h"
#include "z_zone.h"

#define CHANNELS        2
#define SAMPLECOUNT     512

static dboolean         music_initialized;

// If this is true, this module initialized SDL sound and has the
// responsibility to shut it down
static dboolean         sdl_was_initialized;

static int              current_music_volume;
static int              paused_midi_volume;

musictype_t             musictype;

#if defined(_WIN32)
static dboolean         haveMidiServer;
static dboolean         haveMidiClient;
dboolean                serverMidiPlaying;
#endif

char                    *s_timiditycfgpath = s_timiditycfgpath_default;

static char             *temp_timidity_cfg;

// If the temp_timidity_cfg config variable is set, generate a "wrapper"
// config file for TiMidity to point to the actual config file. This
// is needed to inject a "dir" command so that the patches are read
// relative to the actual config file.
static dboolean WriteWrapperTimidityConfig(char *write_path)
{
    char        *p;
    FILE        *fstream;

    if (!*s_timiditycfgpath)
        return false;

    if (!(fstream = fopen(write_path, "w")))
        return false;

    if ((p = strrchr(s_timiditycfgpath, DIR_SEPARATOR)))
    {
        char    *path = strdup(s_timiditycfgpath);

        path[p - s_timiditycfgpath] = '\0';
        fprintf(fstream, "dir %s\n", path);
        free(path);
    }

    fprintf(fstream, "source %s\n", s_timiditycfgpath);
    fclose(fstream);

    return true;
}

void I_InitTimidityConfig(void)
{
    temp_timidity_cfg = M_TempFile("timidity.cfg");

    // Set the TIMIDITY_CFG environment variable to point to the temporary
    // config file.
    if (WriteWrapperTimidityConfig(temp_timidity_cfg))
        putenv(M_StringJoin("TIMIDITY_CFG=", temp_timidity_cfg, NULL));
    else
    {
        free(temp_timidity_cfg);
        temp_timidity_cfg = NULL;
    }
}

void CheckTimidityConfig(void)
{
    if (*s_timiditycfgpath)
    {
        if (M_FileExists(s_timiditycfgpath))
            C_Output("Using <i><b>TiMidity</b></i> configuration file <b>%s</b>.",
                s_timiditycfgpath);
        else
            C_Warning("Can't find <i>TiMidity</i> configuration file %s.", s_timiditycfgpath);
    }
}

// Remove the temporary config file generated by I_InitTimidityConfig().
static void RemoveTimidityConfig(void)
{
    if (temp_timidity_cfg)
    {
        remove(temp_timidity_cfg);
        free(temp_timidity_cfg);
    }
}

// Shutdown music
void I_ShutdownMusic(void)
{
    if (music_initialized)
    {
        Mix_HaltMusic();
        music_initialized = false;

        if (sdl_was_initialized)
        {
            Mix_CloseAudio();
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            sdl_was_initialized = false;
        }

#if defined(_WIN32)
        I_MidiRPCClientShutDown();
#endif
    }
}

static dboolean SDLIsInitialized(void)
{
    int         freq;
    int         channels;
    Uint16      format;

    return !!Mix_QuerySpec(&freq, &format, &channels);
}

// Initialize music subsystem
dboolean I_InitMusic(void)
{
    // If SDL_mixer is not initialized, we have to initialize it
    // and have the responsibility to shut it down later on.
    if (!SDLIsInitialized())
    {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
            return false;
        else if (Mix_OpenAudio(SAMPLERATE, MIX_DEFAULT_FORMAT, CHANNELS,
            SAMPLECOUNT * SAMPLERATE / 11025) < 0)
        {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
    }

    SDL_PauseAudio(0);

    sdl_was_initialized = true;
    music_initialized = true;

#if defined(_WIN32)
    // Initialize RPC server
    haveMidiServer = I_MidiRPCInitServer();
#endif

    // Once initialization is complete, the temporary TiMidity config
    // file can be removed.
    RemoveTimidityConfig();

    return music_initialized;
}

//
// SDL_mixer's native MIDI music playing does not pause properly.
// As a workaround, set the volume to 0 when paused.
//
static void UpdateMusicVolume(void)
{
#if defined(_WIN32)
    // adjust server volume
    if (serverMidiPlaying)
    {
        I_MidiRPCSetVolume(current_music_volume);
        return;
    }
#endif

    Mix_VolumeMusic(current_music_volume);
}

// Set music volume (0 - MAX_MUSIC_VOLUME)
void I_SetMusicVolume(int volume)
{
    // Internal state variable.
    current_music_volume = volume;

    UpdateMusicVolume();
}

// Start playing a mid
void I_PlaySong(void *handle, dboolean looping)
{
    if (!music_initialized)
        return;

#if defined(_WIN32)
    if (serverMidiPlaying)
    {
        I_MidiRPCPlaySong(looping);
        return;
    }
#endif

    if (handle)
        Mix_PlayMusic((Mix_Music *)handle, (looping ? -1 : 1));
}

void I_PauseSong(void)
{
    if (!music_initialized)
        return;

#if defined(_WIN32)
    if (serverMidiPlaying)
    {
        I_MidiRPCPauseSong();
        return;
    }
#endif

    if (musictype != MUSTYPE_MIDI)
        Mix_PauseMusic();
    else
    {
        paused_midi_volume = Mix_VolumeMusic(-1);
        Mix_VolumeMusic(0);
    }
}

void I_ResumeSong(void)
{
    if (!music_initialized)
        return;

#if defined(_WIN32)
    if (serverMidiPlaying)
    {
        I_MidiRPCResumeSong();
        return;
    }
#endif

    if (musictype != MUSTYPE_MIDI)
        Mix_ResumeMusic();
    else
        Mix_VolumeMusic(paused_midi_volume);
}

void I_StopSong(void)
{
    if (!music_initialized)
        return;

#if defined(_WIN32)
    if (serverMidiPlaying)
    {
        I_MidiRPCStopSong();
        serverMidiPlaying = false;
        return;
    }
#endif

    Mix_HaltMusic();
}

void I_UnRegisterSong(void *handle)
{
    if (!music_initialized)
        return;

#if defined(_WIN32)
    if (serverMidiPlaying)
    {
        I_MidiRPCStopSong();
        serverMidiPlaying = false;
        return;
    }
#endif

    if (handle)
        Mix_FreeMusic(handle);
}

void *I_RegisterSong(void *data, int size)
{
    if (!music_initialized)
        return NULL;
    else
    {
        dboolean        isMIDI = false;
        dboolean        isMUS = false;
        Mix_Music       *music = NULL;
        SDL_RWops       *rwops;

        musictype = MUSTYPE_NONE;

        // Check for MIDI or MUS format first:
        if (size >= 14)
        {
            if (!memcmp(data, "MThd", 4))                       // Is it a MIDI?
            {
                musictype = MUSTYPE_MIDI;
                isMIDI = true;
            }
            else if (mmuscheckformat((byte *)data, size))       // Is it a MUS?
            {
                musictype = MUSTYPE_MUS;
                isMUS = true;
            }
        }

        // If it's a MUS, convert it to MIDI now
        if (isMUS)
        {
            MIDI        mididata;
            UBYTE       *mid;
            int         midlen;

            memset(&mididata, 0, sizeof(MIDI));

            if (mmus2mid((byte *)data, (size_t)size, &mididata))
                return NULL;

            // Hurrah! Let's make it a mid and give it to SDL_mixer
            MIDIToMidi(&mididata, &mid, &midlen);

            data = mid;
            size = midlen;
            isMIDI = true;              // now it's a MIDI
        }

#if defined(_WIN32)
        // Check for option to invoke RPC server if isMIDI
        if (isMIDI && haveMidiServer)
        {
            if (!haveMidiClient)
            {
                if ((haveMidiClient = I_MidiRPCInitClient()))
                    I_MidiRPCSetVolume(current_music_volume);
                else
                    C_Warning("The RPC client couldn't be initialized.");
            }

            if (haveMidiClient && I_MidiRPCRegisterSong(data, size))
            {
                serverMidiPlaying = true;
                return NULL;    // server will play this song
            }
        }
#endif

        if ((rwops = SDL_RWFromMem(data, size)))
        {
            if ((music = Mix_LoadMUSType_RW(rwops, MUS_MID, SDL_FALSE)))
                musictype = MUSTYPE_MIDI;
            else if ((music = Mix_LoadMUSType_RW(rwops, MUS_OGG, SDL_FALSE)))
                musictype = MUSTYPE_OGG;
            else if ((music = Mix_LoadMUSType_RW(rwops, MUS_MP3, SDL_FALSE)))
                musictype = MUSTYPE_MP3;
            else if ((music = Mix_LoadMUSType_RW(rwops, MUS_WAV, SDL_FALSE)))
                musictype = MUSTYPE_WAV;
            else if ((music = Mix_LoadMUSType_RW(rwops, MUS_FLAC, SDL_FALSE)))
                musictype = MUSTYPE_FLAC;
            else if ((music = Mix_LoadMUSType_RW(rwops, MUS_MOD, SDL_FALSE)))
                musictype = MUSTYPE_MOD;
        }

        return music;
    }
}
