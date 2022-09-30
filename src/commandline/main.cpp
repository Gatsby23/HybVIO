#include <opencv2/opencv.hpp>
#include <deque>
#include <thread>
#include <future>

#include "input.hpp"
#include "../commandline/command_queue.hpp"
#include "../commandline/parameters.hpp"
#include "../api/vio.hpp"
#include "../tracker/util.hpp"
#include "video_input.hpp"
#include "imu_visualization.hpp"
#include "../util/util.hpp"
#include "../util/timer.hpp"
#include "../odometry/debug.hpp"
#include "../odometry/util.hpp"
#include "../views/views.hpp"
#include "../views/visualization_internals.hpp"
#include "../api/output_buffer.hpp"
#include "../api/type_convert.hpp"

#include <accelerated-arrays/future.hpp>
#include <accelerated-arrays/opencv_adapter.hpp>
#ifdef DAZZLING_GPU_ENABLED
#include <accelerated-arrays/opengl/image.hpp>
#include <accelerated-arrays/opengl/operations.hpp>
#endif

#ifdef USE_SLAM
#include "../slam/slam_implementation.hpp"
#ifdef BUILD_VISUALIZATIONS
#include "../slam/slam_viewer.hpp"
#endif
#endif

#ifdef BUILD_VISUALIZATIONS
#include "../api/slam_map_point_record.hpp"
#include "visual_update_viewer.hpp"
#endif

namespace {

using PoseHistoryMap = std::map<api::PoseHistory, std::vector<api::Pose>>;

/*
 *
 */
class Visualizer {
public:
    /***********************
     * Visualizer类初始化
     * @param parameters 可视化中对应的参数设置
     * @param hasMapViewer 是否有地图可视化[开启SLAM后才会启用]
     * @param hasVuViewer  是否有里程计可视化地图
     **********************/
    Visualizer(const cmd::Parameters &parameters, bool hasMapViewer, bool hasVuViewer) :
#ifdef BUILD_VISUALIZATIONS
#ifdef USE_SLAM
        mapViewer(parameters, commands),
#endif
        vuViewer(odometry::viewer::VisualUpdateViewer::create(parameters, commands)),
#endif
        hasMapViewer(hasMapViewer),
        hasVuViewer(hasVuViewer)
    {
        // Silence warnings in BUILD_VISUALIZATIONS=OFF build.
        (void)this->hasMapViewer;
        (void)this->hasVuViewer;

        if (parameters.main.gpu) {
        #ifdef DAZZLING_GPU_ENABLED
            if (hasMapViewer || hasVuViewer) {
                auto queue = accelerated::Processor::createQueue();
                gpuProcessingQueue = queue.get();
                visualProcessor = std::move(queue);
            } else {
            #ifdef DAZZLING_GLFW_ENABLED
                // if not OpenGL window & context, create a hidden one
                #ifdef __APPLE__
                // On Mac we must run OpenGL stuff in main thread, so use sync mode.
                // TOOD: Process OpenGL stuff from queue on main thread and run algorithm on separate thread
                // 用OpenGL来进行加速
                visualProcessor = accelerated::opengl::createGLFWProcessor(
                    accelerated::opengl::GLFWProcessorMode::SYNC);
                #else
                visualProcessor = accelerated::opengl::createGLFWProcessor(
                    accelerated::opengl::GLFWProcessorMode::ASYNC);
                #endif
            #else
                assert(false && "No GLFW available!");
            #endif
            }
        #else
            assert(false && "compiled without GPU support");
        #endif
        } else {
            // 依据第三方库(accelerated-arrays)来构建处理对象
            visualProcessor = accelerated::Processor::createInstant();
        }
    }
    // TODO.
    void addVisualizationMat(const std::string &title, const cv::Mat &frame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (visualizations.find(title) == visualizations.end()) {
            visualizations[title] = frame;
        }
    }

    // Call from the main thread. OS X cannot do graphics from outside main thread.
    // TODO.
    bool visualizeMats() {
        std::lock_guard<std::mutex> lock(mutex);
        // 如果有退出命令则直接退出
        if (shouldQuit) return false;
        // 这里是对SLAM地图对象进行显示.
#if defined(BUILD_VISUALIZATIONS) && defined(USE_SLAM)
        for (const auto &v : mapViewer.get_data_publisher().pollVisualizations()) {
            visualizations[v.first] = v.second;
        }
#endif
        // 这里是对taggled frame进行显示
        for (auto const &v : visualizations) {
            assert(!v.second.empty());
            cv::imshow(v.first, v.second);
        }
        // Note! This doesn't conflict with Pangolin, only one of them can detect a single keypress, there is no duplication
        // 等待输入（注意这里并不和Pangolin相冲突）
        int key = cv::waitKey(1);
        if (key >= 0) {
            commands.keyboardInput(key);
        }
        visualizations.clear();
        return true;
    }

    void terminate() {
        log_debug("Visualizer::terminate");
        std::lock_guard<std::mutex> lock(mutex);
        shouldQuit = true;
    }

    void cleanup() {
        log_debug("Visualizer::cleanup start");
        if (gpuProcessingQueue) {
            gpuProcessingQueue->processAll();
        }
        else {
            visualProcessor->enqueue([]() {}).wait();
        }
        log_debug("Visualizer::cleanup completed");
    }

    void setFrameRotation(int rotation) {
#if defined(BUILD_VISUALIZATIONS) && defined(USE_SLAM)
        std::lock_guard<std::mutex> lock(mutex);
        mapViewer.get_data_publisher().setFrameRotation(rotation);
#else
        (void)rotation;
#endif
    }

    CommandQueue& getCommands() {
        return commands;
    }

    accelerated::Future processMaybeOnGpu(const std::function<void()> &op) {
        assert(visualProcessor);
        return visualProcessor->enqueue(op);
    }

#ifdef BUILD_VISUALIZATIONS
    void blockWhilePaused() {
#ifdef USE_SLAM
        while (mapViewer.is_paused()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
#endif
    }

    // 启动可视化窗口
    void setupViewers() {
        std::lock_guard<std::mutex> lock(mutex);
        // 对于SLAM的地图可视化
#ifdef USE_SLAM
        if (hasMapViewer) mapViewer.setup();
#endif
        // 对于里程计的可视化
        if (hasVuViewer) vuViewer->setup();
    }

    void setFixedData(
        const PoseHistoryMap &poseHistories,
        const Eigen::Matrix4d &imuToCamera,
        const Eigen::Matrix4d &secondImuToCamera
    ) {
        std::lock_guard<std::mutex> lock(mutex);
        if (hasVuViewer) vuViewer->setFixedData(poseHistories, imuToCamera, secondImuToCamera);
    }

#ifdef USE_SLAM
    slam::viewer::Viewer mapViewer;
#endif
    std::unique_ptr<odometry::viewer::VisualUpdateViewer> vuViewer;
#else
    void blockWhilePaused() {}
    void setupViewers() {}
    void drawViewer() {}
#endif

    // 这里进行窗口绘制
    void draw() {
        std::lock_guard<std::mutex> lock(mutex);
        if (shouldQuit) return;
#ifdef BUILD_VISUALIZATIONS
#ifdef USE_SLAM
        if (hasMapViewer) mapViewer.draw();
#endif
        if (hasVuViewer) vuViewer->draw();
#endif
        if (gpuProcessingQueue) gpuProcessingQueue->processAll();
    }

    accelerated::Processor &getProcessor() {
        assert(visualProcessor);
        return *visualProcessor;
    }

private:
    const bool hasMapViewer;
    const bool hasVuViewer;
    CommandQueue commands;
    std::mutex mutex;
    std::map<std::string, cv::Mat> visualizations;
    bool shouldQuit = false;
    std::unique_ptr<accelerated::Processor> visualProcessor;
    accelerated::Queue* gpuProcessingQueue = nullptr;
};

class FpsMeter {
private:
    std::chrono::steady_clock::time_point t0;
    size_t frameCount = 0;
    static constexpr size_t UPDATE_INTERVAL_FRAMES = 10;
    float curFps = 0;

public:
    FpsMeter() { t0 = std::chrono::steady_clock::now(); }

    float update() {
        if (++frameCount % UPDATE_INTERVAL_FRAMES == 0) {
            auto now = std::chrono::steady_clock::now();
            auto tNanos = (now - t0).count();
            curFps = UPDATE_INTERVAL_FRAMES / (1e-9 * tNanos);
            t0 = now;
        }
        return curFps;
    }
};

struct VideoConfig {
    int algorithmWidth, algorithmHeight;
    int inputWidth, inputHeight;
    int frameSub = 1;
    double displayScale = 1.0;
    double algorithmScale = 1.0;
    std::map<int, std::unique_ptr<VideoInput>> videoInputs;
};

std::vector<int> getCameraInds(const CommandLineParameters &cmd) {
    if (cmd.parameters.tracker.useStereo) {
        assert(cmd.parameters.tracker.leftCameraId < 2);
        assert(cmd.parameters.tracker.rightCameraId < 2);
        return {cmd.parameters.tracker.leftCameraId, cmd.parameters.tracker.rightCameraId};
    } else {
        return {0};
    }
}

std::unique_ptr<odometry::InputI> setupInputAndOutput(
  CommandLineParameters& cmd,
  VideoConfig &videoConfig,
  std::string &outputPath,
  int argc, char *argv[])
{
    const cmd::ParametersMain &main = cmd.cmd.main;
    int datasetVideoIndex = main.datasetVideoIndex ;

    if (main.inputPath == "" && datasetVideoIndex <= 0) {
        log_error("No input specified.");
        return 0;
    }

    int inputWidth, inputHeight;

    std::string inputPath = main.inputPath;
    if (datasetVideoIndex > 0) {
        // ADVIO data.
        std::ostringstream oss;
        oss << "../data/benchmark/advio-" << std::setw(2) << std::setfill('0') << datasetVideoIndex;
        inputPath = oss.str();

        log_debug("ADVIO %d: shorthand for -i=%s", datasetVideoIndex, inputPath.c_str());
    }

    std::unique_ptr<odometry::InputI> input;

    if (odometry::pathHasFile(inputPath, "data.jsonl")) {
        input = odometry::buildInputJSONL(inputPath, true, main.parametersPath);
    }
    else if (odometry::pathHasFile(inputPath, "data.csv")) {
        input = odometry::buildInputCSV(inputPath);
    }
    else {
        log_warn("No data.{jsonl,csv} file found. Does %s/ exist?", inputPath.c_str());
        return nullptr;
    }

    // Read camera parameters and such from `data.jsonl` and …
    input->setAlgorithmParametersFromData(cmd.parameters);

    // … then set `parameters.txt`, allowing to overwrite any `data.jsonl` parameters.
    std::string parametersString = input->getParametersString();
    if (!parametersString.empty()) {
        log_debug("Setting parameters from:");
        log_debug("  %s", parametersString.c_str());
    }
    input->setParameters(cmd);

    // … read calibration file if it exists, overwriting values from data.jsonl and parameters.txt …
    std::string calibrationPath = !main.calibrationPath.empty()
        ? main.calibrationPath : util::joinUnixPath(inputPath, "calibration.json");
    std::ifstream calibrationFile(calibrationPath);
    if (calibrationFile.is_open()) {
        if (!parametersString.empty()) {
            log_warn("Using both parameters.txt and calibration.json. Values from latter override the former.");
        }
        log_debug("Using calibration from:");
        log_debug("  %s", calibrationPath.c_str());
        cmd.parse_calibration_json(calibrationFile);
    }
    else if (!main.calibrationPath.empty()) {
        log_error("File %s could not be opened.", main.calibrationPath.c_str());
        assert(false);
    }

    // parse again to allow overriding automatically set parameters.
    cmd.parse_argv(argc, argv);

    if (main.outputPath != "") {
        outputPath = main.outputPath;
        log_debug("output path overridden as %s", outputPath.c_str());
    }

    // Setup video inputs.
    // 这里CameraInds主要是是为了多目来设置->目前单目的话，可以暂时不用管.
    for (int cameraInd : getCameraInds(cmd)) {
        std::string videoPath = input->getInputVideoPath(cameraInd);
        // 这里构建video读取方式->这里的时间戳是怎么来设定的呢？
        videoConfig.videoInputs[cameraInd] = VideoInput::build(
            videoPath,
            cmd.parameters.tracker.convertVideoToGray,
            cmd.parameters.tracker.videoReaderThreads,
            cmd.parameters.tracker.ffmpeg,
            cmd.parameters.tracker.vf
        );
    }

    if (videoConfig.videoInputs.at(0)) {
        double fps = videoConfig.videoInputs.at(0)->probeFPS();
        assert(fps > 0.0);

        videoConfig.frameSub = static_cast<int>(round(fps / cmd.parameters.tracker.targetFps));
        if (videoConfig.frameSub <= 0) videoConfig.frameSub = 1;
        log_debug("The framerate is %.1f/%d = %.1f fps.", fps, videoConfig.frameSub, fps / static_cast<double>(videoConfig.frameSub));

        videoConfig.videoInputs.at(0)->probeResolution(inputWidth, inputHeight);
        if (cmd.parameters.tracker.focalLength < 0 && datasetVideoIndex > 0) {
            // Otherwise need to scale the given focal length values.
            assert(inputWidth == 1280 && inputHeight == 720);
        }

        // Scale of image to feed odometry (tracker). Upsample only explicitly.
        if (main.targetFrameWidthUpsample > 0) {
            videoConfig.algorithmScale = double(main.targetFrameWidthUpsample) / inputWidth;
            if (videoConfig.algorithmScale > 1.0) log_warn("Upsampling algorithm frame input.");
        }
        else {
            videoConfig.algorithmScale = std::min(double(main.targetFrameWidth) / inputWidth, 1.0);
        }
        // Use a dummy resize to predict resized frame dimensions because
        // neither round() nor floor() produce correct results for very small scales.
        cv::Mat resizeTest(inputHeight, inputWidth, CV_8UC1);
        cv::resize(resizeTest, resizeTest, cv::Size(), videoConfig.algorithmScale, videoConfig.algorithmScale, cv::INTER_CUBIC);
        videoConfig.algorithmWidth = resizeTest.cols;
        videoConfig.algorithmHeight = resizeTest.rows;

        // Size of image to display on screen.
        double displayLongerSide = main.windowResolution;
        const int algoLongerSide = std::max(videoConfig.algorithmWidth, videoConfig.algorithmHeight);
        if (displayLongerSide <= 0) displayLongerSide = algoLongerSide;

        videoConfig.inputWidth = inputWidth;
        videoConfig.inputHeight = inputHeight;

        // To avoid hassle of scaling pixel coordinates of drawn tracks etc, for display window,
        // scale the already scaled algorithm input image.
        videoConfig.displayScale = displayLongerSide / algoLongerSide;
    } else if (cmd.parameters.odometry.visualUpdateEnabled) {
        std::cerr << "Video is required if running without -vu=false" << std::endl;
        return nullptr;
    }

    if (videoConfig.algorithmScale != 1.0)
        for (auto &it : videoConfig.videoInputs) {
            it.second->resize(videoConfig.algorithmWidth, videoConfig.algorithmHeight);
        }

    return input;
}

void writePointCloudToCsv(double t, const std::vector<api::FeaturePoint> &pointCloud, std::ostream &out) {
    for (const auto &p : pointCloud) {
        out << t << ","
          << p.id << ","
          << p.position.x << ","
          << p.position.y << ","
          << p.position.z
          << std::endl;
    }
}

} // namespace

// 这里是程序启动，算法系统运行的地方.
int run_algorithm(int argc, char *argv[], Visualizer &visualizer, CommandLineParameters &cmd) {
    const cmd::ParametersMain &main = cmd.cmd.main;
    // 这里的含义后面再看
    if (main.logLevel >= odometry::Parameters::VERBOSITY_DEBUG)
        cmd.parameters.verbosity = odometry::Parameters::VERBOSITY_DEBUG;
    if (main.logLevel >= odometry::Parameters::VERBOSITY_EXTRA)
        cmd.parameters.verbosity = odometry::Parameters::VERBOSITY_EXTRA;
    // RAII计时器系统，退出的时候表明计时
    std::unique_ptr<util::TimeStats> mainTimeStats;
    if (cmd.cmd.main.timer) {
        odometry::TIME_STATS = std::make_unique<util::TimeStats>();
        slam::TIME_STATS = std::make_unique<util::TimeStats>();
        // 计时系统
        mainTimeStats = std::make_unique<util::TimeStats>();
    }
    // 视频数据解析.
    VideoConfig videoConfig;
    // 结果输出存放的地方.
    std::string outputPath;
    std::unique_ptr<std::ofstream> pointCloudOutput; // use unique_ptr to auto-close
    // 将视频信息解析输入.
    auto input = setupInputAndOutput(cmd, videoConfig, outputPath, argc, argv);
    // 如果没有视频信息，则直接退出
    if (!input) {
        visualizer.terminate();
        return 1;
    }
    // debug调试参数[这里怎么打开得细看下源码]->都是用来做debug可视化的.
    api::InternalAPI::DebugParameters debugParameters;
    debugParameters.recordingPath = main.recordingPath;
    debugParameters.videoRecordingPath = main.videoRecordingPath;
    debugParameters.api.inputFrameWidth = videoConfig.algorithmWidth;
    debugParameters.api.inputFrameHeight = videoConfig.algorithmHeight;
    debugParameters.api.parameters = cmd.parameters;
    debugParameters.nVisualBuffers = 5;
    debugParameters.visualizePointCloud = main.displayPointCloud;
    debugParameters.visualizePoseWindow = main.displayPose;
    debugParameters.videoAlgorithmScale = videoConfig.algorithmScale;
    if (!main.displayVideo) {
        debugParameters.visualization = api::InternalAPI::VisualizationMode::NONE;
    } else if (main.displayTracks) {
        debugParameters.visualization = api::InternalAPI::VisualizationMode::TRACKS;
    } else if (main.displayTracksAll) {
        debugParameters.visualization = api::InternalAPI::VisualizationMode::TRACKS_ALL;
    } else if (main.displayOpticalFlow != odometry::OpticalFlowVisualization::NONE) {
        debugParameters.api.parameters.tracker.saveOpticalFlow = main.displayOpticalFlow;
        if (main.displayOpticalFlow == odometry::OpticalFlowVisualization::FAILURES) {
            debugParameters.visualization = api::InternalAPI::VisualizationMode::OPTICAL_FLOW_FAILURES;
        } else {
            debugParameters.visualization = api::InternalAPI::VisualizationMode::OPTICAL_FLOW;
        }
    } else if (main.displayStereoMatching) {
        debugParameters.visualization = api::InternalAPI::VisualizationMode::STEREO_MATCHING;
    } else if (main.displayCornerMeasure) {
        debugParameters.visualization = api::InternalAPI::VisualizationMode::CORNER_MEASURE;
    } else if (main.displayStereoDisparity) {
        debugParameters.visualization = api::InternalAPI::VisualizationMode::STEREO_DISPARITY;
    } else if (main.displayStereoDepth) {
        debugParameters.visualization = api::InternalAPI::VisualizationMode::STEREO_DEPTH;
    } else if (main.displayStereoEpipolarCurves != odometry::StereoEpipolarVisualization::NONE) {
        debugParameters.api.parameters.tracker.saveStereoEpipolar = main.displayStereoEpipolarCurves;
        debugParameters.visualization = api::InternalAPI::VisualizationMode::STEREO_EPIPOLAR;
    } else if (main.displayPlainVideo) {
        debugParameters.visualization = api::InternalAPI::VisualizationMode::PLAIN_VIDEO;
    }

    // 如果是在step模式下->则按键a开始正常播放.
    if (main.stepMode) {
        visualizer.getCommands().keyboardInput('a'); // TODO. hacky
    }

    // 这里是创建VIO系统
    std::shared_ptr<api::InternalAPI> apiPtr = api::buildVio(debugParameters);
    // 获得VIO引用.
    auto &api = *apiPtr;

    api::OutputBuffer outputBuffer;
    outputBuffer.targetDelaySeconds = debugParameters.api.parameters.odometry.targetOutputDelaySeconds;

    {
        // Tracker对象的配置参数.
        const auto &pt = cmd.parameters.tracker;
        // 输出这里的对应的使用参数
        log_debug("Focal length: %g, %g, PP: %g, %g, Input size: %dx%d, Algorithm frame size: %d, scale %.2f",
            pt.focalLengthX, pt.focalLengthY,
            pt.principalPointX, pt.principalPointY,
            videoConfig.inputWidth, videoConfig.inputHeight,
            videoConfig.algorithmWidth, videoConfig.algorithmScale);
        if (pt.useStereo) {
            log_debug("Second camera: Focal length: %g, %g, PP: %g, %g",
                pt.secondFocalLengthX, pt.secondFocalLengthY,
                pt.secondPrincipalPointX, pt.secondPrincipalPointY);
        }
    }
    // IMU可视化
    ImuVisualization gyroVisu(5.0), accVisu(30.0), uncertaintyVisuPos(.02), uncertaintyVisuRot(.2);
#ifdef BUILD_VISUALIZATIONS
    std::vector<slam::MapPointRecord> pointCloud;
    odometry::DebugAPI odometryDebug;
    odometryDebug.endDebugCallback = [&pointCloud](const std::map<int, slam::MapPointRecord> &pointCloudExtend) {
        // Convert to vector because we don't need the track ids.
        pointCloud.reserve(pointCloud.size() + pointCloudExtend.size());
        for (const auto &it : pointCloudExtend) {
            pointCloud.push_back(it.second);
        }
    };
    // Get the publisher....[Don't know how to connect with two process.]
    if (main.visualUpdateViewer) {
        odometryDebug.publisher = &visualizer.vuViewer->getPublisher();
    }
// 如果使用SLAM模式[第二阶段再进行解析]
#ifdef USE_SLAM
    if (cmd.parameters.slam.useSlam) {
        odometryDebug.slamDebug = std::make_unique<slam::DebugAPI>();
        odometryDebug.slamDebug->dataPublisher = &visualizer.mapViewer.get_data_publisher();
        odometryDebug.slamDebug->commandQueue = &visualizer.getCommands();
        odometryDebug.slamDebug->mapSavePath = main.slamMapPosesPath;
        odometryDebug.slamDebug->endDebugCallback = [&pointCloud](const std::vector<slam::MapPointRecord> &pointCloudExtend) {
            pointCloud.reserve(pointCloud.size() + pointCloudExtend.size());
            pointCloud.insert(pointCloud.end(), pointCloudExtend.begin(), pointCloudExtend.end());
        };
    }
#endif
    // 关联
    apiPtr->connectDebugApi(odometryDebug);
#endif

    // 点云输出路径
    std::string pointCloudOutputPath = main.pointCloudOutputPath;
    if (!pointCloudOutputPath.empty()) {
        pointCloudOutput = std::make_unique<std::ofstream>(pointCloudOutputPath.c_str());
        if (!*pointCloudOutput) {
            std::cerr << "Unable to open point cloud output file "
                << pointCloudOutputPath << std::endl;
            return 1;
        }
    }

    // 输出路径
    std::ofstream outputFile;
    if (outputPath != "") {
        outputFile.open(outputPath);
    }

    // 历史地图，不知道是什么含义
    PoseHistoryMap poseHistories = input->getPoseHistories();
    for (auto it = poseHistories.begin(); it != poseHistories.end(); ++it) {
        // TODO: move to new api
        apiPtr->setPoseHistory(it->first, it->second);
    }

#ifdef BUILD_VISUALIZATIONS
    // Duplication from automaticCameraParametersWhereUnset().
    cmd.parameters.imuToCamera = odometry::util::vec2matrix(cmd.parameters.odometry.imuToCameraMatrix);
    if (cmd.parameters.odometry.secondImuToCameraMatrix.size() > 1) {
        cmd.parameters.secondImuToCamera = odometry::util::vec2matrix(cmd.parameters.odometry.secondImuToCameraMatrix);
    } else {
        cmd.parameters.secondImuToCamera = cmd.parameters.imuToCamera;
    }
    // Don't know...
    visualizer.setFixedData(poseHistories, cmd.parameters.imuToCamera, cmd.parameters.secondImuToCamera);
#endif

    // 这里lambda表达式->最主要什么时候调用.
    api.onOutput = [&](std::shared_ptr<const api::VioApi::VioOutput> output) {
        outputBuffer.addProcessedFrame(output);
        if (outputFile) {
            std::string outputJson = api::outputToJson(*output, main.outputType == "tail");
            outputFile << outputJson << std::endl;
        }
        if (pointCloudOutput) {
            writePointCloudToCsv(output->pose.time, output->pointCloud, *pointCloudOutput);
        }
    };

    // 是否需要旋转视频
    int rotation = cmd.parameters.odometry.rot;
    if (rotation != 0) {
        log_debug("Rotating display video %d degrees clockwise.", 90 * rotation);
    }
    visualizer.setFrameRotation(rotation);

    const double CAMERA_PAUSE_LENGTH_SECONDS = 1.0;
    bool cameraPaused = false;
    double cameraPauseStartTime = -1.0;
    double lastVisuTime = 0.0;
    bool firstFrame = true;
    bool shouldQuit = false;
    int processedFrames = 0, outputCounter = 0;
    int lastFramesInd = 0;
    int maxFrames = main.maxFrames;
    bool useStereo = cmd.parameters.tracker.useStereo;
    std::shared_ptr<const api::VioApi::VioOutput> apiOutput = nullptr;

    // 都是可视化对象
    std::shared_ptr<accelerated::Image> frameVisualizationImage;
    std::shared_ptr<api::Visualization> visualizationVideo;
    if (main.displayVideo) visualizationVideo = apiPtr->createVisualization("VIDEO");
    std::shared_ptr<api::Visualization> visualizationKfCorrelation;
    std::shared_ptr<api::Visualization> visualizationPose;
    std::shared_ptr<api::Visualization> visualizationCovarianceMagnitude;
    std::map<int, std::shared_ptr<cv::Mat>> inputFrames;
    cv::Mat cpuVisuFrame, resizedColorFrame, rotatedColorFrame, corrFrame, poseFrame, magnitudeFrame;
    cv::Mat prevGrayFrame;
    FpsMeter fpsMeter;

    api::CameraParameters lastIntrinsic[2];
    std::vector<odometry::InputFrame> frames;
    std::vector<int> cameraInds = getCameraInds(cmd);

    #ifdef DAZZLING_GPU_ENABLED
    std::unique_ptr<accelerated::Image::Factory> gpuImageFactory;
    std::array<std::shared_ptr<accelerated::Image>, 2> gpuInputImages;
    #endif

    // only needed in certain experimental combinations with pyrLKUseGpu
    // 得弄懂lambda后才能弄清楚
    api.onOpenGlWork = [&visualizer, &apiPtr] {
        std::weak_ptr<api::InternalAPI> apiWeak = apiPtr;
        visualizer.processMaybeOnGpu([apiWeak] {
            if (auto api = apiWeak.lock()) {
                api->processOpenGl();
            }
        });
    };

    while (true) {
        // 得到当前的传感器输入数据
        odometry::InputSample inputType = input->nextType();
        // 如果什么数据都没有，则直接退出
        if (inputType == odometry::InputSample::NONE) break;

        bool shouldVisualize = false;
        constexpr double INS_ONLY_VISU_INTERVAL = 0.1; // seconds
        bool didOutput = false;
        cv::Mat extraFrame;
        // 依据输入数据类型来进行数据操作.
        switch (inputType) {
            case odometry::InputSample::NONE: {
                break;
            }
            case odometry::InputSample::ECHO_RECORDING: {
                // Copy lines between the recordings that are not passed to the odometry API.
                apiPtr->addAuxiliaryJsonData(input->getLastJSONL());
                break;
            }
            // 输入陀螺仪数据.
            case odometry::InputSample::GYROSCOPE: {
                timer(mainTimeStats, "gyroscope");
                // 陀螺仪对应的时间戳和旋转角度
                double t;
                api::Vector3d p;
                input->getGyroscope(t, p);
                // 添加到系统中
                api.addGyro(t, { p.x, p.y, p.z });
                // 如果要可视化IMU数据，则放到gyroVisu中.
                if (main.displayImuSamples) gyroVisu.addSample(t, p);
                // TODO....
                if (!cmd.parameters.odometry.visualUpdateEnabled && t > lastVisuTime + INS_ONLY_VISU_INTERVAL) {
                    lastVisuTime = t;
                    shouldVisualize = true;
                }
                break;
            }
            // 添加IMU加速度计数据
            case odometry::InputSample::ACCELEROMETER: {
                timer(mainTimeStats, "accelerometer");
                // IMU加速度计对应的时间戳和加速度计数据
                double t;
                api::Vector3d p;
                input->getAccelerometer(t, p);
                api.addAcc(t, { p.x, p.y, p.z });
                if (main.displayImuSamples) accVisu.addSample(t, p);
                break;
            }
            // 添加图像数据
            case odometry::InputSample::FRAME: {
                if (mainTimeStats) mainTimeStats->startFrame();
                // 创建frame计时器系统.
                timer(mainTimeStats, "frame");

                double t;
                int framesInd;
                // 获得当前frame数据[时间戳、Frame Id 、图像数据[双目的话有两张图]].
                input->getFrames(t, framesInd, frames);
                assert(frames.size() >= (useStereo ? 2 : 1));

                int skipNFrames = 0;
                // 如果是第一帧，则要初始化操作->这里对应android设备需要注意图像数据并不是从0开始（感觉可以写成嵌入式）.
                if (firstFrame) {
                    // The first frame number should ideally be 0 so that
                    // the modulus operation below doesn't skip early frames,
                    // but in some Android recordings it is 1.
                    if (framesInd > 1) {
                        log_warn("First frame number is %d.", framesInd);
                    }
                    firstFrame = false;
                    lastFramesInd = framesInd;
                }
                else {
                    // Check frame indices are consecutive.
                    // 工程化处理（不同硬件设备上，frame id并不是连续的）
                    if (!useStereo) {
                        int expectedFrameNumber = lastFramesInd + 1;
                        if (framesInd > expectedFrameNumber) {
                            log_warn("skipping frame numbers %d to %d", expectedFrameNumber, framesInd - 1);
                            assert(cmd.parameters.odometry.allowSkippedFrames);
                            skipNFrames = framesInd - expectedFrameNumber;
                        } else if (framesInd < expectedFrameNumber) {
                            log_warn("missing / duplicate / out-of-order frame %d", framesInd);
                            assert(cmd.parameters.odometry.allowSkippedFrames);
                            continue;
                        }
                    }
                    // 更新上一帧id.
                    lastFramesInd = framesInd;
                }

                bool gotFrame = false;
                for (int j = 0; j < skipNFrames + 1; ++j) {
                    for (int cameraInd : cameraInds) {
                        // 得到图像数据.
                        auto frame = videoConfig.videoInputs.at(cameraInd)->readFrame();
                        // 这里是对多目处理.
                        inputFrames[cameraInd] = frame;
                        gotFrame = !!frame;
                        // 判断当前是否拿到了图像数据.
                        // Happens with OpenCV input at the end of the video sometimes.
                        if (frame->rows == 0 && frame->cols == 0) {
                            log_debug("Discarding a bad frame.");
                            gotFrame = false;
                        }

                        if (!gotFrame) break;

                        assert(frame->cols == videoConfig.algorithmWidth &&
                            frame->rows == videoConfig.algorithmHeight);
                        // 如果命令行中对图像数据进行了缩放，则将内参也进行缩放.
                        if (videoConfig.algorithmScale != 1.0) {
                            odometry::InputFrame &meta = frames.at(cameraInd);
                            meta.intrinsic.focalLengthX *= videoConfig.algorithmScale;
                            meta.intrinsic.focalLengthY *= videoConfig.algorithmScale;
                        }
                    }
                    if (!gotFrame) break;
                }
                //TODO....
                if (framesInd % videoConfig.frameSub != 0) {
                    if (!cmd.parameters.tracker.useStereo && debugParameters.videoRecordingPath.empty()) {
                        // Record also skipped frames so that the frame data remains compatible with video.
                        // TODO: Separate recorder?
                        apiPtr->recordFrames(t, 0.0, &frames[0].intrinsic);
                    }
                    break;
                }
                if (!gotFrame) break;
                // 处理一帧数据，进行计数
                processedFrames++;
                if (maxFrames > 0 && processedFrames > maxFrames) {
                    shouldQuit = true;
                }
                // 这里依据log等级来将数据进行输出.
                if (cmd.parameters.verbosity >= odometry::Parameters::VERBOSITY_DEBUG && apiOutput) {
                    // TODO: Relies on internal API
                    auto &debug = reinterpret_cast<const api::InternalAPI::Output&>(*apiOutput);
                    std::cout << debug.stateAsString << std::endl;
                    // Velocity pseudo update only considers the x and y components, so
                    // this print can give some idea how it's doing.
                    auto v = apiOutput->velocity;
                    double speed = std::sqrt(v.x * v.x + v.y * v.y);
                    std::cout << "xy speed: " << (speed * 3.6) << "km/h"
                        << " FPS " << fpsMeter.update()
                        << std::endl;
                }
                // 这里输出的是图像的时间戳.
                if (cmd.parameters.verbosity >= odometry::Parameters::VERBOSITY_EXTRA) {
                    std::cout << "t in microseconds "
                        << static_cast<long>(std::round(t * 1e6)) << std::endl;
                }

                if (cameraPaused && t < cameraPauseStartTime + CAMERA_PAUSE_LENGTH_SECONDS) {
                    break;
                }
                cameraPaused = false;


                shouldVisualize = true;
                // Don't know what did the tracker do？【这里似乎是为了双目，暂时看起来用不到】【这里intensity是默认为0.】
                if (cmd.parameters.tracker.matchSuccessiveIntensities > 0.0) {
                    cv::Mat &im = *inputFrames[0];
                    if (im.type() != CV_8UC1) cvtColor(im, im, cv::COLOR_BGR2GRAY);
                    if (prevGrayFrame.empty()) prevGrayFrame = im.clone();
                    tracker::util::matchIntensities(prevGrayFrame, im, cmd.parameters.tracker.matchSuccessiveIntensities);
                    im.copyTo(prevGrayFrame);
                }
                if (cmd.parameters.tracker.useStereo && cmd.parameters.tracker.matchStereoIntensities) {
                    assert(frames.size() == 2);
                    cv::Mat &im0 = *inputFrames[0];
                    cv::Mat &im1 = *inputFrames[1];
                    if (im0.type() != CV_8UC1) cvtColor(im0, im0, cv::COLOR_BGR2GRAY);
                    if (im1.type() != CV_8UC1) cvtColor(im1, im1, cv::COLOR_BGR2GRAY);
                    tracker::util::matchIntensities(im0, im1);
                }

                std::vector<std::shared_ptr<accelerated::Image>> frameImages;
                for (size_t i = 0; i < cameraInds.size(); ++i) {
                    int cameraInd = cameraInds[i];
                    assert(inputFrames.at(cameraInd)->data != nullptr);
                    auto openCvImg = accelerated::opencv::ref(*inputFrames.at(cameraInd), true); // prefer fixed point
                    if (main.gpu) {
                    #ifdef DAZZLING_GPU_ENABLED
                        if (!gpuImageFactory)
                            gpuImageFactory = accelerated::opengl::Image::createFactory(visualizer.getProcessor());
                        if (!gpuInputImages[i])
                            gpuInputImages[i] = gpuImageFactory->createLike(*openCvImg);
                        // async, but no need to wait here.
                        // NOTE: there is a risk that the cv::Mat is freed / overwritten unless there is
                        // sufficient buffering in VideoInput
                        accelerated::opencv::copy(*inputFrames.at(cameraInd), *gpuInputImages[i]);
                        frameImages.push_back(gpuInputImages[i]);
                    #else
                        assert(false);
                    #endif
                    } else {
                        frameImages.push_back(std::move(openCvImg));
                    }
                }

                const int tag = 0;
                auto &firstImage = *frameImages[0];
                // 这里最大的问题是不知道outputBuffer是如何与VIO系统进行交互的
                auto out = outputBuffer.pollOutput(t);
                didOutput = !!out;
                if (out) {
                    apiOutput = out->output;
                    outputCounter++;
                }

                api::VioApi::ColorFormat colorFormat =
                    firstImage.channels == 1 ? api::VioApi::ColorFormat::GRAY : api::VioApi::ColorFormat::RGB;

                if (!cmd.parameters.tracker.useStereo) {
                    assert(t == frames[0].t);
                    visualizer.processMaybeOnGpu([&]() {
                        if (firstImage.storageType == accelerated::Image::StorageType::CPU) {
                            api.addFrameMonoVarying(
                                frames[0].t,
                                frames[0].intrinsic,
                                firstImage.width,
                                firstImage.height,
                                accelerated::cpu::Image::castFrom(firstImage).getDataRaw(),
                                colorFormat,
                                tag);
                        } else {
                            #ifdef DAZZLING_GPU_ENABLED
                            api.addFrameMonoOpenGl(
                                frames[0].t,
                                frames[0].intrinsic,
                                firstImage.width,
                                firstImage.height,
                                accelerated::opengl::Image::castFrom(firstImage).getTextureId(),
                                colorFormat,
                                tag);
                            #endif
                        }
                    }).wait();
                } else {
                    auto &secondImage = *frameImages[1];
                    visualizer.processMaybeOnGpu([&]() {
                        if (firstImage.storageType == accelerated::Image::StorageType::CPU) {
                            api.addFrameStereo(
                                frames[0].t,
                                firstImage.width,
                                firstImage.height,
                                accelerated::cpu::Image::castFrom(firstImage).getDataRaw(),
                                accelerated::cpu::Image::castFrom(secondImage).getDataRaw(),
                                colorFormat,
                                tag);
                        } else {
                            #ifdef DAZZLING_GPU_ENABLED
                            api.addFrameStereoOpenGl(
                                frames[0].t,
                                firstImage.width,
                                firstImage.height,
                                accelerated::opengl::Image::castFrom(firstImage).getTextureId(),
                                accelerated::opengl::Image::castFrom(secondImage).getTextureId(),
                                colorFormat,
                                tag);
                            #endif
                        }
                    }).wait();
                }

                for (size_t frameIdx = 0; frameIdx < frames.size(); ++frameIdx) {
                    lastIntrinsic[frameIdx] = frames[frameIdx].intrinsic;
                }
                break;
            } // case FRAME
        } // switch inputType

        if (outputCounter % main.visuUpdateInterval != 0) shouldVisualize = false;
        // 这里是可视化
        if (shouldVisualize) {
            timer(mainTimeStats, "visualizations");
            if (main.displayVideo && didOutput) {
                assert(apiOutput);
                if (!frameVisualizationImage)
                    frameVisualizationImage = visualizationVideo->createDefaultRenderTarget();
                visualizationVideo->update(apiOutput);
                visualizer.processMaybeOnGpu([&]() {
                    visualizationVideo->render(*frameVisualizationImage);
                }).wait();
                if (frameVisualizationImage->storageType == accelerated::Image::StorageType::CPU) {
                    // The visualization produced output in CPU format (as a cv::Mat)
                    cpuVisuFrame = accelerated::opencv::ref(*frameVisualizationImage);
                } else {
                    // ... in GPU format
                    accelerated::opencv::copy(*frameVisualizationImage, cpuVisuFrame).wait();
                    // note: it would also be relatively simple to GPU-rescale here
                }
                // Scale and rotate for display on screen.
                if (videoConfig.displayScale == 1.0) {
                    resizedColorFrame = cpuVisuFrame;
                } else {
                    cv::resize(cpuVisuFrame,
                        resizedColorFrame, cv::Size(),
                        videoConfig.displayScale, videoConfig.displayScale, cv::INTER_CUBIC);
                }
                tracker::util::rotateMatrixCW90(resizedColorFrame, rotatedColorFrame, rotation);
                visualizer.addVisualizationMat("odometry", rotatedColorFrame);
            }
            if (main.displayCorrelation) {
                if (!visualizationKfCorrelation)
                    visualizationKfCorrelation = apiPtr->createVisualization("KF_CORRELATION");
                visualizer.processMaybeOnGpu([&]() { visualizationKfCorrelation->render(corrFrame); }).wait();
                visualizer.addVisualizationMat("correlation", corrFrame);
            }
            if (main.displayPose && apiOutput) {
                constexpr int gray = 150;
                poseFrame = cv::Scalar(gray, gray, gray);
                if (!visualizationPose)
                    visualizationPose = apiPtr->createVisualization("POSE");
                visualizationPose->update(apiOutput);
                if (visualizationPose->ready()) {
                    visualizer.processMaybeOnGpu([&]() { visualizationPose->render(poseFrame); }).wait();
                    visualizer.addVisualizationMat("pose", poseFrame);
                }
            }
            if (main.displayCovarianceMagnitude) {
                if (!visualizationCovarianceMagnitude)
                    visualizationCovarianceMagnitude = apiPtr->createVisualization("COVARIANCE_MAGNITUDES");
                visualizer.processMaybeOnGpu([&]() { visualizationCovarianceMagnitude->render(magnitudeFrame); }).wait();
                visualizer.addVisualizationMat("covariance magnitude, absolute, log scale", magnitudeFrame);
            }
            if (main.displayImuSamples && apiOutput) {
                const double t = apiOutput->pose.time;
                visualizer.addVisualizationMat("gyroscope", gyroVisu.draw(t));
                visualizer.addVisualizationMat("accelerometer", accVisu.draw(t));
            }

            // Handle user input.
            visualizer.blockWhilePaused();
            if (visualizer.getCommands().getStepMode() == CommandQueue::StepMode::ODOMETRY) {
                visualizer.getCommands().waitForAnyKey();
            }
            while(!visualizer.getCommands().empty()) {
                auto command = visualizer.getCommands().dequeue();
                if (command.type ==  CommandQueue::Type::QUIT) {
                    shouldQuit = true;
                }
                else if (command.type ==  CommandQueue::Type::POSE) {
                    auto kind = api::PoseHistory::OUR;
                    bool valid = true;
                    switch (command.value) {
                        case 1: kind = api::PoseHistory::OUR; break;
                        case 2: kind = api::PoseHistory::GROUND_TRUTH; break;
                        case 3: kind = api::PoseHistory::EXTERNAL; break;
                        case 4: kind = api::PoseHistory::OUR_PREVIOUS; break;
                        case 5: kind = api::PoseHistory::GPS; break;
                        case 6: kind = api::PoseHistory::RTK_GPS; break;
                        default: valid = false; break;
                    }
                    if (valid && main.displayPose) {
                        bool value = apiPtr->getPoseOverlayHistoryShown(kind);
                        apiPtr->setPoseOverlayHistoryShown(kind, !value);
                    }
                }
                else if (command.type ==  CommandQueue::Type::LOCK_BIASES) {
                    apiPtr->lockBiases();
                }
                else if (command.type ==  CommandQueue::Type::ROTATE) {
                    rotation++;
                    visualizer.setFrameRotation(rotation);
                }
                else if (command.type ==  CommandQueue::Type::CONDITION_ON_LAST_POSE) {
                    apiPtr->conditionOnLastPose();
                }
                else if (command.type ==  CommandQueue::Type::NONE) {
                    // This shouldn't happen, because we check that Queue isn't empty
                    break;
                }
                else if (command.type ==  CommandQueue::Type::ANY_KEY) {
                    // Ignore other keypresses, just remove them from queue
                }
                else {
                    log_warn("Unknown command type %u", command.type);
                }
            }
        }
        if (shouldQuit) break;
    }

    #ifdef DAZZLING_GPU_ENABLED
    for (auto &img : gpuInputImages) img.reset();
    gpuImageFactory.reset();
    #endif

    frameVisualizationImage.reset();
    apiOutput.reset();

    if (!main.skipOpenGlCleanup) {
        visualizer.processMaybeOnGpu([&]() {
            log_debug("api.cleanupOpenGl()");
            api.destroyOpenGl();
        }).wait();
    }

    visualizer.terminate();

    // Blocks until the SLAM worker threads have quit
    log_debug("apiPtr.reset()");
    apiPtr.reset();

    if (odometry::TIME_STATS) {
        log_info("Odometry %s", odometry::TIME_STATS->perFrameTimings().c_str());
    }
    if (cmd.parameters.slam.useSlam && slam::TIME_STATS) {
        log_info("Slam %s", slam::TIME_STATS->perFrameTimings().c_str());
    }
    if (mainTimeStats) {
        log_info("Main %s", mainTimeStats->perFrameTimings().c_str());
    }

    return 0;
}

// 开启可视化线程
int run_visualizer(
    bool anyVisualProcessing,
    Visualizer &visualizer
) {
    // 如果什么都不开，则退出，只跑算法
    if (!anyVisualProcessing) return 0;

    // 开启全局系统绘画（里程计或SLAM地图）
    visualizer.setupViewers();
    // 绘画出单张图像
    while (visualizer.visualizeMats()) {
        // 对系统类再描绘
        visualizer.draw();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    visualizer.cleanup();
    log_debug("run_visualizer end");
    return 0;
}

int main(int argc, char *argv[]) {
    // 读取命令行中的参数配置.
    CommandLineParameters cmd(argc, argv);
    util::setup_logging(cmd.cmd.main.logLevel);
    // 如果有任意一个打开，则进行可视化操作.
    const bool anyVisualizations =
        cmd.cmd.main.displayPose
        || cmd.cmd.main.visualUpdateViewer
        || cmd.cmd.main.displayVideo
        || cmd.cmd.main.displayCorrelation
        || cmd.cmd.main.displayCovarianceMagnitude
        || cmd.cmd.main.displayImuSamples
        || cmd.cmd.main.displayStereoMatching
        || cmd.cmd.slam.displayViewer
        || cmd.cmd.slam.displayKeyframe
        || cmd.cmd.slam.visualizeMapPointSearch
        || cmd.cmd.slam.visualizeOrbMatching
        || cmd.cmd.slam.visualizeLoopOrbMatching;

// 如果是在苹果设备上，则确定将可视化放到主线程中.
#ifdef __APPLE__
    const bool visuInMainThread = anyVisualizations;
    const bool needVisualProcessing = anyVisualizations;
    if (anyVisualizations && cmd.cmd.main.gpu) {
        assert(false && "Visualization aren't supported on Mac with -gpu flag");
    }
#else
    constexpr bool visuInMainThread = false;
    const bool needVisualProcessing = anyVisualizations || cmd.cmd.main.gpu;
#endif
    const bool mapViewer = cmd.parameters.slam.useSlam && cmd.cmd.slam.displayViewer;
    const bool vuViewer = cmd.cmd.main.visualUpdateViewer;
    Visualizer visualizer(cmd.cmd, mapViewer, vuViewer);
    // 如果可视化是在主线程中，则把VIO算法放到子线程中进行异步计算.
    auto ret = visuInMainThread ?
        std::async(run_algorithm, argc, argv, std::ref(visualizer), std::ref(cmd)) :
        std::async(run_visualizer, needVisualProcessing, std::ref(visualizer));
    // 这里在主线程中调用对应的算法.
    if (visuInMainThread) {
        run_visualizer(needVisualProcessing, visualizer);
    } else {
        run_algorithm(argc, argv, visualizer, cmd);
    }
    // 跑完结束
    log_debug("waiting for ret.get() at the end of main");
    return ret.get();
}
