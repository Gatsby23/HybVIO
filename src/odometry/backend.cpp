#include "control.hpp"

#include "debug.hpp"
#include "triangulation.hpp"
#include "util.hpp"
#include "sample_sync.hpp"
#include "ekf.hpp"
#include "parameters.hpp"
#include "../api/slam.hpp"
#include "processed_frame.hpp"
#include "tagged_frame.hpp"
#include "../util/allocator.hpp"
#include "../util/util.hpp"
#include "../util/timer.hpp"
#include "ekf_state_index.hpp"
#include "../tracker/camera.hpp"
#include "../tracker/image.hpp"
#include "../tracker/tracker.hpp"

// Helpers
#include "visual_update_stats.hpp"

#include <Eigen/StdVector>
#include <map>
#include <random>
#include <queue>
#include <accelerated-arrays/opencv_adapter.hpp> // TODO: handle somewhere else
#include <iostream>

namespace odometry {
namespace {

class SlamOdometryCoordinateTransformer {
private:
    Parameters parameters;
    bool ready;
    Eigen::Matrix4d slamToOdometry, odometryToSlam;
    std::unique_ptr<EKF> ekf;

    // workspace
    Eigen::Matrix<double, INER_DIM, 1> inertialMean;
    Eigen::Matrix<double, INER_DIM, INER_DIM> inertialCov;

    ::util::Allocator<std::vector<double>> poseTrailTimestampAllocator;
    ::util::Allocator<Eigen::VectorXd> fullMeanAllocator;

public:
    SlamOdometryCoordinateTransformer(const Parameters &p) :
        parameters(p),
        ready(!parameters.slam.useSlam),
        slamToOdometry(Eigen::Matrix4d::Identity()),
        odometryToSlam(Eigen::Matrix4d::Identity()),
        poseTrailTimestampAllocator([](){ return std::make_unique<std::vector<double>>(); }),
        fullMeanAllocator([](){ return std::make_unique<Eigen::VectorXd>(); })
    {
        parameters.odometry.cameraTrailLength = 1;
        ekf = EKF::build(parameters);
    }

    void setCoordinates(const Eigen::Matrix4d &odo, const Eigen::Matrix4d &slam) {
        ready = true;
        slamToOdometry = odo.inverse() * slam;
        odometryToSlam = slam.inverse() * odo;
    }

    bool isReady() const {
        return ready;
    }

    void transformInertialState(const EKF &orig) {
        orig.getInertialState(inertialMean, inertialCov);
        ekf->setInertialState(inertialMean, inertialCov);

        Eigen::Vector4d ori = ekf->orientation();
        Eigen::Vector3d pos = ekf->position();
        const Eigen::Matrix4d wToCOdo = util::toWorldToCamera(pos, ori, parameters.imuToCamera);
        const Eigen::Matrix4d wToCSlam = worldToCameraPoseOdometryToSlam(wToCOdo);
        util::toOdometryPose(wToCSlam, pos, ori, parameters.imuToCamera); // reuse ori & pos
        ekf->transformTo(pos, ori);
    }

    void setOutput(Output &out, const EKFStateIndex &stateIndex) {
        out.setFromEKF(*ekf, stateIndex, fullMeanAllocator.next(), poseTrailTimestampAllocator.next());
    }

    Eigen::Vector3d pointSlamToOdometry(const Eigen::Vector3d &point) const {
        return util::transformVec3ByMat4(slamToOdometry, point);
    }

    Eigen::Vector3d pointOdometryToSlam(const Eigen::Vector3d &point) const {
        return util::transformVec3ByMat4(odometryToSlam, point);
    }

    Eigen::Matrix4d worldToCameraPoseOdometryToSlam(const Eigen::Matrix4d &wToCOdometry) const {
        return wToCOdometry * slamToOdometry; // P -> P W^{-1}
    }
};

/**
 * Contains the things that need to be reinitialized when resetting the
 * algorithm
 */
struct Session : BackEnd {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    struct SharedData {
        const Parameters &parameters;
        std::mt19937 rng;

        // Visualizations.
        odometry::DebugAPI *odometryDebugAPI = nullptr;

        VisualUpdateStats visualUpdate;
        ::util::Allocator< Output::PointCloud > pointCloudStore;

        SharedData(const Parameters &parameters_) :
            parameters(parameters_),
            rng(parameters_.odometry.rngSeed),
            visualUpdate(parameters_.odometry.printVisualUpdateStats),
            pointCloudStore([]() { return std::make_unique<Output::PointCloud>(); })
        {}
    };

    std::unique_ptr<SharedData> sharedData;

    const Parameters &parameters;
    std::unique_ptr<EKF> ekf;
    std::unique_ptr<tracker::Tracker> tracker;
    std::unique_ptr<slam::Slam> slam;
    Triangulator triangulator;
    std::vector<int> blacklistedPrev;
    EKFStateIndex ekfStateIndex;
    SlamOdometryCoordinateTransformer coordTrans;
    const Eigen::Matrix4d transformSecondToFirstCamera;

    int framesSinceKeyframe = 0;
    unsigned slamFrameCounter = 0;
    bool initializedOrientation = false;

    std::future<slam::Slam::Result> slamResult;
    Output::PointCloud odometryPointCloud;
    slam::Slam::Result::PointCloud slamPointCloud;
    // Maps track id.
    std::map<int, slam::MapPointRecord> odometryVisualizationPointCloud;
    // map from track ID to slamPointCloud index
    std::map<int, int> slamTracks;

    struct SlamFrame {
        std::vector<slam::Feature> slamFeatures;
        std::shared_ptr<tracker::Image> slamFrame;
        double t;

        cv::Mat colorFrame; // for debugging, can be empty
    };

    std::queue<SlamFrame> slamQueue;
    std::mt19937 &rng;
    SharedData &stats;

    util::CircularBuffer<float> visualUpdateCounter;
    api::TrackingStatus trackingStatus;

    /**
     * Temporary variables that could be defined in method bodies but are
     * included here to avoid reallocations
     */
     /************************************************************************
      * 这里是把visual update当中的部分先给保存下来，做成这个临时变量，后面用来三角化.
      ***********************************************************************/
    struct Workspace {
        std::vector<unsigned> trackOrder;
        tracker::Tracker::Output trackerOutput;
        CameraPoseTrail trail;
        std::vector<int> blacklisted;
        std::vector<int> poseTrailIndex;
        std::map<int, int> mapPointIndex;
        Eigen::VectorXd y, yblock;
        Eigen::MatrixXd H, Hblock;
        Eigen::VectorXd f, fblock;
        vecVector2d imageFeatures;
        vecVector2d featureVelocities;
        // 这里是保存下来的pose，用来做pose graph.
        std::vector<slam::Pose> poseTrailForSlam;
        TriangulationArgsOut triangulationArgsOut;
        vecVector3f stereoPointCloud, stereoPointCloudColor;
    } tmp;

    Session(std::unique_ptr<SharedData> data) :
        sharedData(std::move(data)),
        parameters(sharedData->parameters),
        ekf(EKF::build(parameters)),
        tracker(tracker::Tracker::build(parameters)),
        triangulator(parameters.odometry),
        ekfStateIndex(parameters),
        coordTrans(parameters),
        transformSecondToFirstCamera(parameters.imuToCamera * parameters.secondImuToCamera.inverse()),
        rng(sharedData->rng),
        stats(*sharedData),
        visualUpdateCounter( // visual updates per second * time window
            (size_t)((double)parameters.tracker.targetFps / (double)parameters.odometry.visualUpdateForEveryNFrame
            * parameters.odometry.goodFramesTimeWindowSeconds)),
        trackingStatus(api::TrackingStatus::INIT),
        tmp()
    {
        if (parameters.slam.useSlam) {
            slam = slam::Slam::build(parameters);
            // Slam uses camera trail, slam calculations can't be delayed further than camera trail allows
            assert((unsigned)parameters.odometry.cameraTrailLength > parameters.slam.keyframeCandidateInterval * (parameters.slam.delayIntervalMultiplier + 1));

            if (stats.odometryDebugAPI && stats.odometryDebugAPI->slamDebug) {
                slam->connectDebugAPI(*stats.odometryDebugAPI->slamDebug);
            }
        }
    }

    ~Session() {
        if (sharedData && sharedData->odometryDebugAPI && sharedData->odometryDebugAPI->endDebugCallback) {
            sharedData->odometryDebugAPI->endDebugCallback(odometryVisualizationPointCloud);
        }
        if (slam) {
            auto f = slam->end();
            // Compute synchronously so that we get a finished result even when calling
            // just before terminating main().
            f.get();
        }
    }

    void initializeAtPose(const Eigen::Vector3d &pos, const Eigen::Vector4d &q) final {
        Eigen::Vector3d a; a << 0, 0, 0;
        ekf->initializeOrientation(a);
        initializedOrientation = true;
        ekf->transformTo(pos, q);
    }

    void lockBiases() final {
        ekf->lockBiases();
    }

    void conditionOnLastPose() final {
        ekf->conditionOnLastPose();
    }

    void connectDebugAPI(odometry::DebugAPI &odometryDebug) final {
        stats.odometryDebugAPI = &odometryDebug;
        if (slam && odometryDebug.slamDebug) slam->connectDebugAPI(*odometryDebug.slamDebug);
    }

    const EKF &getEKF() const final {
        return *ekf;
    }

    std::string stateAsString() const final {
        std::stringstream ss;
        ss << ekf->stateAsString();
        ss << ", trail len (s) " << (ekfStateIndex.getTimestamp(0) - ekfStateIndex.getTimestamp(ekfStateIndex.poseTrailSize()-1));
        return ss.str();
    }

    void getPointCloud(Output::PointCloud &r) const {
        r.clear();
        if (!coordTrans.isReady()) return;

        for (const auto &mp : slamPointCloud) {
            // hacky "missing value" for untracked SLAM map points
            Eigen::Vector2f pixelCoords(-1, -1);
            // note: no need to check mp.trackId >= 0 here
            ekfStateIndex.getCurrentTrackPixelCoordinates(mp.trackId, pixelCoords);

            r.push_back(PointFeature {
                // hacky: prevent ID conflicts by using negative numbers
                // for SLAM map IDs and positive for PIVO track IDs
                .id = mp.trackId >= 0 ? mp.trackId : -mp.id,
                .status = PointFeature::Status::SLAM,
                .firstPixel = pixelCoords,
                .point = mp.position
          });
        }
        for (const auto &p : odometryPointCloud) {
            if (!slamTracks.count(p.id)) r.push_back(p);
        }
        for (auto &p : r) {
            p.point = coordTrans.pointOdometryToSlam(p.point);
        }
    }

    Eigen::Matrix<double, 3, 6> odometryUncertainty(int currenFrame, int previousFrame) {
        const Eigen::MatrixXd &P = ekf->getStateCovarianceRef();
        const Eigen::VectorXd &m = ekf->getState();

        int previousOffset = odometry::CAM + previousFrame * odometry::POSE_DIM;
        int currentOffset = odometry::CAM + currenFrame * odometry::POSE_DIM;

        // For we want to know the uncertainty of the difference in poses between
        // current frame and previous frame. This difference y for postiion, can
        // be precented as: y = H * m = [I1 - I2] * m
        // The uncertainty for y can be calculated Py = H * P * H.transpose()

        // Take covariances we are interested in from P and create 6x6 matrix
        //     x  y  z  u  v  w
        // x  xx xy xz xu xv xw
        // y  yx yy yz .. .. ..
        // z  .. .. .. .. .. ..
        // u  .. .. .. .. .. ..
        // v  .. .. .. .. .. ..
        // w  .. .. .. .. .. ..
        Eigen::Matrix<double, 6, 6> kfP;
        kfP.block<3, 3>(0, 0) = P.block(currentOffset, currentOffset, 3, 3);
        kfP.block<3, 3>(3, 0) = P.block(previousOffset, currentOffset, 3, 3);
        kfP.block<3, 3>(0, 3) = P.block(currentOffset, previousOffset, 3, 3);
        kfP.block<3, 3>(3, 3) = P.block(previousOffset, previousOffset, 3, 3);

        // Construct diff matrix, [I1 - I2] = H
        //  1  0  0 -1  0  0
        //  0  1  0  0 -1  0
        //  0  0  1  0  0 -1
        Eigen::Matrix<double, 3, 6> H;
        H.block<3, 3>(0, 0) = Eigen::MatrixXd::Identity(3, 3);
        H.block<3, 3>(0, 3) = -Eigen::MatrixXd::Identity(3, 3);

        // Calculate the 3x3 ucertainty matrix
        const Eigen::Matrix3d posP = H * kfP * H.transpose();

        // Uncertainty for rotation is more complex, because difference between
        // two quaternions h(x) = q1 * q2^-1 = q1 * q2.quat_inverse() = y
        // Uncertainty is then Py = Jh * Px * Jh.transpose(), where Jh is
        // Jacobian matrix for h(x).
        // Jh = [B1, B2], where
        // B1 = dy / dq1 = M_R(q2^-1), is Jacobian matrix for q -> q * q2^-1
        // B2 = dy / dq2 = M_L(q1) * C(q2), is Jacobian matrix for q -> q1 * q^-1
        //         [ 1,  0,  0,  0 ]
        // QINV =  [ 0, -1,  0,  0 ]
        //         [ 0,  0, -1,  0 ]
        //         [ 0,  0,  0, -1 ]
        // C(q)
        //      = d / dq * q^-1
        //      = d / qd * (q* / |q|^2)
        //      = 1 / |q|^2 * ( QINV - 2 * q^-1 * q.transpose() )
        //        ^^^^^^^^^  = 1, because unit quaternions magnitude is 1
        //      = QINV - 2 * q^-1 * q.transpose()
        // M_R and M_L are matrix representations of quaternion products:
        // q * p = M_L(q) * p
        // q * p = M_R(p) * q

        // Move to rotation part
        currentOffset += 3;
        previousOffset += 3;

        // Take rotation covariances
        Eigen::Matrix<double, 8, 8> Px;
        Px.block<4, 4>(0, 0) = P.block(currentOffset, currentOffset, 4, 4);
        Px.block<4, 4>(4, 0) = P.block(previousOffset, currentOffset, 4, 4);
        Px.block<4, 4>(0, 4) = P.block(currentOffset, previousOffset, 4, 4);
        Px.block<4, 4>(4, 4) = P.block(previousOffset, previousOffset, 4, 4);

        // q^-1 = q* / |q|^2, where |q|^2 is 1 for unit quaternion. q* or conjugate
        // of q  is conj(a + b*i + c*j + d*k) = a - b*i - c*j - d*k). Thus
        // quatInverseMatrix * q = q* = q^-1 for unit quaternion.
        Eigen::Matrix4d quatInverseMatrix;
        quatInverseMatrix <<
            1,  0,  0,  0,
            0, -1,  0,  0,
            0,  0, -1,  0,
            0,  0,  0, -1;

        const Eigen::Vector4d q1 = m.segment<4>(currentOffset);
        Eigen::Matrix4d M_L;
        M_L <<
            q1[0], -q1[1], -q1[2], -q1[3],
            q1[1],  q1[0], -q1[3],  q1[2],
            q1[2],  q1[3],  q1[0], -q1[1],
            q1[3], -q1[2],  q1[1],  q1[0];

        const Eigen::Vector4d q2inv = quatInverseMatrix * m.segment(previousOffset, 4);
        Eigen::Matrix4d M_R;
        M_R <<
            q2inv[0], -q2inv[1], -q2inv[2], -q2inv[3],
            q2inv[1],  q2inv[0],  q2inv[3], -q2inv[2],
            q2inv[2], -q2inv[3],  q2inv[0],  q2inv[1],
            q2inv[3],  q2inv[2], -q2inv[1],  q2inv[0];

        // C = 1 / |q2|^2 * (quatInverseMatrix - 2 * q2inv * q2.transpose())
        //     ^^^^^^^^^^ = 1                                ^^ q2 = quatInverseMatrix * q2inv
        Eigen::Matrix4d C = quatInverseMatrix - 2. * q2inv * (quatInverseMatrix * q2inv).transpose();

        Eigen::Matrix<double, 4, 8> J;
        J.block(0, 0, 4, 4) = M_R; // B1
        J.block(0, 4, 4, 4) = M_L * C; // B2

        const Eigen::Matrix4d Py = J * Px * J.transpose();

        // TODO: We only compute simple average uncertainty to avoid conversion from
        // quaternion -> g2o orientation. Doing the conversion might yield better results.
        double rotationUncertainty = Py.norm(); // Frobenius norm

        // Create 3 x 6 uncertainty matrix, first 3x3 is for rotation, second 3x3 for postion.
        Eigen::Matrix<double, 3, 6> uncertainty;
        uncertainty.block(0, 0, 3, 3) = Eigen::MatrixXd::Identity(3, 3) * rotationUncertainty;
        uncertainty.block(0, 3, 3, 3) = posP;

        return uncertainty;
    }

    /*******************************************************************
     * @brief
     * @param frame
     * @param trackerOutput
     * @param keyframe
     * @param frameNumber
     * @return 返回说明SLAM的操作是否成功.
     *******************************************************************/
    bool applySlam(const ProcessedFrame &frame, const tracker::Tracker::Output &trackerOutput, bool keyframe, int frameNumber) {
        bool wasSlamFrame = false;

        // happens if either USE_SLAM or slam.useSlam is disabled
        if (!slam) return wasSlamFrame;

        // 这里应该说的是至少隔4帧才会作为SLAM的keyframe来进行处理
        // Example: keyframeCandidateInterval = 4, delayIntervalMultiplier = 0
        // Frame           00 01 02 03 04 05 06 07 08 ..
        // Slam            00          04          08
        // Result                      00          04
        // 不太清楚这里的q是什么意思？
        // Example: keyframeCandidateInterval = 4, delayIntervalMultiplier = 2
        // Frame           00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 ..
        // Slam            q           q           00          04          08
        // Result                                              00          04
        // 在论文呢原文中:
        //The result is read asynchronously on the next key frame candidate, which we add every N = 8 frames.
        const unsigned interval = parameters.slam.keyframeCandidateInterval;
        const int delayMultiplier = parameters.slam.delayIntervalMultiplier;
        const unsigned resultFrameDelay = interval * (delayMultiplier + 1);
        // 这里的sync是代表什么呢？

        const bool syncSlam = delayMultiplier < 0;

        if (!keyframe && (!syncSlam || interval > 1)) return wasSlamFrame;

        // note that in odometry & syncSlam, !keyframe actually means that
        // the previous tracker frame was discarded. so we don't actually
        // know at this point if the head of the pose trail will be dropped
        // later
        // if (!keyframe) log_debug("applySlam on non-keyframe %d", frameNumber);

        const unsigned currentFrame = slamFrameCounter++;

        // 每隔八张图像才会作为一次keyframe来进行slam优化.
        if (currentFrame % parameters.slam.keyframeCandidateInterval != 0) {
            return wasSlamFrame;
        }

        if (!syncSlam && currentFrame >= resultFrameDelay) {
            applySlamResult(resultFrameDelay);
            wasSlamFrame = true;
        }

        const bool hasCpuColorFrame = frame.taggedFrame
            && frame.taggedFrame->colorFrame->storageType == accelerated::ImageTypeSpec::StorageType::CPU;

        cv::Mat slamColorFrame;
        if (hasCpuColorFrame) {
            // 这里是将accelerated::opencv frame 转换成 cv::Mat.
            slamColorFrame = accelerated::opencv::ref(*frame.taggedFrame->colorFrame);
            if (parameters.tracker.useStereo) slamColorFrame = slamColorFrame(frame.taggedFrame->firstImageRect);
        }
        slamQueue.push(SlamFrame {
            .slamFeatures = trackerOutput.tracks,
            .slamFrame = frame.firstGrayFrame,
            .t = frame.t,
            .colorFrame = slamColorFrame
        });

        // uncomment this if you really need to use SLAM color frames with GPU
        // if (!hasCpuColorFrame) accelerated::opencv::copy(*frame.taggedFrame->colorFrame, slamQueue.back().colorFrame).wait();

        unsigned slamFrameDelay = interval * delayMultiplier;
        if (!syncSlam && currentFrame < slamFrameDelay) {
            return wasSlamFrame;
        }

        auto nextFrame = std::move(slamQueue.front());
        slamQueue.pop();

        auto &odoPoseTrail = tmp.poseTrailForSlam;
        odoPoseTrail.clear();

        // 这里是构建一系列的pose，先利用VIO的结果来进行pose之间的uncertainty的构建，构建完之后，将pose放到一个delay的滑窗中，然后再来做Pose graph的优化.
        for (unsigned index = interval * unsigned(std::max(0, delayMultiplier)); index < ekfStateIndex.poseTrailSize() - 1; index++) {
            Eigen::Matrix<double, 3, 6> uncertainty;
            unsigned prevIndex = index + 1;
            if (prevIndex < ekfStateIndex.poseTrailSize() - 1) {
                // 计算出来每一帧odometry和其他帧odometry之间的置信度.
                uncertainty = odometryUncertainty(index, prevIndex);
            } else {
                // Previous pose is outside cam trail. This is OK, see handling in SLAM
                uncertainty.setZero();
            }

            // 如果index是0的话，就先放进去，因为visual update还不一定弄完.
            if (index == 0) {
                // Current frame is a special case, because visual update might not have been done
                odoPoseTrail.push_back(
                    slam::Pose {
                        .pose = odometryToWorldToCamera(-1),
                        .uncertainty = uncertainty, // TODO: This is probably wrong because odo pose -1 != 0 when no visual update
                        .t = ekf->historyTime(-1),
                        .frameNumber = static_cast<int>(frameNumber)
                    }
                );
            } else {
                odoPoseTrail.push_back(
                    slam::Pose {
                        .pose = odometryToWorldToCamera(index),
                        .uncertainty = uncertainty,
                        .t = ekf->historyTime(index),
                        // the indexing is unfortunately different in PoseTrail and Odometry:
                        // in Odometry, the current pose is handled separately and has index -1
                        // in some contexts. In PoseTrail, the current pose has index 0
                        .frameNumber = ekfStateIndex.getFrameNumber(index + 1)
                    }
                );
            }
            // Skip rest of the poses unless full path is used
            if (!parameters.slam.useOdometryPoseTrailDelta) break;
        }

        // 如果odoPoseTrail是空的话....
        if (odoPoseTrail.empty()) {
            assert(!keyframe);
            return wasSlamFrame;
        }

        // 添加KeyFrame....->这里addFrame感觉是添加任务.
        slamResult = slam->addFrame(
            nextFrame.slamFrame,
            odoPoseTrail,
            nextFrame.slamFeatures,
            nextFrame.colorFrame);

        // synchronous SLAM
        if (syncSlam) {
            applySlamResult(resultFrameDelay);
            wasSlamFrame = true;
        }
        return wasSlamFrame;
    }

    // 得到之前的SLAM结果？？
    void applySlamResult(int resultFrameDelay) {
        // Get the previous SLAM result (computed asynchronously).
        // 这里是之前的结果直接计算得到的吗？
        assert(slamResult.valid());
        slam::Slam::Result result = slamResult.get();
        // 先将结果转换坐标系->变成world->camera的顺序（那之前的IMU顺序应该是body->world？）.

        coordTrans.setCoordinates(
            odometryToWorldToCamera(resultFrameDelay),
            result.poseMat);

        // SLAM的跟踪清楚？SLAM的跟踪和VIO里的有什么区别？
        slamTracks.clear();
        // SLAM点云也清楚，为啥呢？
        slamPointCloud.clear();

        int id = 0;
        // 这里是在做什么？
        for (const slam::Slam::Result::MapPoint &mp : result.pointCloud) {
            if (mp.trackId >= 0) slamTracks[mp.trackId] = id;
            auto mp1 = mp;
            mp1.position = coordTrans.pointSlamToOdometry(mp.position);
            slamPointCloud.push_back(mp1);
            id++;
        }
    }

    /*********************************************************************
     * @brief 这里是用光流来追踪特征点，并将追踪到的特征点从param2中来返回给主程序
     * @param frame 当前图像帧.
     * @param trackerOutput 追踪到的图像特征点.
     ********************************************************************/
    void applyTracker(
        ProcessedFrame &frame,
        tracker::Tracker::Output &trackerOutput
    ) {
        // Predict optical flow by using odometry poses.
        // Use a lambda so that we can use `ekf` conveniently without passing it into tracker.
        tracker::OpticalFlowPredictor opticalFlowPredictor;
        opticalFlowPredictor.func = [&](
            const std::vector<tracker::Feature::Point> &c0,
            const std::vector<tracker::Feature> &tracks,
            std::vector<tracker::Feature::Point> &c1,
            tracker::OpticalFlowPredictor::Type predictionType
        ) {
            c1.clear();
            assert(ekfStateIndex.poseTrailSize() >= 1);
            assert(c0.size() == tracks.size());
            ekfStateIndex.createFullIndex(tmp.poseTrailIndex);

            extractCameraPoseTrail(*ekf, tmp.poseTrailIndex,
                parameters, parameters.tracker.useStereo, tmp.trail);

            // Preallocate.
            Eigen::Vector2d imagePoint0;
            Eigen::Vector2d imagePoint1;
            Eigen::Vector3d pf;
            Eigen::Matrix<double, 3, 2 * POSE_DIM + 1> dpfTwoCameras;

            tmp.imageFeatures.clear();
            tmp.imageFeatures.resize(tmp.poseTrailIndex.size());

            std::shared_ptr<const tracker::Camera> camera0, camera1;
            Eigen::Matrix4d camToWorld0, worldToCam1;

            using K = tracker::OpticalFlowPredictor::Type;
            if (predictionType == K::LEFT) {
                camera0 = frame.firstGrayFrame->getCamera();
                camera1 = frame.firstGrayFrame->getCamera();
                camToWorld0 = util::toCameraToWorld(
                    ekf->historyPosition(0),
                    ekf->historyOrientation(0),
                    parameters.imuToCamera);
                worldToCam1 = util::toWorldToCamera(
                    ekf->position(),
                    ekf->orientation(),
                    parameters.imuToCamera);
            }
            else if (predictionType == K::RIGHT) {
                camera0 = frame.secondGrayFrame->getCamera();
                camera1 = frame.secondGrayFrame->getCamera();
                camToWorld0 = util::toCameraToWorld(
                    ekf->historyPosition(0),
                    ekf->historyOrientation(0),
                    parameters.secondImuToCamera);
                worldToCam1 = util::toWorldToCamera(
                    ekf->position(),
                    ekf->orientation(),
                    parameters.secondImuToCamera);
            }
            else {
                assert(predictionType == K::STEREO);
                assert(parameters.tracker.useStereo && frame.secondGrayFrame);
                camera0 = frame.firstGrayFrame->getCamera();
                camera1 = frame.secondGrayFrame->getCamera();
                camToWorld0 = util::toCameraToWorld(
                    ekf->position(),
                    ekf->orientation(),
                    parameters.imuToCamera);
                worldToCam1 = util::toWorldToCamera(
                    ekf->position(),
                    ekf->orientation(),
                    parameters.secondImuToCamera);
            }
            for (size_t i = 0; i < c0.size(); ++i) {
                // Estimate feature 3d distance roughly.
                double distance = -1.0;
                size_t keyframe0;
                size_t keyframe1;

                if (ekfStateIndex.widestBaseline(
                    tracks.at(i).id, keyframe0, keyframe1, imagePoint0, imagePoint1))
                {
                    assert(keyframe0 < tmp.imageFeatures.size());
                    assert(keyframe1 < tmp.imageFeatures.size());
                    // TODO Try disabling this check, inaccurate triangulations hardly matter if `predictOpticalFlowMinTriangulationDistance` is not too small. The widestBaseline()
                    // check above still implies a minimum length of 2 or 3 poses.
                    // Triangulation is likely to be accurate only if there is considerable baseline.
                    constexpr double MIN_TWO_CAMERA_FLOW_TRIANGULATION_BASELINE = 10; // number of poses
                    if (keyframe1 - keyframe0 >= MIN_TWO_CAMERA_FLOW_TRIANGULATION_BASELINE) {
                        const TwoCameraTriangulationArgsIn args {
                            .pose0 = tmp.trail.at(keyframe0),
                            .pose1 = tmp.trail.at(keyframe1),
                            .ip0 = imagePoint0,
                            .ip1 = imagePoint1,
                        };
                        pf = triangulateWithTwoCameras(args);
                        // Distance from the keyframe0 camera, along ray corresponding to image feature
                        // in that keyframe. Check the point is in front of camera.
                        if (pf(2) > 0.0) {
                            distance = pf.norm();
                        }
                    }
                }
                // Set minimum distance to prevent big errors.
                double m = parameters.tracker.predictOpticalFlowMinTriangulationDistance;
                if (distance < m) distance = m;

                // Unproject using previous pose to world and project back using current pose.
                auto ip = c0.at(i);
                Eigen::Vector2d pixel0(ip.x, ip.y);
                Eigen::Vector3d ray0;
                bool success = camera0->pixelToRay(pixel0, ray0);
                ray0 *= distance;

                const Eigen::Vector3d p = util::transformVec3ByMat4(camToWorld0, ray0);
                const Eigen::Vector3d ray1 = util::transformVec3ByMat4(worldToCam1, p);
                Eigen::Vector2d pixel1;
                if (success && camera1->rayToPixel(ray1, pixel1)) {
                    c1.push_back({ float(pixel1(0)), float(pixel1(1)) });
                }
                else {
                    c1.push_back({ float(pixel0(0)), float(pixel0(1)) });
                }
            }
        };

        const bool isStereo = !!frame.secondGrayFrame;
        // 这里的Poses用于后面的跟踪.
        std::array<Eigen::Matrix4d, 2> poses;
        if (parameters.tracker.useStereoUpright2p) {
            poses = {
                util::toCameraToWorld(
                    ekf->historyPosition(0),
                    ekf->historyOrientation(0),
                    parameters.imuToCamera),
                util::toCameraToWorld(
                    ekf->position(),
                    ekf->orientation(),
                    parameters.imuToCamera)
            };
        }
        tracker::TrackerArgsIn args = {
            .firstImage = frame.firstGrayFrame,
            .secondImage = isStereo ? frame.secondGrayFrame : nullptr,
            .t = frame.t,
            .opticalFlowPredictor = opticalFlowPredictor,
            .poses = parameters.tracker.useStereoUpright2p ? &poses : nullptr,
        };
        // 主要这里用的是特征点抽取的情况吧？
        tracker->add(args, trackerOutput);

        if (frame.taggedFrame) {
            // populate corner data for visualizations
            for (const auto &track : trackerOutput.tracks) {
                const auto &last = track.points[0];
                frame.taggedFrame->corners.push_back({ last.x, last.y });
                if (isStereo) {
                    const auto &lastSecond = track.points[1];
                    frame.taggedFrame->secondCorners.push_back({ lastSecond.x, lastSecond.y });
                }
            }
            if (parameters.tracker.saveOpticalFlow != odometry::OpticalFlowVisualization::NONE) {
                frame.taggedFrame->flowCorners0 = std::move(trackerOutput.flowCorners0);
                frame.taggedFrame->flowCorners1 = std::move(trackerOutput.flowCorners1);
                frame.taggedFrame->flowCorners2 = std::move(trackerOutput.flowCorners2);
                frame.taggedFrame->flowStatus = std::move(trackerOutput.flowStatus);
            }
            if (parameters.tracker.saveStereoEpipolar != odometry::StereoEpipolarVisualization::NONE) {
                frame.taggedFrame->epipolarCorners0 = std::move(trackerOutput.epipolarCorners0);
                frame.taggedFrame->epipolarCorners1 = std::move(trackerOutput.epipolarCorners1);
                frame.taggedFrame->epipolarCurves = std::move(trackerOutput.epipolarCurves);
            }
        }
    }

    /***********************************************
     * @brief
     * @param sample
     * @param output 这里是为了可视化输出.
     * @return output is written if output != NONE
     ***********************************************/
    ProcessResult process(SyncedSample& sample, Output &output) final {
        const ParametersOdometry& po = parameters.odometry;

        double dtLf = std::abs(sample.t - sample.tF);
        if (dtLf > 0.025) {
            log_warn("Large leader-follower timestamp difference: %.3f\n", dtLf);
        }

        Eigen::Vector3d g(sample.l.x, sample.l.y, sample.l.z);
        Eigen::Vector3d a(sample.f.x, sample.f.y, sample.f.z);
        // 这里只是赋值，并没有其余操作，可以删了

        // IMU初始化->这里只用了第一帧加速度计数据来进行初始化，不是特别合理
        // 这里不太好的地方是单帧数据，并不是序列数据->初始化的时候需要处理
        if (!initializedOrientation) {
            ekf->initializeOrientation(a);
            initializedOrientation = true;
        }

        // KF predict.
        // EKF进行预测
        ekf->predict(sample.t, g, a);
        // 预测完后需要将旋转进行归一化
        ekf->normalizeQuaternions(true);

        // KF control updates.
        if (po.useDecayingZeroVelocityUpdate) {
            // Aids initialization in some cases by assuming the device is stationary
            // in the beginning of the session.
            ekf->updateZuptInitialization();
        }

        const double horizontalSpeed = ekf->horizontalSpeed();
        if (po.usePseudoVelocity && horizontalSpeed > po.pseudoVelocityLimit) {
            // this (soft) pseudo-velocity update controls the velocity under
            // normal (walking) circumstances
            ekf->updatePseudoVelocity(po.pseudoVelocityTarget, po.pseudoVelocityR);
        }

        if (sharedData->odometryDebugAPI && sharedData->odometryDebugAPI->publisher) {
            Eigen::Matrix3d R = util::quat2rmat(ekf->orientation()).transpose();
            sharedData->odometryDebugAPI->publisher->addSample(sample.t,
                (R * (g - ekf->biasGyroscopeAdditive())).cast<float>(),
                (R * ekf->biasAccelerometerTransform().asDiagonal() * a
                    - ekf->biasAccelerometerAdditive()).cast<float>());
        }

        // KF visual update.
        bool keyframe = false;
        bool stationaryVisual = false;
        bool slamFrame = false;

        if (sample.frame != nullptr) {
            std::cout << "=============FRAME " << sample.frame->num << "  UPDATE============" << std::endl;
            const bool fullVisualUpdate = sample.frame->num % po.visualUpdateForEveryNFrame == 0 || !ekfStateIndex.canPopKeyframe();
            // trackerOutput是用来作为可视化使用
            auto &trackerOutput = tmp.trackerOutput;
            // 这里是特征点光流追踪，用output来存储追踪的特征点.
            applyTracker(*sample.frame, trackerOutput);
            // 这里的tracker output主要输出来是用于可视化的.
            // note: should actually do the stationarity checks from the last
            // "fullVisualUpdate". This logic is technically a bit flawed but
            // probably still a good approximation

            // concerns this particular update, unlike output.stationaryVisual
            keyframe = trackerOutput.keyframe;
            if (keyframe) {
                framesSinceKeyframe = 0;
            } else {
                framesSinceKeyframe++;
            }
            // 这里做了一个简化->keyframe之后的连续三帧都不是keyframe的话，则认为场景变化不大，则证明是静止（感觉还是不太对）
            stationaryVisual = framesSinceKeyframe >= po.visualStationarityFrameCountThreshold;
            if (po.useVisualStationarity && stationaryVisual) {
                ekf->updateZupt(po.visualZuptR);
            }

            // 实际上到目前为止我还没理解sharedData是用来做什么.
            // SharedData，EKFStateIndex这些到底是用来做什么
            if (sharedData->odometryDebugAPI && sharedData->odometryDebugAPI->publisher) {
                sharedData->odometryDebugAPI->publisher->startFrame(*ekf, ekfStateIndex, parameters);
            }

            // causes every Nth (key)frame to be left in the trail
            if (!fullVisualUpdate) keyframe = false;

            // 这里还是跟延迟策略不一样，先是把首帧剔除
            // 然后用现在frame帧来做更新.
            std::cout << "Before update, the state index has: " ;
            for(auto frame : ekfStateIndex.keyframes)
                std::cout << frame.frameNumber << ", ";
            std::cout << std::endl;
            if (po.visualUpdateEnabled) {

                /**********************************************************************************
                 * 这里要结合下面的head操作一块理解，如果当前帧是普通帧，则把上一帧给去掉，留下的是上一个关键帧.
                 * 然后队列的队头帧则用当前帧进行替代.
                 * 然后再把关键帧插入到队头.这样的话，在当前帧进来前总能看到上一帧，并且能找打和它最近的关键帧.aa
                 ***********************************************************************************/
                if (!keyframe) {
                    ekfStateIndex.popHeadKeyframe();
                    ekf->updateUndoAugmentation();
                }

                std::cout << "The current frame is: " << (keyframe ? "key" : "") << "frame,  After pop, the state vector has: ";
                for(auto frame : ekfStateIndex.keyframes)
                    std::cout << frame.frameNumber << ", ";
                std::cout << std::endl;

                auto &head = ekfStateIndex.headKeyFrame();
                head.frameNumber = sample.frame->num;
                head.timestamp = sample.t;

                // 用来做视觉更新.
                const bool goodFrame = trackerVisualUpdate(sample, trackerOutput, output, fullVisualUpdate, stationaryVisual);

                int droppedPose = ekfStateIndex.pushHeadKeyframe(sample.frame->num, sample.t);
                ekf->updateVisualPoseAugmentation(droppedPose - 1); // different indexing
                std::cout << "After update the " << (keyframe ? "key" : "") << "frame" << ", the index is: ";
                for(auto frame : ekfStateIndex.keyframes)
                    std::cout << frame.frameNumber << ", ";
                std::cout << " and drop the " << droppedPose << " poses." << std::endl;

                // 这里是fully Visual Update是什么
                if (fullVisualUpdate) {
                    visualUpdateCounter.put(goodFrame ? 1.0f : 0.0f);
                    if  (visualUpdateCounter.entries() > visualUpdateCounter.maxSize() / 2) {
                        float meanVisualUpdates = visualUpdateCounter.mean();
                        if (trackingStatus != api::TrackingStatus::TRACKING
                            && meanVisualUpdates > parameters.odometry.goodFramesToTracking) {
                            trackingStatus = api::TrackingStatus::TRACKING;
                        } else if (trackingStatus == api::TrackingStatus::TRACKING
                            && meanVisualUpdates < parameters.odometry.goodFramesToTrackingFailed) {
                            trackingStatus = api::TrackingStatus::LOST_TRACKING;
                        }
                    }
                }
            }

            // 只有开启SLAM模式后，这里系统才会进行进一步操作.
            slamFrame = applySlam(*sample.frame, trackerOutput, keyframe, sample.frame->num);

            // Prepare visualization output->这里是用来做可视化界面的输出.
            if (sample.frame->taggedFrame != nullptr) {
                // modify, but don't take ownership of the TaggedFrame
                auto &outputFrame = *sample.frame->taggedFrame;

                projectMapPoints(outputFrame.slamPointReprojections, *sample.frame->firstGrayFrame->getCamera());

                for (const auto &track : trackerOutput.tracks) {
                    const auto st = slamTracks.find(track.id);
                    const int idx = st != slamTracks.end() ? st->second : -1;
                    outputFrame.cornerSlamPointIndex.push_back(idx);
                }

                output.taggedFrame = std::move(sample.frame->taggedFrame);
            }

            output.t = sample.t;
            coordTrans.transformInertialState(*ekf);
            coordTrans.setOutput(output, ekfStateIndex);

            {
                // ugly
                const size_t poseCount = ekfStateIndex.poseTrailSize() - 1;
                if (output.poseTrailLength() < poseCount) {
                    for (size_t i = 0; i < poseCount; ++i) {
                        Eigen::Vector3d pos;
                        Eigen::Vector4d ori;
                        computePose(i, pos, ori);
                        output.addPoseTrailElementMeanOnly(i, ekfStateIndex.getTimestamp(i + 1), pos, ori);
                    }
                }
            }
            output.trackingStatus = trackingStatus;
            output.stationaryVisual = stationaryVisual;

            output.pointCloud = sharedData->pointCloudStore.next();
            getPointCloud(*output.pointCloud);

            if (slamFrame) return ProcessResult::SLAM_FRAME;
            return ProcessResult::FRAME;
        }

        return ProcessResult::NONE;
    }

    void projectMapPoints(
        std::vector<tracker::Feature::Point> &reprojections,
        const tracker::Camera &camera
    ) {
        const Eigen::Matrix4d worldToCam = util::toWorldToCamera(
                ekf->position(),
                ekf->orientation(),
                parameters.imuToCamera);

        for (const auto &mp : slamPointCloud) {
            Eigen::Vector2d ip;
            if (camera.rayToPixel(util::transformVec3ByMat4(worldToCam, mp.position), ip)) {
                reprojections.push_back({ float(ip.x()), float(ip.y()) });
            }
            else {
                // not expected to happen often, but only used for visualization
                reprojections.push_back({ -1, -1 });
            }
        }
    }

    // Loop tracks and do visual updates using them.
    /**
     *
     * @param sample -> 融合的多源数据包.
     * @param trackerOutput -> tracker的结果输出.
     * @param output -> 结果的输出.
     * @param fullVisualUpdate
     * @param stationaryVisual
     * @return
     */
    bool trackerVisualUpdate(
        SyncedSample &sample,
        const tracker::Tracker::Output &trackerOutput,
        Output &output,
        bool fullVisualUpdate,
        bool stationaryVisual)
    {
        timer(odometry::TIME_STATS, __FUNCTION__);
        const ParametersOdometry& po = parameters.odometry;
        output.focalLength = sample.frame->firstGrayFrame->getCamera()->getFocalLength();

        int updateAttemptCount = 0;
        int updateSuccessCount = 0;

        odometryPointCloud.clear();
        tmp.blacklisted.clear();
        tmp.trackOrder.clear();

        // 这里是针对双目的情况，将跟踪的点三角化出来.
        for (unsigned i = 0; i < trackerOutput.tracks.size(); ++i) {
            const auto &track = trackerOutput.tracks.at(i);
            // Construct normalized feature points.
            const size_t frameCount = parameters.tracker.useStereo ? 2 : 1;
            bool success = false;
            Feature feature;
            for (size_t frameInd = 0; frameInd < frameCount; ++frameInd) {
                const auto &uv = track.points[frameInd];
                feature.frames[frameInd].imagePoint = Eigen::Vector2d(uv.x, uv.y);

                const std::shared_ptr<tracker::Image> grayFrame = frameInd == 0
                    ? sample.frame->firstGrayFrame
                    : sample.frame->secondGrayFrame;
                // 这里主要是判断反向投影后的特征点是不是还在可视化区域中，如果不在可视化区域中，则直接抛弃掉.
                success = grayFrame->getCamera()->normalizePixel(feature.frames[frameInd].imagePoint, feature.frames[frameInd].normalizedImagePoint);
                if (!success) break;
            }

            // 这里是三角化当中的特征点.
            // 默认这里是不会执行.
            if (success && po.useIndependentStereoTriangulation) {
                Eigen::Vector3d idp;
                // 这里是三角化->并且能得到三角化后的Cov.
                success = triangulateStereoFeatureIdp(
                    feature.frames[0].normalizedImagePoint,
                    feature.frames[1].normalizedImagePoint,
                    transformSecondToFirstCamera,
                    idp,
                    &feature.triangulatedStereoCov);
                if (success && track.depth > 0) {
                    // use dense depth, if available (but keep sensitivity / covariance from triangulation)
                    idp = Eigen::Vector3d(idp.x(), idp.y(), 1) / idp.z();
                    idp = idp.normalized() * track.depth;
                    idp = Eigen::Vector3d(idp.x(), idp.y(), 1) / idp.z();
                }
                feature.triangulatedStereoPointIdp = idp;
            }

            if (success) {
                ekfStateIndex.headKeyFrame().insertFeatureUnlessExists(track.id, feature);
                if (parameters.odometry.estimateImuCameraTimeShift) {
                    ekfStateIndex.updateVelocities(track.id);
                }
                tmp.trackOrder.push_back(i);
            }
        }

        // Remove all keyframes that do not share any tracks with the current frame.
        // Also remove all hybrid EKF SLAM map points that are no longer LK-tracked
        // 将状态中和当前帧没有关联的关键帧全部去掉，并且没有跟踪关联上的地图点也去掉
        ekfStateIndex.prune();

        // 这里是可视化，将三角化后的点给创建出来.
        if (sample.frame->taggedFrame != nullptr) {
            ekfStateIndex.getVisualizationTracks(sample.frame->taggedFrame->trackerTracks);
        }

        // Iterate through the tracks in a random order (deterministic sequence)
        // to avoid using certain tracks more or always first because of their
        // position in the track list.
        ::util::shuffleDeterministic(tmp.trackOrder.begin(), tmp.trackOrder.end(), rng);

        ekfStateIndex.createMapPointIndex(tmp.mapPointIndex);

        // Move map point tracks to the beginning in iteration order
        auto mapPointBeginItr = tmp.trackOrder.begin();
        for (auto itr = tmp.trackOrder.begin(); itr != tmp.trackOrder.end(); ++itr) {
            if (tmp.mapPointIndex.count(trackerOutput.tracks.at(*itr).id) > 0) {
                std::swap(*mapPointBeginItr++, *itr);
            }
        }

        float minTrackScore = 0.0;
        // 这里看起来是做了一次筛选，只把当中大于光心距离一半的图像点给保存下来
        if (po.scoreVisualUpdateTracks) {
            // Sort tracks by frame length and consider only the half with longer tracks.
            // Besides increasing the average length of used tracks, this allows making
            // the absolute minimum length smaller, to help when tracks are few without
            // trashing performance when they are plenty and mostly long.
            // Going to the extreme of using the tracks in the length order without any
            // randomization is probably bad because we also want the tracks to be
            // somewhat "diverse".
            std::vector<int> trackScores;
            for (const unsigned trackIndex : tmp.trackOrder) {
                const auto &track = trackerOutput.tracks.at(trackIndex);
                trackScores.push_back(static_cast<int>(ekfStateIndex.trackScore(track.id, po.trackSampling)));
            }
            std::sort(trackScores.begin(), trackScores.end());
            minTrackScore = !trackScores.empty() ? trackScores[trackScores.size() / 2] : -1;
        }

        // adaptive threshold, start with base value, increase on failures
        // 这里是做特种筛选，过滤掉一些不重要的特征.
        double rmseThreshold = po.trackRmseThreshold / output.focalLength;
        double chiOutlierR = po.trackChiTestOutlierR / output.focalLength;
        const double visualR = po.visualR / output.focalLength;

        bool needMoreVisualUpdates = true;
        int currentUpdateSize = 0;
        const int maxUpdateSize = int(ekf->getStateDim() * po.batchVisualUpdateMaxSizeMultiplier + 0.5);
        auto &Hbatch = tmp.H;
        auto &ybatch = tmp.y;
        auto &fbatch = tmp.f;
        // 有些批量更新，有些不是批量更新
        const bool batchUpdate = po.batchVisualUpdate || !fullVisualUpdate;
        // 如果选择的是批量更新算法【但每个feature update，怎么来做批量更新.】
        if (batchUpdate) {
            Hbatch = Eigen::MatrixXd::Zero(maxUpdateSize, ekf->getStateDim());
            ybatch = Eigen::VectorXd::Zero(maxUpdateSize);
            fbatch = Eigen::VectorXd::Zero(maxUpdateSize);
        }
        // 这里的tmp.trackOder是每一个tracker的情况.
        // 对之前跟踪出来的特征来进行tracker update.
        for (const unsigned trackIndex : tmp.trackOrder) {
            stats.visualUpdate.newTrack();

            const auto &track = trackerOutput.tracks.at(trackIndex);
            // 开了hybrid才会打开，平时不会关心.
            const bool mapPointUpdate = tmp.mapPointIndex.count(track.id);
            if(mapPointUpdate) std::cout << "The mappoint update is: " << mapPointUpdate << "for track " << track.id << std::endl;
            ekfStateIndex.createTrackIndex(track.id, tmp.poseTrailIndex, po.trackSampling, rng);
            const int nValid = static_cast<int>(tmp.poseTrailIndex.size());
            // 如果没有的情况下
            if (!mapPointUpdate) {
                // 看看跟踪的结果怎么样，如果合适的话，才能用来做tracking update->得看下里面细节，再想想全景的时候能不能无脑调用.
                const float score = ekfStateIndex.trackScore(track.id, po.trackSampling);
                if (po.scoreVisualUpdateTracks && score < minTrackScore) {
                    stats.visualUpdate.notEnoughFrames(); // TODO: not optimally named for "scores"
                    continue;
                }
                if (nValid < po.trackMinFrames) {
                    stats.visualUpdate.notEnoughFrames();
                    continue;
                }
            }

            // only do map point updates in the "lite" visual updates
            // (only applies to visualUpdateEveryNFrames > 1)
            if (!fullVisualUpdate && !mapPointUpdate) continue;

            // Skip blacklisted tracks.
            // The needMoreVisualUpdates check is not strictly necessary here
            // but included to avoid changing the results
            // 跳过不重要的更新点.
            if (po.blacklistTracks &&
                    std::find(blacklistedPrev.begin(), blacklistedPrev.end(), track.id) != blacklistedPrev.end() &&
                    needMoreVisualUpdates) {
                // Also skip this track in next frame.
                tmp.blacklisted.push_back(track.id);
                stats.visualUpdate.blacklisted();
                continue;
            }

            tmp.imageFeatures.clear();
            tmp.featureVelocities.clear();
            // 是多个像素块来更新还是每个feature来进行更新？
            auto &y = batchUpdate ? tmp.yblock : tmp.y;
            // 构建追踪向量，用来做后面的跟踪情况.
            ekfStateIndex.buildTrackVectors(track.id, tmp.poseTrailIndex, tmp.imageFeatures,
                tmp.featureVelocities, y, parameters.tracker.useStereo);

            // NOTE: do not reuse, the trail changes on each visual update!
            // 主要是依据poseTrailIndex来抽取出camera pose放到tmp.trail中，用于后面的visual update.
            extractCameraPoseTrail(*ekf, tmp.poseTrailIndex,
                parameters, parameters.tracker.useStereo, tmp.trail);

            // 这里应该是双目的使用情况，直接通过双目来构建特征点.
            if (po.useIndependentStereoTriangulation) {
                ekfStateIndex.extract3DFeatures(track.id, tmp.poseTrailIndex, tmp.trail);
            }

            // 这里使用来做visual update了.
            if (sharedData->odometryDebugAPI && sharedData->odometryDebugAPI->publisher) {
                sharedData->odometryDebugAPI->publisher->startVisualUpdate(
                    sample.t, *ekf, tmp.poseTrailIndex, tmp.imageFeatures, parameters);
            }

            PointFeature pointCloudFeature = {
              .id = track.id,
              .status = PointFeature::Status::UNUSED,
              .firstPixel = Eigen::Vector2f(track.points[0].x, track.points[0].y),
            };

            // 三角化输出的结果.
            auto &triangulationOut = tmp.triangulationArgsOut;
            TriangulatorStatus triangulateStatus;
            int mapPointIndex = -1;
            // 如果有hyb mappoint的话，则这里需要把triangulate部分调成hybrid来做.
            // 当状态量中有hyb mappoint点时
            if (mapPointUpdate) {
                // 这里应该是直接初始化.
                triangulateStatus = TriangulatorStatus::HYBRID;
                mapPointIndex = tmp.mapPointIndex.at(track.id);
                assert(mapPointIndex >= 0);
                triangulationOut.pf = ekf->getMapPoint(mapPointIndex);
                triangulationOut.dpfdp.clear();
                triangulationOut.dpfdq.clear();
                pointCloudFeature.status = PointFeature::Status::HYBRID;
            } else {
                // 先进行三角化.
                std::cout << "Prepare to do the triangulator." << std::endl;
                const TriangulationArgsIn args {
                    .imageFeatures = tmp.imageFeatures,
                    .featureVelocities = tmp.featureVelocities,
                    .trail = tmp.trail,
                    .stereo = parameters.tracker.useStereo,
                    .calculateDerivatives = true,
                    .estimateImuCameraTimeShift = parameters.odometry.estimateImuCameraTimeShift,
                };
                // 这里是进行三角化->Build the 3D Features.
                // 现在得明确哪块三角化的点用来做更新.
                triangulateStatus = triangulator.triangulate(args, triangulationOut,
                    sharedData->odometryDebugAPI);
                // 得到三角化后的输出信息.
                auto &o = tmp.triangulationArgsOut;

                double depth = (o.pf - tmp.trail.at(0).p).norm();
                // 先判断，如果这里的depth较小或过大的话，则这个三角化出来的值并不适用于来做visual update，将值设定为BAD_DEPTH.
                if (depth < po.triangulationMinDist || depth > po.triangulationMaxDist) {
                    triangulateStatus = TriangulatorStatus::BAD_DEPTH;
                }

                // 如果并不是OK的话，则可以直接返回了，这里的jacobian可以直接置为0.
                if (triangulateStatus != TriangulatorStatus::OK) {
                    // The triangulation function can return early and these may or may not be empty.
                    o.dpfdp.clear();
                    o.dpfdq.clear();
                }
                // 如果是双目，并且状态是OK的话，则做一系列初始化的操作.
                if (parameters.tracker.useStereo && triangulateStatus == TriangulatorStatus::OK) {
                    // Sum the stereo contributions per pose.
                    const size_t n = tmp.poseTrailIndex.size();
                    assert(o.dpfdp.size() == 2 * n);
                    assert(o.dpfdq.size() == 2 * n);
                    for (size_t i = 0; i < n; ++i) {
                        o.dpfdp[i] += o.dpfdp[i + n];
                        o.dpfdq[i] += o.dpfdq[i + n];
                    }
                    o.dpfdp.resize(n);
                    o.dpfdq.resize(n);
                }
                pointCloudFeature.status = PointFeature::Status::POSE_TRAIL;

                // only count the heavy triangulated versions "update attempts"
                // 记录下来这里是尝试进行了一次update.
                updateAttemptCount++;
            }

            pointCloudFeature.point = triangulationOut.pf;

            if (!needMoreVisualUpdates) {
                if (triangulateStatus == TriangulatorStatus::OK) {
                    odometryPointCloud.push_back(pointCloudFeature);
                }

                stats.visualUpdate.triangulationForPointCloud();
                continue;
            }

            auto &H = batchUpdate ? tmp.Hblock : tmp.H;
            auto &f = batchUpdate ? tmp.fblock : tmp.f;

            // 准备好用来做Update的变量.
            const PrepareVisualUpdateArgsIn args {
                .triangulationOut = triangulationOut,
                .featureVelocities = tmp.featureVelocities,
                .trail = tmp.trail,
                .poseTrailIndex = tmp.poseTrailIndex,
                .stateDim = ekf->getStateDim(),
                .useStereo = parameters.tracker.useStereo,
                .truncated = !batchUpdate,
                .mapPointOffset = ekf->getMapPointStateIndex(mapPointIndex),
                .estimateImuCameraTimeShift = parameters.odometry.estimateImuCameraTimeShift,
            };
            //
            const auto prepareVuStatus = prepareVisualUpdate(args, H, f);

            // Criteria for skipping remaining code that does the critical visual update.
            bool doVisualUpdate = prepareVuStatus == PREPARE_VU_OK &&
                (triangulateStatus == TriangulatorStatus::OK || mapPointUpdate);

            // The visual update.
            VuOutlierStatus outlierStatus = VuOutlierStatus::NOT_COMPUTED;
            if (doVisualUpdate) {
                outlierStatus = ekf->visualTrackOutlierCheck(
                    H, f, y, chiOutlierR, rmseThreshold);

                if (outlierStatus == VuOutlierStatus::INLIER) {
                    mapPointIndex = -1;
                    if (!mapPointUpdate && nValid >= po.trackMinFrames)
                        mapPointIndex = ekfStateIndex.offerMapPoint(track.id);

                    if (mapPointIndex >= 0) {
                        ekf->insertMapPoint(mapPointIndex, triangulationOut.pf);
                        // could already do a another visual update, with this track but skipping for simplicity
                    } else {
                        // 如果是批量更新.
                        if (batchUpdate) {
                            const int curBlockSize = H.rows();
                            if (currentUpdateSize + curBlockSize > maxUpdateSize) {
                                // 这里来做bias更新.
                                ekf->updateVisualTrack(
                                    Hbatch.topRows(currentUpdateSize),
                                    fbatch.head(currentUpdateSize),
                                    ybatch.head(currentUpdateSize),
                                    visualR);
                                currentUpdateSize = 0;
                            }
                            Hbatch.block(currentUpdateSize, 0, curBlockSize, Hbatch.cols()) = H;
                            ybatch.segment(currentUpdateSize, curBlockSize) = y;
                            fbatch.segment(currentUpdateSize, curBlockSize) = f;
                            currentUpdateSize += curBlockSize;
                        } else {
                            // 如果不是批量更新.
                            ekf->updateVisualTrack(H, f, y, visualR);
                        }
                    }
                    updateSuccessCount++;
                    ekfStateIndex.markTrackUsed(track.id, tmp.poseTrailIndex, po.trackSampling);
                } else {
                    pointCloudFeature.status = PointFeature::Status::OUTLIER;
                    chiOutlierR *= po.trackOutlierThresholdGrowthFactor;
                    rmseThreshold *= po.trackOutlierThresholdGrowthFactor;
                }
            }

            if (outlierStatus == VuOutlierStatus::INLIER && !batchUpdate
                    && sharedData->odometryDebugAPI && sharedData->odometryDebugAPI->publisher) {
                sharedData->odometryDebugAPI->publisher->finishSuccessfulVisualUpdate(
                    *ekf, tmp.poseTrailIndex, tmp.imageFeatures, parameters);
            }

            // Stop tracking bad tracks.
            bool shouldBlacklist = outlierStatus != VuOutlierStatus::INLIER;
            bool blacklistedTrack = false;
            if (po.blacklistTracks && shouldBlacklist) {
                blacklistedTrack = true;
                tmp.blacklisted.push_back(track.id);
                // Inform the tracker of a bad track so it can replace it with a new feature.
                // This may not have instant effect because of camera frame buffering.
                // Note that this causes the feature detection to be run more often which slows
                // down the algorithm a little bit.
                tracker->deleteTrack(track.id);
            }

            // Store metadata for track visualization.
            // 这里也是用来存储做可视化.
            if (sample.frame->taggedFrame != nullptr) {
                TrackVisualization tv {
                        .prepareVuStatus = prepareVuStatus,
                        .triangulateStatus = triangulateStatus,
                        .visualUpdateSuccess = outlierStatus == VuOutlierStatus::INLIER,
                        .blacklisted = blacklistedTrack,
                        .trackProjection = Eigen::VectorXd(),
                        .trackTracker = Eigen::VectorXd(),
                        .secondTrackProjection = Eigen::VectorXd(),
                        .secondTrackTracker = Eigen::VectorXd()
                };
                populateTrackVisualization(tv, y, f, sample);
                sample.frame->taggedFrame->trackVisualizations.push_back(tv);
            }

            // TODO：后面再看.
            stats.visualUpdate.fullyProcessedTrack(
                triangulateStatus,
                prepareVuStatus,
                outlierStatus,
                doVisualUpdate
            );

            // Speed up by limiting number of visual updates per frame.
            bool limitSuccessful = po.maxSuccessfulVisualUpdates > 0 && updateSuccessCount >= po.maxSuccessfulVisualUpdates;
            bool limitTotal = po.maxVisualUpdates > 0 && updateAttemptCount >= po.maxVisualUpdates;
            if (limitSuccessful || limitTotal) {
                needMoreVisualUpdates = false;
                if (!po.fullPointCloud) break;
                // keep triangulating all tracked features for a full
                // point cloud if requested
            }

            if (triangulateStatus == TriangulatorStatus::OK || mapPointUpdate) {
                odometryPointCloud.push_back(pointCloudFeature);
            }
        } // for each track

        if (currentUpdateSize > 0) {
            assert(batchUpdate);
            ekf->updateVisualTrack(
                Hbatch.topRows(currentUpdateSize),
                fbatch.head(currentUpdateSize),
                ybatch.head(currentUpdateSize),
                visualR);
        }

        handleStereoDepthPointCloud(*sample.frame);

        // roundoff errors can make the state covariance non-symmetric and the below op forces symmetry
        // it should not be needed often. Once a frame (here) is better than once per track update
        // 这里是保证协方差的半正定性->不需要太纠结，都是定律.
        ekf->maintainPositiveSemiDefinite();

        blacklistedPrev.swap(tmp.blacklisted);
        stats.visualUpdate.finishFrame();

        int constexpr FAILED_UPDATES_THRESHOLD = 5;
        bool tooManyFailures = updateAttemptCount - updateSuccessCount > FAILED_UPDATES_THRESHOLD;
        bool goodFrame = (stationaryVisual || !needMoreVisualUpdates) && !tooManyFailures;
        // ignored with non-full visual updates
        return goodFrame;
    }

    void populateTrackVisualization(TrackVisualization &tv, const Eigen::VectorXd &y, const Eigen::VectorXd &f, const SyncedSample &sample) const {
        assert(f.rows() % 2 == 0);
        assert(f.rows() == y.rows());

        const unsigned trackLen = (parameters.tracker.useStereo ? f.rows() / 2 : f.rows()) / 2;

        Eigen::VectorXd blank = Eigen::VectorXd::Zero(trackLen*2);
        tv.secondTrackProjection = blank * 1;
        tv.secondTrackTracker = blank * 1;
        tv.trackProjection = blank * 1;
        tv.trackTracker = blank * 1;

        if (trackLen <= 0) return;
        for (unsigned secondCam = 0; secondCam < (parameters.tracker.useStereo ? 2 : 1); ++secondCam) {
            const unsigned offset = secondCam ? trackLen * 2 : 0;
            const auto &cam = *(secondCam ? sample.frame->secondGrayFrame : sample.frame->firstGrayFrame)->getCamera();
            unsigned validFLength = 0, validYLength = 0;

            Eigen::VectorXd &proj = secondCam ? tv.secondTrackProjection : tv.trackProjection;
            Eigen::VectorXd &track = secondCam ? tv.secondTrackTracker : tv.trackTracker;

            for (unsigned j = 0; j < trackLen; ++j) {
                int xidx = offset + j * 2, yidx = offset + j * 2 + 1;
                Eigen::Vector2d frow, yrow;
                assert(xidx < f.rows() && yidx < f.rows());
                assert(validFLength*2+2 <= proj.rows());
                assert(validYLength*2+2 <= track.rows());
                if (cam.rayToPixel(Eigen::Vector3d(f(xidx), f(yidx), 1), frow))
                    proj.segment<2>(validFLength++ * 2) = frow;

                if (cam.rayToPixel(Eigen::Vector3d(y(xidx), y(yidx), 1), yrow))
                    track.segment<2>(validYLength++ * 2) = yrow;
            }

            proj.conservativeResize(validFLength * 2, 1);
            track.conservativeResize(validYLength * 2, 1);
        }
    }

    void handleStereoDepthPointCloud(ProcessedFrame &frame) {
        if (!sharedData->odometryDebugAPI || !sharedData->odometryDebugAPI->publisher) {
            return;
        }
        auto &gray1 = *frame.firstGrayFrame;
        if (!gray1.hasStereoPointCloud()) {
            sharedData->odometryDebugAPI->publisher->addPointCloud({});
            return;
        }

        tmp.stereoPointCloud = gray1.getStereoPointCloud();

        // log_debug("%zu stereo points", tmp.stereoPointCloud.size());
        Eigen::Matrix4d camToWorld = odometryToWorldToCamera(-1).inverse();
        int id = 1 << 25;

        tmp.stereoPointCloudColor.clear();
        cv::Mat grayImg = reinterpret_cast<tracker::CpuImage&>(gray1).getOpenCvMat();
        for (const Eigen::Vector3f &pCam : tmp.stereoPointCloud) {
            const Eigen::Vector3d pWorld = (camToWorld * pCam.homogeneous().cast<double>()).hnormalized();
            PointFeature feature = {
              .id = id++,
              .status = PointFeature::Status::STEREO,
              .firstPixel = Eigen::Vector2f(-1, -1),
              .point = pWorld
            };
            odometryPointCloud.push_back(feature);

            Eigen::Vector3f color = Eigen::Vector3f::Zero();
            Eigen::Vector2d pix;
            assert(grayImg.channels() == 1);
            if (gray1.getCamera()->rayToPixel(pCam.cast<double>(), pix)) {
                int x = int(pix.x()), y = int(pix.y());
                if (x >= 0 && y >= 0 && x < grayImg.cols && y < grayImg.rows) {
                    // note
                    color = Eigen::Vector3f::Ones() * grayImg.at<uchar>(y, x) / 255.0;
                }
            }
            tmp.stereoPointCloudColor.push_back(color);
        }

        sharedData->odometryDebugAPI->publisher->addPointCloud(
            tmp.stereoPointCloud,
            &tmp.stereoPointCloudColor);
    }

    /**
     * Compute odometry history pose for frame `i`, taking into account SLAM output if available.
     * Use i = -1 to get the current pose.
     */
    void computePose(
        int i,
        Eigen::Vector3d &position,
        Eigen::Vector4d &orientation
    ) const {
        if (coordTrans.isReady()) {
            Eigen::Matrix4d output = coordTrans.worldToCameraPoseOdometryToSlam(odometryToWorldToCamera(i));
            util::toOdometryPose(output, position, orientation, parameters.imuToCamera);
        }
        else {
            position = Eigen::Vector3d::Zero();
            orientation = Eigen::Vector4d::Zero();
        }
    }

    Eigen::Matrix4d odometryToWorldToCamera(int i) const {
        return util::toWorldToCamera(ekf->historyPosition(i),
            ekf->historyOrientation(i), parameters.imuToCamera);
    }
};
}

BackEndBase::~BackEndBase() = default;
std::unique_ptr<BackEnd> BackEnd::build(const Parameters &p) {
    return std::unique_ptr<BackEnd>(new Session(std::make_unique<Session::SharedData>(p)));
}

std::unique_ptr<BackEnd> BackEnd::build(std::unique_ptr<BackEnd> previous) {
    auto &p = reinterpret_cast<Session&>(*previous);
    // TODO: Calling this method can lead to "libc++abi.dylib: terminating with uncaught exception of type std::__1::system_error: mutex lock failed: Invalid argument"
    if (p.slamResult.valid()) p.slamResult.wait();
    return std::unique_ptr<BackEnd>(new Session(std::move(p.sharedData)));
}
}
