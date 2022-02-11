/*
 SPDX-License-Identifier: GPL-3.0-or-later
 myMPD (c) 2018-2022 Juergen Mang <mail@jcgames.de>
 https://github.com/jcorporation/mympd
*/

#include "mympd_config_defs.h"
#include "mympd_api_utility.h"

#include "../lib/jsonrpc.h"
#include "../lib/log.h"
#include "../lib/mem.h"
#include "../lib/mimetype.h"
#include "../lib/mympd_configuration.h"
#include "../lib/sds_extras.h"
#include "../lib/utility.h"
#include "../lib/validate.h"
#include "../mpd_client/mpd_client_jukebox.h"
#include "../mpd_shared.h"
#include "../mpd_shared/mpd_shared_sticker.h"
#include "../mpd_shared/mpd_shared_tags.h"
#include "mympd_api_timer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <libgen.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

//optional includes
#ifdef ENABLE_LIBID3TAG
    #include <id3tag.h>
#endif

#ifdef ENABLE_FLAC
    #include <FLAC/metadata.h>
#endif

//private definitons
static void _get_extra_files(struct t_mympd_state *mympd_state, const char *uri, sds *booklet_path, struct t_list *images, bool is_dirname);
static int _get_embedded_covers_count(const char *media_file);
static int _get_embedded_covers_count_id3(const char *media_file);
static int _get_embedded_covers_count_flac(const char *media_file, bool is_ogg);

static sds get_local_ip(void) {
    struct ifaddrs *ifaddr;
    struct ifaddrs *ifa;
    char host[NI_MAXHOST];

    errno = 0;
    if (getifaddrs(&ifaddr) == -1) {
        MYMPD_LOG_ERROR("Can not get list of inteface ip addresses");
        MYMPD_LOG_ERRNO(errno);
        return sdsempty();
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL ||
            strcmp(ifa->ifa_name, "lo") == 0)
        {
            continue;
        }
        int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET ||
            family == AF_INET6)
        {
            int s = getnameinfo(ifa->ifa_addr,
                    (family == AF_INET) ? sizeof(struct sockaddr_in) :
                                          sizeof(struct sockaddr_in6),
                    host, NI_MAXHOST,
                    NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                MYMPD_LOG_ERROR("getnameinfo() failed: %s\n", gai_strerror(s));
                continue;
            }
            char *crap;
            // remove zone info from ipv6
            char *ip = strtok_r(host, "%", &crap);
            sds ip_str = sdsnew(ip);
            freeifaddrs(ifaddr);
            return ip_str;
        }
    }
    freeifaddrs(ifaddr);
    return sdsempty();
}

static sds get_mympd_host(struct t_mympd_state *mympd_state) {
    if (strncmp(mympd_state->mpd_state->mpd_host, "/", 1) == 0) {
        //local socket - use localhost
        return sdsnew("localhost");
    }
    if (strcmp(mympd_state->config->http_host, "0.0.0.0") != 0) {
        //host defined in configuration
        return sdsdup(mympd_state->config->http_host);
    }
    //get local ip
    return get_local_ip();
}

//public functions
sds mympd_api_status_print(struct t_mympd_state *mympd_state, sds buffer, struct mpd_status *status) {
    enum mpd_state playstate = mpd_status_get_state(status);
    const char *playstate_str =
        playstate == MPD_STATE_STOP ? "stop" :
        playstate == MPD_STATE_PLAY ? "play" :
        playstate == MPD_STATE_PAUSE ? "pause" : "unknown";

    buffer = tojson_char(buffer, "state", playstate_str, true);
    buffer = tojson_long(buffer, "volume", mpd_status_get_volume(status), true);
    buffer = tojson_long(buffer, "songPos", mpd_status_get_song_pos(status), true);
    buffer = tojson_uint(buffer, "elapsedTime", mympd_api_get_elapsed_seconds(status), true);
    buffer = tojson_uint(buffer, "totalTime", mpd_status_get_total_time(status), true);
    buffer = tojson_long(buffer, "currentSongId", mpd_status_get_song_id(status), true);
    buffer = tojson_uint(buffer, "kbitrate", mpd_status_get_kbit_rate(status), true);
    buffer = tojson_uint(buffer, "queueLength", mpd_status_get_queue_length(status), true);
    buffer = tojson_uint(buffer, "queueVersion", mpd_status_get_queue_version(status), true);
    buffer = tojson_long(buffer, "nextSongPos", mpd_status_get_next_song_pos(status), true);
    buffer = tojson_long(buffer, "nextSongId", mpd_status_get_next_song_id(status), true);
    buffer = tojson_long(buffer, "lastSongId", (mympd_state->mpd_state->last_song_id ?
        mympd_state->mpd_state->last_song_id : -1), true);
    if (mympd_state->mpd_state->feat_mpd_partitions == true) {
        buffer = tojson_char(buffer, "partition", mpd_status_get_partition(status), true);
    }
    const struct mpd_audio_format *audioformat = mpd_status_get_audio_format(status);
    buffer = printAudioFormat(buffer, audioformat);
    buffer = sdscatlen(buffer, ",", 1);
    buffer = tojson_char(buffer, "lastError", mpd_status_get_error(status), false);
    return buffer;
}

enum jukebox_modes mympd_parse_jukebox_mode(const char *str) {
    if (strcmp(str, "off") == 0) {
        return JUKEBOX_OFF;
    }
    if (strcmp(str, "song") == 0) {
        return JUKEBOX_ADD_SONG;
    }
    if (strcmp(str, "album") == 0) {
        return JUKEBOX_ADD_ALBUM;
    }
    return JUKEBOX_UNKNOWN;
}

const char *mympd_lookup_jukebox_mode(enum jukebox_modes mode) {
	switch (mode) {
        case JUKEBOX_OFF:
            return "off";
        case JUKEBOX_ADD_SONG:
            return "song";
        case JUKEBOX_ADD_ALBUM:
            return "album";
        default:
            return NULL;
    }
	return NULL;
}

sds resolv_mympd_uri(sds uri, struct t_mympd_state *mympd_state) {
    if (strncmp(uri, "mympd://webradio/", 17) == 0) {
        sdsrange(uri, 17, -1);
        sds host = get_mympd_host(mympd_state);
        sds new_uri = sdscatfmt(sdsempty(), "http://%s:%s/browse/webradios/%s", host, mympd_state->config->http_port, uri);
        sdsfree(uri);
        sdsfree(host);
        return new_uri;
    }
    return uri;
}

sds get_extra_files(struct t_mympd_state *mympd_state, sds buffer, const char *uri, bool is_dirname) {
    struct t_list images;
    list_init(&images);
    sds booklet_path = sdsempty();
    if (is_streamuri(uri) == false &&
        mympd_state->mpd_state->feat_mpd_library == true)
    {
        _get_extra_files(mympd_state, uri, &booklet_path, &images, is_dirname);
    }
    buffer = tojson_char(buffer, "bookletPath", booklet_path, true);
    buffer = sdscat(buffer, "\"images\": [");
    struct t_list_node *current = images.head;
    while (current != NULL) {
        if (current != images.head) {
            buffer = sdscatlen(buffer, ",", 1);
        }
        buffer = sds_catjson(buffer, current->key, sdslen(current->key));
        current = current->next;
    }
    buffer = sdscatlen(buffer, "]", 1);
    list_clear(&images);
    FREE_SDS(booklet_path);
    return buffer;
}

bool is_smartpls(const char *workdir, sds playlist) {
    bool smartpls = false;
    if (vcb_isfilename_silent(playlist) == true) {
        sds smartpls_file = sdscatfmt(sdsempty(), "%s/smartpls/%s", workdir, playlist);
        if (access(smartpls_file, F_OK ) != -1) { /* Flawfinder: ignore */
            smartpls = true;
        }
        FREE_SDS(smartpls_file);
    }
    return smartpls;
}

bool mympd_api_set_binarylimit(struct t_mympd_state *mympd_state) {
    bool rc = true;
    if (mympd_state->mpd_state->feat_mpd_binarylimit == true) {
        MYMPD_LOG_INFO("Setting binarylimit to %u", mympd_state->mpd_state->mpd_binarylimit);
        rc = mpd_run_binarylimit(mympd_state->mpd_state->conn, mympd_state->mpd_state->mpd_binarylimit);
        sds message = sdsempty();
        rc = check_rc_error_and_recover(mympd_state->mpd_state, &message, NULL, 0, true, rc, "mpd_run_binarylimit");
        if (sdslen(message) > 0) {
            ws_notify(message);
            rc = false;
        }
        FREE_SDS(message);
    }
    return rc;
}

//replacement for deprecated mpd_status_get_elapsed_time
unsigned mympd_api_get_elapsed_seconds(struct mpd_status *status) {
    return mpd_status_get_elapsed_ms(status) / 1000;
}

void mympd_state_default(struct t_mympd_state *mympd_state) {
    mympd_state->music_directory = sdsnew("auto");
    mympd_state->music_directory_value = sdsempty();
    mympd_state->playlist_directory = sdsnew("/var/lib/mpd/playlists");
    mympd_state->jukebox_mode = JUKEBOX_OFF;
    mympd_state->jukebox_playlist = sdsnew("Database");
    mympd_state->jukebox_unique_tag.len = 1;
    mympd_state->jukebox_unique_tag.tags[0] = MPD_TAG_ARTIST;
    mympd_state->jukebox_last_played = 24;
    mympd_state->jukebox_queue_length = 1;
    mympd_state->jukebox_enforce_unique = true;
    mympd_state->coverimage_names = sdsnew("folder,cover");
    mympd_state->tag_list_search = sdsnew("Artist,Album,AlbumArtist,Title,Genre");
    mympd_state->tag_list_browse = sdsnew("Artist,Album,AlbumArtist,Genre");
    mympd_state->smartpls_generate_tag_list = sdsnew("Genre");
    mympd_state->last_played_count = 200;
    mympd_state->smartpls = true;
    mympd_state->smartpls_sort = sdsempty();
    mympd_state->smartpls_prefix = sdsnew("myMPDsmart");
    mympd_state->smartpls_interval = 14400;
    mympd_state->booklet_name = sdsnew("booklet.pdf");
    mympd_state->auto_play = false;
    mympd_state->cols_queue_current = sdsnew("[\"Pos\",\"Title\",\"Artist\",\"Album\",\"Duration\"]");
    mympd_state->cols_search = sdsnew("[\"Title\",\"Artist\",\"Album\",\"Duration\"]");
    mympd_state->cols_browse_database_detail = sdsnew("[\"Track\",\"Title\",\"Duration\"]");
    mympd_state->cols_browse_playlists_detail = sdsnew("[\"Pos\",\"Title\",\"Artist\",\"Album\",\"Duration\"]");
    mympd_state->cols_browse_filesystem = sdsnew("[\"Pos\",\"Title\",\"Artist\",\"Album\",\"Duration\"]");
    mympd_state->cols_playback = sdsnew("[\"Artist\",\"Album\"]");
    mympd_state->cols_queue_last_played = sdsnew("[\"Pos\",\"Title\",\"Artist\",\"Album\",\"LastPlayed\"]");
    mympd_state->cols_queue_jukebox = sdsnew("[\"Pos\",\"Title\",\"Artist\",\"Album\"]");
    mympd_state->cols_browse_radio_webradiodb = sdsnew("[\"Name\",\"Country\",\"Language\",\"Genre\"]");
    mympd_state->cols_browse_radio_radiobrowser = sdsnew("[\"name\",\"country\",\"language\",\"tags\"]");
    mympd_state->volume_min = 0;
    mympd_state->volume_max = 100;
    mympd_state->volume_step = 5;
    mympd_state->mpd_stream_port = 8080;
    mympd_state->webui_settings = sdsnew("{}");
    mympd_state->lyrics_uslt_ext = sdsnew("txt");
    mympd_state->lyrics_sylt_ext = sdsnew("lrc");
    mympd_state->lyrics_vorbis_uslt = sdsnew("LYRICS");
    mympd_state->lyrics_vorbis_sylt = sdsnew("SYNCEDLYRICS");
    mympd_state->covercache_keep_days = 7;
    reset_t_tags(&mympd_state->tag_types_search);
    reset_t_tags(&mympd_state->tag_types_browse);
    reset_t_tags(&mympd_state->smartpls_generate_tag_types);
    //init last played songs list
    list_init(&mympd_state->last_played);
    //init sticker queue
    list_init(&mympd_state->sticker_queue);
    //sticker cache
    mympd_state->sticker_cache_building = false;
    mympd_state->sticker_cache = NULL;
    //album cache
    mympd_state->album_cache_building = false;
    mympd_state->album_cache = NULL;
    //jukebox queue
    list_init(&mympd_state->jukebox_queue);
    list_init(&mympd_state->jukebox_queue_tmp);
    //mpd state
    mympd_state->mpd_state = malloc_assert(sizeof(struct t_mpd_state));
    mpd_shared_default_mpd_state(mympd_state->mpd_state);
    //triggers;
    list_init(&mympd_state->triggers);
    //home icons
    list_init(&mympd_state->home_list);
    //timer
    mympd_api_timer_timerlist_init(&mympd_state->timer_list);
}

void mympd_state_free(struct t_mympd_state *mympd_state) {
    mpd_client_clear_jukebox(&mympd_state->jukebox_queue);
    mpd_client_clear_jukebox(&mympd_state->jukebox_queue_tmp);
    list_clear(&mympd_state->sticker_queue);
    list_clear(&mympd_state->triggers);
    list_clear(&mympd_state->last_played);
    list_clear(&mympd_state->home_list);
    mympd_api_timer_timerlist_free(&mympd_state->timer_list);
    //mpd state
    mpd_shared_free_mpd_state(mympd_state->mpd_state);
    //caches
    sticker_cache_free(&mympd_state->sticker_cache);
    album_cache_free(&mympd_state->album_cache);
    //sds
    FREE_SDS(mympd_state->tag_list_search);
    FREE_SDS(mympd_state->tag_list_browse);
    FREE_SDS(mympd_state->smartpls_generate_tag_list);
    FREE_SDS(mympd_state->jukebox_playlist);
    FREE_SDS(mympd_state->cols_queue_current);
    FREE_SDS(mympd_state->cols_search);
    FREE_SDS(mympd_state->cols_browse_database_detail);
    FREE_SDS(mympd_state->cols_browse_playlists_detail);
    FREE_SDS(mympd_state->cols_browse_filesystem);
    FREE_SDS(mympd_state->cols_playback);
    FREE_SDS(mympd_state->cols_queue_last_played);
    FREE_SDS(mympd_state->cols_queue_jukebox);
    FREE_SDS(mympd_state->cols_browse_radio_webradiodb);
    FREE_SDS(mympd_state->cols_browse_radio_radiobrowser);
    FREE_SDS(mympd_state->coverimage_names);
    FREE_SDS(mympd_state->music_directory);
    FREE_SDS(mympd_state->music_directory_value);
    FREE_SDS(mympd_state->smartpls_sort);
    FREE_SDS(mympd_state->smartpls_prefix);
    FREE_SDS(mympd_state->booklet_name);
    FREE_SDS(mympd_state->navbar_icons);
    FREE_SDS(mympd_state->webui_settings);
    FREE_SDS(mympd_state->playlist_directory);
    FREE_SDS(mympd_state->lyrics_sylt_ext);
    FREE_SDS(mympd_state->lyrics_uslt_ext);
    FREE_SDS(mympd_state->lyrics_vorbis_uslt);
    FREE_SDS(mympd_state->lyrics_vorbis_sylt);
    //struct itself
    FREE_PTR(mympd_state);
}

//private functions
static void _get_extra_files(struct t_mympd_state *mympd_state, const char *uri, sds *booklet_path, struct t_list *images, bool is_dirname) {
    sds path = sdsnew(uri);
    if (is_dirname == false) {
        dirname(path);
        sdsupdatelen(path);
    }

    if (is_virtual_cuedir(mympd_state->music_directory_value, path)) {
        //fix virtual cue sheet directories
        dirname(path);
        sdsupdatelen(path);
    }
    sds albumpath = sdscatfmt(sdsempty(), "%s/%s", mympd_state->music_directory_value, path);
    sds fullpath = sdsempty();
    MYMPD_LOG_DEBUG("Read extra files from albumpath: \"%s\"", albumpath);
    errno = 0;
    DIR *album_dir = opendir(albumpath);
    if (album_dir != NULL) {
        struct dirent *next_file;
        while ((next_file = readdir(album_dir)) != NULL) {
            const char *ext = strrchr(next_file->d_name, '.');
            if (strcmp(next_file->d_name, mympd_state->booklet_name) == 0) {
                MYMPD_LOG_DEBUG("Found booklet for uri %s", uri);
                *booklet_path = sdscatfmt(*booklet_path, "/browse/music/%s/%s", path, mympd_state->booklet_name);
            }
            else if (ext != NULL) {
                if (strcasecmp(ext, ".webp") == 0 || strcasecmp(ext, ".jpg") == 0 ||
                    strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".png") == 0 ||
                    strcasecmp(ext, ".avif") == 0 || strcasecmp(ext, ".svg") == 0)
                {
                    fullpath = sdscatfmt(fullpath, "/browse/music/%s/%s", path, next_file->d_name);
                    list_push(images, fullpath, 0, NULL, NULL);
                    sdsclear(fullpath);
                }
            }
        }
        closedir(album_dir);
    }
    else {
        MYMPD_LOG_ERROR("Can not open directory \"%s\" to get list of extra files", albumpath);
        MYMPD_LOG_ERRNO(errno);
    }
    if (is_dirname == false) {
        int count = _get_embedded_covers_count(uri);
        for (int i = 0; i < count; i++) {
            fullpath = sdscatfmt(fullpath, "/albumart?offset=%d&uri=%s", count, uri);
            list_push(images, fullpath, 0, NULL, NULL);
            sdsclear(fullpath);
        }
    }
    FREE_SDS(fullpath);
    FREE_SDS(path);
    FREE_SDS(albumpath);
}

static int _get_embedded_covers_count(const char *media_file) {
    int count = 0;
    const char *mime_type_media_file = get_mime_type_by_ext(media_file);
    MYMPD_LOG_DEBUG("Counting coverextract for uri \"%s\"", media_file);
    MYMPD_LOG_DEBUG("Mimetype of %s is %s", media_file, mime_type_media_file);
    if (strcmp(mime_type_media_file, "audio/mpeg") == 0) {
        count = _get_embedded_covers_count_id3(media_file);
    }
    else if (strcmp(mime_type_media_file, "audio/ogg") == 0) {
        count = _get_embedded_covers_count_flac(media_file, true);
    }
    else if (strcmp(mime_type_media_file, "audio/flac") == 0) {
        count = _get_embedded_covers_count_flac(media_file, false);
    }
    return count;
}

static int _get_embedded_covers_count_id3(const char *media_file) {
    int count = 0;
    #ifdef ENABLE_LIBID3TAG
    MYMPD_LOG_DEBUG("Counting coverimages from %s", media_file);
    struct id3_file *file_struct = id3_file_open(media_file, ID3_FILE_MODE_READONLY);
    if (file_struct == NULL) {
        MYMPD_LOG_ERROR("Can't parse id3_file: %s", media_file);
        return 0;
    }
    struct id3_tag *tags = id3_file_tag(file_struct);
    if (tags == NULL) {
        MYMPD_LOG_ERROR("Can't read id3 tags from file: %s", media_file);
        return 0;
    }
    struct id3_frame *frame;
    do {
        frame = id3_tag_findframe(tags, "APIC", (unsigned)count);
        count++;
    } while (frame != NULL);
    id3_file_close(file_struct);
    #else
    (void) media_file;
    #endif
    return count;
}

static int _get_embedded_covers_count_flac(const char *media_file, bool is_ogg) {
    int count = 0;
    #ifdef ENABLE_FLAC
    MYMPD_LOG_DEBUG("Counting coverimages from %s", media_file);
    FLAC__StreamMetadata *metadata = NULL;

    FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();

    if(! (is_ogg? FLAC__metadata_chain_read_ogg(chain, media_file) : FLAC__metadata_chain_read(chain, media_file)) ) {
        MYMPD_LOG_DEBUG("%s: ERROR: reading metadata", media_file);
        FLAC__metadata_chain_delete(chain);
        return 0;
    }

    FLAC__Metadata_Iterator *iterator = FLAC__metadata_iterator_new();
    FLAC__metadata_iterator_init(iterator, chain);
    assert(iterator);
    do {
        do {
            FLAC__StreamMetadata *block = FLAC__metadata_iterator_get_block(iterator);
            if (block->type == FLAC__METADATA_TYPE_PICTURE) {
                metadata = block;
            }
        } while (FLAC__metadata_iterator_next(iterator) && metadata == NULL);
        count++;
    } while (metadata != NULL);

    FLAC__metadata_iterator_delete(iterator);
    FLAC__metadata_chain_delete(chain);
    #else
    (void) media_file;
    (void) is_ogg;
    #endif
    return count;
}
