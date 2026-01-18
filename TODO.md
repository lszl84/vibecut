# VibeCut TODO

## The Project Feature
- When opening the program, we should have three buttons: New Empty Project, Open Project, New Project From Video. The first two are obvious, the third lets the user select a video and creates a new project with that video in the timeline (done)
- The window title bar should show the path of the current project file (done)
- The "Library" text on the left should be replaced with "Project" (done)
- "Add..." should say "Add a Clip..." (done)
- There should be buttons to save the current project and load a different project (alongside the add button) (done)
- The Preview button should start playing the selected clip from the library. If the user hits space, OR starts interacting with the timeline, the preview play should stop (done)
- There should be an ability to remove a clip from the library (done)
- There should be a confirmation dialog when the user tries to exit the program with unsaved changes to the project (done)
- The buttons to Export/PNG/open File from the bottom should be removed (done)
- The Export button should be in the Project panel (done)
- The Project Panel currently overlays on top of the preview UI. Instead, these two views should be just next to each other (done)
- There should be no compiler warnings (done)
- We need a separate Save dialog with correct features - right now the user is not even able to type a file name for the project being saved (done)
- the export button should be in the button group below "Project" (done)
- the buttons below "Project" have cut off text (done)
- the vertical margin below the timeline is too big. (done)
- the exit confirmation dialog should be a native dialog, just like our open file dialog (done)
- add a "New Project" button in the Project group (done)
- add a tiny vertical margin just above the timeline (done)
- the save dialog layout is wrong. The bottom buttons are cut off, no matter how much I resize that window (done)
- the zoom level should be saved in a project file and restored when the project is loaded (done)
- when the "unsaved changes" dialog shows up, the main window should be inactive and darkened a bit, so that the user cannot click inside it (done)
- when a clip is selected, we should have a button saying "Insert to Timeline" that should do the same thing as the key W (done)
- zoom is still not preserved (done)
- we can make the window a bit wider by default so that the Project section and the 16:9 preview all fit nicely (done)
- the buttons for clip are cut off, so we need better ui for these (done)

19 Jan 2026:
- the Export progress bar can become stuck just before the end - bad UX experience, looks like the app hangs, even though it doesnt
- the last frame is missing from the export!