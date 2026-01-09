# VibeCut TODO

## Bugs

### Bug #1: Playback gets stuck in middle clips (non-deterministic)
When playing from a middle clip, sometimes the playhead doesn't advance to the next clip after the current clip ends. This happens intermittently and is non-deterministic - works for some clips but not others.

**Likely cause**: Race condition or timing issue in the `update()` function when checking clip boundaries and transitioning between clips.

### Bug #2: Blocky transitions when exporting multiple clips
When exporting a video with multiple clips, the transitions between clips appear blocky and look bad.

**Likely cause**: Stream copy without re-encoding means clips may not start/end on keyframes. The decoder doesn't have reference frames at clip boundaries, causing visual artifacts. 

**Potential fix**: Re-encode the video (at least at clip boundaries) or ensure clips are cut at keyframe positions.

## Future Improvements
- Delete clips (not just trim them)
- Drag clips to reorder
- Undo/redo functionality

