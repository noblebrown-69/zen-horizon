CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra
LDFLAGS = `sdl2-config --libs` -lSDL2_ttf -lSDL2_image -lcurl -lm
INCLUDES = `sdl2-config --cflags`

TARGET = zen-horizon
SOURCES = main.cpp
OBJECTS = $(SOURCES:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean