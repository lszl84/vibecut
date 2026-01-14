# VibeCut TODO

## Known Bugs

### Bug #3: Export timing/frame issues

EXPORT: Buffered frame 118 (buffer size: 1)
EXPORT: Encoding source frame 118 as output frame 118
EXPORT: Writing packet #117 PTS=59904 DTS=59904
EXPORT: Got frame PTS=60928 time=4.958333 source_idx=119, want [0, 120)
EXPORT: Buffered frame 119 (buffer size: 1)
EXPORT: Encoding source frame 119 as output frame 119
EXPORT: Writing packet #118 PTS=60416 DTS=60416
EXPORT: Flushing decoder for remaining frames...
EXPORT: Clip done - skipped 0 frames, encoded 120 frames
EXPORT: Flush packet PTS=60928 DTS=60928

why are we writing packet #118 as the last packet? what happens with received source frame no 119? The last flush "EXPORT: Flush packet PTS=60928 DTS=60928" - flushes the NULL data, so we are missing the last frame here? race condition?

Try to write the PNGs when reading ("EXPORT: Buffered Frame...") and writing ("EXPORT: Writing packet.../EXPORT: Flush packet...") to see what actuall gets read and written.

But that might not be the culprit - also frame 91 content is duplicated in the exported video. So exporting read/written PNGs should help with investigation.