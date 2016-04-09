#include <stdio.h>

#include <pulse/pulseaudio.h>

pa_mainloop *mainloop;
pa_mainloop_api *api;
pa_context *context;

void volume_cb(pa_context *context, int success, void *userdata) {
    assert(context);
    if (success) {
        printf("Volume set\n");
    } else {
        printf("Volume not set\n");
    }
    pa_context_disconnect(context);
}

void sink_list(pa_context *context, const pa_sink_info *info, int eol, void *userdata) {
    assert(context);
    if (eol < 0) {
        fprintf(stderr, "unknown error listing sinks");
        return;
    }

    if (eol) {
        return;
    }

    assert(info);
    printf("sink #%u <%s> (%d channels) %s:\n",
           info->index, info->name,
           info->channel_map.channels,
           info->description ? info->description : "(null)");

    for (int i = 0; i < info->volume.channels; ++i) {
        char vol_val[PA_VOLUME_SNPRINT_MAX];
        pa_volume_snprint(vol_val, sizeof(vol_val), info->volume.values[i]);

        const char *pos_name = pa_channel_position_to_pretty_string(info->channel_map.map[i]);
        printf("%20s - %s (%0.5f -> %0.5f)\n", pos_name, vol_val,
               pa_sw_volume_to_linear(info->volume.values[i]),
               info->volume.values[i] / (double) PA_VOLUME_NORM
        );

    }

    pa_cvolume v;
    pa_cvolume_init(&v);
    v.channels = info->volume.channels;
    for (int i = 0; i < info->volume.channels; ++i) {
        v.values[i] = (pa_volume_t) (0.20 * PA_VOLUME_NORM);
    }

    pa_context_set_sink_volume_by_index(context, info->index, &v, volume_cb, NULL);
}

static void context_state_callback(pa_context *context, void *userdata) {
    assert(context);

    switch (pa_context_get_state(context)) {
        case PA_CONTEXT_TERMINATED:
            pa_mainloop_quit(mainloop, 0);
            break;

        case PA_CONTEXT_READY:
            pa_operation_unref(pa_context_get_sink_info_list(context, sink_list, NULL));
            break;

        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_FAILED:
        default:
            pa_mainloop_quit(mainloop, 2);
            break;
    }
}


int main() {
    mainloop = pa_mainloop_new();
    if (!mainloop) {
        perror("couldn't get mainloop");
        return 1;
    }
    api = pa_mainloop_get_api(mainloop);
    if (!api) {
        perror("couldn't get api");
        pa_mainloop_free(mainloop);
        return 2;
    }

    char *client_name = pa_xstrdup("pasane");
    assert(client_name);

    context = pa_context_new(api, client_name);
    if (!context) {
        perror("couldn't get context");
        pa_mainloop_free(mainloop);
        return 3;
    }

    int ret = 1;
    pa_context_set_state_callback(context, context_state_callback, mainloop);

    if (pa_context_connect(context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        fprintf(stderr, "pa_context_connect() failed: %s", pa_strerror(pa_context_errno(context)));
        goto done;
    }

    if (pa_mainloop_run(mainloop, &ret) < 0) {
        fprintf(stderr, "pa_mainloop_run() failed.\n");
        goto done;
    }

    done:
    pa_context_unref(context);
    pa_signal_done();
    pa_mainloop_free(mainloop);
    return ret;
}
