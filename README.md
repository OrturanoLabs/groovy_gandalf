# Groovy Gandalf Project

Per avere sempre il tuo gandalf al tempo giusto

### Dependencies

Before compiling, the following packages must be installed:  
'''
sudo apt install \
 libsdl2-dev \
 ffmpeg \
 libavcodec-dev \
 libavformat-dev \
 libavutil-dev \
 libswscale-dev
'''

### Compiling

The right libraries must be linked:  
'''
gcc player.c -o player \
 -lavformat \
 -lavcodec \
 -lavutil \
 -lswscale \
 -lSDL2 \
 -lm
'''

### Usage

./player FPS
