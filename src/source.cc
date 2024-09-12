/*
Copyright (C) 2022 DEV47APPS, github.com/dev47apps

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdlib.h>
#include <util/threading.h>
#include <util/platform.h>

#if DROIDCAM_OVERRIDE
#define ENABLE_GUI 1
#endif

#if ENABLE_GUI
#include "obs.hpp"
#include "obs-frontend-api.h"
#include <QMainWindow>
#include <QMessageBox>
extern QMainWindow *main_window;
#endif

#include "plugin.h"
#include "source.h"
#include "plugin_properties.h"
#include "ffmpeg_decode.h"
#include "mjpeg_decode.h"
#include "net.h"
#include "buffer_util.h"
#include "device_discovery.h"

#define PLUGIN_VERSION_STR "233"
#define FPS 25
#define MILLI_SEC 1000
#define NANO_SEC  1000000000

extern char os_name_version[64];
extern const char* bindIP;

#define SOURCE_EXISTS() (os_event_try(plugin->stop_signal) == EAGAIN)

struct droidcam_obs_source {
    Tally_t tally;
#ifndef _DISABLE_ADB
    AdbMgr adbMgr;
#endif
    USBMux iosMgr;
    MDNS mdnsMgr;
    Decoder* video_decoder;
    Decoder* audio_decoder;
    obs_source_t *source;
    os_event_t *stop_signal;
    os_event_t *reset_signal;
    os_event_t *comms_signal;
    pthread_t audio_thread;
    pthread_t video_thread;
    pthread_t video_decode_thread;
    pthread_t comms_thread;
    enum video_range_type range;
    bool is_showing;
    bool activated;
    bool deactivateWNS;
    bool enable_audio;
    bool use_hw;
    bool audio_running;
    bool video_running;
    int video_resolution;
    int usb_port;
    enum VideoFormat video_format;
    struct active_device_info device_info;
    struct obs_source_audio obs_audio_frame;
    struct obs_source_frame2 obs_video_frame;
    uint64_t time_start;
    #if DROIDCAM_OVERRIDE
    std::vector<OBSSignal> signal_handlers;
    #endif
    Queue<CommsTask> comms_queue;
};

#if DROIDCAM_OVERRIDE
static void signal_source_update(obs_source_t* source, const char* battery_level, int battery_alert) {
    signal_handler_t *h = obs_source_get_signal_handler(source);
    calldata_t cd;
    calldata_init(&cd);
    calldata_set_int(&cd, "battery_alert", battery_alert);
    calldata_set_string(&cd, "battery_level", battery_level);
    signal_handler_signal(h, "droidcam_source_update", &cd);
    calldata_free(&cd);
}
#endif

#define comms_task(t) do {\
    plugin->comms_queue.add_item(t);\
    os_event_signal(plugin->comms_signal);\
    } while(0)

static socket_t connect(struct droidcam_obs_source *plugin) {
    Device* dev;
    #ifndef _DISABLE_ADB
    AdbMgr* adbMgr = &plugin->adbMgr;
    #endif
    USBMux* iosMgr = &plugin->iosMgr;
    MDNS  *mdnsMgr = &plugin->mdnsMgr;

    struct active_device_info *device_info = &plugin->device_info;

    dlog("connect device: id=%s type=%d", device_info->id, (int) device_info->type);

    if (device_info->type == DeviceType::WIFI) {
        return net_connect(device_info->ip, bindIP, device_info->port);
    }

    if (device_info->type == DeviceType::MDNS) {
        dev = mdnsMgr->GetDevice(device_info->id);
        if (dev) {
            return net_connect(dev->address, bindIP, device_info->port);
        }

        mdnsMgr->Reload();
        goto out;
    }
#ifndef _DISABLE_ADB

    if (device_info->type == DeviceType::ADB) {
        dev = adbMgr->GetDevice(device_info->id);
        if (dev) {
            if (adbMgr->DeviceOffline(dev)) {
                elog("device is offline...");
                goto out;
            }

            int port_start = device_info->port + (adbMgr->Iter() * 10);
            if (plugin->usb_port < port_start) {
                plugin->usb_port = port_start;
            }
            else if (plugin->usb_port > (port_start + 8)) {
                plugin->usb_port = port_start;
                adbMgr->ClearForwards(dev);
            }

            dlog("ADB: mapping %d -> %d\n", plugin->usb_port, device_info->port);
            if (!adbMgr->AddForward(dev, plugin->usb_port, device_info->port)) {
                plugin->usb_port++;
                goto out;
            }

            socket_t rc = net_connect(localhost_ip, plugin->usb_port);
            if (rc != INVALID_SOCKET) return rc;

            adbMgr->ClearForwards(dev);
            goto out;
        }

        adbMgr->Reload();
        goto out;
    }
#endif
    if (device_info->type == DeviceType::IOS) {
        dev = iosMgr->GetDevice(device_info->id);
        if (dev) {
            return iosMgr->Connect(dev, device_info->port, &plugin->usb_port);
        }

        iosMgr->Reload();
        goto out;
    }

    out:
    return INVALID_SOCKET;
}

#define MAXCONFIG 1024
#define MAXPACKET 1024 * 1024 * 16
static DataPacket*
read_frame(Decoder *decoder, socket_t sock, int *has_config)
{
    uint8_t header[HEADER_SIZE];
    uint8_t config[MAXCONFIG];
    size_t r;
    size_t len, config_len = 0;
    uint64_t pts;

    AGAIN:
    r = net_recv_all(sock, header, HEADER_SIZE);
    if (r != HEADER_SIZE) {
        elog("read header recv returned %ld", r);
        return NULL;
    }

    pts = buffer_read64be(header);
    len = buffer_read32be(&header[8]);
    // dlog("read_frame: header: pts=%llu len=%ld", pts, len);

    if (pts == NO_PTS) {
        if (config_len != 0) {
             elog("double config ???");
             return NULL;
        }

        if ((int)len == -1) {
            elog("stop/error from app side");
            return NULL;
        }

        if (len == 0 || len > MAXCONFIG) {
            elog("config packet too large at %ld!", len);
            return NULL;
        }

        r = net_recv_all(sock, config, len);
        if (r != len) {
            elog("read config recv returned %ld", r);
            return NULL;
        }

        ilog("have config: %ld", len);
        config_len = len;
        *has_config = 1;
        goto AGAIN;
    }

    if (len == 0 || len > MAXPACKET) {
        elog("data packet too large at %ld!", len);
        return NULL;
    }

    DataPacket* data_packet = decoder->pull_empty_packet(config_len + len);
    uint8_t *p = data_packet->data;
    if (config_len) {
        memcpy(p, config, config_len);
        p += config_len;
    }

    r = net_recv_all(sock, p, len);
    if (r != len) {
        elog("read_frame: read %ld bytes wanted %ld", r, len);
        decoder->push_empty_packet(data_packet);
        return NULL;
    }

    data_packet->pts = pts;
    data_packet->used = config_len + len;
    return data_packet;
}

static void *video_decode_thread(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);

    Decoder *decoder = NULL;
    DataPacket* data_packet = NULL;
    bool got_output;

    ilog("video_decode_thread start");

    while (SOURCE_EXISTS()) {
        if ((decoder = plugin->video_decoder) == NULL || (data_packet = decoder->pull_ready_packet()) == NULL) {
            os_sleep_ms(5);
            continue;
        }

        if (decoder->failed)
            goto LOOP;

        if (!decoder->decode_video(&plugin->obs_video_frame, data_packet, &got_output)) {
            elog("error decoding video");
            decoder->failed = true;
            goto LOOP;
        }

        if (got_output) {
            plugin->obs_video_frame.timestamp = data_packet->pts * 1000;
            //if (flip) plugin->obs_video_frame.flip = !plugin->obs_video_frame.flip;
            #if 0
            dlog("output video: %dx%d %lu",
                plugin->obs_video_frame.width,
                plugin->obs_video_frame.height,
                plugin->obs_video_frame.timestamp);
            #endif
            obs_source_output_video2(plugin->source, &plugin->obs_video_frame);
        }

        LOOP:
        decoder->push_empty_packet(data_packet);
    }

    ilog("video_decode_thread end");
    return NULL;
}

static bool
recv_video_frame(droidcam_obs_source *plugin, socket_t sock) {
    int has_config = 0;
    DataPacket* data_packet;
    Decoder *decoder = plugin->video_decoder;

    if (!decoder) {
        if (plugin->video_format == FORMAT_AVC) {
            decoder = new FFMpegDecoder();
        }
        else if (plugin->video_format == FORMAT_MJPG) {
            decoder = new MJpegDecoder();
        }
        else {
            elog("unexpected video format %d", plugin->video_format);
            decoder = new MJpegDecoder();
            decoder->failed = true;
        }
        plugin->video_decoder = decoder;
    }

    data_packet = read_frame(decoder, sock, &has_config);
    if (!data_packet)
        return false;

    // NOTE: data_packet must be properly disposed from here

    // Decoder failures should not happen generally.
    // Rather than causing a connection reset, just idle
    if (decoder->failed) {
        FAILED:
        dlog("discarding frame.. decoder failed");
        decoder->push_empty_packet(data_packet);
        return true;
    }


    if (!decoder->ready) {
        bool init = false;
        bool use_hw = plugin->use_hw;
        dlog("init video decoder");

        if (plugin->video_format == FORMAT_AVC) {
            init = (((FFMpegDecoder*)decoder)->init(NULL, AV_CODEC_ID_H264, use_hw) >= 0);
        }
        else if (plugin->video_format == FORMAT_MJPG) {
            init = ((MJpegDecoder*)decoder)->init();
        }
        else {
            init = false;
        }

        plugin->obs_video_frame.format = VIDEO_FORMAT_NONE;
        plugin->obs_video_frame.range  = VIDEO_RANGE_DEFAULT;
        if (init) {
            comms_task(CommsTask::TALLY);
            droidcam_signal(plugin->source, "droidcam_connect");
        } else {
            elog("could not initialize decoder");
            decoder->failed = true;
            goto FAILED;
        }
    }

    decoder->push_ready_packet(data_packet);
    return true;
}

static void *video_thread(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    const char *obs_version_str = obs_get_version_string();
    socket_t sock = INVALID_SOCKET;
    char remote_url[256];
    char video_req[256];
    int video_req_len = 0;

    #if DROIDCAM_OVERRIDE
    // todo: dont do this
    char obs_version_str_flat[4];
    obs_version_str_flat[0] = obs_version_str[0];
    obs_version_str_flat[1] = obs_version_str[2];
    obs_version_str_flat[2] = obs_version_str[4];
    obs_version_str_flat[3] = 0;
    #endif


    ilog("video_thread start");

    // Preload devices if plugin is created already active
    // (ex. when obs is re-launched)
    // This saves an unnecessary initial SLOW_LOOP
    if (plugin->activated) {
        switch (plugin->device_info.type) {
            case DeviceType::MDNS:
                plugin->mdnsMgr.Reload();
                plugin->mdnsMgr.ResetIter();
                break;
#ifndef _DISABLE_ADB
            case DeviceType::ADB:
                plugin->adbMgr.Reload();
                plugin->adbMgr.ResetIter();
                break;
#endif
            case DeviceType::IOS:
                plugin->iosMgr.Reload();
                plugin->iosMgr.ResetIter();
                break;
            case DeviceType::WIFI:
            case DeviceType::NONE:
                break;
        }
    }

    while (SOURCE_EXISTS()) {
        if (plugin->activated && plugin->is_showing) {
            if (plugin->video_running) {
                if (os_event_try(plugin->reset_signal) == EAGAIN
                    && recv_video_frame(plugin, sock))
                    continue;

                plugin->video_running = false;
                dlog("closing failed video socket %d", sock);
                net_close(sock);
                sock = INVALID_SOCKET;
                goto SLOW_LOOP;
            }

            if ((sock = connect(plugin)) == INVALID_SOCKET)
                goto SLOW_LOOP;

            video_req_len = snprintf(video_req, sizeof(video_req), VIDEO_REQ,
                VideoFormatNames[plugin->video_format][1],
                Resolutions[plugin->video_resolution],
                plugin->usb_port,
                os_name_version,
                #if DROIDCAM_OVERRIDE
                "", obs_version_str_flat, 5912);
                #else
                obs_version_str, PLUGIN_VERSION_STR, 5912);
                #endif

            dlog("%s", video_req);
            if (net_send_all(sock, video_req, video_req_len) <= 0) {
                elog("send(/video) failed");
                net_close(sock);
                sock = INVALID_SOCKET;

                SLOW_LOOP:
                os_sleep_ms(MILLI_SEC * 2);
                goto LOOP;
            }

            set_recv_buf_len(sock, 65536 * 4);
            plugin->video_running = true;
            dlog("starting video via socket %d", sock);

            int port = (
#ifndef _DISABLE_ADB
                        plugin->device_info.type == DeviceType::ADB ||
#endif
                        plugin->device_info.type == DeviceType::IOS
            )
                ? plugin->usb_port
                : plugin->device_info.port;

            if (port > 0) {
                snprintf(remote_url, sizeof(remote_url), "http://%s:%d", plugin->device_info.ip, port);
                obs_data_t *settings = obs_source_get_settings(plugin->source);
                obs_data_set_string(settings, "remote_url", remote_url);
                obs_data_release(settings);
            }

            os_event_reset(plugin->reset_signal);
            continue;
        }
        // else: not activated
        video_req_len = 0;

        LOOP:
        if (plugin->video_running) {
            plugin->video_running = false;
        }

        if (sock != INVALID_SOCKET) {
            dlog("closing active video socket %d", sock);
            net_close(sock);
            sock = INVALID_SOCKET;
        }

        if (plugin->video_decoder) {
            if (plugin->video_decoder->ready)
                droidcam_signal(plugin->source, "droidcam_disconnect");

            while (plugin->video_decoder->recieveQueue.items.size() < plugin->video_decoder->alloc_count
                    && SOURCE_EXISTS())
            {
                dlog("waiting for decode thread: %lu/%lu",
                    plugin->video_decoder->recieveQueue.items.size(),
                    plugin->video_decoder->alloc_count);
                os_sleep_ms(MILLI_SEC / FPS);
            }

            dlog("release video_decoder");
            delete plugin->video_decoder;
            plugin->video_decoder = NULL;
        }

        obs_source_output_video2(plugin->source, NULL);
        os_sleep_ms(MILLI_SEC / FPS);
    }

    ilog("video_thread end");
    plugin->video_running = false;
    if (sock != INVALID_SOCKET) net_close(sock);
    return NULL;
}

static bool
do_audio_frame(droidcam_obs_source *plugin, socket_t sock) {
    FFMpegDecoder *decoder = (FFMpegDecoder*)plugin->audio_decoder;
    if (!decoder) {
        dlog("create audio decoder");
        decoder = new FFMpegDecoder();
        plugin->audio_decoder = decoder;
    }

    int has_config = 0;
    bool got_output;
    DataPacket* data_packet = read_frame(decoder, sock, &has_config);
    if (!data_packet)
        return false;

    // NOTE: data_packet must be properly disposed from here

    // Decoder failures should not happen generally.
    // Rather than causing a connection reset, just idle
    if (decoder->failed) {
        FAILED:
        dlog("discarding audio frame.. decoder failed");
        decoder->push_empty_packet(data_packet);
        return true;
    }

    if (has_config || !decoder->ready) {
        if (decoder->ready) {
            ilog("unexpected audio config change while decoder is init'd");
            decoder->failed = true;
            goto FAILED;
        }

        if (decoder->init(data_packet->data, AV_CODEC_ID_AAC, false) < 0) {
            elog("could not initialize AAC decoder");
            decoder->failed = true;
            goto FAILED;
        }

        plugin->obs_audio_frame.format = AUDIO_FORMAT_UNKNOWN;
        decoder->push_empty_packet(data_packet);
        return true;
    }


    // decoder->push_ready_packet(data_packet);
    if (!decoder->decode_audio(&plugin->obs_audio_frame, data_packet, &got_output)) {
        elog("error decoding audio");
        decoder->failed = true;
        goto FAILED;
    }

    if (got_output) {
        plugin->obs_audio_frame.timestamp = os_gettime_ns(); // data_packet->pts * 1000;
        #if 0
        dlog("output audio: %d frames: %d HZ, Fmt %d, Chan %d,  pts %lu",
            plugin->obs_audio_frame.frames,
            plugin->obs_audio_frame.samples_per_sec,
            plugin->obs_audio_frame.format,
            plugin->obs_audio_frame.speakers,
            plugin->obs_audio_frame.timestamp);
        #endif
        obs_source_output_audio(plugin->source, &plugin->obs_audio_frame);
    }

    decoder->push_empty_packet(data_packet);
    return true;
}

static void *audio_thread(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    socket_t sock = INVALID_SOCKET;
    const char *audio_req = AUDIO_REQ;

    ilog("audio_thread start");
    while (SOURCE_EXISTS()) {
        if (plugin->activated && plugin->is_showing && plugin->enable_audio) {
            if (plugin->audio_running) {
                if (do_audio_frame(plugin, sock)) {
                    continue;
                }

                plugin->audio_running = false;
                dlog("closing failed audio socket %d", sock);
                net_close(sock);
                sock = INVALID_SOCKET;
                goto SLOW_LOOP;
            }

            // connect audio only after video works
            if (!plugin->video_running)
                goto LOOP;

            // no rush..
            os_sleep_ms(MILLI_SEC);

            if ((sock = connect(plugin)) == INVALID_SOCKET)
                goto SLOW_LOOP;

            if (net_send_all(sock, audio_req, sizeof(AUDIO_REQ)-1) <= 0) {
                elog("send(/audio) failed");
                net_close(sock);
                sock = INVALID_SOCKET;

                SLOW_LOOP:
                os_sleep_ms(MILLI_SEC * 2);
                goto LOOP;
            }

            plugin->audio_running = true;
            dlog("starting audio via socket %d", sock);
            continue;
        }

        // else: not activated
        if (plugin->audio_running) {
            plugin->audio_running = false;
        }

        LOOP:
        if (sock != INVALID_SOCKET) {
            dlog("closing active audio socket %d", sock);
            net_close(sock);
            sock = INVALID_SOCKET;
        }

        if (plugin->audio_decoder) {
            dlog("release audio_decoder");
            delete plugin->audio_decoder;
            plugin->audio_decoder = NULL;
        }

        if (plugin->enable_audio) obs_source_output_audio(plugin->source, NULL);
        os_sleep_ms(MILLI_SEC / FPS);
    }

    ilog("audio_thread end");
    plugin->audio_running = false;
    if (sock != INVALID_SOCKET) net_close(sock);
    return NULL;
}

static int
basic_http(socket_t sock, char* buf, const size_t maxlen, const char *request, const size_t len) {
    if (net_send_all(sock, request, len) <= 0) {
        return 0;
    }

    memset(buf, 0, maxlen);
    int i = 0;
    while (i < maxlen) {
        int rc = net_recv(sock, &buf[i], maxlen - i);
        if (rc > 0) {
            i += rc;
            continue;
        }

        #if _WIN32
        WSAErrno();
        if (rc < 0 && (errno == WSAEWOULDBLOCK || errno == WSAETIMEDOUT))
            break;

        #else
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS))
            break;

        #endif

        return 0;
    }

    for (i = 0; i < maxlen; i++) {
        if (!(buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n'))
            continue;

        return i+4;
    }

    return 0;
}

static void *comms_thread(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    socket_t sock = INVALID_SOCKET;
    char buf[4096] = {0};
    const size_t maxlen = sizeof(buf) - 4;

    #if DROIDCAM_OVERRIDE
    const char *battery_req = BATT_REQ;
    const int WARN = 15;
    int prevBattery = 100;
    #endif /* DROIDCAM_OVERRIDE */

    int event = 0;

    dlog("comms_thread start");

    while ((event = os_event_timedwait(plugin->comms_signal, (30*MILLI_SEC))) != EINVAL
        && SOURCE_EXISTS())
    {
        os_event_reset(plugin->comms_signal);

        if (plugin->activated && plugin->video_running) {

            if (sock == INVALID_SOCKET) {
                if ((sock = connect(plugin)) == INVALID_SOCKET)
                    continue;
                set_recv_timeout(sock, 1);
            }
        }
        else {
            if (sock != INVALID_SOCKET) {
                #if DROIDCAM_OVERRIDE
                prevBattery = 100;
                buf[0] = 0;
                signal_source_update(plugin->source, (const char*) &buf[0], 0);
                #endif

                CLOSE:
                dlog("closing comms socket %d // (%d) %s", sock, errno, strerror(errno));
                net_close(sock);
                sock = INVALID_SOCKET;
            }
        }

        if (sock == INVALID_SOCKET)
            continue;

        if (event == ETIMEDOUT) {
            #if DROIDCAM_OVERRIDE
            int i = basic_http(sock, buf, maxlen, battery_req, sizeof(BATT_REQ) - 1);
            if (i > 0) {
                int start = i;
                for (; i < maxlen && isdigit(buf[i]); i++);

                if (i > start) {
                    buf[i++] = '%';
                    buf[i  ] = 0;

                    const char* value = (const char*) &buf[start];
                    i = atoi(value);
                    const int alert = (prevBattery > WARN && i <= WARN);
                    dlog("battery %d -> %d (%s) alert=%d", prevBattery, i, value, alert);
                    signal_source_update(plugin->source, value, alert);
                    prevBattery = i;
                }
            }
            else goto CLOSE;

            #endif // DROIDCAM_OVERRIDE
        }

        CommsTask task;
        const char *tally = NULL;

        while ((task = plugin->comms_queue.next_item()) != CommsTask::NONE) {
            if (task == CommsTask::TALLY) {
                // consume all tally events and send the latest one
                if (plugin->tally.on_program) {
                    tally = "program";
                }
                else if (plugin->tally.on_preview) {
                    tally = "preview";
                }
                else {
                    tally = "idle";
                }
                dlog("comms: task (%d) // %s", task, tally);
            }
        }

        if (tally != NULL) {
            int len = snprintf(buf, maxlen, TALLY_REQ, tally);
            if (basic_http(sock, buf, maxlen, (const char*) &buf[0], len) > 0) {
                dlog("comms: tally -> %s", tally);
            }
            else {
                if (errno != 0) {
                    // Try again if the request actually failed.
                    // If there is no error, most likely the app closed the connection,
                    // ie. tally is not supported (such as with old app versions).
                    os_sleep_ms(MILLI_SEC * 5);
                    comms_task(CommsTask::TALLY);
                }
                goto CLOSE;
            }
        }
    } // while (SOURCE_EXISTS)

    if (sock != INVALID_SOCKET) net_close(sock);

    dlog("comms_thread end");
    return NULL;
}

void source_destroy(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    ilog("destroy: \"%s\"", obs_source_get_name(plugin->source));

    if (plugin) {
        if (plugin->time_start != 0) {
            ilog("stopping");
            os_event_signal(plugin->stop_signal);
            pthread_join(plugin->video_thread, NULL);
            pthread_join(plugin->audio_thread, NULL);

            os_event_signal(plugin->comms_signal);
            pthread_join(plugin->comms_thread, NULL);
            pthread_join(plugin->video_decode_thread, NULL);

            os_event_destroy(plugin->stop_signal);
            os_event_destroy(plugin->reset_signal);
            os_event_destroy(plugin->comms_signal);
        }

        ilog("cleanup");
        if (plugin->video_decoder) delete plugin->video_decoder;
        if (plugin->audio_decoder) delete plugin->audio_decoder;
        delete plugin;
    }
}

#if DROIDCAM_OVERRIDE
static const char *droidcam_signals[] = {
    "void droidcam_source_status(in out int status)",
    "void droidcam_source_context(in out ptr context)",
    "void droidcam_source_update(string battery)",
    NULL,
};
#endif

void *source_create(obs_data_t *settings, obs_source_t *source) {
    ilog("Source: \"%s\" - " PLUGIN_VERSION_STR, obs_source_get_name(source));
    obs_source_set_async_unbuffered(source, true);

    droidcam_obs_source *plugin = new droidcam_obs_source();
    plugin->source = source;
    plugin->audio_running = false;
    plugin->video_running = false;
    plugin->audio_decoder = NULL;
    plugin->video_decoder = NULL;
    plugin->usb_port = 0;
    plugin->use_hw = obs_data_get_bool(settings, OPT_USE_HW_ACCEL);
    plugin->video_format = (VideoFormat) obs_data_get_int(settings, OPT_VIDEO_FORMAT);
    plugin->video_resolution = obs_data_get_int(settings, OPT_RESOLUTION);
    plugin->enable_audio  = obs_data_get_bool(settings, OPT_ENABLE_AUDIO);
    plugin->deactivateWNS = obs_data_get_bool(settings, OPT_DEACTIVATE_WNS);
    plugin->activated = obs_data_get_bool(settings, OPT_IS_ACTIVATED);
    obs_data_set_string(settings, "remote_url", "");

    #if DROIDCAM_OVERRIDE
    plugin->deactivateWNS = true;
    signal_handler_t *h = obs_source_get_signal_handler(source);
    signal_handler_add_array(h, droidcam_signals);

    plugin->signal_handlers.emplace_back(h, "droidcam_source_status",
        [](void *data, calldata_t *cd) {
            droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
            int status = 0;
            if (plugin->activated)     status |= 1;
            if (plugin->video_running) status |= 2;
            if (plugin->audio_running) status |= 4;
            calldata_set_int(cd, "status", status);
        }, plugin);

    plugin->signal_handlers.emplace_back(h, "droidcam_source_context",
        [](void *data, calldata_t *cd) {
            calldata_set_ptr(cd, "context", data);
        }, plugin);

    #endif

    ilog("activated=%d, deactivateWNS=%d, is_showing=%d, enable_audio=%d",
        plugin->activated, plugin->deactivateWNS, plugin->is_showing, plugin->enable_audio);
    ilog("video_format=%s video_resolution=%s",
        VideoFormatNames[plugin->video_format][1],
        Resolutions[plugin->video_resolution]);

    // dummy source, do not create threads & decoders
    if (obs_data_get_bool(settings, OPT_DUMMY_SOURCE)) {
        dlog("dummy source created");
        plugin->time_start = 0;
        return plugin;
    }

    if (plugin->activated) {
        plugin->device_info.id = obs_data_get_string(settings, OPT_ACTIVE_DEV_ID);
        plugin->device_info.ip = obs_data_get_string(settings, OPT_ACTIVE_DEV_IP);
        plugin->device_info.port = (int) obs_data_get_int(settings, OPT_APP_PORT);
        plugin->device_info.type = (DeviceType) obs_data_get_int(settings, OPT_ACTIVE_DEV_TYPE);
        ilog("device_info.id=%s device_info.ip=%s device_info.port=%d device_info.type=%d",
            plugin->device_info.id, plugin->device_info.ip,
            plugin->device_info.port, (int) plugin->device_info.type);

        if (plugin->device_info.type == DeviceType::NONE
            || plugin->device_info.port <= 0 || plugin->device_info.port > 65535
            || !plugin->device_info.id || plugin->device_info.id[0] == 0)
            plugin->activated = false;

        if (plugin->device_info.type == DeviceType::WIFI && (!plugin->device_info.ip || plugin->device_info.ip[0] == 0))
            plugin->activated = false;

        // Not sure if this is a bug or a feature.
        // Launching while activated & hidden will not start video until visibility is toggled.
        //
        // activated=1, deactivateWNS=0, is_showing=0, enable_audio=0
        //
        // if (plugin->activated || !plugin->deactivateWNS)
        //     plugin->is_showing = true;
    }

    if (os_event_init(&plugin->stop_signal, OS_EVENT_TYPE_MANUAL) != 0) {
        source_destroy(plugin);
        return NULL;
    }

    if (os_event_init(&plugin->reset_signal, OS_EVENT_TYPE_MANUAL) != 0) {
        source_destroy(plugin);
        return NULL;
    }

    if (os_event_init(&plugin->comms_signal, OS_EVENT_TYPE_MANUAL) != 0) {
        source_destroy(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->video_thread, NULL, video_thread, plugin) != 0) {
        source_destroy(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->video_decode_thread, NULL, video_decode_thread, plugin) != 0) {
        source_destroy(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->comms_thread, NULL, comms_thread, plugin) != 0) {
        source_destroy(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->audio_thread, NULL, audio_thread, plugin) != 0) {
        source_destroy(plugin);
        return NULL;
    }

    plugin->time_start = os_gettime_ns() / 100;
    return plugin;
}

void source_show(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    plugin->is_showing = true;

    #if ENABLE_GUI
    obs_source_t *scene = obs_frontend_get_current_scene();
    if (scene) {
        obs_sceneitem_t *item = obs_scene_sceneitem_from_source(obs_scene_from_source(scene), plugin->source);
        if (item) {
            vec2 pos;
            vec2 scale;
            struct obs_sceneitem_crop crop;
            obs_sceneitem_get_pos(item, &pos);
            obs_sceneitem_get_crop(item, &crop);
            obs_sceneitem_get_scale(item, &scale);
            ilog("pos:%.0f,%.0f scale:%.1f,%.1f rot:%.1f crop:%d,%d; %d,%d",
                pos.x, pos.y, scale.x, scale.y,
                obs_sceneitem_get_rot(item),
                crop.left, crop.top, crop.right, crop.bottom);
            obs_sceneitem_release(item);
        }
        obs_source_release(scene);
    }
    #endif

    plugin->tally.on_preview = true;
    comms_task(CommsTask::TALLY);
    dlog("source_show: is_showing=%d", plugin->is_showing);
}

void source_hide(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    if (plugin->deactivateWNS && plugin->activated)
        plugin->is_showing = false;

    plugin->tally.on_preview = false;
    comms_task(CommsTask::TALLY);
    dlog("source_hide: is_showing=%d", plugin->is_showing);
}

void source_show_main(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    plugin->tally.on_program = true;
    comms_task(CommsTask::TALLY);
}

void source_hide_main(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    plugin->tally.on_program = false;
    comms_task(CommsTask::TALLY);
}

static inline void toggle_ppts(obs_properties_t *ppts, bool enable) {
    obs_property_set_enabled(obs_properties_get(ppts, OPT_REFRESH)     , enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_DEVICE_LIST) , enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_WIFI_IP)     , enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_APP_PORT)    , enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_ENABLE_AUDIO), enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_USE_HW_ACCEL), enable);
}

void resolve_device_type(struct active_device_info *device_info, void* data) {
    if (!device_info || !data)
        return;

    const char *id = device_info->id;
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);

    Device* dev;
#ifndef _DISABLE_ADB
    AdbMgr* adbMgr = &plugin->adbMgr;
#endif
    USBMux* iosMgr = &plugin->iosMgr;
    MDNS  *mdnsMgr = &plugin->mdnsMgr;

    dev = mdnsMgr->GetDevice(id);
    if (dev) {
        device_info->ip = dev->address;
        device_info->type = DeviceType::MDNS;
        return;
    }
#ifndef _DISABLE_ADB
    dev = adbMgr->GetDevice(id);
    if (dev) {
        if (adbMgr->DeviceOffline(dev)) {
            elog("adb device is offline");
            goto out;
        }

        device_info->ip = localhost_ip;
        device_info->type = DeviceType::ADB;
        return;
    }
#endif
    dev = iosMgr->GetDevice(id);
    if (dev) {
        device_info->ip = localhost_ip;
        device_info->type = DeviceType::IOS;
        return;
    }

    out:
    device_info->type = DeviceType::NONE;
    return;
}

static bool video_parms_changed(void *data, obs_properties_t*, obs_property_t*,
                 obs_data_t *settings) {

    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);

    int video_resolution = obs_data_get_int(settings, OPT_RESOLUTION);
    enum VideoFormat video_format = (VideoFormat) obs_data_get_int(settings, OPT_VIDEO_FORMAT);

    if (video_resolution == plugin->video_resolution
        && video_format == plugin->video_format)
        return false;

    plugin->video_resolution = video_resolution;
    plugin->video_format = video_format;
    ilog("video_parms_changed: video_format=%d/%s video_resolution=%d/%s",
        plugin->video_format, VideoFormatNames[plugin->video_format][1],
        plugin->video_resolution, Resolutions[plugin->video_resolution]);
    os_event_signal(plugin->reset_signal);
    return false;
}

static bool connect_clicked(obs_properties_t *ppts, obs_property_t *p, void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    struct active_device_info *device_info = &plugin->device_info;

    obs_data_t *settings = obs_source_get_settings(plugin->source);
    obs_property_t *cp = obs_properties_get(ppts, OPT_CONNECT);
    obs_property_set_enabled(cp, false);

    bool activated = obs_data_get_bool(settings, OPT_IS_ACTIVATED);
    int video_resolution = obs_data_get_int(settings, OPT_RESOLUTION);
    enum VideoFormat video_format = (VideoFormat) obs_data_get_int(settings, OPT_VIDEO_FORMAT);

    if (activated) {
        plugin->usb_port = 0;
        plugin->activated = false;
        toggle_ppts(ppts, true);
        obs_data_set_bool(settings, OPT_IS_ACTIVATED, false);
        obs_property_set_description(cp, TEXT_CONNECT);
        ilog("deactivate");
        goto out;
    }

    #if ENABLE_GUI
    if (video_format == FORMAT_MJPG && video_resolution > RESOLUTION_1080) {
        QString title = QString(obs_module_text("DroidCam"));
        QString msg = QString(obs_module_text("MJPEGLimit"));
        QMessageBox mb(QMessageBox::Information, title, msg,
            QMessageBox::StandardButtons(QMessageBox::Ok), main_window);
        mb.exec();
        goto out;
    }
    #endif

    device_info->type = DeviceType::NONE;
    device_info->id = obs_data_get_string(settings, OPT_DEVICE_LIST);
    if (!device_info->id || device_info->id[0] == 0){
        elog("target device id is empty");
        goto out;
    }

    device_info->port = (int) obs_data_get_int(settings, OPT_APP_PORT);
    if (device_info->port <= 0 || device_info->port > 65535) {
        elog("invalid port: %d", device_info->port);
        goto out;
    }

    if (strncmp(device_info->id, opt_use_wifi, strlen(opt_use_wifi)) == 0) {
        device_info->ip = obs_data_get_string(settings, OPT_WIFI_IP);
        if (!device_info->ip || device_info->ip[0] == 0) {
            elog("target IP is empty");

            #if ENABLE_GUI
            QString title = QString(obs_module_text("DroidCam"));
            QString msg = QString(obs_module_text("NoWifiIP"));
            QMessageBox mb(QMessageBox::Information, title, msg,
                QMessageBox::StandardButtons(QMessageBox::Ok), main_window);
            mb.exec();
            #endif

            goto out;
        }

        device_info->type = DeviceType::WIFI;
        #if DROIDCAM_OVERRIDE==0
        if (   (device_info->ip[0] == '4')
            && (device_info->ip[1] == 'k' || device_info->ip[1] == 'K')
            && (device_info->ip[2] == '\0'))
        {
            obs_data_set_bool(settings, OPT_UHD_UNLOCK, true);
            obs_data_set_string(settings, OPT_WIFI_IP, "");

            #if ENABLE_GUI
            QString title = QString(obs_module_text("DroidCam"));
            QString msg = QString(obs_module_text("UHDUnlocked"));
            QMessageBox mb(QMessageBox::Information, title, msg,
                QMessageBox::StandardButtons(QMessageBox::Ok), main_window);
            mb.exec();
            #endif

            goto out;
        }
        #endif
    }
    else {
        resolve_device_type(device_info, data);
    }

    if (device_info->type == DeviceType::NONE) {
        elog("unable to determine devce type, refresh device list and try again");
        goto out;
    }

    obs_property_set_description(cp, TEXT_DEACTIVATE);
    plugin->video_format = video_format;
    plugin->video_resolution = video_resolution;

    toggle_ppts(ppts, false);
    obs_data_set_string(settings, OPT_ACTIVE_DEV_ID, device_info->id);
    obs_data_set_string(settings, OPT_ACTIVE_DEV_IP, device_info->ip);
    obs_data_set_int(settings, OPT_ACTIVE_DEV_TYPE, (long long) device_info->type);
    obs_data_set_bool(settings, OPT_IS_ACTIVATED, true);
    plugin->activated = true;
    ilog("activated: id=%s type=%d ip=%s port=%d", device_info->id, (int)device_info->type, device_info->ip, device_info->port);
    ilog("video_format=%d/%s video_resolution=%d/%s",
        plugin->video_format, VideoFormatNames[plugin->video_format][1],
        plugin->video_resolution, Resolutions[plugin->video_resolution]);

    out:
    obs_property_set_enabled(cp, true);
    if (settings) obs_data_release(settings);
    return true;
}

static bool refresh_clicked(obs_properties_t *ppts, obs_property_t *p, void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    Device* dev;
#ifndef _DISABLE_ADB
    AdbMgr *adbMgr = &plugin->adbMgr;
#endif
    USBMux* iosMgr = &plugin->iosMgr;
    MDNS  *mdnsMgr = &plugin->mdnsMgr;
    obs_property_t *cp = obs_properties_get(ppts, OPT_CONNECT);
    obs_property_set_enabled(cp, false);

    if (plugin->time_start == 0) {
        // dummy mode
        ilog("ReLoading Device List...");
    }
    else {
        ilog("Refresh Device List clicked");
    }

    mdnsMgr->Reload();
#ifndef _DISABLE_ADB
    adbMgr->Reload();
#endif
    iosMgr->Reload();

    p = obs_properties_get(ppts, OPT_DEVICE_LIST);
    obs_property_list_clear(p);
#ifndef _DISABLE_ADB
    adbMgr->ResetIter();
    while ((dev = adbMgr->NextDevice()) != NULL) {
        adbMgr->GetModel(dev);
        char *label = dev->model[0] != 0 ? dev->model : dev->serial;
        dlog("ADB: label:%s serial:%s", label, dev->serial);
        size_t idx = obs_property_list_add_string(p, label, dev->serial);
        if (adbMgr->DeviceOffline(dev))
            obs_property_list_item_disable(p, idx, true);
    }
#endif
    iosMgr->ResetIter();
    while ((dev = iosMgr->NextDevice()) != NULL) {
        iosMgr->GetModel(dev);
        char *label = dev->model[0] != 0 ? dev->model : dev->serial;
        dlog("IOS: handle:%d label:%s serial:%s", dev->handle, label, dev->serial);
        obs_property_list_add_string(p, label, dev->serial);
    }

    mdnsMgr->ResetIter();
    while ((dev = mdnsMgr->NextDevice()) != NULL) {
        char *label = dev->model[0] != 0 ? dev->model : dev->serial;
        dlog("MDNS: label:%s serial:%s", label, dev->serial);
        obs_property_list_add_string(p, label, dev->serial);
    }

    obs_property_list_add_string(p, TEXT_USE_WIFI, opt_use_wifi);
    obs_property_set_enabled(cp, true);
    return true;
}

void source_update(void *data, obs_data_t *settings) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    plugin->deactivateWNS = obs_data_get_bool(settings, OPT_DEACTIVATE_WNS);
    plugin->enable_audio  = obs_data_get_bool(settings, OPT_ENABLE_AUDIO);
    plugin->use_hw = obs_data_get_bool(settings, OPT_USE_HW_ACCEL);
    bool sync_av = false; // obs_data_get_bool(settings, OPT_SYNC_AV);
    bool activated = obs_data_get_bool(settings, OPT_IS_ACTIVATED);

    dlog("plugin_udpate: activated=%d (actual=%d) audio=%d sync_av=%d",
        plugin->activated,
        activated,
        plugin->enable_audio,
        sync_av);
    obs_source_set_async_decoupled(plugin->source, !sync_av);

    // handle [Cancel] case
    if (activated != plugin->activated) {
        plugin->activated = activated;
    }
}

obs_properties_t *source_properties(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    obs_properties_t *ppts = obs_properties_create();
    obs_property_t *cp;
    bool activated = false;
    bool uhd_unlock = false;

    if (plugin) {
        obs_data_t *settings = obs_source_get_settings(plugin->source);
        activated = obs_data_get_bool(settings, OPT_IS_ACTIVATED);
        #if DROIDCAM_OVERRIDE==0
        uhd_unlock = obs_data_get_bool(settings, OPT_UHD_UNLOCK);
        #endif
        obs_data_release(settings);
    }

    dlog("plugin_properties: activated=%d, uhd_unlock=%d", activated, uhd_unlock);

    cp = obs_properties_add_list(ppts, OPT_RESOLUTION, TEXT_RESOLUTION, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (size_t i = 0; i < ARRAY_LEN(Resolutions); i++) {
        obs_property_list_add_int(cp, Resolutions[i], i);
        if (!uhd_unlock && i == RESOLUTION_1080) break;
    }

    obs_property_set_modified_callback2(cp, video_parms_changed, data);

    cp = obs_properties_add_list(ppts, OPT_VIDEO_FORMAT, TEXT_VIDEO_FORMAT, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (size_t i = 0; i < ARRAY_LEN(VideoFormatNames); i++)
        obs_property_list_add_int(cp, VideoFormatNames[i][0], i);

    obs_property_set_modified_callback2(cp, video_parms_changed, data);

    obs_properties_add_list(ppts, OPT_DEVICE_LIST, TEXT_DEVICE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    cp = obs_properties_get(ppts, OPT_DEVICE_LIST);
    if (plugin) {
        Device* dev;
#ifndef _DISABLE_ADB
        AdbMgr *adbMgr = &plugin->adbMgr;
#endif
        USBMux* iosMgr = &plugin->iosMgr;
        MDNS  *mdnsMgr = &plugin->mdnsMgr;
#ifndef _DISABLE_ADB
        adbMgr->ResetIter();
        while ((dev = adbMgr->NextDevice()) != NULL) {
            char *label = dev->model[0] != 0 ? dev->model : dev->serial;
            size_t idx = obs_property_list_add_string(cp, label, dev->serial);
            if (adbMgr->DeviceOffline(dev))
                obs_property_list_item_disable(cp, idx, true);
        }
#endif

        iosMgr->ResetIter();
        while ((dev = iosMgr->NextDevice()) != NULL) {
            char *label = dev->model[0] != 0 ? dev->model : dev->serial;
            obs_property_list_add_string(cp, label, dev->serial);
        }

        mdnsMgr->ResetIter();
        while ((dev = mdnsMgr->NextDevice()) != NULL) {
            char *label = dev->model[0] != 0 ? dev->model : dev->serial;
            obs_property_list_add_string(cp, label, dev->serial);
        }
    }

    obs_property_list_add_string(cp, TEXT_USE_WIFI, opt_use_wifi);
    obs_properties_add_button(ppts, OPT_REFRESH, TEXT_REFRESH, refresh_clicked);
    cp = obs_properties_add_button(ppts, OPT_CONNECT, TEXT_CONNECT, connect_clicked);

    obs_properties_add_text(ppts, OPT_WIFI_IP, "WiFi IP", OBS_TEXT_DEFAULT);
    obs_properties_add_int(ppts, OPT_APP_PORT, "DroidCam Port", 1, 65535, 1);

    obs_properties_add_bool(ppts, OPT_ENABLE_AUDIO, TEXT_ENABLE_AUDIO);
    // obs_properties_add_bool(ppts, OPT_SYNC_AV, TEXT_SYNC_AV);
    #if DROIDCAM_OVERRIDE==0
    obs_properties_add_bool(ppts, OPT_DEACTIVATE_WNS, TEXT_DWNS);
    #endif
    obs_properties_add_bool(ppts, OPT_USE_HW_ACCEL, TEXT_USE_HW_ACCEL);

    if (activated) {
        toggle_ppts(ppts, false);
        obs_property_set_description(cp, TEXT_DEACTIVATE);
    }

    return ppts;
}

void source_defaults(obs_data_t *settings) {
    obs_data_set_default_bool(settings, OPT_DUMMY_SOURCE, false);
    obs_data_set_default_bool(settings, OPT_UHD_UNLOCK, false);
    obs_data_set_default_bool(settings, OPT_IS_ACTIVATED, false);
    obs_data_set_default_bool(settings, OPT_SYNC_AV, false);
    obs_data_set_default_bool(settings, OPT_USE_HW_ACCEL, true);
    obs_data_set_default_bool(settings, OPT_ENABLE_AUDIO, false);
    obs_data_set_default_bool(settings, OPT_DEACTIVATE_WNS, false);
    obs_data_set_default_int(settings, OPT_APP_PORT, DEFAULT_PORT);
}
