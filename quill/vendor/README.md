# vendor/

quill links against **`libqsgepaper.so`**, reMarkable's proprietary e-ink
waveform engine. It is **not** included in this repo — it's reMarkable's
copyrighted software, and redistributing it isn't ours to do.

You already have a copy on your own tablet. Copy it from there:

```sh
scp root@10.11.99.1:/usr/lib/plugins/scenegraph/libqsgepaper.so vendor/
```

(On some OS versions it lives elsewhere under `/usr/lib` — find it with
`ssh root@10.11.99.1 'find /usr/lib -name libqsgepaper.so'`.)

That's a personal copy from a device you own, for building software that runs on
that same device — not redistribution. With the file in place, `./build.sh`
links against it.
