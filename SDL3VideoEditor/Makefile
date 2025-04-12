PROJECTNAME = Zest
OUTPUT_DIR = build

# Includes for SDL3, ImGui, FFmpeg, and GLAD
INCLUDE_DIRS = -Iinclude -Iinclude/SDL3 -Iinclude/imgui -Iinclude/ffmpeg

# Library search paths
LIB_DIRS = -Llib/ffmpeg -Llib

# Link libraries
LIBS = -lavformat -lavcodec -lavutil -lswscale -lswresample -lavfilter \
       -lSDL3 -lSDL3_image \
       -lopengl32 -lbcrypt -lsecur32 -lws2_32

# Source files
SRC = $(wildcard src/*.cpp) $(wildcard imgui/*.cpp)
SRC := $(filter-out src/main_old.cpp, $(SRC))
SRC += src/glad.c

# Object files from both .cpp and .c
OBJ = $(SRC:.cpp=.o)
OBJ := $(OBJ:.c=.o)

# Resource file
RES = res/zest_icon.res

# Create output directory
$(shell mkdir -p $(OUTPUT_DIR))

# Default rule
default: $(OUTPUT_DIR)/$(PROJECTNAME).exe

# Compile .rc to .res
$(RES): res/zest_icon.rc
	windres $< -O coff -o $@

# Compile .cpp to .o
%.o: %.cpp
	g++ -c $< -o $@ $(INCLUDE_DIRS) -std=c++17

# Compile .c to .o
%.o: %.c
	gcc -c $< -o $@ $(INCLUDE_DIRS)

# Link everything into final executable
$(OUTPUT_DIR)/$(PROJECTNAME).exe: $(OBJ) $(RES)
	g++ $^ -o $@ $(LIB_DIRS) $(LIBS)

# Clean build
clean:
	rm -rf $(OUTPUT_DIR)/*.exe res/*.res $(OBJ)

.PHONY: default clean
