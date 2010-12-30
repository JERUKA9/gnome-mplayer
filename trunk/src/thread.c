/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * thread.c
 * Copyright (C) Kevin DeKorte 2006 <kdekorte@gmail.com>
 * 
 * thread.c is free software.
 * 
 * You may redistribute it and/or modify it under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 * 
 * thread.c is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with thread.c.  If not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#include "thread.h"
#include "common.h"

void mplayer_shutdown()
{
    // printf("state = %i quit = %i\n",state,QUIT);
    if (state != QUIT) {
        idledata->percent = 1.0;
        g_idle_add(set_progress_value, idledata);
        g_idle_add(set_stop, idledata);
        if (g_file_test(idledata->af_export, G_FILE_TEST_EXISTS)) {
            // stop audio export monitor
            // as part of the stopping, remove file when memmapped file is closed
            g_idle_add(unmap_af_export_file, idledata);
        }
        send_command("quit\n", FALSE);
        dbus_send_event("Ended", 0);
    } else {
        if (verbose > 1) {
            printf
                ("plugin calling mplayer shutdown when mplayer might have already been shutdown\n");
        }
        send_command("quit\n", FALSE);
    }

}

gboolean write_to_mplayer(gpointer data)
{
    gchar *cmd = (gchar *) data;
    GIOStatus result;
    gsize bytes_written;

    if (verbose > 1)
        printf("send command = %s\n", cmd);

    if (channel_in) {
        result = g_io_channel_write_chars(channel_in, cmd, -1, &bytes_written, NULL);
        if (result != G_IO_STATUS_ERROR && bytes_written > 0)
            result = g_io_channel_flush(channel_in, NULL);
    }

    g_free(cmd);
    return FALSE;
}

gboolean send_command(gchar * command, gboolean retain_pause)
{
    gchar *cmd;

    if (retain_pause) {
        if (use_pausing_keep_force) {
            cmd = g_strdup_printf("pausing_keep_force %s", command);
        } else {
            cmd = g_strdup_printf("pausing_keep %s", command);
        }
    } else {
        cmd = g_strdup(command);
    }
    // printf("command = %s\n", cmd);
    g_idle_add(write_to_mplayer, (gpointer) cmd);
    return TRUE;

}

gboolean play(void *data)
{
    PlayData *p = (PlayData *) data;

    if (ok_to_play && p != NULL) {
        if (!gtk_list_store_iter_is_valid(playliststore, &iter)) {
            // printf("iter is not valid, getting first one\n");
            gtk_tree_model_get_iter_first(GTK_TREE_MODEL(playliststore), &iter);
        }
        gtk_list_store_set(playliststore, &iter, PLAYLIST_COLUMN, p->playlist, ITEM_COLUMN, p->uri,
                           -1);
        play_iter(&iter, 0);
    }
    g_free(p);

    return FALSE;
}

gboolean thread_complete(GIOChannel * source, GIOCondition condition, gpointer data)
{
    //ThreadData *threaddata = (ThreadData *) data;

    if (verbose > 1)
        printf("thread complete\n");
    g_idle_add(set_stop, idledata);
    state = QUIT;
    lastguistate = STOPPED;
    g_idle_add(set_gui_state, NULL);
    g_source_remove(watch_in_id);
    g_source_remove(watch_err_id);
    g_cond_signal(mplayer_complete_cond);
    return FALSE;
}

gboolean thread_reader_error(GIOChannel * source, GIOCondition condition, gpointer data)
{
    GString *mplayer_output;
    GIOStatus status;
    gchar *error_msg = NULL;
    GtkWidget *dialog;
    ThreadData *threaddata = (ThreadData *) data;

    if (source == NULL) {
        g_source_remove(watch_in_id);
        g_source_remove(watch_in_hup_id);
        return FALSE;
    }

    if (data == NULL) {
        if (verbose)
            printf("shutting down threadquery since threaddata is NULL\n");
        return FALSE;
    }

    if (threaddata != NULL && threaddata->done == TRUE) {
        if (verbose)
            printf("shutting down threadquery for %s since threaddata->done is TRUE\n",
                   threaddata->filename);
        if (threaddata != NULL)
            g_free(threaddata);
        threaddata = NULL;
        return FALSE;
    }

    mplayer_output = g_string_new("");
    status = g_io_channel_read_line_string(source, mplayer_output, NULL, NULL);

    if (state == QUIT) {
        if (verbose > 1) {
            printf("Thread Error: state = QUIT, shutting down\n");
            printf("ERROR: %s", mplayer_output->str);
        }
        g_string_free(mplayer_output, TRUE);
        g_idle_add(set_stop, idledata);
        state = QUIT;
        g_source_remove(watch_in_id);
        g_source_remove(watch_in_hup_id);
        //g_mutex_unlock(thread_running);
        g_cond_signal(mplayer_complete_cond);
        return FALSE;
    }

    if (verbose == 1 && strstr(mplayer_output->str, "ANS_") == NULL)
        printf("ERROR: %s", mplayer_output->str);

    // print out everything in really verbose mode
    if (verbose > 1)
        printf("thread reader error: %s", mplayer_output->str);

    if (strstr(mplayer_output->str, "Couldn't open DVD device") != 0) {
        error_msg = g_strdup(mplayer_output->str);
    }

    if (strstr(mplayer_output->str, "signal") != NULL) {
        error_msg = g_strdup(mplayer_output->str);
    }

    if (strstr(mplayer_output->str, "Failed to open") != NULL) {
        if (strstr(mplayer_output->str, "LIRC") == NULL &&
            strstr(mplayer_output->str, "/dev/rtc") == NULL &&
            strstr(mplayer_output->str, "registry file") == NULL) {
            // error_msg = g_strdup(mplayer_output->str);
            if (strstr(mplayer_output->str, "<") == NULL && strstr(mplayer_output->str, ">") == NULL
                && idledata->streaming == FALSE) {
                error_msg =
                    g_strdup_printf(_("Failed to open %s"),
                                    mplayer_output->str + strlen("Failed to open "));
            }

            if (strstr(mplayer_output->str, "mms://") != NULL && idledata->streaming) {
                dontplaynext = TRUE;
                playback_error = ERROR_RETRY_WITH_MMSHTTP;
            }
        }
    }

    if (strstr(mplayer_output->str, "No stream found to handle url mmshttp://") != NULL) {
        dontplaynext = TRUE;
        playback_error = ERROR_RETRY_WITH_HTTP;
    }

    if (strstr(mplayer_output->str, "Server returned 404:File Not Found") != NULL
        && g_strrstr(threaddata->filename, "mmshttp://") != NULL) {
        dontplaynext = TRUE;
        playback_error = ERROR_RETRY_WITH_HTTP;
    }

    if (strstr(mplayer_output->str, "unknown ASF streaming type") != NULL
        && g_strrstr(threaddata->filename, "mmshttp://") != NULL) {
        dontplaynext = TRUE;
        playback_error = ERROR_RETRY_WITH_HTTP;
    }

    if (strstr(mplayer_output->str, "Error while parsing chunk header") != NULL) {
        dontplaynext = TRUE;
        playback_error = ERROR_RETRY_WITH_HTTP;
    }

    if (strstr(mplayer_output->str, "Couldn't open DVD device:") != NULL) {
        error_msg =
            g_strdup_printf(_("Couldn't open DVD device: %s"),
                            mplayer_output->str + strlen("Couldn't open DVD device: "));
    }

    if (strstr(mplayer_output->str, "Cannot load subtitles: ") != NULL) {
        error_msg =
            g_strdup_printf(_("Cannot load subtitles: %s"),
                            mplayer_output->str + strlen("Cannot load subtitles: "));
    }

    if (strstr(mplayer_output->str, "Failed to initiate \"video/X-ASF-PF\" RTP subsession") != NULL) {
        dontplaynext = TRUE;
        playback_error = ERROR_RETRY_WITH_PLAYLIST;
    }

    if (strstr(mplayer_output->str, "playlist support will not be used") != NULL) {
        dontplaynext = TRUE;
        playback_error = ERROR_RETRY_WITH_PLAYLIST;
    }

    if (strstr(mplayer_output->str, "MOV: missing header (moov/cmov) chunk") != NULL) {
        idledata->retry_on_full_cache = TRUE;
        g_strlcpy(idledata->progress_text, _("Delaying start until cache is full"), 1024);
        //printf("delaying start\n");
        g_idle_add(set_progress_text, idledata);
    }

    if (strstr(mplayer_output->str, "Compressed SWF format not supported") != NULL) {
        error_msg = g_strdup_printf(_("Compressed SWF format not supported"));
    }

    if (strstr(mplayer_output->str, "Error while decoding frame") != NULL) {
        //g_idle_add(set_rew, idledata);
    }

    if (strstr(mplayer_output->str, "LD_PRELOAD") != NULL) {
        // not a real error
    }

    if (error_msg != NULL) {
        dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                                        GTK_BUTTONS_CLOSE, "%s", error_msg);
        gtk_window_set_title(GTK_WINDOW(dialog), _("GNOME MPlayer Error"));
        if (control_id == 0)
            gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_free(error_msg);
    }

    g_string_free(mplayer_output, TRUE);
    return TRUE;

}

gboolean thread_reader(GIOChannel * source, GIOCondition condition, gpointer data)
{

    GString *mplayer_output;
    GIOStatus status;
    gchar *buf, *message = NULL, *icy = NULL;
    gchar *cmd;
    gint pos, i;
    gfloat vol;
    gfloat percent;
    GError *error = NULL;
    gchar *error_msg = NULL;
    GtkWidget *dialog;
    gdouble old_pos;
    LangMenu *menu;
    ThreadData *threaddata = (ThreadData *) data;
    MetaData *metadata;

    if (source == NULL) {
        g_source_remove(watch_err_id);
        g_source_remove(watch_in_hup_id);
        return FALSE;
    }

    idledata->fromdbus = FALSE;

    if (data == NULL) {
        if (verbose)
            printf("shutting down threadquery since threaddata is NULL\n");
        return FALSE;
    }

    if (threaddata != NULL && threaddata->done == TRUE) {
        if (verbose)
            printf("shutting down threadquery for %s since threaddata->done is TRUE\n",
                   threaddata->filename);
        if (threaddata != NULL)
            g_free(threaddata);
        threaddata = NULL;
        return FALSE;
    }

    if (state == QUIT) {
        if (verbose)
            printf("Thread: state = QUIT, shutting down\n");
        g_idle_add(set_stop, idledata);
        state = QUIT;
        g_source_remove(watch_err_id);
        g_source_remove(watch_in_hup_id);
        //g_mutex_unlock(thread_running);
        g_cond_signal(mplayer_complete_cond);
        return FALSE;
    }
    mplayer_output = g_string_new("");

    status = g_io_channel_read_line_string(source, mplayer_output, NULL, &error);
    if (status != G_IO_STATUS_NORMAL) {
        if (error != NULL) {
            //printf("%i: %s\n",error->code,error->message);
            if (error->code == G_CONVERT_ERROR_ILLEGAL_SEQUENCE) {
                g_error_free(error);
                error = NULL;
                g_io_channel_set_encoding(source, NULL, NULL);
            } else {
                g_string_free(mplayer_output, TRUE);
                g_idle_add(set_stop, idledata);
                state = QUIT;
                //g_mutex_unlock(thread_running);
                g_cond_signal(mplayer_complete_cond);
                g_error_free(error);
                error = NULL;
                return FALSE;
            }
        } else {
            g_string_free(mplayer_output, TRUE);
            g_idle_add(set_stop, idledata);
            state = QUIT;
            //g_mutex_unlock(thread_running);
            g_cond_signal(mplayer_complete_cond);
            error = NULL;
            return FALSE;
        }
    }

    if (strstr(mplayer_output->str, "ID_SIGNAL") != NULL) {
        //g_mutex_unlock(thread_running);
        g_cond_signal(mplayer_complete_cond);
        g_string_free(mplayer_output, TRUE);
        state = QUIT;
        return FALSE;
    }

    if ((strstr(mplayer_output->str, "A:") != NULL) || (strstr(mplayer_output->str, "V:") != NULL)) {
        g_string_free(mplayer_output, TRUE);
        return TRUE;
    }
    //printf("thread_reader state = %i : status = %i\n",state,status);
    if (verbose == 1 && strstr(mplayer_output->str, "ANS_") == NULL)
        printf("%s", mplayer_output->str);

    // print out everything in really verbose mode
    if (verbose > 1)
        printf("thread reader: %s", mplayer_output->str);

    if ((strstr(mplayer_output->str, "(Quit)") != NULL)
        || (strstr(mplayer_output->str, "(End of file)") != NULL)) {
        state = QUIT;
        g_idle_add(set_stop, idledata);
        g_string_free(mplayer_output, TRUE);
        //g_mutex_unlock(thread_running);
        g_cond_signal(mplayer_complete_cond);

        if (idledata->position < 0.05 && g_strrstr(threaddata->filename, "http://") != NULL
            && g_strrstr(threaddata->filename, "mmshttp://") == NULL) {
            dontplaynext = TRUE;
            playback_error = ERROR_RETRY_WITH_PLAYLIST;
        }

        return FALSE;
    }

    if (strstr(mplayer_output->str, "AO:") != NULL) {
        idledata->audiopresent = TRUE;
        send_command("get_property switch_audio\n", TRUE);
    }

    if (strstr(mplayer_output->str, "VO:") != NULL) {
        buf = strstr(mplayer_output->str, "VO:");
        sscanf(buf, "VO: [%9[^]]] %ix%i => %ix%i", vm, &actual_x, &actual_y, &play_x, &play_y);

        if (play_x >= actual_x || play_y >= actual_y) {
            if (actual_x == non_fs_width && actual_y == non_fs_height) {
                non_fs_height = 0;
                non_fs_width = 0;
            }
            actual_x = play_x;
            actual_y = play_y;
        }
        if (verbose)
            printf("Resizing to %i x %i\n", actual_x, actual_y);
        idledata->width = actual_x;
        idledata->height = actual_y;
        if (idledata->original_w == -1 && idledata->original_h == -1) {
            idledata->original_w = idledata->width;
            idledata->original_h = idledata->height;
        }
        idledata->videopresent = TRUE;
        g_idle_add(resize_window, idledata);
        g_idle_add(set_subtitle_visibility, idledata);
        videopresent = 1;
        g_idle_add(set_volume_from_slider, NULL);
        if (idledata->length < 1.0)
            send_command("get_time_length\n", TRUE);
        send_command("get_property chapters\n", TRUE);
        // send_command("get_property switch_audio\n", TRUE);
        send_command("pausing_keep_force get_property path\n", FALSE);
        if (sub_source_file) {
            if (verbose)
                printf("setting subtitle source to file\n");
            send_command("sub_file 0\n", TRUE);
        } else {
            send_command("get_property sub_demux\n", TRUE);
        }

        if (embed_window == 0)
            idledata->cachepercent = 0.0;

        if (vo != NULL && g_ascii_strncasecmp(vo, "gl2", strlen("gl2")) != 0) {
            cmd = g_strdup_printf("brightness %i\n", idledata->brightness);
            send_command(cmd, TRUE);
            g_free(cmd);

            cmd = g_strdup_printf("contrast %i\n", idledata->contrast);
            send_command(cmd, TRUE);
            g_free(cmd);

            cmd = g_strdup_printf("gamma %i\n", idledata->gamma);
            send_command(cmd, TRUE);
            g_free(cmd);

            cmd = g_strdup_printf("hue %i\n", idledata->hue);
            send_command(cmd, TRUE);
            g_free(cmd);

            cmd = g_strdup_printf("saturation %i\n", idledata->saturation);
            send_command(cmd, TRUE);
            g_free(cmd);
        }
    }

    if (strstr(mplayer_output->str, "Video: no video") != NULL) {
        actual_x = 150;
        actual_y = 1;

        // printf("Resizing to %i x %i \n", actual_x, actual_y);
        idledata->width = actual_x;
        idledata->height = actual_y;
        idledata->videopresent = FALSE;
        g_idle_add(resize_window, idledata);
        g_idle_add(set_volume_from_slider, NULL);
        //send_command("get_property switch_audio\n", TRUE);
        if (idledata->length < 1.0)
            send_command("get_time_length\n", TRUE);
        send_command("pausing_keep_force get_property path\n", FALSE);
    }

    if (strstr(mplayer_output->str, "ANS_PERCENT_POSITION") != 0) {
        buf = strstr(mplayer_output->str, "ANS_PERCENT_POSITION");
        sscanf(buf, "ANS_PERCENT_POSITION=%i", &pos);
        //printf("Position = %i\n",pos);
        idledata->percent = pos / 100.0;
        g_idle_add(set_progress_value, idledata);
    }

    if (strstr(mplayer_output->str, "ANS_LENGTH") != 0) {
        buf = strstr(mplayer_output->str, "ANS_LENGTH");
        sscanf(buf, "ANS_LENGTH=%lf", &idledata->length);
        g_idle_add(set_progress_time, idledata);
    }

    if (strstr(mplayer_output->str, "ID_START_TIME") != 0) {
        buf = strstr(mplayer_output->str, "ID_START_TIME");
        sscanf(buf, "ID_START_TIME=%lf", &idledata->start_time);
        g_idle_add(set_progress_time, idledata);
    }

    if (strstr(mplayer_output->str, "ID_LENGTH") != 0) {
        buf = strstr(mplayer_output->str, "ID_LENGTH");
        sscanf(buf, "ID_LENGTH=%lf", &idledata->length);
        if (idledata->streaming)
            idledata->cachepercent = 0.0;
        g_idle_add(set_progress_time, idledata);
    }

    if (strstr(mplayer_output->str, "ANS_TIME_POSITION") != 0) {
        old_pos = idledata->position;
        buf = strstr(mplayer_output->str, "ANS_TIME_POSITION");
        sscanf(buf, "ANS_TIME_POSITION=%lf", &idledata->position);
        idledata->position -= idledata->start_time;
        if (idledata->position < old_pos) {
            send_command("get_time_length\n", FALSE);
            state = PLAYING;
        }
        g_idle_add(set_progress_time, idledata);
        if ((int) idledata->length != 0) {
            idledata->percent = idledata->position / idledata->length;
            g_idle_add(set_progress_value, idledata);
        } else {
            send_command("get_percent_pos\n", FALSE);
        }
    }

    if (strstr(mplayer_output->str, "ANS_stream_pos") != 0) {
        buf = strstr(mplayer_output->str, "ANS_stream_pos");
        sscanf(buf, "ANS_stream_pos=%li", &idledata->byte_pos);
        g_idle_add(set_progress_time, idledata);
        lastguistate = STOPPED;
        guistate = PLAYING;
        g_idle_add(set_gui_state, NULL);
    }

    if (strstr(mplayer_output->str, "ANS_volume") != 0) {
        buf = strstr(mplayer_output->str, "ANS_volume");
        sscanf(buf, "ANS_volume=%f", &vol);
        // Need to track what the master volume is, gui is updated in make mouse invisible
        idledata->mplayer_volume = vol;
        idledata->mute = !((gint) vol > 0);
        g_idle_add(update_volume, idledata);
    }

    if (strstr(mplayer_output->str, "ANS_chapters") != 0) {
        buf = strstr(mplayer_output->str, "ANS_chapters");
        sscanf(buf, "ANS_chapters=%i", &idledata->chapters);
        if (idledata->chapters > 0)
            idledata->has_chapters = TRUE;
        g_idle_add(set_update_gui, NULL);
    }

    if (strstr(mplayer_output->str, "ANS_sub_visibility") != NULL) {
        if (strstr(mplayer_output->str, "ANS_sub_visibility=yes")) {
            idledata->sub_visible = TRUE;
        } else {
            idledata->sub_visible = FALSE;
        }
        g_idle_add(set_update_gui, NULL);
    }

    if (strstr(mplayer_output->str, "ANS_sub_demux") != 0) {
        buf = strstr(mplayer_output->str, "ANS_sub_demux");
        sscanf(buf, "ANS_sub_demux=%i", &idledata->sub_demux);
        g_idle_add(set_update_gui, NULL);
    }

    if (strstr(mplayer_output->str, "ANS_switch_audio") != 0) {
        //printf("%s\n", mplayer_output->str);
        buf = strstr(mplayer_output->str, "ANS_switch_audio");
        sscanf(buf, "ANS_switch_audio=%i", &idledata->switch_audio);
        idledata->switch_audio--;
        g_idle_add(set_update_gui, NULL);
    }
/*
    if (strstr(mplayer_output->str, "ID_AUDIO_ID") != 0) {
        buf = strstr(mplayer_output->str, "ID_AUDIO_ID");
        sscanf(buf, "ID_AUDIO_ID=%i", &idledata->switch_audio);
        g_idle_add(set_update_gui, NULL);
    }
*/

    if (strstr(mplayer_output->str, "ANS_brightness") != 0) {
        buf = strstr(mplayer_output->str, "ANS_brightness");
        sscanf(buf, "ANS_brightness=%i", &idledata->brightness);
    }

    if (strstr(mplayer_output->str, "ANS_contrast") != 0) {
        buf = strstr(mplayer_output->str, "ANS_contrast");
        sscanf(buf, "ANS_contrast=%i", &idledata->contrast);
    }

    if (strstr(mplayer_output->str, "ANS_gamma") != 0) {
        buf = strstr(mplayer_output->str, "ANS_gamma");
        sscanf(buf, "ANS_gamma=%i", &idledata->gamma);
    }

    if (strstr(mplayer_output->str, "ANS_hue") != 0) {
        buf = strstr(mplayer_output->str, "ANS_hue");
        sscanf(buf, "ANS_hue=%i", &idledata->hue);
    }

    if (strstr(mplayer_output->str, "ANS_saturation") != 0) {
        buf = strstr(mplayer_output->str, "ANS_saturation");
        sscanf(buf, "ANS_saturation=%i", &idledata->saturation);
    }

    if (strstr(mplayer_output->str, "ANS_path") != 0) {
        if (verbose > 1)
            printf("pausing keep force enabled\n");
        use_pausing_keep_force = TRUE;
    }

    if (strstr(mplayer_output->str, "Cache fill") != 0) {
        buf = strstr(mplayer_output->str, "Cache fill");
        sscanf(buf, "Cache fill: %f%%", &percent);
        buf = g_strdup_printf(_("Cache fill: %2.2f%%"), percent);
        g_strlcpy(idledata->progress_text, buf, 1024);
        g_free(buf);
        g_idle_add(set_progress_text, idledata);
        idledata->cachepercent = percent / 100.0;
        g_idle_add(set_progress_value, idledata);
    }

    if (strstr(mplayer_output->str, "Connecting") != 0) {
        buf = g_strdup_printf(_("Connecting"));
        g_strlcpy(idledata->progress_text, buf, 1024);
        g_free(buf);
        g_idle_add(set_progress_text, idledata);
        idledata->percent = 0.0;
        g_idle_add(set_progress_value, idledata);
    }

    if (strstr(mplayer_output->str, "ID_VIDEO_FORMAT") != 0) {
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "ID_VIDEO_FORMAT") + strlen("ID_VIDEO_FORMAT=");
        g_strlcpy(idledata->video_format, buf, 64);
    }

    if (strstr(mplayer_output->str, "ID_VIDEO_CODEC") != 0) {
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "ID_VIDEO_CODEC") + strlen("ID_VIDEO_CODEC=");
        g_strlcpy(idledata->video_codec, buf, 16);
    }

    if (strstr(mplayer_output->str, "ID_VIDEO_FPS") != 0) {
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "ID_VIDEO_FPS") + strlen("ID_VIDEO_FPS=");
        g_strlcpy(idledata->video_fps, buf, 16);
    }

    if (strstr(mplayer_output->str, "ID_VIDEO_BITRATE") != 0) {
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "ID_VIDEO_BITRATE") + strlen("ID_VIDEO_BITRATE=");
        g_strlcpy(idledata->video_bitrate, buf, 16);
    }

    if (strstr(mplayer_output->str, "ID_AUDIO_CODEC") != 0) {
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "ID_AUDIO_CODEC") + strlen("ID_AUDIO_CODEC=");
        g_strlcpy(idledata->audio_codec, buf, 16);
    }

    if (strstr(mplayer_output->str, "ID_AUDIO_BITRATE") != 0) {
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "ID_AUDIO_BITRATE") + strlen("ID_AUDIO_BITRATE=");
        g_strlcpy(idledata->audio_bitrate, buf, 16);
    }

    if (strstr(mplayer_output->str, "ID_AUDIO_RATE") != 0) {
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "ID_AUDIO_RATE") + strlen("ID_AUDIO_RATE=");
        g_strlcpy(idledata->audio_samplerate, buf, 16);
    }

    if (strstr(mplayer_output->str, "ID_AUDIO_NCH") != 0) {
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "ID_AUDIO_NCH") + strlen("ID_AUDIO_NCH=");
        g_strlcpy(idledata->audio_channels, buf, 16);
    }

    if (strstr(mplayer_output->str, "ID_SID_") != 0) {
        menu = g_new0(LangMenu, 1);
        buf = strstr(mplayer_output->str, "ID_SID_");
        sscanf(buf, "ID_SID_%i_", &menu->value);
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "_LANG=");
        if (buf != NULL) {
            buf += strlen("_LANG=");
            menu->label = g_strdup(buf);
            g_idle_add(set_new_lang_menu, menu);
        }
    }

    if (strstr(mplayer_output->str, "ID_SUBTITLE_ID=") != 0) {
        menu = g_new0(LangMenu, 1);
        buf = strstr(mplayer_output->str, "ID_SUBTITLE_ID");
        sscanf(buf, "ID_SUBTITLE_ID=%i", &menu->value);
        menu->label = g_strdup_printf("%i", menu->value);
        g_idle_add(set_new_lang_menu, menu);
    }

    if (strstr(mplayer_output->str, "ID_FILE_SUB_ID=") != 0) {
        menu = g_new0(LangMenu, 1);
        buf = strstr(mplayer_output->str, "ID_FILE_SUB_ID");
        sscanf(buf, "ID_FILE_SUB_ID=%i", &menu->value);
        menu->label = g_strdup_printf("FILE %i", menu->value);
        menu->value += 9000;
        g_idle_add(set_new_lang_menu, menu);
        sub_source_file = TRUE;
    }

    if (strstr(mplayer_output->str, "ID_AID_") != 0) {
        menu = g_new0(LangMenu, 1);
        buf = strstr(mplayer_output->str, "ID_AID_");
        sscanf(buf, "ID_AID_%i_", &menu->value);
        g_string_truncate(mplayer_output, mplayer_output->len - 1);
        buf = strstr(mplayer_output->str, "_LANG=");
        if (buf != NULL) {
            buf += strlen("_LANG=");
            menu->label = g_strdup(buf);
            g_idle_add(set_new_audio_menu, menu);
            if (alang != NULL && g_strrstr(alang, menu->label) != NULL) {
                buf = g_strdup_printf("switch_audio %i\n", menu->value);
                send_command(buf, TRUE);
                g_free(buf);
            }
        }
        buf = strstr(mplayer_output->str, "_NAME=");
        if (buf != NULL) {
            buf += strlen("_NAME=");
            menu->label = g_strdup(buf);
            if (menu->label != NULL && strlen(menu->label) > 0) {
                g_idle_add(set_new_audio_menu, menu);
                if (alang != NULL && g_strrstr(alang, menu->label) != NULL) {
                    buf = g_strdup_printf("switch_audio %i\n", menu->value);
                    send_command(buf, TRUE);
                    g_free(buf);
                }
            }
        }
    }

    if (strstr(mplayer_output->str, "ID_AUDIO_ID=") != 0) {
        menu = g_new0(LangMenu, 1);
        buf = strstr(mplayer_output->str, "ID_AUDIO_ID");
        sscanf(buf, "ID_AUDIO_ID=%i", &menu->value);
        menu->label = g_strdup_printf("%i", menu->value);
        g_idle_add(set_new_audio_menu, menu);
    }

    if (strstr(mplayer_output->str, "File not found") != 0) {
    }

    if ((strstr(mplayer_output->str, "CHAPTERS") != 0)
        && !(strstr(mplayer_output->str, "ID_CHAPTERS=0") != 0)) {
        idledata->has_chapters = TRUE;
        g_idle_add(set_update_gui, NULL);
    }

    if (strstr(mplayer_output->str, "ID_SEEKABLE=") != 0) {
        buf = strstr(mplayer_output->str, "ID_SEEKABLE");
        sscanf(buf, "ID_SEEKABLE=%i", &idledata->seekable);
        g_idle_add(set_show_seek_buttons, idledata);
    }

    if (strstr(mplayer_output->str, "DVDNAV_TITLE_IS_MENU") != 0) {
        dvdnav_title_is_menu = TRUE;
    }

    if (strstr(mplayer_output->str, "DVDNAV_TITLE_IS_MOVIE") != 0) {
        dvdnav_title_is_menu = FALSE;
    }

    if (strstr(mplayer_output->str, "Couldn't open DVD device") != 0) {
        error_msg = g_strdup(mplayer_output->str);
    }

    if (strstr(mplayer_output->str, "*** screenshot") != 0) {
        buf = strstr(mplayer_output->str, "'") + 1;
        buf[12] = '\0';
        message = g_strdup_printf(_("Screenshot saved to '%s'"), buf);
        dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO,
                                        GTK_BUTTONS_OK, "%s", message);
        gtk_window_set_title(GTK_WINDOW(dialog), _("GNOME MPlayer Notification"));
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_free(message);
        message = NULL;
    }

    if (strstr(mplayer_output->str, "ICY Info") != NULL) {
        buf = strstr(mplayer_output->str, "'");
        if (message) {
            g_free(message);
            message = NULL;
        }
        if (buf != NULL) {
            for (i = 1; i < (int) strlen(buf) - 1; i++) {
                if (!strncmp(&buf[i], "\';", 2)) {
                    buf[i] = '\0';
                    break;
                }
            }
            if (g_utf8_validate(buf + 1, strlen(buf + 1), 0))
                message =
                    g_markup_printf_escaped("<small>\n\t<big><b>%s</b></big>\n</small>", buf + 1);
        }
        if (message) {
            // reset max values in audio meter
            for (i = 0; i < METER_BARS; i++) {
                max_buckets[i] = 0;
            }
            g_strlcpy(idledata->media_info, message, 1024);
            g_free(message);
            message = g_markup_printf_escaped("\n\t<b>%s</b>\n", buf + 1);
            icy = g_strdup(buf + 1);
            if ((buf = strstr(icy, " - ")) != NULL) {
                metadata = (MetaData *) g_new0(MetaData, 1);
                metadata->title = g_strdup(buf + 3);
                buf[0] = '\0';
                metadata->artist = g_strdup(icy);
                g_thread_create(get_cover_art, metadata, FALSE, NULL);
            }
            g_free(icy);
            g_strlcpy(idledata->media_notification, message, 1024);
            g_free(message);
            message = NULL;
        }


        g_idle_add(set_media_label, idledata);
    }
    //if (verbose > 1) {
    //    printf("MPLAYER OUTPUT: %s\n", mplayer_output->str);
    //}

    if (error_msg != NULL) {
        dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                                        GTK_BUTTONS_OK, "%s", error_msg);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_free(error_msg);
    }

    g_string_free(mplayer_output, TRUE);

    return TRUE;

}

gboolean thread_query(gpointer data)
{
    int size;
    ThreadData *threaddata = (ThreadData *) data;

    //printf("thread_query state = %i\n",state);

    // this function wakes up every 1/2 second and so 
    // this is where we should put in some code to detect if the media is getting 
    // to close to the amount cached and if so pause until we have a 5% gap or something

    // idledata->percent > (idledata->cachepercent - 0.05)
    //              autopause
    // else
    // 
    if (data == NULL) {
        if (verbose)
            printf("shutting down threadquery since threaddata is NULL\n");
        return FALSE;
    }

    if (threaddata != NULL && threaddata->done == TRUE) {
        if (verbose)
            printf("shutting down threadquery for %s since threaddata->done is TRUE\n",
                   threaddata->filename);
        g_free(threaddata);
        threaddata = NULL;
        return FALSE;
    }

    if (idledata->mapped_af_export == NULL && g_file_test(idledata->af_export, G_FILE_TEST_EXISTS)) {
        // start audio export monitor
        // but don't start polling until meter is visible
        g_idle_add(map_af_export_file, idledata);
    }

    if (state == PLAYING) {
        // size = write(std_in, "get_percent_pos\n", strlen("get_percent_pos\n"));
        size = write(std_in, "get_time_pos\n", strlen("get_time_pos\n"));
        if (size == -1) {
            g_idle_add(set_kill_mplayer, NULL);
            return FALSE;
        } else {

            //send_command("get_time_pos\n");
            send_command("get_time_length\n", TRUE);
            send_command("get_property stream_pos\n", TRUE);
            send_command("get_property volume\n", TRUE);
            if (threaddata->streaming)
                send_command("get_property metadata\n", TRUE);
            g_idle_add(make_panel_and_mouse_invisible, NULL);
            return TRUE;
        }
    } else {
        if (state == QUIT) {
            return FALSE;
        } else {
            return TRUE;
        }
    }
}

gpointer launch_player(gpointer data)
{

    gint ok;
    gint player_window;
    char *argv[255];
    gint arg = 0;
    gchar *filename;
    gint count;
    PlayData *p = NULL;
    gchar *fontname;
    gchar *size;
    gchar *buffer;
    GError *error;
    // GIOFlags flags;
    GPid pid;
#ifdef GIO_ENABLED
    GFile *file;
#endif

    ThreadData *threaddata = (ThreadData *) data;

    videopresent = 0;
    playback_error = NO_ERROR;

    //while (embed_window != -1 && !GTK_WIDGET_VISIBLE(window)) {
    //    if (verbose)
    //        printf("waiting for gui\n");
    //}

    g_mutex_lock(thread_running);
#ifdef GIO_ENABLED
    buffer = g_uri_unescape_string(threaddata->uri, NULL);
#else
    buffer = g_strdup(threaddata->uri);
#endif

    g_strlcpy(idledata->info, buffer, 1024);
    g_free(buffer);
    idledata->percent = 0.0;
    g_strlcpy(idledata->progress_text, "", 1024);
    idledata->volume = volume;
    idledata->length = 0.0;
    sub_source_file = FALSE;

    g_idle_add(set_progress_value, idledata);
    g_idle_add(set_progress_text, idledata);
    g_idle_add(set_media_info, idledata);
    g_idle_add(set_window_visible, idledata);
    g_idle_add(resize_window, idledata);

    if (mplayer_bin == NULL || !g_file_test(mplayer_bin, G_FILE_TEST_EXISTS)) {
        argv[arg++] = g_strdup_printf("mplayer");
    } else {
        argv[arg++] = g_strdup_printf("%s", mplayer_bin);
    }

    // argv[arg++] = g_strdup_printf("-v");

    if ((vo != NULL && strlen(vo) > 0) || (ao != NULL && strlen(ao) > 0)) {
        argv[arg++] = g_strdup_printf("-profile");
        argv[arg++] = g_strdup_printf("gnome-mplayer");
    }

    if (vo != NULL && g_ascii_strncasecmp(vo, "xvmc", strlen("xvmc")) == 0) {
        if (g_ascii_strncasecmp(threaddata->filename, "dvd://", strlen("dvd://")) == 0
            || g_ascii_strncasecmp(threaddata->filename, "dvdnav://", strlen("dvdnav://")) == 0) {
            argv[arg++] = g_strdup_printf("-vc");
            argv[arg++] = g_strdup_printf("ffmpeg12mc");
        } else {
            argv[arg++] = g_strdup_printf("-vo");
            argv[arg++] = g_strdup_printf("xvmc,");
            if (!disable_deinterlace) {
                argv[arg++] = g_strdup_printf("-vf-pre");
                argv[arg++] = g_strdup_printf("yadif,softskip,scale");
            }
        }
    }

    if (vo != NULL && g_ascii_strncasecmp(vo, "vdpau", strlen("vdpau")) == 0) {
        //printf("video_codec = '%s'\n",idledata->video_codec);

        argv[arg++] = g_strdup_printf("-vo");
        if (!disable_deinterlace) {
            argv[arg++] = g_strdup_printf("vdpau:deint=2,");
        } else {
            argv[arg++] = g_strdup_printf("%s,vdpau,", vo);
        }

        argv[arg++] = g_strdup_printf("-vc");
        argv[arg++] = g_strdup_printf("ffmpeg12vdpau,ffh264vdpau,ffwmv3vdpau,ffvc1vdpau,");
    }

    if (use_hw_audio) {
        argv[arg++] = g_strdup_printf("-afm");
        argv[arg++] = g_strdup_printf("hwac3,");
    }

    if (verbose < 2)
        argv[arg++] = g_strdup_printf("-quiet");
    argv[arg++] = g_strdup_printf("-slave");
    argv[arg++] = g_strdup_printf("-identify");

    // this argument seems to cause noise in some videos
    if (softvol) {
        if (volume_gain != 0) {
            argv[arg++] = g_strdup_printf("-af-add");
            argv[arg++] = g_strdup_printf("volume=%i", volume_gain);
        } else {
            argv[arg++] = g_strdup_printf("-softvol");
        }
    }

    if (use_volume_option) {
        argv[arg++] = g_strdup_printf("-volume");
        //if (idledata->mute) {
        //    argv[arg++] = g_strdup_printf("0");
        //} else {
        argv[arg++] = g_strdup_printf("%i", (gint) idledata->volume);
        idledata->mplayer_volume = idledata->volume;
        //}
    }

    if (mixer != NULL && strlen(mixer) > 0) {
        if (ao == NULL || (ao != NULL && g_ascii_strncasecmp(ao, "alsa", 4) == 0)) {
            argv[arg++] = g_strdup_printf("-mixer-channel");
            argv[arg++] = g_strdup_printf("%s", mixer);
        }
    }

    if (!disable_framedrop)
        argv[arg++] = g_strdup_printf("-framedrop");

    if (vo == NULL
        || !(g_ascii_strncasecmp(vo, "xvmc", strlen("xvmc")) == 0
             || g_ascii_strncasecmp(vo, "vaapi", strlen("vaapi")) == 0
             || g_ascii_strncasecmp(vo, "vdpau", strlen("vdpau")) == 0)) {
        if (!disable_deinterlace) {
            argv[arg++] = g_strdup_printf("-vf-pre");
            argv[arg++] = g_strdup_printf("yadif,softskip,scale");
        }
    }

    argv[arg++] = g_strdup_printf("-noconsolecontrols");
    argv[arg++] = g_strdup_printf("-noidle");

#if !(SM_INHIBIT || SS_INHIBIT)
//    if (idledata->width > 0 && idledata->height > 0)
//        argv[arg++] = g_strdup_printf("-stop-xscreensaver");
#endif

    argv[arg++] = g_strdup_printf("-osdlevel");
    argv[arg++] = g_strdup_printf("%i", osdlevel);
    if (strcmp(threaddata->filename, "dvdnav://") == 0) {
        argv[arg++] = g_strdup_printf("-mouse-movements");
        argv[arg++] = g_strdup_printf("-nocache");
    } else {
        // argv[arg++] = g_strdup_printf("-nograbpointer");
        if (g_ascii_strncasecmp(threaddata->filename, "dvd://", strlen("dvd://")) == 0) {
            // argv[arg++] = g_strdup_printf("-nocache");
        } else {
            argv[arg++] = g_strdup_printf("-nomouseinput");
            if (threaddata->streaming || forcecache == TRUE || threaddata->playlist) {
                argv[arg++] = g_strdup_printf("-cache");
                argv[arg++] = g_strdup_printf("%i", cache_size);
            } else {
#ifdef GIO_ENABLED
                file = g_file_new_for_uri(threaddata->uri);
                if (file != NULL) {
                    if (g_file_is_native(file)) {
                        argv[arg++] = g_strdup_printf("-nocache");
                    } else {
                        forcecache = TRUE;
                        argv[arg++] = g_strdup_printf("-cache");
                        argv[arg++] = g_strdup_printf("%i", cache_size);
                    }
                    g_object_unref(file);
                }
#else
                argv[arg++] = g_strdup_printf("-nocache");
#endif
            }
        }
    }
    // argv[arg++] = g_strdup_printf("-v");
    argv[arg++] = g_strdup_printf("-wid");
    player_window = idledata->windowid;
    argv[arg++] = g_strdup_printf("0x%x", player_window);
#ifdef ENABLE_PANSCAN
    argv[arg++] = g_strdup_printf("-fs");
#endif
    if (threaddata->restart_second > 0) {
        argv[arg++] = g_strdup_printf("-ss");
        argv[arg++] = g_strdup_printf("%i", threaddata->restart_second);
    } else {
        argv[arg++] = g_strdup_printf("-ss");
        argv[arg++] = g_strdup_printf("%i", threaddata->start_second);
    }

    if (threaddata->play_length > 0) {
        argv[arg++] = g_strdup_printf("-endpos");
        argv[arg++] = g_strdup_printf("%i", threaddata->play_length);
    }

    if (control_id == 0) {
//        if (!idledata->streaming)
//            argv[arg++] = g_strdup_printf("-idx");
    } else {
        argv[arg++] = g_strdup_printf("-cookies");
    }

    if (tv_device != NULL) {
        argv[arg++] = g_strdup_printf("-tv:device");
        argv[arg++] = g_strdup_printf("%s", tv_device);
    }
    if (tv_driver != NULL) {
        argv[arg++] = g_strdup_printf("-tv:driver");
        argv[arg++] = g_strdup_printf("%s", tv_driver);
    }
    if (tv_input != NULL) {
        argv[arg++] = g_strdup_printf("-tv:input");
        argv[arg++] = g_strdup_printf("%s", tv_input);
    }
    if (tv_width > 0) {
        argv[arg++] = g_strdup_printf("-tv:width");
        argv[arg++] = g_strdup_printf("%i", tv_width);
    }
    if (tv_height > 0) {
        argv[arg++] = g_strdup_printf("-tv:height");
        argv[arg++] = g_strdup_printf("%i", tv_height);
    }
    if (tv_fps > 0) {
        argv[arg++] = g_strdup_printf("-tv:fps");
        argv[arg++] = g_strdup_printf("%i", tv_fps);
    }

    if (threaddata->audiofile != NULL && strlen(threaddata->audiofile) > 0) {
        argv[arg++] = g_strdup_printf("-audiofile");
        argv[arg++] = g_strdup_printf("%s", threaddata->audiofile);
    }

    if (threaddata->subtitle != NULL && strlen(threaddata->subtitle) > 0) {
        argv[arg++] = g_strdup_printf("-sub");
        argv[arg++] = g_strdup_printf("%s", threaddata->subtitle);
    }
    // subtitle stuff
    if (!disable_ass) {
        argv[arg++] = g_strdup_printf("-ass");

        if (subtitle_margin > 0) {
            argv[arg++] = g_strdup_printf("-ass-bottom-margin");
            argv[arg++] = g_strdup_printf("%i", subtitle_margin);
            argv[arg++] = g_strdup_printf("-ass-use-margins");
        }
        // Simply ommiting '-embeddedfonts' did not work
        // printf("demuxer = %s\n",idledata->demuxer);
        // if (!disable_embeddedfonts) {
        if (!disable_embeddedfonts
            && (g_strrstr(idledata->demuxer, "mkv") || g_strrstr(idledata->demuxer, "lavfpref"))) {
            argv[arg++] = g_strdup_printf("-embeddedfonts");
        } else {
            argv[arg++] = g_strdup_printf("-noembeddedfonts");

            if (subtitlefont != NULL && strlen(subtitlefont) > 0) {
                fontname = g_strdup(subtitlefont);
                size = g_strrstr(fontname, " ");
                size[0] = '\0';
                size = g_strrstr(fontname, " Bold");
                if (size)
                    size[0] = '\0';
                size = g_strrstr(fontname, " Italic");
                if (size)
                    size[0] = '\0';
                argv[arg++] = g_strdup_printf("-ass-force-style");
                argv[arg++] = g_strconcat("FontName=", fontname,
                                          ((g_strrstr(subtitlefont, "Italic") !=
                                            NULL) ? ",Italic=1" : ",Italic=0"),
                                          ((g_strrstr(subtitlefont, "Bold") !=
                                            NULL) ? ",Bold=1" : ",Bold=0"),
                                          (subtitle_outline ? ",Outline=1" : ",Outline=0"),
                                          (subtitle_shadow ? ",Shadow=2" : ",Shadow=0"), NULL);
                g_free(fontname);
            }
        }

        argv[arg++] = g_strdup_printf("-ass-font-scale");
        argv[arg++] = g_strdup_printf("%1.2f", subtitle_scale);

        if (subtitle_color != NULL && strlen(subtitle_color) > 0) {
            argv[arg++] = g_strdup_printf("-ass-color");
            argv[arg++] = g_strdup_printf("%s", subtitle_color);
        }

    } else {
        argv[arg++] = g_strdup_printf("-subfont-text-scale");
        argv[arg++] = g_strdup_printf("%d", (int) (subtitle_scale * 5));        // 5 is the default

        if (subtitlefont != NULL && strlen(subtitlefont) > 0) {
            fontname = g_strdup(subtitlefont);
            size = g_strrstr(fontname, " ");
            size[0] = '\0';
            argv[arg++] = g_strdup_printf("-subfont");
            argv[arg++] = g_strdup_printf("%s", fontname);
            g_free(fontname);
        }


    }

    if (subtitle_codepage != NULL && strlen(subtitle_codepage) > 0) {
        argv[arg++] = g_strdup_printf("-subcp");
        argv[arg++] = g_strdup_printf("%s", subtitle_codepage);
    }

    argv[arg++] = g_strdup_printf("-channels");
    switch (audio_channels) {
    case 1:
        argv[arg++] = g_strdup_printf("4");
        break;
    case 2:
        argv[arg++] = g_strdup_printf("6");
        break;
    default:
        argv[arg++] = g_strdup_printf("2");
        break;
    }

    if (vo == NULL || !(g_ascii_strncasecmp(vo, "xvmc", strlen("xvmc")) == 0
                        || g_ascii_strncasecmp(vo, "vaapi", strlen("vvapi")) == 0
                        || g_ascii_strncasecmp(vo, "vdpau", strlen("vdpau")) == 0)) {

        if (pplevel > 0) {
            argv[arg++] = g_strdup_printf("-vf-add");
            argv[arg++] = g_strdup_printf("pp=ac/tn:a");
            argv[arg++] = g_strdup_printf("-autoq");
            argv[arg++] = g_strdup_printf("%d", pplevel);
        }
    }

    if (extraopts != NULL) {
        char **opts = g_strsplit(extraopts, " ", -1);
        int i;
        for (i = 0; opts[i] != NULL; i++)
            argv[arg++] = g_strdup(opts[i]);
        g_strfreev(opts);
    }

    if (vo == NULL
        || !(g_ascii_strncasecmp(vo, "xvmc", strlen("xvmc")) == 0
             || g_ascii_strncasecmp(vo, "vaapi", strlen("vvapi")) == 0
             || g_ascii_strncasecmp(vo, "vdpau", strlen("vdpau")) == 0)) {
        argv[arg++] = g_strdup_printf("-vf-add");
        argv[arg++] = g_strdup_printf("screenshot");
    }

    if (idledata->device != NULL) {
        argv[arg++] = g_strdup_printf("-dvd-device");
        argv[arg++] = g_strdup_printf("%s", idledata->device);
    } else {
        if (mplayer_dvd_device != NULL) {
            argv[arg++] = g_strdup_printf("-dvd-device");
            argv[arg++] = g_strdup_printf("%s", mplayer_dvd_device);
        }
    }

    /* 
       This is here in order to be able to switch the
       audio track in OGM files while playing, see
       http://lists.mplayerhq.hu/pipermail/mplayer-users/2007-February/065316.html 
     */
    filename = g_utf8_strdown(threaddata->filename, -1);
    if (strstr(filename, ".ogm") || strstr(filename, ".ogv")) {
        argv[arg++] = g_strdup_printf("-demuxer");
        argv[arg++] = g_strdup_printf("lavf");
    }
    g_free(filename);


/*	This is needed when lavc is the default decoder, but mkv is the default as of 9/9/08
	filename = g_utf8_strdown(threaddata->filename,-1);
	if (strstr(filename,".mkv")) {
        argv[arg++] = g_strdup_printf("-demuxer");
        argv[arg++] = g_strdup_printf("mkv");
	}		
	g_free(filename);
*/

    if (!use_hw_audio) {
        argv[arg++] = g_strdup_printf("-af-add");
        argv[arg++] = g_strdup_printf("export=%s:512", idledata->af_export);
    }

    if (g_strrstr(threaddata->filename, "apple.com")) {
        argv[arg++] = g_strdup_printf("-user-agent");
        argv[arg++] = g_strdup_printf("QuickTime/7.6.4");
    }

    /* disable msg stuff to make sure extra console characters don't mess around */
    argv[arg++] = g_strdup_printf("-nomsgcolor");
    argv[arg++] = g_strdup_printf("-nomsgmodule");


    if (threaddata->playlist) {
        argv[arg++] = g_strdup_printf("-cache");
        argv[arg++] = g_strdup_printf("%i", cache_size);
        argv[arg++] = g_strdup_printf("-playlist");
    }

    argv[arg] = g_strdup_printf("%s", threaddata->filename);
    argv[arg + 1] = NULL;

    if (verbose) {
        arg = 0;
        while (argv[arg] != NULL) {
            printf("%s ", argv[arg++]);
        }
        printf("\n");
    }

    state = PAUSED;
    error = NULL;
    ok = g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_SEARCH_PATH,
                                  NULL, NULL, &pid, &std_in, &std_out, &std_err, &error);

    if (error != NULL) {
        printf("error code = %i - %s\n", error->code, error->message);
        g_error_free(error);
        error = NULL;
    }

    arg = 0;
    while (argv[arg] != NULL) {
        g_free(argv[arg]);
        argv[arg] = NULL;
        arg++;
    }

    if (ok) {
        if (verbose)
            printf("Spawn succeeded for filename %s\n", threaddata->filename);
        state = PLAYING;

        if (channel_in != NULL) {
            g_io_channel_unref(channel_in);
            channel_in = NULL;
        }

        if (channel_out != NULL) {
            g_io_channel_unref(channel_out);
            channel_out = NULL;
        }

        if (channel_err != NULL) {
            g_io_channel_unref(channel_err);
            channel_err = NULL;
        }

        channel_in = g_io_channel_unix_new(std_in);
        channel_out = g_io_channel_unix_new(std_out);
        channel_err = g_io_channel_unix_new(std_err);

        g_io_channel_set_close_on_unref(channel_in, TRUE);
        g_io_channel_set_close_on_unref(channel_out, TRUE);
        g_io_channel_set_close_on_unref(channel_err, TRUE);
        watch_in_id =
            g_io_add_watch_full(channel_out, G_PRIORITY_LOW, G_IO_IN | G_IO_HUP, thread_reader,
                                threaddata, NULL);
        watch_err_id =
            g_io_add_watch_full(channel_err, G_PRIORITY_LOW, G_IO_IN | G_IO_ERR | G_IO_HUP,
                                thread_reader_error, threaddata, NULL);
        watch_in_hup_id =
            g_io_add_watch_full(channel_out, G_PRIORITY_LOW, G_IO_ERR | G_IO_HUP, thread_complete,
                                threaddata, NULL);
//        watch_in_id = g_io_add_watch(channel_in, G_IO_IN, thread_reader, NULL);
//        watch_err_id = g_io_add_watch(channel_err, G_IO_IN | G_IO_ERR | G_IO_HUP, thread_reader_error, NULL);
//        watch_in_hup_id = g_io_add_watch(channel_in, G_IO_ERR | G_IO_HUP, thread_complete, NULL);

        //g_idle_add(set_play, NULL);
        guistate = PLAYING;
        g_idle_add(set_gui_state, NULL);
#ifdef GLIB2_14_ENABLED
        g_timeout_add_seconds(1, thread_query, threaddata);
#else
        g_timeout_add(1000, thread_query, threaddata);
#endif

        g_cond_wait(mplayer_complete_cond, thread_running);
        if (verbose)
            printf("Thread completing\n");
        threaddata->done = TRUE;
        g_source_remove(watch_in_id);
        g_source_remove(watch_err_id);
        g_source_remove(watch_in_hup_id);
        idledata->percent = 1.0;
        idledata->position = idledata->length;
        g_idle_add(set_progress_value, idledata);
        g_idle_add(set_progress_time, idledata);

        if (channel_in != NULL) {
            g_io_channel_shutdown(channel_in, FALSE, NULL);
            g_io_channel_unref(channel_in);
            channel_in = NULL;
        }

        if (channel_out != NULL) {
            g_io_channel_shutdown(channel_out, FALSE, NULL);
            g_io_channel_unref(channel_out);
            channel_out = NULL;
        }

        if (channel_err != NULL) {
            g_io_channel_shutdown(channel_err, FALSE, NULL);
            g_io_channel_unref(channel_err);
            channel_err = NULL;
        }
        close(std_in);
        std_in = -1;
        g_spawn_close_pid(pid);

#ifdef GIO_ENABLED
        if (idledata->tmpfile) {
            if (verbose)
                printf("removing temp file '%s'\n", threaddata->filename);
            if (g_mutex_trylock(idledata->caching)) {
                g_mutex_unlock(idledata->caching);
            } else {
                g_cancellable_cancel(idledata->cancel);
            }
            g_unlink(threaddata->filename);
        }
#endif

        if (g_file_test(idledata->af_export, G_FILE_TEST_EXISTS)) {
            // stop audio export monitor
            // as part of the stopping, remove file when memmapped file is closed
            g_idle_add(unmap_af_export_file, idledata);
        }

        dbus_enable_screensaver();
        g_mutex_unlock(thread_running);

        if (idledata->cachepercent < 0 && g_str_has_prefix(threaddata->filename, "mms")) {
            dontplaynext = TRUE;
            playback_error = ERROR_RETRY_WITH_MMSHTTP;
        }

        if (idledata->cachepercent < 0 && g_str_has_prefix(threaddata->filename, "mmshttp")) {
            dontplaynext = TRUE;
            playback_error = ERROR_RETRY_WITH_HTTP;
        }

        if (dontplaynext == FALSE) {
            if (next_item_in_playlist(&iter)) {
                if (gtk_list_store_iter_is_valid(playliststore, &iter)) {
                    gtk_tree_model_get(GTK_TREE_MODEL(playliststore), &iter, ITEM_COLUMN, &filename,
                                       COUNT_COLUMN, &count, PLAYLIST_COLUMN, &playlist, -1);
                    g_strlcpy(idledata->info, filename, 4096);
                    g_idle_add(set_media_info, idledata);
                    p = (PlayData *) g_malloc(sizeof(PlayData));
                    g_strlcpy(p->uri, filename, 4096);
                    p->playlist = playlist;
                    g_idle_add(play, p);
                    g_free(filename);
                }
            } else {
                // printf("end of thread playlist is empty\n");
                if (loop) {
                    if (first_item_in_playlist(&iter)) {
                        gtk_tree_model_get(GTK_TREE_MODEL(playliststore), &iter, ITEM_COLUMN,
                                           &filename, COUNT_COLUMN, &count, PLAYLIST_COLUMN,
                                           &playlist, -1);
                        g_strlcpy(idledata->info, filename, 4096);
                        g_idle_add(set_media_info, idledata);
                        p = (PlayData *) g_malloc(sizeof(PlayData));
                        g_strlcpy(p->uri, filename, 4096);
                        p->playlist = playlist;
                        g_idle_add(play, p);
                        g_free(filename);
                    }
                } else {
                    idledata->fullscreen = 0;
                    g_idle_add(set_fullscreen, idledata);
                    g_idle_add(set_stop, idledata);

                    // nothing is on the playlist and we are not looping so ask plugin for next item
                    if (embed_window != 0 || control_id != 0) {
                        js_state = STATE_MEDIAENDED;
                        dbus_send_event("MediaComplete", 0);
                        dbus_open_next();
                    }
                }

                if (quit_on_complete) {
                    g_idle_add(set_quit, idledata);
                }
            }
        } else {
            if (playback_error == ERROR_RETRY_WITH_PLAYLIST) {
                p = (PlayData *) g_malloc(sizeof(PlayData));
                g_strlcpy(p->uri, threaddata->filename, 4096);
                p->playlist = 1;
                g_idle_add(play, p);
            }

            if (playback_error == ERROR_RETRY_WITH_HTTP) {
                p = (PlayData *) g_malloc(sizeof(PlayData));
                filename = switch_protocol(threaddata->filename, "http");
                g_strlcpy(p->uri, filename, 4096);
                g_free(filename);
                p->playlist = threaddata->playlist;
                g_idle_add(play, p);
            }

            if (playback_error == ERROR_RETRY_WITH_MMSHTTP) {
                p = (PlayData *) g_malloc(sizeof(PlayData));
                filename = switch_protocol(threaddata->filename, "mmshttp");
                g_strlcpy(p->uri, filename, 4096);
                g_free(filename);
                p->playlist = threaddata->playlist;
                g_idle_add(play, p);
            }

            if (playback_error == NO_ERROR) {
                // nothing is on the playlist and we are not looping so ask plugin for next item
                if (embed_window != 0 || control_id != 0) {
                    dbus_send_event("MediaComplete", 0);
                    dbus_open_next();
                }
            }

            dontplaynext = FALSE;
        }

    } else {
        state = QUIT;
        printf("Spawn failed for filename %s\n", threaddata->filename);
        g_mutex_unlock(thread_running);
    }

    // printf("Thread done\n");

    thread = NULL;
    return NULL;
}
