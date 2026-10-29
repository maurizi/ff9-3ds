#!/bin/sh
# Record what melonDS plays during a DS bench run (build/validation/dsmode_bench_w.sh) into a WAV, through a null
# PulseAudio sink so nothing reaches the speakers: melonDS's stream is moved to the sink as soon as it appears.
# usage: tools/ds_audio_capture.sh OUT.wav FIELD [CFGDIR]   (OUT under build/; the sink: SINK, default ff9_emu)
set -e
out=$1; field=$2; cfgdir=${3:-fieldsweep}; sink=${SINK:-ff9_emu}
case "$out" in build/*) ;; *) echo "OUT must be under build/"; exit 1;; esac
pactl list short sinks | grep -q "	$sink	" || pactl load-module module-null-sink sink_name="$sink" > /dev/null
rm -f "$out"
# real time for the run (the benches run unthrottled): melonDS's frame limiter on, the configuration restored after
conf=$HOME/.var/app/net.kuribo64.melonDS/config/melonDS/melonDS.toml
cp "$conf" "$out.melonDS.toml"
trap 'cp "$out.melonDS.toml" "$conf"; rm -f "$out.melonDS.toml"' EXIT
sed -i 's/^LimitFPS = .*/LimitFPS = true/' "$conf"
# (and paced by its audio output: every emulated sample reaches the recording, in emulated time, however fast this
# machine runs it; a wall-clock recording stretches when melonDS can't hold full speed)
sed -i 's/^AudioSync = .*/AudioSync = true/' "$conf"
parec -d "$sink.monitor" --file-format=wav --rate=48000 --channels=2 --format=s16le "$out" & rec=$!
( # move melonDS's stream to the sink whenever one shows up on another sink
  while kill -0 $rec 2>/dev/null; do
    pactl list sink-inputs | awk -v s="$sink" '/^Sink Input #/ {id=substr($3,2)} /application.process.binary = "melonDS"/ {print id}' |
      while read -r id; do pactl move-sink-input "$id" "$sink" 2>/dev/null || true; done
    sleep 0.2
  done ) & mover=$!
MELON_FLATPAK_ARGS="--env=SDL_AUDIODRIVER=pulseaudio" FIELDS="$field" CFGDIR="$cfgdir" SUFFIX=_audcap \
  sh build/validation/dsmode_bench_w.sh
kill $rec 2>/dev/null || true
wait $mover 2>/dev/null || true
echo "recorded $out"
