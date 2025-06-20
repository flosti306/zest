PROJECTNAME = Zest
OUTPUT_DIR = build

# --- Build Configuration ---
# Set BUILD_TYPE=release for optimized build, debug for debugging (default)
BUILD_TYPE ?= debug

# Optimization flags based on build type
ifeq ($(BUILD_TYPE),release)
    OPT_FLAGS = -O2 -DNDEBUG -march=native
else
    OPT_FLAGS = -O0 -g
endif

# --- OpenCV Configuration (Point to your installed OpenCV) ---
OPENCV_INSTALL_DIR = C:/Programs/OpenCV/opencv/build_mingw/install
OPENCV_SUBDIR_FOR_LIBS_BINS = x64/mingw

# Corrected construction - ensure no extra spaces are introduced
OPENCV_INCLUDE_DIR = $(OPENCV_INSTALL_DIR)/include
OPENCV_LIB_DIR = $(OPENCV_INSTALL_DIR)/$(OPENCV_SUBDIR_FOR_LIBS_BINS)/lib
OPENCV_BIN_DIR = $(OPENCV_INSTALL_DIR)/$(OPENCV_SUBDIR_FOR_LIBS_BINS)/bin

OPENCV_VERSION_SUFFIX = 4110

OPENCV_LIBS = -lopencv_core$(OPENCV_VERSION_SUFFIX) \
              -lopencv_imgproc$(OPENCV_VERSION_SUFFIX)

# Use quotes around each path individually for -I and -L
INCLUDE_DIRS = -Iinclude -Iinclude/SDL3 -Iinclude/imgui -Iinclude/ffmpeg -I"$(OPENCV_INCLUDE_DIR)"

# Here's the critical part for LIB_DIRS:
# Each -L option should be distinct and its path quoted if it contains spaces.
# Your OPENCV_LIB_DIR itself might contain spaces if OPENCV_INSTALL_DIR does.
LIB_DIRS = -Llib/ffmpeg -Llib -L"$(OPENCV_LIB_DIR)"

# Link libraries
LIBS = $(OPENCV_LIBS) \
       -lavformat -lavcodec -lavutil -lswscale -lswresample -lavfilter \
       -lSDL3 -lSDL3_image \
       -lopengl32 -lbcrypt -lsecur32 -lws2_32 \
       -lole32 -luuid -lcomdlg32

# Source files (NO OpenCV sources here)
SRC = $(wildcard src/*.cpp) $(wildcard imgui/*.cpp)
SRC := $(filter-out src/main_old.cpp, $(SRC))
SRC += src/glad.c
SRC += src/miniz.c
SRC += src/tinyfiledialogs.c

# Object files
OBJ = $(SRC:.cpp=.o)
OBJ := $(OBJ:.c=.o)

# Resource file
RES = res/zest_icon.res

$(shell mkdir -p $(OUTPUT_DIR))

$(info === Build Configuration ===============)
$(info BUILD_TYPE: $(BUILD_TYPE))
$(info OPT_FLAGS: $(OPT_FLAGS))
$(info =======================================)
$(info OPENCV_INSTALL_DIR is: [$(OPENCV_INSTALL_DIR)])
$(info OPENCV_SUBDIR_FOR_LIBS_BINS is: [$(OPENCV_SUBDIR_FOR_LIBS_BINS)])
$(info OPENCV_LIB_DIR is: [$(OPENCV_LIB_DIR)])
$(info LIB_DIRS for linker is: [$(LIB_DIRS)])
$(info OPENCV_LIBS for linker is: [$(OPENCV_LIBS)])

default: $(OUTPUT_DIR)/$(PROJECTNAME).exe

$(RES): res/zest_icon.rc
	windres $< -O coff -o $@

%.o: %.cpp
	g++ -c $< -o $@ $(INCLUDE_DIRS) -std=c++17 -Wall $(OPT_FLAGS) -Wno-deprecated-declarations

%.o: %.c
	gcc -c $< -o $@ $(INCLUDE_DIRS) -Wall $(OPT_FLAGS)

$(OUTPUT_DIR)/$(PROJECTNAME).exe: $(OBJ) $(RES)
	@echo "--- Linking ---"
	@echo "Full linker command would be:"
	@echo "g++ $(OBJ) $(RES) -o $@ $(LIB_DIRS) $(LIBS)"
	g++ $(OBJ) $(RES) -o $@ $(LIB_DIRS) $(LIBS) $(OPT_FLAGS)

clean:
	rm -rf $(OUTPUT_DIR)/*.exe res/*.res $(OBJ)

.PHONY: default clean