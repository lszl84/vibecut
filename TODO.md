# VibeCut TODO

## Known Bugs

### Bug #3: Export timing precision (partially fixed)
- ~~Major issues with missing frames resolved~~
- Precise frame-accurate timing may need further work
- Will address with timeline navigation improvements

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
