# VibeCut TODO

# Timeline UX Improvements
- when changing the clip size by drawing the left or right side, the side that is being moved should move with the mouse,
- when user clicks on a clip in the timeline, this clip should be selected, indicated by the yellow border
- user should be able to delete the selected clip with backspace
- left-trim playhead should not move unless the clip start passes it (bug)

# Connected Clips
- the timeline's size should be much larger (vertically) to allow connecting clips to the main timeline
- clips can be connected from the clip list using key Q
- clips should connect to the current frame indicated by the playhead
- the connected clip should appear above the main timeline
- the connected clip should have a connection line from the connection point (the beginning of the clip) to the timeline
- the connected clip should be resizable just like a regular clip
- connected clips can be placed above or below the main timeline by the user
- connected clips can be moved by the user by dragging
- connected clips can be selected and deleted
- when the main timeline plays and the connected clip is encountered, the connected clip should play (same for export). Clips stack from above - the top clip hides the clip behind it, etc. The clips behind the main timeline normally are not visible (these will be used for audio, or when the main timeline has reduced opacity in the future, etc)
- the pixel height of connected clips should be the same as the clips in the main timeline

# Undo Stack
- implement infinite undo stack for all the operations that are saved in the project
- use standard keyboard keys to undo and redo