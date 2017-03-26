#include <stdio.h>
#include <string.h>

#include <cmath>

#include <getopt.h>
#include <regex.h>
#include <wordexp.h>

#include <pulse/pulseaudio.h>

#include "parse.h"

pa_mainloop *mainloop = NULL;
pa_mainloop_api *api = NULL;
pa_context *context = NULL;
regex_t sink_regex = {};

uint16_t pending_operations = 0;

struct CompiledChannelMapping {
    float percentage;
    regex_t name;
};

std::vector<CompiledChannelMapping> mappings;
float adjustment = 0;

static int compile_regex(const char *text, regex_t &regex) {
    const int regex_status = regcomp(&regex, text, REG_EXTENDED | REG_ICASE);
    if (!regex_status) {
        return 0;
    }

    size_t required_size = regerror(regex_status, &regex, NULL, 0);
    char *buf = new char[required_size];

    regerror(regex_status, &sink_regex, buf, required_size);
    fprintf(stderr, "'%s' is an invalid regex: %s\n", text, buf);
    delete[] buf;

    return regex_status;
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

    float channel_target[PA_CHANNELS_MAX];
    for (int chan = 0; chan < info->volume.channels; ++chan) {

        const char *pos_name = pa_channel_position_to_pretty_string(info->channel_map.map[chan]);
        for (const CompiledChannelMapping &mapping : mappings) {
            if (const int regex_failed = regexec(&mapping.name, pos_name, 0, NULL, 0)) {
                assert(REG_NOMATCH == regex_failed);
                channel_target[chan] = 1;
            } else {
                channel_target[chan] = mapping.percentage;
                break;
            }
        }
    }

    float sum = 0;
    int enabled_channels = 0;
    for (int chan = 0; chan < info->volume.channels; ++chan) {
        if (0 != channel_target[chan]) {
            sum += channel_target[chan];
            ++enabled_channels;
        }
    }

    if (!enabled_channels) {
        // TODO: Expected behaviour here?
        enabled_channels = 1;
    }

    sum /= enabled_channels;

    // normalise fractions
    for (int chan = 0; chan < info->volume.channels; ++chan) {
        if (0 != channel_target[chan]) {
            channel_target[chan] /= sum;
        }
    }

    float target_mean = 0;
    for (int chan = 0; chan < info->volume.channels; ++chan) {
        const char *pos_name = pa_channel_position_to_pretty_string(info->channel_map.map[chan]);

        char vol_val[PA_VOLUME_SNPRINT_MAX];
        pa_volume_snprint(vol_val, sizeof(vol_val), info->volume.values[chan]);

        const double normal_volume = info->volume.values[chan] / (double) PA_VOLUME_NORM;
        printf("%20s - %s (%0.5f -> %0.5f). targ: %0.5f\n", pos_name, vol_val,
               pa_sw_volume_to_linear(info->volume.values[chan]),
               normal_volume,
               channel_target[chan]
        );

        if (0 != channel_target[chan]) {
            target_mean += normal_volume / channel_target[chan];
        }
    }

    target_mean /= enabled_channels;
    target_mean += adjustment;

    if (target_mean < 0) {
        fprintf(stderr, "warning: we decided to go negative (%0.2f); muting instead\n", target_mean);
        target_mean = 0;
    }

    pa_cvolume v = {};
    pa_cvolume_init(&v);
    v.channels = info->volume.channels;
    for (int chan = 0; chan < info->volume.channels; ++chan) {
        float factor = target_mean * channel_target[chan];
        if (factor < target_mean / 4) {
            // Doing otherwise seems to cause horrible sound corruption;
            // e.g. if you set all channels to zero but one -> horrible.
            fprintf(stderr, "warning: clamping channel %d up from %0.5f to %0.5f to prevent it sounding like ass\n",
                chan, 100 * factor, 100 * target_mean / 4);
            factor = target_mean / 4;
        }

        pa_volume_t wanted_volume = (pa_volume_t) (factor * PA_VOLUME_NORM);
        if (!PA_VOLUME_IS_VALID(wanted_volume)) {
            fprintf(stderr, "warning: clipping channel %d to maximum value\n", chan);
            wanted_volume = PA_VOLUME_MAX;
        }
        v.values[chan] = wanted_volume;
    }

    pa_operation_unref(pa_context_set_sink_volume_by_index(context, info->index, &v, volume_cb, NULL));
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
    char *balance_spec_name = NULL;
    char *client_name = NULL;

    mapping_t balance_spec = {(ChannelMapping) {1, ".*"}};

    while (true) {
        static struct option long_options[] = {
                {"sink",    required_argument, 0, 0},
                {"balance", required_argument, 0, 0},
                {0, 0,                         0, 0}
        };
        int option_index = 0;
        int c = getopt_long(argc, argv, "s:b:", long_options, &option_index);

        if (-1 == c) {
            break;
        }

        switch (c) {
            case 0:
                switch (option_index) {
                    case 0:
                        free(sink_search);
                        sink_search = strdup(optarg);
                        break;
                    case 1:
                        free(balance_spec_name);
                        balance_spec_name = strdup(optarg);
                        break;
                    default:
                        assert(!"impossible getopt return value");
                }
                break;

            case '?':
                fprintf(stderr, "Usage: %s [--sink regex-for-sink] [--balance balance-specification] command\n",
                        argv[0]);
                goto done;

            default:
                fprintf(stderr, "unrecognised getopt return value: %d\n", c);
                goto done;
        }
    }

    if (optind == argc) {
        // okay
    } else if (optind == argc - 1) {
        const char *const val = argv[optind];
        const size_t len = strlen(val);
        char *endptr = NULL;
        adjustment = strtod(val, &endptr);

        if (len && '-' == val[len - 1]) {
            if (endptr != val + len - 1) {
                fprintf(stderr, "argument must be a number: '%s'\n", val);
                goto done;
            }
            adjustment = -adjustment;
        } else if (len && '+' == val[len - 1]){
            if (endptr != val + len - 1) {
                fprintf(stderr, "argument must be a number: '%s'\n", val);
                goto done;
            }
        } else {
            fprintf(stderr, "argument must have a trailing + or - (for up or down): '%s'\n", val);
            goto done;
        }

        if (fabs(adjustment) >= 1) {
            adjustment /= 100.f;
        }
    } else {
        fprintf(stderr, "too many extra arguments, starting at: '%s'\n", argv[optind]);
        goto done;
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

    if (balance_spec_name) {
        const mappings_t map = parse(balance_file_path);
        mappings_t::const_iterator found = map.find(balance_spec_name);
        if (map.end() == found) {
            fprintf(stderr, "mapping '%s' not found in spec '%s'\n", balance_spec_name, balance_file_path);
            goto done;
        }
        balance_spec = found->second;
    }

    for (const ChannelMapping &mapping : balance_spec) {
        regex_t regex = {};
        if (compile_regex(mapping.name.c_str(), regex)) {
            goto done;
        }
        mappings.push_back(CompiledChannelMapping {mapping.percentage, regex});
    }

    if (compile_regex(sink_search, sink_regex)) {
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
    free(balance_spec_name);
    free(balance_file_path);
    regfree(&sink_regex);

    for (CompiledChannelMapping &mapping : mappings) {
        regfree(&mapping.name);
    }
    return ret;
}
