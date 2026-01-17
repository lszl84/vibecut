# VibeCut TODO

## Notes

- Export issues were traced to preview/loading; export pipeline is correct.
- Non-integer FPS (e.g. 24.2) required timestamp→frame mapping to use floor rounding.

## TODO

- Test export for missing/incorrect last frame (manual verification)
- Verify export output path/location (currently unclear)
- Improve drag-to-end reordering UX (hard to place at end)
- Add visual indicator while dragging a clip (done)
- Manual test pass for clip reordering + play/seek/resize
- Add audio decode + playback (miniaudio or alternative) (done)
- Define desired UX for library layout, project open/save, insert, preview
