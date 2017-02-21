`pasane` works like `amixer set Master 3%+`, except for the fact
that it doesn't totally mess up your channel balance. e.g.

```
pasane 3+
pasane 3-
```

If you have multiple devices, you can use a regex to locate them,
instead of the totally messed up definition of "Master" in ALSA:

```
pasane --source '5\.1' 3+
```

In fact, you can specify some balance profiles in a config file
(`~/.config/pasane.yml` or `env PASANE_CONFIG_FILE`), and use them to
select a non-flat profile if you have any idea what ALSA is doing, and
you like it (you sicko). e.g.

```yaml
balance_profiles:
  - flat:
    - 100% .*
  - boost_centre:
    - 120% Center
    - 100% .*
  - only_centre:
    - 100% Center
    - 5% .*
```

A profile consists of:

 * a name, for your usage (to be passed on the command line)
   * a percentage weighting for this channel (group)
   * a regex which matches the channel name

All of a profile's items are applied (in order) until one matches, otherwise
the `100% .*` profile is used.

The tool doesn't let you set values to weird values (it'll print warnings while
clipping things) to prevent PulseAudio from spazzing out.

(I am not very happy.)
