# VibeCut TODO

# Connected Clips (Implemented)
- ~~the timeline's size should be much larger (vertically) to allow connecting clips to the main timeline~~
- ~~clips can be connected from the clip list using key Q~~
- ~~clips should connect to the current frame indicated by the playhead~~
- ~~the connected clip should appear above the main timeline~~
- ~~the connected clip should have a connection line from the connection point (the beginning of the clip) to the timeline~~
- ~~the connected clip should be resizable just like a regular clip~~
- ~~connected clips can be moved by the user by dragging~~
- ~~connected clips can be selected and deleted~~
- ~~when the main timeline plays and the connected clip is encountered, the connected clip should play~~
- ~~the pixel height of connected clips should be the same as the clips in the main timeline~~
- connected clips can be placed above or below the main timeline by the user (TODO: UI for changing lane)
- ~~export with connected clips compositing~~
- ~~fix connected clip jumping behavior when trimming main clips~~

# Connected Clips - behavior when trimming the parent clip (Partially Implemented)
- ~~when resizing the parent clip, the connection points should in general stay at the same frame of both the parent clip and connected clip~~
- ~~if trimming the parent clip from the left side: when the left side of the parent clip passes after the connection point, then the connection point should move with the parent clip's left edge. The connection point slides along the connected clip~~
- ~~if trimming the parent clip from the left and the parent clip is the first clip in the timeline: the part of the connected clip that is before the parent's left edge should be greyed out and not played~~
- ~~if the parent clip is not 1st: that part of the connected clip will hover over the previous clip. Parts before the first clip are greyed out~~
- ~~when trimming the parent clip from the right and the handle passes the connection point, then the connection point moves with the handle~~
- TODO: if during right-trim, the handle moves past the beginning of the connected clip, a special "empty" clip should be added to the main timeline
- TODO: the empty clip is only permanently added when the user releases the mouse while it is visible


# Undo Stack
- implement infinite undo stack for all the operations that are saved in the project
- use standard keyboard keys to undo and redo

# Audio Clips