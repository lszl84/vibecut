# Connected Clips - Behavioral Specification

> **Reference**: This feature is designed to mirror the behavior of **Apple Final Cut Pro's Connected Clips**. When in doubt about expected behavior, Final Cut Pro is the reference implementation.

Connected Clips are overlay clips that play on top of the main timeline, allowing you to layer additional video content at specific points. They remain attached and synced to their parent clip until explicitly moved or removed.

## Common Uses

- **Cutaway shots**: Add a cutaway by connecting it to a video clip in the timeline
- **Superimposed titles**: Add titles or lower thirds to video clips
- **Sound effects**: Sync audio clips to clips in the primary timeline (future feature)
- **Background music**: Layer music that stays synced with the edit (future feature)

## Overview

A connected clip is attached to the main timeline at a specific frame called the **connection point**. The clip displays in lanes above or below the main track, with a visual line connecting it to the main timeline.

### Key Concepts

- **Connection Frame**: The timeline frame where the anchor point is located
- **Connection Offset**: How many frames into the connected clip the anchor is (0 = left edge of clip)
- **Timeline Start**: Where the clip visually begins on the timeline (`connection_frame - connection_offset`)
- **Lane**: Vertical position relative to main track (positive = above, negative = below)
- **Gap Clip**: An empty placeholder clip in the main timeline that connected clips can attach to (pending feature)

## Creating Connected Clips

1. Select a source in the library panel
2. Position the playhead at the desired connection point on the timeline
3. Press **Q** to connect the clip

The clip will automatically be placed in an available lane to avoid overlap with existing connected clips.

> **Note**: In Final Cut Pro, video clips connect above the primary storyline by default, and audio clips connect below. VibeCut currently only supports video connected clips.

## Visual Representation

### Lanes
- **Positive lanes** (1, 2, ...): Displayed above the main timeline track
- **Negative lanes** (-1, -2, ...): Displayed below the main timeline track
- Higher lane numbers appear further from the main track

### Connection Anchor
- A vertical line connects the clip to the main timeline
- A small circle marks the anchor point on the main track
- The anchor can be at any position within the clip (not just the left edge)

### Greyed-Out Regions
When part of a connected clip extends before timeline frame 0:
- That portion is rendered with a darker color and diagonal stripe pattern
- This indicates the frames will not play during playback or export

## Interaction

### Selection
- Click on a connected clip to select it
- Selected clips show a gold/yellow highlight border

### Moving
- Drag the clip body (not the handles) to reposition
- The clip moves to a new connection point on the timeline

### Resizing
- Drag the **left handle** to trim the start (adjusts `start_frame`)
- Drag the **right handle** to trim the end (adjusts `end_frame`)
- The connection point remains stable during resizing

### Deletion
- Select the clip and press **Backspace** to delete

### Adjusting Connection Point (Pending Feature)
In Final Cut Pro, you can move the connection point within a connected clip:
- Hold **Command+Option** and click on the connected clip at the desired frame
- The connection point moves to that frame within the clip

This allows you to control which frame of the connected clip is anchored to the parent.

## Relationship with Parent Clips

Connected clips stay synced with their parent clip in the main timeline:

- **Moving parent**: When you move a clip in the main timeline, connected clips move with it
- **Deleting parent**: When you delete a parent clip, connected clips attached to it are also deleted
- **Reordering clips**: Connected clips follow their parent when clips are reordered

### Editing Without Affecting Connected Clips (Pending Feature)
In Final Cut Pro, holding the **Grave Accent (`) key** while editing allows you to:
- Move a primary clip without moving connected clips
- Trim a primary clip without affecting connected clips
- Delete a primary clip while preserving connected clips

This feature is not yet implemented in VibeCut.

## Playback Behavior

### Priority Rules
1. Connected clips override main timeline content at their position
2. **Topmost lane wins**: If multiple connected clips overlap, the one in the highest positive lane takes priority
3. Only clips in **positive lanes** play video (negative lanes are reserved for future audio features)

### Timeline Boundaries
- Parts of connected clips extending before timeline frame 0 are **skipped** during playback
- Parts extending over other main clips play normally (they override those clips)

### Extended Timeline
When connected clips extend past the end of the main timeline:
- **Playback continues** until the furthest connected clip ends
- The main video shows black, but connected clips play their content
- **Export includes** all frames up to the end of the furthest connected clip
- Audio from extended connected clips is included in the mix

This ensures that connected clips that overlap the end of the main timeline are fully played and exported.

## Trimming Parent Clips

This is the most complex aspect of connected clips behavior.

### General Rule
When resizing a parent clip on the main timeline, connection points stay at the **same source frame** in both the parent clip and the connected clip.

### Left-Trim Behavior

When trimming the left edge of a parent clip:

1. **Normal case**: The connected clip moves on the timeline to stay at the same source frame position in the parent
2. **Edge passes connection**: When the left edge moves past the original connection point's source frame:
   - The connection anchor sticks to the parent's left edge
   - The anchor slides along the connected clip (connection_offset increases)
   - The connected clip visually extends before the connection point

**First clip special case**: If the parent is the first clip in the timeline:
- Parts of connected clips extending before timeline 0 are greyed out and not played

**Non-first clip case**: If there are clips before the parent:
- Parts extending over previous clips hover above them and play normally
- Only parts extending before the very first clip (timeline 0) are greyed out

### Right-Trim Behavior

When trimming the right edge of a parent clip:

1. **Normal case**: The connected clip stays at its position (source frame unchanged)
2. **Edge passes connection**: When the right edge moves past the connection point's source frame:
   - The connection anchor sticks to the parent's right edge
   - The anchor slides along the connected clip (connection_offset becomes negative)
   - The connected clip visually extends past the parent into the next clip's area

### Gap Clip Insertion

When right-trimming causes the handle to move past the **beginning** of a connected clip:
- A **gap clip** (empty placeholder) is automatically inserted in the main timeline after the parent
- The connected clip becomes attached to this gap clip
- The gap clip is only permanently added when the mouse is released while visible
- If the user moves the handle back before releasing, the gap clip is removed

Gap clips are rendered with a dark color and diagonal stripe pattern, labeled "Gap".

> **Final Cut Pro behavior**: Gap clips are empty clips that serve as anchors for connected clips. They can be inserted manually with **Option-W** and act as placeholders in the timeline.

## Export

Connected clips are composited into the exported video:

- Frame-by-frame compositing based on timeline position
- Same priority rules as playback (topmost positive lane wins)
- Greyed-out regions (before timeline 0) are not included in export

## Pending Features Summary

The following Final Cut Pro behaviors are not yet implemented:

1. **Adjust connection point**: Command+Option+Click to move anchor within connected clip
2. **Grave Accent key**: Edit parent clips without affecting connected clips
3. **Lift vs Delete**: Shift+Delete to replace clip with gap, preserving connected clips
4. **Audio connected clips**: Negative lanes for audio (currently visual only)
5. **Manual gap clip insertion**: Option-W to insert gap clips manually

## Project Persistence

Connected clips are saved and loaded with project files. All properties are persisted:
- Source ID
- Start/end frames
- Connection frame and offset
- Lane assignment
- Color ID
