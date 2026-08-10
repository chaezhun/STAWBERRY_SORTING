#!/bin/sh
# Streams detection results from the AI board to the controller.
echo "===================================="
echo "  starting detection stream"
echo "  D3-G: 192.168.10.101:5000"
echo "===================================="

tcnnapp \
    -n /usr/share/strawberry_shifted_v2_quantized/ \
    -i camera \
    -p /dev/video2 \
    -o display \
    -w 1280 -h 720 \
    -W 800 -H 480 \
    -g log 2>&1 | \
# Match only the raw detection lines, so the post-process summary is not sent twice.
awk '/x1 y1 x2 y2 =.*class:.*score:/ {
    cls=0; score=0; eq=0
    for(i=1;i<=NF;i++) {
        if($i=="=")      eq=i
        if($i=="class:") cls=$(i+1)+0
        if($i=="score:") score=$(i+1)+0
    }
    # remap the NPU class index onto the project classes
    if(cls==2) m=0          # fresh
    else if(cls==3) m=2     # unripe
    else if(cls==4) m=1     # rotten
    else next               # anything else is a dummy class, drop it
    # bounding box centre in source pixels: the four values after "="
    cx = ($(eq+1) + $(eq+3)) / 2
    cy = ($(eq+2) + $(eq+4)) / 2
    printf "CLASS:%d,CONF:%d,X:%d,Y:%d,W:0,H:0\n", m, score, cx, cy
    fflush()
}' | nc 192.168.10.101 5000

echo "[info] stopped"
