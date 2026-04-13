PROJECTNAME = Zest
OUTPUT_DIR = build
OBJ_DIR = $(OUTPUT_DIR)/obj

# --- Build Configuration ---
# Set BUILD_TYPE=release for optimized build, debug for debugging (default)
BUILD_TYPE ?= debug

# Optimization flags based on build type
ifeq ($(BUILD_TYPE),release)
    OPT_FLAGS = -O3 -flto -DNDEBUG -march=native
    LDFLAGS = -flto -O3
else
    OPT_FLAGS = -O0 -g
    LDFLAGS = -g
endif

# --- OpenCV Configuration (Point to your installed OpenCV) ---
OPENCV_INSTALL_DIR = C:/Programs/OpenCV/opencv-4.12.0/build_mingw/install
OPENCV_SUBDIR_FOR_LIBS_BINS = x64/mingw

OPENCV_INCLUDE_DIR = $(OPENCV_INSTALL_DIR)/include
OPENCV_LIB_DIR = $(OPENCV_INSTALL_DIR)/$(OPENCV_SUBDIR_FOR_LIBS_BINS)/lib
OPENCV_BIN_DIR = $(OPENCV_INSTALL_DIR)/$(OPENCV_SUBDIR_FOR_LIBS_BINS)/bin

OPENCV_VERSION_SUFFIX = 4120

OPENCV_LIBS = -lopencv_core$(OPENCV_VERSION_SUFFIX) \
              -lopencv_imgproc$(OPENCV_VERSION_SUFFIX) \
              -lopencv_tracking$(OPENCV_VERSION_SUFFIX)

INCLUDE_DIRS = -Iinclude -Iinclude/SDL3 -Iinclude/imgui -Iinclude/ffmpeg -I"$(OPENCV_INCLUDE_DIR)"

LIB_DIRS = -Llib/ffmpeg -Llib -L"$(OPENCV_LIB_DIR)"

# Link libraries
LIBS = $(OPENCV_LIBS) \
       -lavformat -lavcodec -lavutil -lswscale -lswresample -lavfilter \
       -lSDL3 -lSDL3_image \
       -lopengl32 -lbcrypt -lsecur32 -lws2_32 \
       -lole32 -luuid -lcomdlg32

# Source files
SRC_CPP = $(wildcard src/*.cpp) $(wildcard imgui/*.cpp)
SRC_CPP := $(filter-out src/main_old.cpp, $(SRC_CPP))
SRC_C = src/glad.c src/miniz.c src/tinyfiledialogs.c

# Object files moved to build/obj/ directory instead of littering the source tree
OBJ_CPP = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(notdir $(SRC_CPP)))
OBJ_C = $(patsubst %.c, $(OBJ_DIR)/%.o, $(notdir $(SRC_C)))
OBJ = $(OBJ_CPP) $(OBJ_C)

# Dependency files for auto-recompilation on header changes
DEP = $(OBJ:.o=.d)

# Resource file
RES = $(OBJ_DIR)/zest_icon.res

# Search paths for source files so Make can find them when writing to $(OBJ_DIR)
vpath %.cpp src imgui
vpath %.c src

# OS specific commands and directory safety
ifeq ($(OS),Windows_NT)
    MKDIR = if not exist "$(subst /,\,$1)" mkdir "$(subst /,\,$1)"
    RM = del /Q /S
    FIX_PATH = $(subst /,\,$1)
else
    MKDIR = mkdir -p "$1"
    RM = rm -rf
    FIX_PATH = $1
endif

$(info === Build Configuration ===============)
$(info BUILD_TYPE: $(BUILD_TYPE))
$(info OPT_FLAGS:  $(OPT_FLAGS))
$(info =======================================)

default: dirs $(OUTPUT_DIR)/$(PROJECTNAME).exe

dirs:
	@$(call MKDIR,$(OUTPUT_DIR))
	@$(call MKDIR,$(OBJ_DIR))

# Dependency tracking flags
DEPFLAGS = -MMD -MP

$(RES): res/zest_icon.rc | dirs
	windres $< -O coff -o $@

$(OBJ_DIR)/%.o: %.cpp | dirs
	g++ -c $< -o $@ $(INCLUDE_DIRS) -std=c++23 -Wall $(OPT_FLAGS) $(DEPFLAGS) -Wno-deprecated-declarations

$(OBJ_DIR)/%.o: %.c | dirs
	gcc -c $< -o $@ $(INCLUDE_DIRS) -Wall $(OPT_FLAGS) $(DEPFLAGS)

$(OUTPUT_DIR)/$(PROJECTNAME).exe: $(OBJ) $(RES)
	@echo "--- Linking ---"
	g++ $^ -o $@ $(LIB_DIRS) $(LIBS) $(LDFLAGS)

clean:
	-@$(RM) "$(call FIX_PATH,$(OUTPUT_DIR)/*.exe)" 2>nul || exit 0
	-@$(RM) "$(call FIX_PATH,$(OBJ_DIR))" 2>nul || exit 0

-include $(DEP)

.PHONY: default clean dirs