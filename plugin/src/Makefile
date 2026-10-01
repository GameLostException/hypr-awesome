ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

BUILD_DIR = ../build
SRCS      = main.cpp LayoutState.cpp Dispatchers.cpp
OBJS      = $(addprefix $(BUILD_DIR)/, $(SRCS:.cpp=.o))
TARGET    = $(BUILD_DIR)/hawesome.so

CXXFLAGS = -shared -fPIC $(EXTRA_FLAGS) \
    `pkg-config --cflags pixman-1 libdrm hyprland libinput wayland-server xkbcommon` \
    -std=c++2b -Wno-narrowing

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) -c -fPIC $(EXTRA_FLAGS) \
	    `pkg-config --cflags pixman-1 libdrm hyprland libinput wayland-server xkbcommon` \
	    -std=c++2b -Wno-narrowing $< -o $@

clean:
	rm -rf $(BUILD_DIR)
