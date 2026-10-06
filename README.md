# Groovy Gandalf Project

Keep your Gandalf perfectly on beat.

An audio-reactive C++ video player that synchronizes video loop playback in real time to musical beats captured via audio input using BTrack. It waits for the first beat to start, dynamically adapts video FPS to estimated BPM, and phase-locks on every beat to eliminate drift.

### Features

- **Real-Time Beat Tracking**: Live audio analysis using BTrack (FFTW3 & libsamplerate).
- **First Beat Sync**: Pauses on the first frame until the initial beat is detected.
- **Continuous Phase Locking**: Resynchronizes video phase on every beat to keep visuals locked to the rhythm.
- **Manual Frame Offset**: Configurable alignment between video movement and the musical beat.

### Configuration

You can tune synchronization parameters at the top of `main.cpp`:

- `FRAME_OFFSET`: Video frame index aligned to the beat.
- `FRAMES_PER_BEAT`: Number of video frames in a single beat loop.
- `filename`: Path to the video file (default: `"gandalf.mp4"`).

### Submodules

If you cloned without submodules, fetch BTrack before compiling:

```
git submodule update --init --recursive
```

### Dependencies

Before compiling, the following packages must be installed:  
```
sudo apt install \
  libsdl2-dev \
  ffmpeg \
  libavcodec-dev \
  libavformat-dev \
  libavutil-dev \
  libswscale-dev \
  libfftw3-dev \
  libsamplerate0-dev
```

### Compiling

The right libraries and BTrack sources must be linked:  
```
g++ -DUSE_FFTW main.cpp \
  external/BTrack/src/BTrack.cpp \
  external/BTrack/src/OnsetDetectionFunction.cpp \
  -I external/BTrack/src \
  -o gandalf_player -O2 \
  -lavformat -lavcodec -lswscale -lavutil -lSDL2 -lfftw3 -lsamplerate -lm
```

### Usage

Run the player:
```
./gandalf_player
```

You can select and manage your preferred audio input via `pavucontrol`.
