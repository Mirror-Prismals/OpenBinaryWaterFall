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
#define FRAME_WIDTH 64
#define FRAME_HEIGHT 64
#define FRAME_RATE 24
#define WINDOW_SCALE 8  // Display scale factor (8x makes the window 512x512)

// Global variables
std::vector<unsigned char> fileData;
size_t currentFrame = 0;
size_t totalFrames = 0;
bool isPaused = false;
float playbackSpeed = 1.0f;

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
    size_t bytesPerFrame = FRAME_WIDTH * FRAME_HEIGHT;

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
    // Get the size of the window
    int windowWidth, windowHeight;
    glfwGetFramebufferSize(window, &windowWidth, &windowHeight);

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

            // Map raw byte value to a color
            // Using a heatmap-like colorization (black->red->yellow->white)
            float r = 0.0f, g = 0.0f, b = 0.0f;

            if (value < 64) {
                // Black to Red (0-63)
                r = value / 63.0f;
            }
            else if (value < 128) {
                // Red to Yellow (64-127)
                r = 1.0f;
                g = (value - 64) / 63.0f;
            }
            else if (value < 192) {
                // Yellow to White (128-191)
                r = 1.0f;
                g = 1.0f;
                b = (value - 128) / 63.0f;
            }
            else {
                // Full white (192-255)
                r = g = b = 1.0f;
            }

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

// Key callback function
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS) return;

    switch (key) {
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(window, GLFW_TRUE);
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

    // Create window
    int windowWidth = FRAME_WIDTH * WINDOW_SCALE;
    int windowHeight = FRAME_HEIGHT * WINDOW_SCALE;
    GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight,
        "Binary Waterfall Media Player", NULL, NULL);

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
