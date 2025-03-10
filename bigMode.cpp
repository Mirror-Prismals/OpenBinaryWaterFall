#include "GLFW/glfw3.h"
#include "windows.h"
#include "commdlg.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

// Configuration constants
#define FRAME_WIDTH 910
#define FRAME_HEIGHT 512 // 16:9 aspect ratio (approximately)
#define FRAME_RATE 24
#define WINDOW_SCALE 4  // Display scale factor (4x makes the window 1820x1024)

// Global variables
std::vector<unsigned char> fileData;
size_t currentFrame = 0;
size_t totalFrames = 0;
bool isPaused = false;
bool isFullscreen = false;
float playbackSpeed = 1.0f;
GLFWmonitor* primaryMonitor = nullptr;

// Time tracking
std::chrono::high_resolution_clock::time_point lastFrameTime;

// Function to open file dialog
std::string openFileDialog() {
    char filename[MAX_PATH] = { 0 };

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "Raw Media Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = "Select Raw Media File";

    if (GetOpenFileNameA(&ofn)) {
        return filename;
    }
    return "";
}

// Function to load raw media file
bool loadMediaFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Could not open file: " << filename << std::endl;
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (fileSize <= 0) {
        std::cerr << "Error: File is empty." << std::endl;
        return false;
    }

    // Calculate pixel count (bytes) per frame
    size_t bytesPerFrame = FRAME_WIDTH * FRAME_HEIGHT;  // 116,480 bytes per frame for 455x256

    // Calculate total number of frames in the file
    totalFrames = fileSize / bytesPerFrame;
    if (totalFrames == 0) {
        std::cerr << "Error: File is too small for even one frame." << std::endl;
        return false;
    }

    // Load file data
    fileData.resize(fileSize);
    if (!file.read(reinterpret_cast<char*>(fileData.data()), fileSize)) {
        std::cerr << "Error: Failed to read file data." << std::endl;
        return false;
    }

    std::cout << "Successfully loaded " << fileData.size() << " bytes." << std::endl;
    std::cout << "Total frames: " << totalFrames << std::endl;

    // Reset playback position
    currentFrame = 0;

    return true;
}

// Function to render the current frame
void renderFrame(GLFWwindow* window) {
    // Get the size of the window (works for both windowed and fullscreen)
    int windowWidth, windowHeight;
    glfwGetFramebufferSize(window, &windowWidth, &windowHeight);

    // Update viewport to match current window size
    glViewport(0, 0, windowWidth, windowHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, windowWidth, windowHeight, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Calculate size of one pixel in the window
    float pixelWidth = static_cast<float>(windowWidth) / FRAME_WIDTH;
    float pixelHeight = static_cast<float>(windowHeight) / FRAME_HEIGHT;

    // Calculate the frame offset in the file data
    size_t frameOffset = currentFrame * FRAME_WIDTH * FRAME_HEIGHT;

    // Check if we have enough data for this frame
    if (frameOffset + (FRAME_WIDTH * FRAME_HEIGHT) > fileData.size()) {
        // End of file reached, loop back to beginning
        currentFrame = 0;
        frameOffset = 0;
    }

    // Clear the screen
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw the frame pixel by pixel
    glBegin(GL_QUADS);
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        for (int x = 0; x < FRAME_WIDTH; x++) {
            // Get pixel value from file data
            unsigned char value = fileData[frameOffset + y * FRAME_WIDTH + x];

            // Map raw byte value to one of 18 colors with 14 intensity levels
            // Define the 18 rainbow colors
            const struct {
                float r, g, b;
            } rainbow[18] = {
                {1.0f, 0.0f, 0.0f},     // Red
                {0.0f, 1.0f, 0.0f},     // Lime
                {0.0f, 0.0f, 1.0f},     // Blue
                {1.0f, 0.0f, 1.0f},     // Magenta
                {0.0f, 1.0f, 1.0f},     // Cyan
                {1.0f, 1.0f, 0.0f},     // Yellow
                {1.0f, 0.75f, 0.8f},    // Pink
                {0.5f, 1.0f, 0.0f},     // Chartreuse
                {0.0f, 0.75f, 1.0f},    // Cerulean
                {0.76f, 0.7f, 0.0f},    // Mustard
                {0.9f, 0.3f, 0.0f},     // Infrared (approximation)
                {0.58f, 0.0f, 0.83f},   // Violet
                {0.29f, 0.0f, 0.51f},   // Indigo
                {0.0f, 0.42f, 0.5f},    // Thalo
                {0.0f, 1.0f, 0.5f},     // Mint
                {0.42f, 0.56f, 0.14f},  // Camo
                {1.0f, 0.65f, 0.0f},    // Orange
                {0.4f, 0.0f, 1.0f}      // Ultraviolet (approximation)
            };

            // Calculate color index and intensity
            // Split the 256 possible values into 18 colors x 14 intensities
            // (Note: 18*14=252, so the last 4 values will wrap)

            int colorIndex = (value / 14) % 18;  // Color cycles every 14 values
            float intensity = (value % 14 + 1) / 14.0f;  // Intensity from 1/14 to 14/14

            // Get base color and apply intensity
            float r = rainbow[colorIndex].r * intensity;
            float g = rainbow[colorIndex].g * intensity;
            float b = rainbow[colorIndex].b * intensity;

            // Set the color for this pixel
            glColor3f(r, g, b);

            // Calculate pixel coordinates in the window
            float x1 = x * pixelWidth;
            float y1 = y * pixelHeight;
            float x2 = (x + 1) * pixelWidth;
            float y2 = (y + 1) * pixelHeight;

            // Draw the pixel as a quad
            glVertex2f(x1, y1);
            glVertex2f(x2, y1);
            glVertex2f(x2, y2);
            glVertex2f(x1, y2);
        }
    }
    glEnd();
}

// Toggle fullscreen mode
void toggleFullscreen(GLFWwindow* window, int windowWidth, int windowHeight) {
    if (isFullscreen) {
        // Switch back to windowed mode
        glfwSetWindowMonitor(window, nullptr, 100, 100, windowWidth, windowHeight, GLFW_DONT_CARE);
        isFullscreen = false;
    }
    else {
        // Get primary monitor and its video mode
        primaryMonitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);

        // Switch to fullscreen mode
        glfwSetWindowMonitor(window, primaryMonitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        isFullscreen = true;
    }
}

// Key callback function
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS) return;

    // Store default window size for returning from fullscreen
    static int windowWidth = FRAME_WIDTH * WINDOW_SCALE;
    static int windowHeight = FRAME_HEIGHT * WINDOW_SCALE;

    switch (key) {
    case GLFW_KEY_ESCAPE:
        if (isFullscreen) {
            // If in fullscreen, ESC first exits fullscreen
            toggleFullscreen(window, windowWidth, windowHeight);
        }
        else {
            // If already windowed, ESC closes the application
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        break;
    case GLFW_KEY_F11:
    case GLFW_KEY_F:
        // F11 or F key toggles fullscreen
        toggleFullscreen(window, windowWidth, windowHeight);
        break;
    case GLFW_KEY_SPACE:
        isPaused = !isPaused;
        break;
    case GLFW_KEY_RIGHT:
        currentFrame = (currentFrame + 1) % totalFrames;
        break;
    case GLFW_KEY_LEFT:
        currentFrame = (currentFrame > 0) ? (currentFrame - 1) : (totalFrames - 1);
        break;
    case GLFW_KEY_UP:
        playbackSpeed *= 1.5f;
        if (playbackSpeed > 10.0f) playbackSpeed = 10.0f;
        break;
    case GLFW_KEY_DOWN:
        playbackSpeed /= 1.5f;
        if (playbackSpeed < 0.1f) playbackSpeed = 0.1f;
        break;
    case GLFW_KEY_HOME:
        currentFrame = 0;
        break;
    case GLFW_KEY_END:
        currentFrame = totalFrames - 1;
        break;
    case GLFW_KEY_R:
        // Reset to beginning and default speed
        currentFrame = 0;
        playbackSpeed = 1.0f;
        isPaused = false;
        break;
    }
}

int main() {
    // Open file dialog
    std::string filename = openFileDialog();
    if (filename.empty()) {
        std::cerr << "No file selected. Exiting." << std::endl;
        return EXIT_FAILURE;
    }

    // Load media file
    if (!loadMediaFile(filename)) {
        return EXIT_FAILURE;
    }

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return EXIT_FAILURE;
    }

    // Get the primary monitor for fullscreen support
    primaryMonitor = glfwGetPrimaryMonitor();

    // Create window
    int windowWidth = FRAME_WIDTH * WINDOW_SCALE;  // 3640 pixels wide
    int windowHeight = FRAME_HEIGHT * WINDOW_SCALE;  // 2048 pixels high (16:9 ratio)

    // Check for command line arguments for starting in fullscreen
    bool startFullscreen = false;  // Could be parameterized if needed

    GLFWwindow* window;
    if (startFullscreen) {
        // Start in fullscreen mode
        const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);
        window = glfwCreateWindow(mode->width, mode->height,
            "Binary Waterfall Media Player", primaryMonitor, NULL);
        isFullscreen = true;
    }
    else {
        // Start in windowed mode
        window = glfwCreateWindow(windowWidth, windowHeight,
            "Binary Waterfall Media Player", NULL, NULL);
    }

    if (!window) {
        std::cerr << "Failed to create window" << std::endl;
        glfwTerminate();
        return EXIT_FAILURE;
    }

    // Set up window and callbacks
    glfwMakeContextCurrent(window);
    glfwSetKeyCallback(window, keyCallback);

    // Set up OpenGL
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glViewport(0, 0, windowWidth, windowHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, windowWidth, windowHeight, 0, -1, 1);  // Origin at top-left
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Main loop
    lastFrameTime = std::chrono::high_resolution_clock::now();

    while (!glfwWindowShouldClose(window)) {
        // Calculate time between frames
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - lastFrameTime).count();

        // Update title with playback info
        std::string title = "Binary Waterfall Player - Frame: " +
            std::to_string(currentFrame + 1) + "/" +
            std::to_string(totalFrames) +
            " - Speed: " + std::to_string(playbackSpeed) + "x" +
            (isPaused ? " [PAUSED]" : "");

        glfwSetWindowTitle(window, title.c_str());

        // Render current frame
        renderFrame(window);

        // Swap buffers and poll events
        glfwSwapBuffers(window);
        glfwPollEvents();

        // Update frame if not paused
        if (!isPaused) {
            // Calculate how many frames to advance based on time and playback speed
            float frameTime = 1.0f / (FRAME_RATE * playbackSpeed);
            if (deltaTime >= frameTime) {
                currentFrame = (currentFrame + 1) % totalFrames;
                lastFrameTime = currentTime;
            }
        }
        else {
            // Reset timer when paused to avoid skipping frames when unpausing
            lastFrameTime = currentTime;

            // Sleep to reduce CPU usage while paused
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    // Clean up
    glfwDestroyWindow(window);
    glfwTerminate();

    return EXIT_SUCCESS;
}
