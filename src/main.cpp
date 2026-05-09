///////////////////////////////////////////////////////////////////////////////
//         Mesh2Splat: fast mesh to 3D gaussian splat conversion             //
//        Copyright (c) 2025 Electronic Arts Inc. All rights reserved.       //
///////////////////////////////////////////////////////////////////////////////

#include "utils/argparser.hpp"
#include "utils/normalizedUvUnwrapping.hpp"
#include "renderer/renderer.hpp"
#include "glewGlfwHandlers/glewGlfwHandler.hpp"
#include "renderer/guiRendererConcreteMediator.hpp"
#include <iostream>

static void printUsage(const char* programName) {
    std::cout << "Mesh2Splat v1.0.0 - Fast mesh to 3D Gaussian Splat conversion\n"
              << "Usage:\n"
              << "  " << programName << "                                   GUI mode (default)\n"
              << "  " << programName << " --cli -i <file.glb> -o <file.ply>  [options]\n"
              << "\nCLI options:\n"
              << "  --cli                     Enable command-line mode (no GUI)\n"
              << "  -i, --input  <file>       Input .glb mesh file (required)\n"
              << "  -o, --output <file>       Output .ply file (required)\n"
              << "  -r, --resolution <N>      Conversion resolution (default: 1024)\n"
              << "  -f, --format <N>          Export format: 0=Standard, 1=PBR, 2=Compressed PBR (default: 0)\n"
              << "  --help                    Show this help message\n";
}

static int runCli(const std::string& inputFile, const std::string& outputFile,
                  int resolution, unsigned int format) {
    // Minimal GLFW + OpenGL init with a hidden window
    if (!glfwInit()) {
        std::cerr << "[Mesh2Splat] Failed to initialize GLFW\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(resolution, resolution, "Mesh2Splat CLI", nullptr, nullptr);
    if (!window) {
        std::cerr << "[Mesh2Splat] Failed to create OpenGL 4.5 context (check GPU/drivers)\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "[Mesh2Splat] Failed to initialize GLEW\n";
        glfwTerminate();
        return 1;
    }

    // Camera is required by the Renderer constructor but unused during conversion
    Camera camera(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, 0.0f);

    // Initialize renderer — compiles all shaders, allocates GPU buffers
    Renderer renderer(window, camera);
    renderer.initialize();

    std::cout << "[Mesh2Splat] Loading: " << inputFile << "\n";

    // Load model
    std::string parentFolder =
        std::filesystem::path(inputFile).parent_path().string() + "//";
    if (!renderer.getSceneManager().loadModel(inputFile, parentFolder)) {
        std::cerr << "[Mesh2Splat] Failed to load model: " << inputFile << "\n";
        glfwTerminate();
        return 1;
    }

    // Configure conversion
    renderer.resetModelMatrices();
    renderer.gaussianBufferFromSize(static_cast<unsigned int>(resolution * resolution));
    renderer.setViewportResolutionForConversion(resolution);
    renderer.setFormatType(format);

    // Run conversion pass
    renderer.enableRenderPass(conversionPassName);
    renderer.renderFrame();

    // Export — read GPU data back synchronously, then save on the main thread.
    // NOTE: we do NOT use SceneManager::exportPly() here because it spawns a
    // detached thread that may not finish before the process exits.
    std::cout << "[Mesh2Splat] Exporting: " << outputFile << "\n";

    {
        RenderContext* ctx = renderer.getRenderContext();
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->gaussianBuffer);

        std::vector<utils::GaussianDataSSBO> cpuData(ctx->numberOfGaussians);

        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        glGetBufferSubData(
            GL_SHADER_STORAGE_BUFFER, 0,
            ctx->numberOfGaussians * sizeof(utils::GaussianDataSSBO),
            cpuData.data()
        );
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        float scaleMultiplier =
            ctx->gaussianStd / static_cast<float>(ctx->resolutionTarget);
        parsers::savePlyVector(outputFile, cpuData, format, scaleMultiplier);
    }

    glfwTerminate();
    std::cout << "[Mesh2Splat] Done.\n";
    return 0;
}

int main(int argc, char** argv) {
    InputParser input(argc, argv);

    if (input.cmdOptionExists("--help")) {
        printUsage(argv[0]);
        return 0;
    }

    if (input.cmdOptionExists("--cli")) {
        std::string inputFile = input.getCmdOption("-i");
        if (inputFile.empty()) inputFile = input.getCmdOption("--input");

        std::string outputFile = input.getCmdOption("-o");
        if (outputFile.empty()) outputFile = input.getCmdOption("--output");

        if (inputFile.empty() || outputFile.empty()) {
            printUsage(argv[0]);
            std::cerr << "\nError: --input (-i) and --output (-o) are required in CLI mode.\n";
            return 1;
        }

        std::string resStr = input.getCmdOption("-r");
        if (resStr.empty()) resStr = input.getCmdOption("--resolution");
        int resolution = resStr.empty() ? 1024 : std::stoi(resStr);

        std::string fmtStr = input.getCmdOption("-f");
        if (fmtStr.empty()) fmtStr = input.getCmdOption("--format");
        unsigned int format = fmtStr.empty() ? 0 : static_cast<unsigned int>(std::stoul(fmtStr));

        return runCli(inputFile, outputFile, resolution, format);
    }

    // ======== GUI mode (original) ========

    GlewGlfwHandler glewGlfwHandler(glm::ivec2(1080, 720), "Mesh2Splat");

    Camera camera(
        glm::vec3(0.0f, 0.0f, 5.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        -90.0f,
        0.0f
    );

    IoHandler ioHandler(glewGlfwHandler.getWindow(), camera);
    if (glewGlfwHandler.init() == -1) return -1;

    ioHandler.setupCallbacks();

    ImGuiUI ImGuiUI(0.65f, 0.5f);
    ImGuiUI.initialize(glewGlfwHandler.getWindow());

    Renderer renderer(glewGlfwHandler.getWindow(), camera);
    renderer.initialize();
    GuiRendererConcreteMediator guiRendererMediator(renderer, ImGuiUI);

    float deltaTime = 0.0f;
    float lastFrame = 0.0f;

    while (!glfwWindowShouldClose(glewGlfwHandler.getWindow())) {

        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        glfwPollEvents();

        ioHandler.processInput(deltaTime);

        renderer.clearingPrePass(ImGuiUI.getSceneBackgroundColor());

        ImGuiUI.preframe();
        ImGuiUI.renderUI();

        guiRendererMediator.update();

        renderer.renderFrame();

        ImGuiUI.displayGaussianCounts(renderer.getTotalGaussianCount(), renderer.getVisibleGaussianCount());
        ImGuiUI.postframe();

        glfwSwapBuffers(glewGlfwHandler.getWindow());
    }

    glfwTerminate();

    return 0;
}
