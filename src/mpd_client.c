/*
 SPDX-License-Identifier: GPL-2.0-or-later
 myMPD (c) 2018-2021 Juergen Mang <mail@jcgames.de>
 https://github.com/jcorporation/mympd
*/

#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>

#include <mpd/client.h>

#include "../dist/src/sds/sds.h"
#include "../dist/src/rax/rax.h"
#include "sds_extras.h"
#include "log.h"
#include "list.h"
#include "mympd_config_defs.h"
#include "tiny_queue.h"
#include "api.h"
#include "global.h"
#include "utility.h"
#include "lua_mympd_state.h"
#include "mympd_state.h"
#include "mpd_shared/mpd_shared_tags.h"
#include "mpd_shared.h"
#include "mpd_shared/mpd_shared_sticker.h"
#include "mpd_client/mpd_client_utility.h"
#include "mpd_client/mpd_client_browse.h"
#include "mpd_client/mpd_client_jukebox.h"
#include "mpd_client/mpd_client_playlists.h"
#include "mpd_client/mpd_client_stats.h"
#include "mpd_client/mpd_client_state.h"
#include "mympd_state.h"
#include "mpd_worker.h"
#include "mpd_client/mpd_client_features.h"
#include "mpd_client/mpd_client_queue.h"
#include "mpd_client/mpd_client_features.h"
#include "mpd_client/mpd_client_sticker.h"
#include "mpd_client/mpd_client_timer.h"
#include "mpd_client/mpd_client_trigger.h"
#include "mympd_api/mympd_api_handler.h"
#include "mpd_client.h"

//private definitions
static bool update_mympd_caches(struct t_mympd_state *mympd_state);

//public functions
void mpd_client_parse_idle(struct t_mympd_state *mympd_state, int idle_bitmask) {
    for (unsigned j = 0;; j++) {
        enum mpd_idle idle_event = 1 << j;
        const char *idle_name = mpd_idle_name(idle_event);
        if (idle_name == NULL) {
            break;
        }
        if (idle_bitmask & idle_event) {
            MYMPD_LOG_INFO("MPD idle event: %s", idle_name);
            sds buffer = sdsempty();
            switch(idle_event) {
                case MPD_IDLE_DATABASE:
                    //database has changed
                    buffer = jsonrpc_event(buffer, "update_database");
                    //initiate cache updates
                    update_mympd_caches(mympd_state);
                    break;
                case MPD_IDLE_STORED_PLAYLIST:
                    buffer = jsonrpc_event(buffer, "update_stored_playlist");
                    break;
                case MPD_IDLE_QUEUE:
                    buffer = mpd_client_get_queue_state(mympd_state, buffer);
                    //jukebox enabled
                    if (mympd_state->jukebox_mode != JUKEBOX_OFF && mympd_state->mpd_state->queue_length < mympd_state->jukebox_queue_length) {
                        mpd_client_jukebox(mympd_state, 0);
                    }
                    //autoPlay enabled
                    if (mympd_state->auto_play == true && mympd_state->mpd_state->queue_length > 1) {
                        if (mympd_state->mpd_state->state != MPD_STATE_PLAY) {
                            MYMPD_LOG_INFO("AutoPlay enabled, start playing");
                            if (!mpd_run_play(mympd_state->mpd_state->conn)) {
                                check_error_and_recover(mympd_state->mpd_state, NULL, NULL, 0);
                            }
                        }
                    }
                    break;
                case MPD_IDLE_PLAYER:
                    //get and put mpd state                
                    buffer = mpd_client_put_state(mympd_state, buffer, NULL, 0);
                    //song has changed
                    if (mympd_state->mpd_state->song_id != mympd_state->mpd_state->last_song_id && 
                        mympd_state->mpd_state->last_skipped_id != mympd_state->mpd_state->last_song_id && 
                        mympd_state->mpd_state->last_song_uri != NULL)
                    {
                        time_t now = time(NULL);
                        if (mympd_state->mpd_state->feat_stickers && //stickers enabled
                            mympd_state->mpd_state->last_song_set_song_played_time > now) //time in the future
                        {
                            //last song skipped
                            time_t elapsed = now - mympd_state->mpd_state->last_song_start_time;
                            if (elapsed > 10 && mympd_state->mpd_state->last_song_start_time > 0 &&
                                sdslen(mympd_state->mpd_state->last_song_uri) > 0)
                            {
                                MYMPD_LOG_DEBUG("Song \"%s\" skipped", mympd_state->mpd_state->last_song_uri);
                                mpd_client_sticker_inc_skip_count(mympd_state, mympd_state->mpd_state->last_song_uri);
                                mpd_client_sticker_last_skipped(mympd_state, mympd_state->mpd_state->last_song_uri);
                                mympd_state->mpd_state->last_skipped_id = mympd_state->mpd_state->last_song_id;
                            }
                        }
                    }
                    break;
                case MPD_IDLE_MIXER:
                    buffer = mpd_client_put_volume(mympd_state, buffer, NULL, 0);
                    break;
                case MPD_IDLE_OUTPUT:
                    buffer = jsonrpc_event(buffer, "update_outputs");
                    break;
                case MPD_IDLE_OPTIONS:
                    mpd_client_get_queue_state(mympd_state, NULL);
                    buffer = jsonrpc_event(buffer, "update_options");
                    break;
                case MPD_IDLE_UPDATE:
                    buffer = mpd_client_get_updatedb_state(mympd_state, buffer);
                    break;
                case MPD_IDLE_PARTITION:
                    //TODO: check list of partitions and create new mpd_client threads
                    break;
                default: {
                    //other idle events not used
                }
            }
            trigger_execute(mympd_state, (enum trigger_events)idle_event);
            if (sdslen(buffer) > 0) {
                ws_notify(buffer);
            }
            sdsfree(buffer);
        }
    }
}

void mpd_client_idle(struct t_mympd_state *mympd_state) {
    struct pollfd fds[1];
    int pollrc;
    sds buffer = sdsempty();
    unsigned mympd_api_queue_length = 0;
    switch (mympd_state->mpd_state->conn_state) {
        case MPD_WAIT: {
            time_t now = time(NULL);
            if (now > mympd_state->mpd_state->reconnect_time) {
                mympd_state->mpd_state->conn_state = MPD_DISCONNECTED;
            }
            //mpd_client_api error response
            mympd_api_queue_length = tiny_queue_length(mympd_api_queue, 50);
            if (mympd_api_queue_length > 0) {
                //Handle request
                MYMPD_LOG_DEBUG("Handle request (mpd disconnected)");
                t_work_request *request = tiny_queue_shift(mympd_api_queue, 50, 0);
                if (request != NULL) {
                    if (is_mympd_only_api_method(request->cmd_id) == true) {
                        //reconnect instantly on change of mpd host
                        if (request->cmd_id == MYMPD_API_CONNECTION_SAVE) {
                            mympd_state->mpd_state->conn_state = MPD_DISCONNECTED;
                        }
                        mympd_api_handler(mympd_state, request);
                    }
                    else {
                        //other requests not allowed
                        if (request->conn_id > -1) {
                            t_work_result *response = create_result(request);
                            response->data = jsonrpc_respond_message(response->data, request->method, request->id, true, "mpd", "error", "MPD disconnected");
                            MYMPD_LOG_DEBUG("Send http response to connection %lu: %s", request->conn_id, response->data);
                            tiny_queue_push(web_server_queue, response, 0);
                        }
                        free_request(request);
                    }
                }
            }
            if (now < mympd_state->mpd_state->reconnect_time) {
                //pause 100ms to prevent high cpu usage
                my_usleep(100000);
            }
            break;
        }
        case MPD_DISCONNECTED:
            /* Try to connect */
            if (strncmp(mympd_state->mpd_state->mpd_host, "/", 1) == 0) {
                MYMPD_LOG_NOTICE("MPD connecting to socket %s", mympd_state->mpd_state->mpd_host);
            }
            else {
                MYMPD_LOG_NOTICE("MPD connecting to %s:%d", mympd_state->mpd_state->mpd_host, mympd_state->mpd_state->mpd_port);
            }
            mympd_state->mpd_state->conn = mpd_connection_new(mympd_state->mpd_state->mpd_host, mympd_state->mpd_state->mpd_port, mympd_state->mpd_state->mpd_timeout);
            if (mympd_state->mpd_state->conn == NULL) {
                MYMPD_LOG_ERROR("MPD connection to failed: out-of-memory");
                buffer = jsonrpc_event(buffer, "mpd_disconnected");
                ws_notify(buffer);
                sdsfree(buffer);
                mympd_state->mpd_state->conn_state = MPD_FAILURE;
                mpd_connection_free(mympd_state->mpd_state->conn);
                return;
            }

            if (mpd_connection_get_error(mympd_state->mpd_state->conn) != MPD_ERROR_SUCCESS) {
                MYMPD_LOG_ERROR("MPD connection: %s", mpd_connection_get_error_message(mympd_state->mpd_state->conn));
                buffer = jsonrpc_notify_phrase(buffer, "mpd", "error", "MPD connection error: %{error}", 
                    2, "error", mpd_connection_get_error_message(mympd_state->mpd_state->conn));
                ws_notify(buffer);
                sdsfree(buffer);
                mympd_state->mpd_state->conn_state = MPD_FAILURE;
                return;
            }

            if (sdslen(mympd_state->mpd_state->mpd_pass) > 0 && !mpd_run_password(mympd_state->mpd_state->conn, mympd_state->mpd_state->mpd_pass)) {
                MYMPD_LOG_ERROR("MPD connection: %s", mpd_connection_get_error_message(mympd_state->mpd_state->conn));
                buffer = jsonrpc_notify_phrase(buffer, "mpd", "error", "MPD connection error: %{error}", 2, 
                    "error", mpd_connection_get_error_message(mympd_state->mpd_state->conn));
                ws_notify(buffer);
                sdsfree(buffer);
                mympd_state->mpd_state->conn_state = MPD_FAILURE;
                return;
            }

            MYMPD_LOG_NOTICE("MPD connected");
            //check version
            if (mpd_connection_cmp_server_version(mympd_state->mpd_state->conn, 0, 20, 0) < 0) {
                MYMPD_LOG_EMERG("MPD version to old, myMPD supports only MPD version >= 0.20.0");
                mympd_state->mpd_state->conn_state = MPD_TOO_OLD;
                s_signal_received = 1;
            }
            
            mpd_connection_set_timeout(mympd_state->mpd_state->conn, mympd_state->mpd_state->mpd_timeout);
            buffer = jsonrpc_event(buffer, "mpd_connected");
            ws_notify(buffer);
            mympd_state->mpd_state->conn_state = MPD_CONNECTED;
            mympd_state->mpd_state->reconnect_interval = 0;
            mympd_state->mpd_state->reconnect_time = 0;
            //reset list of supported tags
            reset_t_tags(&mympd_state->mpd_state->tag_types_mpd);
            //get mpd features
            mpd_client_mpd_features(mympd_state);
            //set binarylimit
            mpd_client_set_binarylimit(mympd_state);
            //initiate cache updates
            update_mympd_caches(mympd_state);
            //set timer for smart playlist update
            mpd_client_set_timer(MYMPD_API_TIMER_SET, "MYMPD_API_TIMER_SET", 10, mympd_state->smartpls_interval, "timer_handler_smartpls_update");
            //jukebox
            if (mympd_state->jukebox_mode != JUKEBOX_OFF) {
                mpd_client_jukebox(mympd_state, 0);
            }
            if (!mpd_send_idle(mympd_state->mpd_state->conn)) {
                MYMPD_LOG_ERROR("Entering idle mode failed");
                mympd_state->mpd_state->conn_state = MPD_FAILURE;
            }
            trigger_execute(mympd_state, TRIGGER_MYMPD_CONNECTED);
            break;

        case MPD_FAILURE:
            MYMPD_LOG_ERROR("MPD connection failed");
            buffer = jsonrpc_event(buffer, "mpd_disconnected");
            ws_notify(buffer);
            trigger_execute(mympd_state, TRIGGER_MYMPD_DISCONNECTED);
            // fall through
        case MPD_DISCONNECT:
        case MPD_RECONNECT:
            if (mympd_state->mpd_state->conn != NULL) {
                mpd_connection_free(mympd_state->mpd_state->conn);
            }
            mympd_state->mpd_state->conn = NULL;
            mympd_state->mpd_state->conn_state = MPD_WAIT;
            if (mympd_state->mpd_state->reconnect_interval < 20) {
                mympd_state->mpd_state->reconnect_interval += 2;
            }
            mympd_state->mpd_state->reconnect_time = time(NULL) + mympd_state->mpd_state->reconnect_interval;
            MYMPD_LOG_INFO("Waiting %u seconds before reconnection", mympd_state->mpd_state->reconnect_interval);
            break;

        case MPD_CONNECTED:
            fds[0].fd = mpd_connection_get_fd(mympd_state->mpd_state->conn);
            fds[0].events = POLLIN;
            pollrc = poll(fds, 1, 50);
            bool jukebox_add_song = false;
            bool set_played = false;
            mympd_api_queue_length = tiny_queue_length(mympd_api_queue, 50);
            time_t now = time(NULL);
            if (mympd_state->mpd_state->state == MPD_STATE_PLAY) {
                //handle jukebox and last played only in mpd play state
                if (now > mympd_state->mpd_state->set_song_played_time && 
                    mympd_state->mpd_state->set_song_played_time > 0 && 
                    mympd_state->mpd_state->last_last_played_id != mympd_state->mpd_state->song_id)
                {
                    set_played = true;
                }
                if (mympd_state->jukebox_mode != JUKEBOX_OFF) {
                    time_t add_time = mympd_state->mpd_state->crossfade < mympd_state->mpd_state->song_end_time ? 
                                      mympd_state->mpd_state->song_end_time - mympd_state->mpd_state->crossfade : 
                                      mympd_state->mpd_state->song_end_time;
                    if (now > add_time && add_time > 0 && 
                        mympd_state->mpd_state->queue_length <= mympd_state->jukebox_queue_length)
                    {
                        jukebox_add_song = true;
                    }
                }
            }
            if (pollrc > 0 || mympd_api_queue_length > 0 || jukebox_add_song == true || 
                set_played == true || mympd_state->sticker_queue.length > 0) 
            {
                MYMPD_LOG_DEBUG("Leaving mpd idle mode");
                if (!mpd_send_noidle(mympd_state->mpd_state->conn)) {
                    check_error_and_recover(mympd_state->mpd_state, NULL, NULL, 0);
                    mympd_state->mpd_state->conn_state = MPD_FAILURE;
                    break;
                }
                if (pollrc > 0) {
                    //Handle idle events
                    MYMPD_LOG_DEBUG("Checking for idle events");
                    enum mpd_idle idle_bitmask = mpd_recv_idle(mympd_state->mpd_state->conn, false);
                    mpd_client_parse_idle(mympd_state, idle_bitmask);
                } 
                else {
                    mpd_response_finish(mympd_state->mpd_state->conn);
                }
                
                if (set_played == true) {
                    mympd_state->mpd_state->last_last_played_id = mympd_state->mpd_state->song_id;
                    
                    if (mympd_state->last_played_count > 0) {
                        mpd_client_add_song_to_last_played_list(mympd_state, mympd_state->mpd_state->song_id);
                    }
                    if (mympd_state->mpd_state->feat_stickers == true) {
                        mpd_client_sticker_inc_play_count(mympd_state, mympd_state->mpd_state->song_uri);
                        mpd_client_sticker_last_played(mympd_state, mympd_state->mpd_state->song_uri);
                    }
                    trigger_execute(mympd_state, TRIGGER_MYMPD_SCROBBLE);
                }
                
                if (jukebox_add_song == true) {
                    mpd_client_jukebox(mympd_state, 0);
                }
                
                if (mympd_api_queue_length > 0) {
                    //Handle request
                    MYMPD_LOG_DEBUG("Handle request");
                    t_work_request *request = tiny_queue_shift(mympd_api_queue, 50, 0);
                    if (request != NULL) {
                        mympd_api_handler(mympd_state, request);
                    }
                }
                
                if (mympd_state->sticker_queue.length > 0) {
                    mpd_client_sticker_dequeue(mympd_state);
                }
                
                MYMPD_LOG_DEBUG("Entering mpd idle mode");
                if (!mpd_send_idle(mympd_state->mpd_state->conn)) {
                    check_error_and_recover(mympd_state->mpd_state, NULL, NULL, 0);
                    mympd_state->mpd_state->conn_state = MPD_FAILURE;
                }
            }
            break;
        default:
            MYMPD_LOG_ERROR("Invalid mpd connection state");
    }
    sdsfree(buffer);
}

static bool update_mympd_caches(struct t_mympd_state *mympd_state) {
    if (mympd_state->mpd_state->feat_stickers == false && mympd_state->mpd_state->feat_tags == false) {
        return true;
    }
    if (mympd_state->mpd_state->feat_stickers == true) {
        mympd_state->sticker_cache_building = true;
    }
    if (mympd_state->mpd_state->feat_tags == true) {
        mympd_state->album_cache_building = true;
    }
    t_work_request *request = create_request(-1, 0, MYMPD_API_CACHES_CREATE, "MYMPD_API_CACHES_CREATE", "");
    request->data = sdscat(request->data, "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"MYMPD_API_CACHES_CREATE\",\"params\":{}}");
    bool rc = mpd_worker_start(mympd_state, request);
    if (rc == false) {
        mympd_state->sticker_cache_building = false;
        mympd_state->album_cache_building = false;
    }
    return rc;
}
