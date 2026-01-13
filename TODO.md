# VibeCut TODO

## Known Bugs

### Bug #3: Export timing precision (partially fixed)
- ~~Major issues with missing frames resolved~~
- Precise frame-accurate timing may need further work
- Will address with timeline navigation improvements

### ~~Bug #4: Up/Down arrow navigation inconsistent with very short clips~~ ✓ MOSTLY FIXED
- When clips are 1 frame each, up/down navigation behaves erratically
- **Fix**: Use 1.5x frame duration for left arrow to compensate for framerate metadata mismatches
- Navigation now works correctly for all frame positions
- See open/closed interval issue below for remaining edge case

## Fixed Bugs

### ~~Bug #1: Playback gets stuck in middle clips (non-deterministic)~~ ✓ FIXED
- **Fix**: Track `active_clip` explicitly during playback instead of deriving from current_time
- Set active_clip when play() is called based on current position
- Use active_clip index in update() to check clip boundaries

### ~~Bug #2: Blocky transitions when exporting multiple clips~~ ✓ FIXED
- **Fix**: Re-encode video instead of stream copy
- Decode source frames and encode to H.264 with proper keyframes
- Audio is still stream-copied (no keyframe issues with audio)
- Output is clean at all clip boundaries

## Future Improvements
- Delete clips (not just trim them)
- Drag clips to reorder
- Undo/redo functionality
- Adjustable export quality/bitrate
- Preview of clip transitions before export

### Open/Closed Interval Issue
- Current behavior: playhead can reach the exact clip end boundary (e.g., 0.082 for clip [0, 0.082])
- With half-open intervals [start, end), position `end` is technically outside the clip
- This causes a visual inconsistency where the playhead appears at the "closed" end
- The last frame's timestamp equals the clip end, making it ambiguous
- **Impact**: Minor visual/conceptual issue; navigation works correctly
- **Future fix**: Consider adjusting clip end to be `last_frame_start + frame_duration` or display playhead at `end - epsilon`
