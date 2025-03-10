#include <GLFW/glfw3.h>
#include <windows.h>
#include <commdlg.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cmath>
#include <jack/jack.h>
#include <thread>
#include <chrono>

// Configuration constants
#define FRAME_WIDTH 64
#define FRAME_HEIGHT 128
#define BASE_FRAME_RATE 24   // The baseline visual frame rate (24 FPS)
#define WINDOW_SCALE 4       // Fixed pixel size scale

const double VISUAL_FPS_CAP = 24.0; // How often we redraw the window

// Global file data and state
std::vector<unsigned char> fileData;
size_t totalFrames = 0;  // Total number of frames in the file

// Playback state
bool isPaused = false;
bool isFullscreen = false;
bool isAudioEnabled = true;
// playbackMultiplier multiplies the baseline frame rate (default = 1.0 means 24 FPS)
double playbackMultiplier = 1.0;
float audioVolume = 1.0f;
// audioPosition is measured in bytes (the offset into fileData)
double audioPosition = 0.0;

// Looping/boomerang (loopStart/loopEnd in bytes)
bool loopEnabled = true;
bool boomerangMode = false;
double loopStart = 0.0;
double loopEnd = 0.0;

// JACK globals
jack_client_t* jackClient = NULL;
jack_port_t* outputPortLeft = NULL;
jack_port_t* outputPortRight = NULL;
jack_nframes_t sampleRate = 44100; // Determined at runtime

// Visual scaling (for fixed pixel size)
int windowScale = WINDOW_SCALE;

// Monitor and timing
GLFWmonitor* primaryMonitor = NULL;
double lastInputTime = 0.0;

// --- Forward declarations ---
std::string openFileDialog(void);
bool loadMediaFile(const std::string& filename);
double calculateLogAdjustment(double currentMultiplier);
void processInput(GLFWwindow* window);
void renderFrame(GLFWwindow* window);
void toggleFullscreen(GLFWwindow* window, int windowWidth, int windowHeight);
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
bool initJackAudio(void);
void closeJackAudio(void);

// --- Utility: Wrap a position into [0, fileSize) ---
static inline double wrapPosition(double pos, double fileSize) {
    if (pos < 0.0) {
        pos = std::fmod(pos, fileSize);
        if (pos < 0.0) pos += fileSize;
    }
    else if (pos >= fileSize) {
        pos = std::fmod(pos, fileSize);
    }
    return pos;
}

// --- Loop handling ---
static inline void handleLoop(double fileSize, double advance) {
    if (!loopEnabled) {
        audioPosition = wrapPosition(audioPosition, fileSize);
        return;
    }
    bool forward = (advance > 0.0);
    if (loopStart == loopEnd) {
        audioPosition = loopStart;
        return;
    }
    if (loopStart < loopEnd) {
        if (forward && audioPosition > loopEnd) {
            if (boomerangMode) {
                double overshoot = audioPosition - loopEnd;
                audioPosition = loopEnd - overshoot;
                playbackMultiplier = -playbackMultiplier;
            }
            else {
                audioPosition = loopStart;
            }
        }
        else if (!forward && audioPosition < loopStart) {
            if (boomerangMode) {
                double overshoot = loopStart - audioPosition;
                audioPosition = loopStart + overshoot;
                playbackMultiplier = -playbackMultiplier;
            }
            else {
                audioPosition = loopEnd;
            }
        }
    }
    else {
        // Wrapped region: [loopStart, fileSize) U [0, loopEnd]
        audioPosition = wrapPosition(audioPosition, fileSize);
        if (forward && audioPosition > loopEnd && audioPosition < loopStart) {
            if (boomerangMode) {
                double overshoot = audioPosition - loopEnd;
                audioPosition = loopEnd - overshoot;
                playbackMultiplier = -playbackMultiplier;
            }
            else {
                audioPosition = loopStart;
            }
        }
        else if (!forward && (audioPosition < loopEnd || audioPosition > loopStart)) {
            if (boomerangMode) {
                double overshoot = (audioPosition < loopEnd) ? (loopEnd - audioPosition) : (audioPosition - loopStart);
                audioPosition = (audioPosition < loopEnd) ? (loopEnd + overshoot) : (loopStart - overshoot);
                playbackMultiplier = -playbackMultiplier;
            }
            else {
                audioPosition = loopEnd;
            }
        }
    }
}

// --- JACK process callback ---
int jackProcessCallback(jack_nframes_t nframes, void* arg) {
    jack_default_audio_sample_t* outLeft = (jack_default_audio_sample_t*)jack_port_get_buffer(outputPortLeft, nframes);
    jack_default_audio_sample_t* outRight = (jack_default_audio_sample_t*)jack_port_get_buffer(outputPortRight, nframes);
    if (isPaused || !isAudioEnabled || fileData.empty()) {
        for (jack_nframes_t i = 0; i < nframes; i++) {
            outLeft[i] = 0.0f;
            outRight[i] = 0.0f;
        }
        return 0;
    }
    double fileSize = static_cast<double>(fileData.size());
    double frameBytes = static_cast<double>(FRAME_WIDTH * FRAME_HEIGHT);
    double baseAdvancement = (frameBytes * BASE_FRAME_RATE) / static_cast<double>(sampleRate);
    double effectiveAdvance = baseAdvancement * playbackMultiplier;
    for (jack_nframes_t i = 0; i < nframes; i++) {
        audioPosition += effectiveAdvance;
        handleLoop(fileSize, effectiveAdvance);
        size_t index = static_cast<size_t>(audioPosition);
        if (index >= fileData.size())
            index = fileData.size() - 1;
        unsigned char val = fileData[index];
        float sample = (static_cast<int>(val) - 128) / 128.0f;
        sample *= audioVolume;
        outLeft[i] = sample;
        outRight[i] = sample;
    }
    return 0;
}

// --- JACK shutdown callback ---
void jackShutdownCallback(void* arg) {
    std::cerr << "JACK server shutdown." << std::endl;
    jackClient = NULL;
    isAudioEnabled = false;
}

// --- Initialize JACK ---
bool initJackAudio() {
    jack_status_t status;
    jackClient = jack_client_open("BinaryWaterfallPlayer", JackNullOption, &status);
    if (!jackClient) {
        std::cerr << "Failed to connect to JACK server." << std::endl;
        return false;
    }
    jack_set_process_callback(jackClient, jackProcessCallback, NULL);
    jack_on_shutdown(jackClient, jackShutdownCallback, NULL);
    sampleRate = jack_get_sample_rate(jackClient);
    outputPortLeft = jack_port_register(jackClient, "output_left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    outputPortRight = jack_port_register(jackClient, "output_right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!outputPortLeft || !outputPortRight) {
        std::cerr << "Failed to create JACK output ports." << std::endl;
        jack_client_close(jackClient);
        return false;
    }
    if (jack_activate(jackClient)) {
        std::cerr << "Failed to activate JACK client." << std::endl;
        jack_client_close(jackClient);
        return false;
    }
    const char** ports = jack_get_ports(jackClient, NULL, NULL, JackPortIsPhysical | JackPortIsInput);
    if (ports) {
        if (ports[0])
            jack_connect(jackClient, jack_port_name(outputPortLeft), ports[0]);
        if (ports[1])
            jack_connect(jackClient, jack_port_name(outputPortRight), ports[1]);
        jack_free(ports);
    }
    std::cout << "JACK audio initialized at " << sampleRate << " Hz." << std::endl;
    return true;
}

// --- Close JACK ---
void closeJackAudio() {
    if (jackClient) {
        jack_deactivate(jackClient);
        jack_client_close(jackClient);
        jackClient = NULL;
    }
}

// --- Open file dialog ---
std::string openFileDialog(void) {
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
    if (GetOpenFileNameA(&ofn))
        return filename;
    return "";
}

// --- Load raw media file ---
bool loadMediaFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Could not open file: " << filename << std::endl;
        return false;
    }
    file.seekg(0, std::ios::end);
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    if (fileSize <= 0) {
        std::cerr << "Error: File is empty." << std::endl;
        return false;
    }
    size_t bytesPerFrame = FRAME_WIDTH * FRAME_HEIGHT;
    totalFrames = fileSize / bytesPerFrame;
    if (totalFrames == 0) {
        std::cerr << "Error: File too small for even one frame." << std::endl;
        return false;
    }
    fileData.resize(fileSize);
    if (!file.read(reinterpret_cast<char*>(fileData.data()), fileSize)) {
        std::cerr << "Error: Failed to read file data." << std::endl;
        return false;
    }
    std::cout << "Loaded " << fileData.size() << " bytes. Total frames: " << totalFrames << std::endl;
    return true;
}

// --- Updated logarithmic adjustment function for finer control ---
double calculateLogAdjustment(double currentMultiplier) {
    double absVal = std::abs(currentMultiplier);
    if (absVal < 1.0)
        return 0.1;  // smallest step when near 0
    if (absVal < 10.0)
        return 0.5;  // moderate step
    return 1.0;      // larger step when values are high
}

// --- Process repeated key input with finer control ---
void processInput(GLFWwindow* window) {
    double currentTime = glfwGetTime();
    float deltaTime = static_cast<float>(currentTime - lastInputTime);
    if (deltaTime < 0.1f)
        return;
    lastInputTime = currentTime;
    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) {
        double step = calculateLogAdjustment(playbackMultiplier);
        playbackMultiplier += step;
    }
    if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) {
        double step = calculateLogAdjustment(playbackMultiplier);
        playbackMultiplier -= step;
    }
    if (glfwGetKey(window, GLFW_KEY_KP_ADD) == GLFW_PRESS) {
        audioVolume += 0.05f;
        if (audioVolume > 2.0f)
            audioVolume = 2.0f;
    }
    if (glfwGetKey(window, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS) {
        audioVolume -= 0.05f;
        if (audioVolume < 0.0f)
            audioVolume = 0.0f;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS) {
        if (windowScale > 1)
            windowScale--;
    }
    if (glfwGetKey(window, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) {
        windowScale++;
    }
}

// --- Render frame ---
// This version fills the window by tiling frames. Each frame is drawn at a fixed pixel size 
// (FRAME_WIDTH*windowScale by FRAME_HEIGHT*windowScale). The number of columns and rows is computed 
// using ceiling division so that the entire window is covered, even if that means drawing a partial frame.
void renderFrame(GLFWwindow* window) {
    // Get full window size.
    int windowWidth, windowHeight;
    glfwGetFramebufferSize(window, &windowWidth, &windowHeight);

    // Fixed size for each frame (in pixels).
    int framePixelWidth = FRAME_WIDTH * windowScale;
    int framePixelHeight = FRAME_HEIGHT * windowScale;

    // Calculate number of columns and rows to fill the window completely.
    int columns = (windowWidth + framePixelWidth - 1) / framePixelWidth;
    int rows = (windowHeight + framePixelHeight - 1) / framePixelHeight;

    // Setup orthographic projection matching window size.
    glViewport(0, 0, windowWidth, windowHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, windowWidth, windowHeight, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Starting frame index based on audioPosition.
    size_t startFrame = static_cast<size_t>(wrapPosition(audioPosition, static_cast<double>(fileData.size()))
        / (FRAME_WIDTH * FRAME_HEIGHT));

    // Clear the screen.
    glClear(GL_COLOR_BUFFER_BIT);

    // Render the grid of frames.
    glBegin(GL_QUADS);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < columns; c++) {
            // Compute frame index (wrap around if needed).
            size_t frameIndex = (startFrame + r * columns + c) % totalFrames;
            size_t frameOffset = frameIndex * FRAME_WIDTH * FRAME_HEIGHT;
            // Compute top-left corner for this frame.
            int offsetX = c * framePixelWidth;
            int offsetY = r * framePixelHeight;

            // Render each pixel in the frame.
            for (int y = 0; y < FRAME_HEIGHT; y++) {
                for (int x = 0; x < FRAME_WIDTH; x++) {
                    unsigned char value = fileData[frameOffset + y * FRAME_WIDTH + x];
                    // 18-color rainbow palette.
                    const struct { float r, g, b; } rainbow[18] = {
                        {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                        {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 0.0f},
                        {1.0f, 0.75f, 0.8f}, {0.5f, 1.0f, 0.0f}, {0.0f, 0.75f, 1.0f},
                        {0.76f, 0.7f, 0.0f}, {0.9f, 0.3f, 0.0f}, {0.58f, 0.0f, 0.83f},
                        {0.29f, 0.0f, 0.51f}, {0.0f, 0.42f, 0.5f}, {0.0f, 1.0f, 0.5f},
                        {0.42f, 0.56f, 0.14f}, {1.0f, 0.65f, 0.0f}, {0.4f, 0.0f, 1.0f}
                    };
                    int colorIndex = (value / 14) % 18;
                    float intensity = ((value % 14) + 1) / 14.0f;
                    glColor3f(rainbow[colorIndex].r * intensity,
                        rainbow[colorIndex].g * intensity,
                        rainbow[colorIndex].b * intensity);

                    // Compute quad coordinates for the pixel.
                    float x1 = offsetX + x * windowScale;
                    float y1 = offsetY + y * windowScale;
                    float x2 = offsetX + (x + 1) * windowScale;
                    float y2 = offsetY + (y + 1) * windowScale;
                    glVertex2f(x1, y1);
                    glVertex2f(x2, y1);
                    glVertex2f(x2, y2);
                    glVertex2f(x1, y2);
                }
            }
        }
    }
    glEnd();
}

// --- Toggle fullscreen ---
void toggleFullscreen(GLFWwindow* window, int windowWidth, int windowHeight) {
    if (isFullscreen) {
        glfwSetWindowMonitor(window, NULL, windowWidth / 10, windowHeight / 10,
            windowWidth, windowHeight, GLFW_DONT_CARE);
        isFullscreen = false;
    }
    else {
        primaryMonitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);
        glfwSetWindowMonitor(window, primaryMonitor, 0, 0,
            mode->width, mode->height, mode->refreshRate);
        isFullscreen = true;
    }
}

// --- Key callback ---
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS)
        return;
    static int fixedWindowWidth = FRAME_WIDTH * windowScale;
    static int fixedWindowHeight = FRAME_HEIGHT * windowScale;
    switch (key) {
    case GLFW_KEY_ESCAPE:
        if (isFullscreen)
            toggleFullscreen(window, fixedWindowWidth, fixedWindowHeight);
        else
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        break;
    case GLFW_KEY_F:
    case GLFW_KEY_F11:
        toggleFullscreen(window, fixedWindowWidth, fixedWindowHeight);
        break;
    case GLFW_KEY_SPACE:
        isPaused = !isPaused;
        break;
    case GLFW_KEY_RIGHT:
        audioPosition += static_cast<double>(FRAME_WIDTH * FRAME_HEIGHT);
        break;
    case GLFW_KEY_LEFT:
        audioPosition -= static_cast<double>(FRAME_WIDTH * FRAME_HEIGHT);
        break;
    case GLFW_KEY_0:
        playbackMultiplier = 1.0;
        break;
    case GLFW_KEY_MINUS:
        playbackMultiplier = -playbackMultiplier;
        if (playbackMultiplier == 0)
            playbackMultiplier = -1.0;
        break;
    case GLFW_KEY_EQUAL:
        playbackMultiplier = std::fabs(playbackMultiplier);
        if (playbackMultiplier == 0)
            playbackMultiplier = 1.0;
        break;
    case GLFW_KEY_M:
        isAudioEnabled = !isAudioEnabled;
        break;
    case GLFW_KEY_R:
        playbackMultiplier = -playbackMultiplier;
        if (playbackMultiplier == 0)
            playbackMultiplier = -1.0;
        break;
    case GLFW_KEY_BACKSPACE:
        audioPosition = 0.0;
        playbackMultiplier = 1.0;
        isPaused = false;
        break;
    case GLFW_KEY_PAGE_UP: {
        double step = calculateLogAdjustment(playbackMultiplier);
        playbackMultiplier += step;
        break;
    }
    case GLFW_KEY_PAGE_DOWN: {
        double step = calculateLogAdjustment(playbackMultiplier);
        playbackMultiplier -= step;
        break;
    }
    case GLFW_KEY_HOME:
        audioPosition = 0.0;
        break;
    case GLFW_KEY_END:
        audioPosition = static_cast<double>(FRAME_WIDTH * FRAME_HEIGHT) * (static_cast<double>(totalFrames - 1));
        break;
    case GLFW_KEY_L:
        loopEnabled = !loopEnabled;
        break;
    case GLFW_KEY_B:
        boomerangMode = !boomerangMode;
        break;
    case GLFW_KEY_COMMA:  // '<'
        if (!loopEnabled)
            loopStart = audioPosition;
        break;
    case GLFW_KEY_PERIOD: // '>'
        if (!loopEnabled)
            loopEnd = audioPosition;
        break;
    case GLFW_KEY_LEFT_BRACKET:
        if (windowScale > 1)
            windowScale--;
        break;
    case GLFW_KEY_RIGHT_BRACKET:
        windowScale++;
        break;
    }
}

int main(void) {
    std::string filename = openFileDialog();
    if (filename.empty()) {
        std::cerr << "No file selected. Exiting." << std::endl;
        return EXIT_FAILURE;
    }
    if (!loadMediaFile(filename))
        return EXIT_FAILURE;
    size_t bytesPerFrame = FRAME_WIDTH * FRAME_HEIGHT;
    // Default loop: from frame 1 to frame 34.
    loopStart = 0.0;
    loopEnd = 34.0 * static_cast<double>(bytesPerFrame);
    audioPosition = loopStart;
    loopEnabled = true;
    boomerangMode = false;
    if (!initJackAudio())
        std::cerr << "Warning: JACK audio init failed; continuing without audio." << std::endl;
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW." << std::endl;
        return EXIT_FAILURE;
    }
    primaryMonitor = glfwGetPrimaryMonitor();
    int windowWidth = FRAME_WIDTH * windowScale;
    int windowHeight = FRAME_HEIGHT * windowScale;
    bool startFullscreen = false;
    GLFWwindow* window = startFullscreen ?
        glfwCreateWindow(glfwGetVideoMode(primaryMonitor)->width,
            glfwGetVideoMode(primaryMonitor)->height,
            "Binary Waterfall Media Player", primaryMonitor, NULL)
        : glfwCreateWindow(windowWidth, windowHeight, "Binary Waterfall Media Player", NULL, NULL);
    if (!window) {
        std::cerr << "Failed to create window." << std::endl;
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    // Enable VSync
    glfwSwapInterval(1);
    glfwSetKeyCallback(window, keyCallback);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    double lastVisualUpdate = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        double currentTime = glfwGetTime();
        processInput(window);
        // Update title bar: show current frame and effective visual FPS.
        double fileSize = static_cast<double>(fileData.size());
        double wrappedPos = wrapPosition(audioPosition, fileSize);
        size_t currentFrame = static_cast<size_t>(wrappedPos / static_cast<double>(bytesPerFrame));
        if (currentFrame >= totalFrames)
            currentFrame = totalFrames - 1;
        char title[512];
        std::snprintf(title, sizeof(title),
            "Binary Waterfall Player - Frame: %zu/%zu - FPS: %.1f%s - Fixed Pixel Size: %d",
            currentFrame + 1, totalFrames,
            BASE_FRAME_RATE * playbackMultiplier,
            (isPaused ? " [PAUSED]" : ""),
            windowScale);
        glfwSetWindowTitle(window, title);
        if (currentTime - lastVisualUpdate >= 1.0 / VISUAL_FPS_CAP) {
            renderFrame(window);
            glfwSwapBuffers(window);
            lastVisualUpdate = currentTime;
        }
        // Wait for events or timeout to avoid busy looping
        glfwWaitEventsTimeout(1.0 / VISUAL_FPS_CAP);
        // Additional short sleep to yield CPU time
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    closeJackAudio();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
