#!/bin/sh
# Send a command to Write or WriteTest running w/ --automation <fifo> and print the reply, e.g.
#   mkfifo /tmp/w; WriteTest --automation /tmp/w &
#   uidrive.sh /tmp/w dump
#   uidrive.sh /tmp/w 'click "button:OK"'
#   uidrive.sh /tmp/w quit
# reply ends w/ an empty line; see `uidrive.sh <fifo> help` for commands
fifo=$1
shift
out="$fifo.out"
[ -p "$fifo" ] || { echo "uidrive.sh: $fifo is not a FIFO" >&2; exit 2; }
start=$(wc -c < "$out")
printf '%s\n' "$*" > "$fifo"
# wait (up to 60 s) for reply to be complete, i.e. for empty line after start
i=0
while [ $i -lt 1200 ]; do
  if tail -c +$((start + 1)) "$out" | grep -q '^$'; then
    tail -c +$((start + 1)) "$out" | sed '/^$/q'
    exit 0
  fi
  sleep 0.05
  i=$((i + 1))
done
echo "uidrive.sh: no reply" >&2
exit 1
