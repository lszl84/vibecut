# Project Instructions

## Feature Documentation
When working on specific features, consult the relevant documentation:
- **Connected Clips**: See [docs/CONNECTED_CLIPS.md](docs/CONNECTED_CLIPS.md) for behavioral specification (mirrors Apple Final Cut Pro behavior)

## Code Style
- use modern C++23 features
- prefer linear procedural/functional style over traditional OOP patterns
- prefer std::expected over exceptions
- prefer data-oriented programming with code/data separation over traditional OOP
- prefer simple and clear code over abstraction overuse
- do make use of modern c++ standard library in an idiomatic way
- prefer a small number of C++23 modules grouping similar functionality over splitting stuff into many files
- use comments sparingly

## Architecture
- CMake
- C++23
- GNU/Linux X11
- OpenGL 4
- GLFW
- ImGui
- ffmpeg via libav

