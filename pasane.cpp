#include <stdio.h>
#include <string.h>

#include <getopt.h>

#include <pulse/pulseaudio.h>
#include <regex.h>
#include <wordexp.h>

#include "parse.h"

pa_mainloop *mainloop = NULL;
pa_mainloop_api *api = NULL;
pa_context *context = NULL;
regex_t sink_regex = {};

uint16_t pending_operations = 0;

static const char *show_null(const char *val) {
    return val ? val : "(null)";
}

void volume_cb(pa_context *context, int success, void *userdata) {
    assert(context);
    if (success) {
        printf("Volume set\n");
    } else {
        printf("Volume not set\n");
    }
    --pending_operations;
}

void sink_list(pa_context *context, const pa_sink_info *info, int eol, void *userdata) {
    assert(context);
    if (eol < 0) {
        fprintf(stderr, "unknown error listing sinks");
        return;
    }

    if (eol) {
        --pending_operations;
        return;
    }

    assert(info);
    char full_name[2048];
    snprintf(full_name, sizeof(full_name), "%02u. <%s> %s (%d channels)",
           info->index, info->name,
           info->description ? info->description : "(no description)",
           info->channel_map.channels);

    int match_failed = regexec(&sink_regex, full_name, 0, NULL, 0);
    if (match_failed) {
        // not some other error
        assert(REG_NOMATCH == match_failed);
    }

    if (match_failed) {
        return;
    }

    printf("%s:\n", full_name);

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
    ++pending_operations;
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


int main(int argc, char *argv[]) {
    int ret = 1;

    char *sink_search = strdup(".");
    char *balance_file_path = NULL;
    char *balance_spec = NULL;
    char *client_name = NULL;

    while (true) {
        static struct option long_options[] = {
                {"sink",    required_argument, 0,  0 },
                {"balance", required_argument, 0,  0 },
                {0,         0,                 0,  0 }
        };
        int option_index = 0;
        int c = getopt_long(argc, argv, "s:b:", long_options, &option_index);

        if (-1 == c) {
            break;
        }

        switch (c) {
            case 0:
                free(sink_search);
                sink_search = strdup(optarg);
                break;
            case 1:
                free(balance_spec);
                balance_spec = strdup(optarg);
                break;

            case '?':
                fprintf(stderr, "Usage: %s [--sink regex-for-sink] [--balance balance-specification] command\n", argv[0]);
                goto done;

            default:
                fprintf(stderr, "unrecognised getopt return value: %d\n", c);
                goto done;
        }
    }

    {
        wordexp_t exp_result;
        const char *balance_file_path_raw = "~/.config/pasane.yml";
        const char *env_file = getenv("PASANE_CONFIG_FILE");
        if (env_file) {
            balance_file_path_raw = env_file;
        }
        int failure = wordexp(balance_file_path_raw, &exp_result, 0);
        if (failure) {
            fprintf(stderr, "invalid path: '%s': man:wordexp(3): %d\n", balance_file_path_raw, failure);
            goto done;
        }
        if (1 != exp_result.we_wordc) {
            fprintf(stderr, "path matched multiple files or something: '%s' (%lu)\n",
                    balance_file_path_raw, exp_result.we_wordc);
            goto done;
        }

        balance_file_path = strdup(exp_result.we_wordv[0]);
        assert(balance_file_path);
    }

    parse(balance_file_path);

    if (const int regex_status = regcomp(&sink_regex, sink_search, REG_EXTENDED | REG_ICASE)) {
        size_t required_size = regerror(regex_status, &sink_regex, NULL, 0);
        char *buf = (char *) malloc(required_size);
        assert(buf);

        regerror(regex_status, &sink_regex, buf, required_size);
        fprintf(stderr, "sink regex problem: %s\n", buf);
        free(buf);
        goto done;
    }

    mainloop = pa_mainloop_new();
    if (!mainloop) {
        perror("couldn't get mainloop");
        goto done;
    }
    api = pa_mainloop_get_api(mainloop);
    if (!api) {
        perror("couldn't get api");
        goto done;
    }

    client_name = pa_xstrdup("pasane");
    assert(client_name);

    context = pa_context_new(api, client_name);
    if (!context) {
        perror("couldn't get context");
        goto done;
    }

    pa_context_set_state_callback(context, context_state_callback, mainloop);

    if (pa_context_connect(context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        fprintf(stderr, "pa_context_connect() failed: %s", pa_strerror(pa_context_errno(context)));
        goto done;
    }

    ++pending_operations;

    while (pending_operations != 0) {
        const int iterate_result = pa_mainloop_iterate(mainloop, true, &ret);
        if (iterate_result <= 0 && ret) {
            fprintf(stderr, "pa_mainloop_iterate() failed: %d; ret: %d.\n", iterate_result, ret);
            goto done;
        }
    }

    pa_context_disconnect(context);

    done:
    if (context) {
        pa_context_unref(context);
    }
    pa_signal_done();
    if (mainloop) {
        pa_mainloop_free(mainloop);
    }

    free(client_name);
    free(sink_search);
    free(balance_spec);
    free(balance_file_path);
    regfree(&sink_regex);
    return ret;
}
