# VibeCut TODO

## Known Bugs

### Bug #3: Export produces incorrect video despite correct frame data

**Status:** Unsolved after extensive debugging

**Symptoms:**
- Exported PPM frames are 100% correct
- Exported video has wrong frames (missing last frame, frame 91 duplicated)
- Same RGB data used for both PPM and video encoding

**What we tried:**
1. Frame buffer reordering - didn't help
2. Decoder flush at EOF - didn't help
3. Single-threaded decode/encode - didn't help
4. Fresh frame allocation per encode - didn't help
5. av_write_frame instead of av_interleaved_write_frame - didn't help
6. zerolatency tune, gop_size=1, no B-frames - didn't help
7. Encoding from exact same RGB bytes as PPM - didn't help
8. VA-API hardware encoding - didn't help
9. Various containers (MP4, MKV, MOV) - didn't help
10. Various codecs (H.264, FFV1, ProRes) - didn't help

**Current state:**
- PPM export works perfectly (correct frames saved)
- Video export uses the SAME RGB data but produces wrong video
- The issue is somewhere in: RGB→YUV conversion, libx264 encoder, or MP4 muxer

**Next steps to try:**
- Use MJPEG codec (each frame is independent JPEG)
- Write raw YUV file and encode with ffmpeg CLI
- Try different FFmpeg version
- Check if issue is specific to the test video

**Log sample:**
```
EXPORT: Decode src=118 -> encode out=118
EXPORT: Sent frame PTS=118 to encoder (ret=0)
EXPORT: Wrote packet #117, PTS=...
EXPORT: Decode src=119 -> encode out=119  
EXPORT: Sent frame PTS=119 to encoder (ret=0)
EXPORT: Wrote packet #118, PTS=...
EXPORT: Flushing encoder...
EXPORT: Flush wrote packet #119, PTS=...
```
Despite all 120 frames being sent and 120 packets written, the output video is incorrect.
