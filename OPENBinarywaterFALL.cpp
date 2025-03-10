#include <GLFW/glfw3.h>
#include <windows.h>
#include <commdlg.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cmath>
#include <jack/jack.h>

// Configuration constants
#define FRAME_WIDTH 455
#define FRAME_HEIGHT 256
#define DEFAULT_FREQUENCY 24   // Baseline: 24 Hz corresponds to 1× speed.
#define WINDOW_SCALE 4
const double VISUAL_FPS_CAP = 24.0; // Maximum visual frames per second

// Global data
std::vector<unsigned char> fileData;
size_t totalFrames = 0;
bool   isPaused = false;
bool   isFullscreen = false;
bool   isAudioEnabled = true;
// Set initial playbackFrequency to 14,000 Hz (which is 14,000/24 ≈ 583.33× speed compared to baseline)
int    playbackFrequency = 14000;
float  audioVolume = 1.0f;
double audioPosition = 0.0;

// Looping & boomerang
bool   loopEnabled = true;      // Start with loop on
bool   boomerangMode = false;   // Start with normal looping
double loopStart = 0.0;
double loopEnd = 0.0;

// JACK variables
jack_client_t* jackClient = NULL;
jack_port_t* outputPortLeft = NULL;
jack_port_t* outputPortRight = NULL;
jack_nframes_t sampleRate = 44100;

GLFWmonitor* primaryMonitor = NULL;
double lastInputTime = 0.0;

// Forward declarations
std::string openFileDialog(void);
bool loadMediaFile(const std::string& filename);
int  calculateLogAdjustment(int currentFrequency);
void processInput(GLFWwindow* window);
void renderFrame(GLFWwindow* window);
void toggleFullscreen(GLFWwindow* window, int windowWidth, int windowHeight);
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
bool initJackAudio(void);
void closeJackAudio(void);

// Utility: wrap a double into [0, fileSize)
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

// Handle looping or boomerang boundaries after advancing audioPosition.
static inline void handleLoop(double fileSize, double bytesPerSample) {
    if (!loopEnabled) {
        audioPosition = wrapPosition(audioPosition, fileSize);
        return;
    }
    bool forward = (bytesPerSample > 0.0);
    if (loopStart == loopEnd) {
        audioPosition = loopStart;
        return;
    }
    if (loopStart < loopEnd) {
        if (forward && audioPosition > loopEnd) {
            if (boomerangMode) {
                double overshoot = audioPosition - loopEnd;
                audioPosition = loopEnd - overshoot;
                playbackFrequency = -playbackFrequency;
            }
            else {
                audioPosition = loopStart;
            }
        }
        else if (!forward && audioPosition < loopStart) {
            if (boomerangMode) {
                double overshoot = loopStart - audioPosition;
                audioPosition = loopStart + overshoot;
                playbackFrequency = -playbackFrequency;
            }
            else {
                audioPosition = loopEnd;
            }
        }
    }
    else {
        audioPosition = wrapPosition(audioPosition, fileSize);
        if (forward) {
            if (audioPosition > loopEnd && audioPosition < loopStart) {
                if (boomerangMode) {
                    double overshoot = audioPosition - loopEnd;
                    audioPosition = loopEnd - overshoot;
                    playbackFrequency = -playbackFrequency;
                }
                else {
                    audioPosition = loopStart;
                }
            }
        }
        else {
            if (audioPosition < loopEnd || audioPosition > loopStart) {
                if (boomerangMode) {
                    double overshoot = (audioPosition < loopEnd) ? (loopEnd - audioPosition) : (audioPosition - loopStart);
                    audioPosition = (audioPosition < loopEnd) ? (loopEnd + overshoot) : (loopStart - overshoot);
                    playbackFrequency = -playbackFrequency;
                }
                else {
                    audioPosition = loopEnd;
                }
            }
        }
    }
}

// JACK process callback
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
    // Calculate advancement per audio sample:
    // bytesPerSample = playbackFrequency / DEFAULT_FREQUENCY
    double bytesPerSample = (double)playbackFrequency / (double)DEFAULT_FREQUENCY;
    double fileSize = (double)fileData.size();
    for (jack_nframes_t i = 0; i < nframes; i++) {
        audioPosition += bytesPerSample;
        handleLoop(fileSize, bytesPerSample);
        size_t index = (size_t)audioPosition;
        unsigned char val = fileData[index];
        float sample = ((int)val - 128) / 128.0f;
        sample *= audioVolume;
        outLeft[i] = sample;
        outRight[i] = sample;
    }
    return 0;
}

// JACK shutdown callback
void jackShutdownCallback(void* arg) {
    std::cerr << "JACK server shutdown." << std::endl;
    jackClient = NULL;
    isAudioEnabled = false;
}

// Initialize JACK
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
    outputPortLeft = jack_port_register(jackClient, "output_left",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    outputPortRight = jack_port_register(jackClient, "output_right",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
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
    const char** ports = jack_get_ports(jackClient, NULL, NULL,
        JackPortIsPhysical | JackPortIsInput);
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

// Close JACK
void closeJackAudio() {
    if (jackClient) {
        jack_deactivate(jackClient);
        jack_client_close(jackClient);
        jackClient = NULL;
    }
}

// Open file dialog
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
    if (GetOpenFileNameA(&ofn)) {
        return filename;
    }
    return "";
}

// Load raw media file
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

// Calculate logarithmic adjustment for frequency changes
int calculateLogAdjustment(int currentFrequency) {
    int absFreq = std::abs(currentFrequency);
    if (absFreq < 10)      return 1;
    if (absFreq < 100)     return 5;
    if (absFreq < 1000)    return 10;
    if (absFreq < 10000)   return 100;
    if (absFreq < 100000)  return 1000;
    return 10000;
}

// Process repeated key input
void processInput(GLFWwindow* window) {
    double currentTime = glfwGetTime();
    float deltaTime = (float)(currentTime - lastInputTime);
    if (deltaTime < 0.1f) return;
    lastInputTime = currentTime;
    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) {
        int step = calculateLogAdjustment(playbackFrequency);
        if (playbackFrequency >= 0) playbackFrequency += step;
        else { playbackFrequency += step; if (playbackFrequency >= 0) playbackFrequency = 1; }
        if (playbackFrequency > 1000000) playbackFrequency = 1000000;
    }
    if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) {
        int step = calculateLogAdjustment(playbackFrequency);
        if (playbackFrequency <= 0) playbackFrequency -= step;
        else { playbackFrequency -= step; if (playbackFrequency <= 0) playbackFrequency = -1; }
        if (playbackFrequency < -1000000) playbackFrequency = -1000000;
    }
    if (glfwGetKey(window, GLFW_KEY_KP_ADD) == GLFW_PRESS) {
        audioVolume += 0.05f; if (audioVolume > 2.0f) audioVolume = 2.0f;
    }
    if (glfwGetKey(window, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS) {
        audioVolume -= 0.05f; if (audioVolume < 0.0f) audioVolume = 0.0f;
    }
}

// Render the frame corresponding to the current audioPosition
void renderFrame(GLFWwindow* window) {
    double fileSize = (double)fileData.size();
    double wrappedPos = wrapPosition(audioPosition, fileSize);
    size_t bytesPerFrame = FRAME_WIDTH * FRAME_HEIGHT;
    size_t frameIndex = (size_t)(wrappedPos / (double)bytesPerFrame);
    if (frameIndex >= totalFrames) frameIndex = totalFrames - 1;
    int windowWidth, windowHeight;
    glfwGetFramebufferSize(window, &windowWidth, &windowHeight);
    glViewport(0, 0, windowWidth, windowHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, windowWidth, windowHeight, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    float pixelWidth = (float)windowWidth / FRAME_WIDTH;
    float pixelHeight = (float)windowHeight / FRAME_HEIGHT;
    size_t frameOffset = frameIndex * bytesPerFrame;
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        for (int x = 0; x < FRAME_WIDTH; x++) {
            unsigned char value = fileData[frameOffset + y * FRAME_WIDTH + x];
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
            float r = rainbow[colorIndex].r * intensity;
            float g = rainbow[colorIndex].g * intensity;
            float b = rainbow[colorIndex].b * intensity;
            glColor3f(r, g, b);
            float x1 = x * pixelWidth, y1 = y * pixelHeight;
            float x2 = (x + 1) * pixelWidth, y2 = (y + 1) * pixelHeight;
            glVertex2f(x1, y1); glVertex2f(x2, y1);
            glVertex2f(x2, y2); glVertex2f(x1, y2);
        }
    }
    glEnd();
}

// Toggle fullscreen mode
void toggleFullscreen(GLFWwindow* window, int windowWidth, int windowHeight) {
    if (isFullscreen) {
        glfwSetWindowMonitor(window, NULL, windowWidth / 10, windowHeight / 10, windowWidth, windowHeight, GLFW_DONT_CARE);
        isFullscreen = false;
    }
    else {
        primaryMonitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);
        glfwSetWindowMonitor(window, primaryMonitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        isFullscreen = true;
    }
}

// Key callback: handles playback controls, looping, and boomerang features.
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS) return;
    static int windowWidth = FRAME_WIDTH * WINDOW_SCALE;
    static int windowHeight = FRAME_HEIGHT * WINDOW_SCALE;
    switch (key) {
    case GLFW_KEY_ESCAPE:
        if (isFullscreen) toggleFullscreen(window, windowWidth, windowHeight);
        else glfwSetWindowShouldClose(window, GLFW_TRUE);
        break;
    case GLFW_KEY_F:
    case GLFW_KEY_F11:
        toggleFullscreen(window, windowWidth, windowHeight);
        break;
    case GLFW_KEY_SPACE:
        isPaused = !isPaused;
        break;
    case GLFW_KEY_RIGHT:
        audioPosition += (double)(FRAME_WIDTH * FRAME_HEIGHT);
        break;
    case GLFW_KEY_LEFT:
        audioPosition -= (double)(FRAME_WIDTH * FRAME_HEIGHT);
        break;
    case GLFW_KEY_0:
        playbackFrequency = 0;
        break;
    case GLFW_KEY_MINUS:
        playbackFrequency = -playbackFrequency;
        if (playbackFrequency == 0) playbackFrequency = -DEFAULT_FREQUENCY;
        break;
    case GLFW_KEY_EQUAL:
        playbackFrequency = std::abs(playbackFrequency);
        if (playbackFrequency == 0) playbackFrequency = DEFAULT_FREQUENCY;
        break;
    case GLFW_KEY_M:
        isAudioEnabled = !isAudioEnabled;
        break;
    case GLFW_KEY_R:
        playbackFrequency = -playbackFrequency;
        if (playbackFrequency == 0) playbackFrequency = -DEFAULT_FREQUENCY;
        break;
    case GLFW_KEY_BACKSPACE:
        audioPosition = 0.0;
        playbackFrequency = DEFAULT_FREQUENCY;
        isPaused = false;
        break;
    case GLFW_KEY_PAGE_UP:
        if (std::abs(playbackFrequency) < 10) playbackFrequency = (playbackFrequency >= 0) ? 10 : -10;
        else if (std::abs(playbackFrequency) < 60) playbackFrequency = (playbackFrequency >= 0) ? 60 : -60;
        else if (std::abs(playbackFrequency) < 100) playbackFrequency = (playbackFrequency >= 0) ? 100 : -100;
        else if (std::abs(playbackFrequency) < 1000) playbackFrequency = (playbackFrequency >= 0) ? 1000 : -1000;
        else if (std::abs(playbackFrequency) < 10000) playbackFrequency = (playbackFrequency >= 0) ? 10000 : -10000;
        else if (std::abs(playbackFrequency) < 100000) playbackFrequency = (playbackFrequency >= 0) ? 100000 : -100000;
        else playbackFrequency = (playbackFrequency >= 0) ? 1000000 : -1000000;
        break;
    case GLFW_KEY_PAGE_DOWN:
        if (std::abs(playbackFrequency) > 100000) playbackFrequency = (playbackFrequency >= 0) ? 100000 : -100000;
        else if (std::abs(playbackFrequency) > 10000) playbackFrequency = (playbackFrequency >= 0) ? 10000 : -10000;
        else if (std::abs(playbackFrequency) > 1000) playbackFrequency = (playbackFrequency >= 0) ? 1000 : -1000;
        else if (std::abs(playbackFrequency) > 100) playbackFrequency = (playbackFrequency >= 0) ? 100 : -100;
        else if (std::abs(playbackFrequency) > 60) playbackFrequency = (playbackFrequency >= 0) ? 60 : -60;
        else if (std::abs(playbackFrequency) > 10) playbackFrequency = (playbackFrequency >= 0) ? 10 : -10;
        else playbackFrequency = (playbackFrequency >= 0) ? 1 : -1;
        break;
    case GLFW_KEY_HOME:
        audioPosition = 0.0;
        break;
    case GLFW_KEY_END:
        audioPosition = (double)(FRAME_WIDTH * FRAME_HEIGHT) * (double)(totalFrames - 1);
        break;
        // Looping / Boomerang features:
    case GLFW_KEY_L:
        loopEnabled = !loopEnabled;
        break;
    case GLFW_KEY_B:
        boomerangMode = !boomerangMode;
        break;
    case GLFW_KEY_COMMA:  // '<'
        if (!loopEnabled) loopStart = audioPosition;
        break;
    case GLFW_KEY_PERIOD: // '>'
        if (!loopEnabled) loopEnd = audioPosition;
        break;
    }
}

int main(void) {
    std::string filename = openFileDialog();
    if (filename.empty()) {
        std::cerr << "No file selected. Exiting." << std::endl;
        return EXIT_FAILURE;
    }
    if (!loadMediaFile(filename)) return EXIT_FAILURE;
    size_t bytesPerFrame = FRAME_WIDTH * FRAME_HEIGHT;
    // Initialize loop to run from frame 1 to frame 34:
    loopStart = 0.0;                        // Frame 1 (0-indexed)
    loopEnd = 34.0 * (double)bytesPerFrame; // Through frame 34
    audioPosition = loopStart;
    loopEnabled = true;
    boomerangMode = false;
    isAudioEnabled = initJackAudio();
    if (!isAudioEnabled)
        std::cerr << "Warning: JACK audio init failed; continuing without audio." << std::endl;
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW." << std::endl;
        return EXIT_FAILURE;
    }
    primaryMonitor = glfwGetPrimaryMonitor();
    int windowWidth = FRAME_WIDTH * WINDOW_SCALE;
    int windowHeight = FRAME_HEIGHT * WINDOW_SCALE;
    bool startFullscreen = false;
    GLFWwindow* window = startFullscreen ?
        glfwCreateWindow(glfwGetVideoMode(primaryMonitor)->width, glfwGetVideoMode(primaryMonitor)->height,
            "Binary Waterfall Media Player", primaryMonitor, NULL)
        : glfwCreateWindow(windowWidth, windowHeight, "Binary Waterfall Media Player", NULL, NULL);
    if (!window) {
        std::cerr << "Failed to create window." << std::endl;
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glfwSetKeyCallback(window, keyCallback);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glViewport(0, 0, windowWidth, windowHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, windowWidth, windowHeight, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    double lastVisualUpdate = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        double currentTime = glfwGetTime();
        processInput(window);
        // Update title bar with current frame and frequency info.
        double fileSize = (double)fileData.size();
        double wrappedPos = wrapPosition(audioPosition, fileSize);
        size_t currentFrame = (size_t)(wrappedPos / (double)bytesPerFrame);
        if (currentFrame >= totalFrames) currentFrame = totalFrames - 1;
        char title[512];
        int absFreq = std::abs(playbackFrequency);
        const char* freqUnit = (absFreq >= 1000) ? "kHz" : "Hz";
        double freqValue = (absFreq >= 1000) ? (double)absFreq / 1000.0 : (double)absFreq;
        std::string flags;
        if (isPaused) flags += " [PAUSED]";
        if (!isAudioEnabled) flags += " [MUTED]";
        if (playbackFrequency < 0 && !isPaused) flags += " [REVERSE]";
        if (loopEnabled) { flags += boomerangMode ? " [BOOMERANG]" : " [LOOP]"; }
        std::snprintf(title, sizeof(title),
            "Binary Waterfall Player - Frame: %zu/%zu - Frequency: %.1f %s%s",
            currentFrame + 1, totalFrames, freqValue, freqUnit, flags.c_str());
        glfwSetWindowTitle(window, title);
        // Cap visual updates to VISUAL_FPS_CAP
        if (currentTime - lastVisualUpdate >= 1.0 / VISUAL_FPS_CAP) {
            renderFrame(window);
            glfwSwapBuffers(window);
            lastVisualUpdate = currentTime;
        }
        glfwPollEvents();
    }
    closeJackAudio();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
