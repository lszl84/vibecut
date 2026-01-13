#!/bin/bash

# Generate a 24fps test video with frame numbers displayed
# Each frame shows its frame number in the center

# Output: test_24fps_numbered.mp4
# Duration: 5 seconds (120 frames)

OUTPUT_DIR="$(dirname "$0")"
OUTPUT_FILE="$OUTPUT_DIR/test_24fps_numbered.mp4"

ffmpeg -y \
    -f lavfi \
    -i "color=c=black:s=640x360:r=24:d=5" \
    -vf "drawtext=fontfile=/usr/share/fonts/TTF/DejaVuSans.ttf:text='Frame %{frame_num}':fontsize=72:fontcolor=white:x=(w-text_w)/2:y=(h-text_h)/2:start_number=0" \
    -c:v libx264 \
    -preset ultrafast \
    -crf 18 \
    "$OUTPUT_FILE"

echo "Created: $OUTPUT_FILE"
echo "Duration: 5 seconds, 24fps, 120 frames total"

