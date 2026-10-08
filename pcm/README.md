# PCM producer

`pcm` needs the optional named `audio` PLAYBACK grant and a readable/sleepable
`clock` grant. It produces fixed 48 kHz S16LE stereo samples without reading
stdin. Playback continues while its space is hidden or another space has input
focus. Each space's audio session has one process owner; a second producer in
that space reports the acquisition failure.

```sh
pcm 1000 500 5
pcm --frames 48000 1000 500
pcm 1000 0 3
pcm 0 0 3
pcm --pause-ms 500 1000 500 5
pcm --repeat 3 --gap-ms 500 1000 500 2
```

The frequencies independently select left and right triangle waves with peak
amplitudes 8192 and 4096. Zero Hz means silence for that channel. Sample phases
continue across writes and repeats, and remain still during an explicit pause or
gap. `--frames` selects an exact number of frames instead of the final integer
seconds argument. Frequencies are 0 through 23999 Hz, duration is at most 3600
seconds per run, and repeats are 1 through 64.

`--pause-ms` pauses submission once halfway through each run while retaining the
session; a pause longer than the queued audio can cause starvation. `--repeat`
releases and reacquires the session for each run. `--gap-ms` waits between those
sessions. Pauses, gaps and tail waits are at most 60000 ms.

Writes copy at most 4096 bytes atomically. A full queue reports `WOULD_BLOCK` and
accepts nothing; the producer waits for `WAIT_WRITABLE` and retries the same
bytes. Diagnostics report session generation, queue capacity/free frames,
starvation/discontinuity counters and backend state, plus cumulative writes,
queue-full responses, waits and elapsed
submission time per run. These counters describe production, not an audible play
cursor or output latency.

After submission, `--tail-ms` waits before releasing; its default is 200 ms.
Elapsed time does not guarantee audible drain. Release discards any remaining
queued input, and already mixed frames can outlive it. `--tail-ms 0` releases
immediately. The application never starts automatically at boot.
