# wisp
a simple, tiny, very fast, cross platform, music streaming tui app for streaming music from navidrome servers, written in c.

## showcase
![screenshot 1](./media/1.png)
![screenshot 2](./media/2.png)
![screenshot 3](./media/3.png)

## why
this is a personal tool i wanted for a while. everything else just doesn't fit my needs (or have too many features i don't care for that get in my way) 

i also wanted something that just works on all of my computers, therefore wisp runs on the following platforms:
- windows 7+ (compiled with clang-cl) - note: it works fine under the default command prompt on windows 7 but if you want the eyecandy features, you'll need a real terminal emulator too.
- macOS 10.15+ (compiled with appleclang)
- linux 6.18+ (compiled with clang 19, gcc 14.2)

## building wisp
pull the repo recursively and build with cmake.  
```
mkdir build && cd build
cmake ..
cmake --build ..
```

## extra
if you think i missed anything, found a bug or have any questions, feel free to open an issue. 
if you'd like to contribute a bugfix or a feature, pull requests are always welcome.
