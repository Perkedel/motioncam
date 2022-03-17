#include "CameraSession.h"
#include "Exceptions.h"
#include "Logger.h"
#include "RawImageConsumer.h"
#include "CameraSessionListener.h"
#include "CameraSessionContext.h"
#include "CameraStateManager.h"

#include <motioncam/Util.h>
#include <motioncam/Settings.h>
#include <motioncam/RawBufferManager.h>

#include <string>
#include <utility>
#include <utility>
#include <vector>

#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraMetadata.h>

namespace motioncam {
    const static uint32_t MAX_BUFFERED_RAW_IMAGES = 8;

    enum class EventAction : int {
        ACTION_OPEN_CAMERA = 0,
        ACTION_CLOSE_CAMERA,
        ACTION_PAUSE_CAPTURE,
        ACTION_RESUME_CAPTURE,

        ACTION_SET_AUTO_EXPOSURE,
        ACTION_SET_MANUAL_EXPOSURE,
        ACTION_SET_EXPOSURE_COMP_VALUE,
        ACTION_SET_FRAME_RATE,
        ACTION_SET_AWB_LOCK,
        ACTION_SET_AE_LOCK,
        ACTION_SET_OIS,
        ACTION_SET_LENS_APERTURE,
        ACTION_SET_FOCUS_DISTANCE,
        ACTION_SET_FOCUS_FOR_VIDEO,
        ACTION_SET_AUTO_FOCUS,
        ACTION_SET_FOCUS_POINT,
        ACTION_UPDATE_PREVIEW,
        ACTION_ACTIVATE_CAMERA_SETTINGS,
        ACTION_PRECAPTURE_HDR,
        ACTION_CAPTURE_HDR,

        EVENT_SAVE,
        EVENT_SAVE_HDR_DATA,

        EVENT_CAMERA_ERROR,
        EVENT_CAMERA_DISCONNECTED,
        EVENT_CAMERA_SESSION_CHANGED,
        EVENT_CAMERA_SEQUENCE_COMPLETED,

        EVENT_CAMERA_EXPOSURE_STATUS_CHANGED,
        EVENT_CAMERA_AUTO_EXPOSURE_STATE_CHANGED,
        EVENT_CAMERA_AUTO_FOCUS_STATE_CHANGED,

        STOP
    };

    struct EventLoopData {
        EventLoopData(EventAction eventAction, json11::Json data) :
                eventAction(eventAction),
                data(std::move(data))
        {
        }

        const EventAction eventAction;
        const json11::Json data;
    };

    CameraFocusState GetFocusState(acamera_metadata_enum_android_control_af_state_t state) {
        switch(state) {
            default:
            case ACAMERA_CONTROL_AF_STATE_INACTIVE:
                return CameraFocusState::INACTIVE;

            case ACAMERA_CONTROL_AF_STATE_PASSIVE_SCAN:
                return CameraFocusState::PASSIVE_SCAN;

            case ACAMERA_CONTROL_AF_STATE_PASSIVE_FOCUSED:
                return CameraFocusState::PASSIVE_FOCUSED;

            case ACAMERA_CONTROL_AF_STATE_ACTIVE_SCAN:
                return CameraFocusState::ACTIVE_SCAN;

            case ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED:
                return CameraFocusState::FOCUS_LOCKED;

            case ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED:
                return CameraFocusState::NOT_FOCUS_LOCKED;

            case ACAMERA_CONTROL_AF_STATE_PASSIVE_UNFOCUSED:
                return CameraFocusState::PASSIVE_UNFOCUSED;
        }
    }

    CameraExposureState GetExposureState(acamera_metadata_enum_android_control_ae_state_t state) {
        switch(state) {
            default:
            case ACAMERA_CONTROL_AE_STATE_INACTIVE:
                return CameraExposureState::INACTIVE;

            case ACAMERA_CONTROL_AE_STATE_SEARCHING:
                return CameraExposureState::SEARCHING;

            case ACAMERA_CONTROL_AE_STATE_CONVERGED:
                return CameraExposureState::CONVERGED;

            case ACAMERA_CONTROL_AE_STATE_LOCKED:
                return CameraExposureState::LOCKED;

            case ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED:
                return CameraExposureState::FLASH_REQUIRED;

            case ACAMERA_CONTROL_AE_STATE_PRECAPTURE:
                return CameraExposureState::PRECAPTURE;
        }
    }

    namespace {
        void OnImageAvailable(void *context, AImageReader *reader) {
            reinterpret_cast<CameraSession *>(context)->onRawImageAvailable(reader);
        }

        void OnCameraError(void* context, ACameraDevice* device, int error) {
            reinterpret_cast<CameraSession *>(context)->onCameraError(error);
        }

        void OnCameraDisconnected(void* context, ACameraDevice* device) {
            reinterpret_cast<CameraSession *>(context)->onCameraDisconnected();
        }

        void OnCameraSessionClosed(void* context, ACameraCaptureSession* session) {
            reinterpret_cast<CameraSession *>(context)->onCameraSessionStateClosed();
        }

        void OnCameraSessionReady(void* context, ACameraCaptureSession* session) {
            reinterpret_cast<CameraSession *>(context)->onCameraSessionStateReady();
        }

        void OnCameraSessionActive(void* context, ACameraCaptureSession* session) {
            reinterpret_cast<CameraSession *>(context)->onCameraSessionStateActive();
        }

        //
        // Capture callbacks
        //

        void OnCameraCaptureStarted(void* context, ACameraCaptureSession* session, const ACaptureRequest* request, int64_t timestamp) {
            auto* callbackContext = reinterpret_cast<CaptureCallbackContext*>(context);
            callbackContext->cameraSession->onCameraCaptureStarted(*callbackContext, request, timestamp);
        }

        void OnCameraCaptureCompleted(void* context, ACameraCaptureSession* session, ACaptureRequest* request, const ACameraMetadata* result) {
            auto* callbackContext = reinterpret_cast<CaptureCallbackContext*>(context);
            callbackContext->cameraSession->onCameraCaptureCompleted(*callbackContext, result);
        }

        void OnCameraCaptureFailed(void* context, ACameraCaptureSession* session, ACaptureRequest* request, ACameraCaptureFailure* failure) {
            auto* callbackContext = reinterpret_cast<CaptureCallbackContext*>(context);
            callbackContext->cameraSession->onCameraCaptureFailed(*callbackContext, failure);
        }

        void OnCameraCaptureProgressed(void* context, ACameraCaptureSession* session, ACaptureRequest* request, const ACameraMetadata* result) {
            auto* callbackContext = reinterpret_cast<CaptureCallbackContext*>(context);
            callbackContext->cameraSession->onCameraCaptureProgressed(*callbackContext, result);
        }

        void OnCameraCaptureBufferLost(void* context, ACameraCaptureSession* session, ACaptureRequest* request, ANativeWindow* window, int64_t frameNumber) {
            auto* callbackContext = reinterpret_cast<CaptureCallbackContext*>(context);
            callbackContext->cameraSession->onCameraCaptureBufferLost(*callbackContext, frameNumber);
        }

        void OnCameraCaptureSequenceCompleted(void* context, ACameraCaptureSession* session, int sequenceId, int64_t frameNumber) {
            auto* callbackContext = reinterpret_cast<CaptureCallbackContext*>(context);
            callbackContext->cameraSession->onCameraCaptureSequenceCompleted(*callbackContext, sequenceId);
        }

        void OnCameraCaptureSequenceAborted(void* context, ACameraCaptureSession* session, int sequenceId) {
            auto* callbackContext = reinterpret_cast<CaptureCallbackContext*>(context);
            callbackContext->cameraSession->onCameraCaptureSequenceAborted(*callbackContext, sequenceId);
        }
    }

    float clamp(float x, float min, float max) {
        return std::max(std::min(x, max), min);
    }

    void generateTonemapCurve(float shadows, float brightness, float contrast, float blackPoint, float whitePoint, int pts, std::vector<float>& output) {
        output.clear();

        for(int i = 0 ; i < pts; i++) {
            float x = (i / static_cast<float>(pts));

            float in = shadows * x;

            // Tonemap
            float a = (in  * (1.0f + x / (shadows*shadows))) / (1.0f + in);

            // Gamma
            float b;
            if(a < 0.0031308f)
                b = a * 12.92;
            else
                b = std::pow(a, 1.0f / 2.4f) * 1.055f - 0.055f;

            // Contrast
            float C = clamp(contrast, 0.0f, 1.0f) + 1.0f;
            float s = b / std::max(1e-5f, 1.0f - b);
            float c = 1.0f / (1 + std::pow(std::max(1e-5f, s), -C));

            // Black/white point
            float u = clamp(c - blackPoint, 0.0f, 1.0f) * (1.0f / (1.0f - blackPoint + 1e-5f));
            float out = u / whitePoint;

            output.push_back(x);
            output.push_back(out);
        }
    }

    CameraSession::CameraSession() :
            mState(CameraCaptureSessionState::CLOSED),
            mLastIso(0),
            mLastExposureTime(0),
            mLastFocusDistance(0),
            mLastFocusState(CameraFocusState::INACTIVE),
            mLastExposureState(CameraExposureState::INACTIVE),
            mScreenOrientation(ScreenOrientation::PORTRAIT),
            mRequestedHdrCaptures(0),
            mRequestHdrCaptureTimestamp(-1),
            mLongHdrCaptureInProgress(false),
            mHdrCaptureSequenceCompleted(true),
            mAcceptEvents(false)
    {
    }

    CameraSession::~CameraSession() {
        closeCamera();
    }

    std::shared_ptr<CameraDescription> CameraSession::cameraDescription() const {
        return mCameraDescription;
    }

    void CameraSession::openCamera(
        std::shared_ptr<CameraSessionListener> sessionListener,
        std::shared_ptr<CameraDescription> cameraDescription,
        const OutputConfiguration& rawOutputConfig,
        const OutputConfiguration& previewOutputConfig,
        std::shared_ptr<ACameraManager> cameraManager,
        std::shared_ptr<ANativeWindow> previewOutputWindow,
        bool setupForRawPreview,
        const json11::Json& cameraStartupSettings,
        const size_t maxMemoryUsageBytes)
    {
        if(mSessionContext) {
            LOGE("Trying to open camera while already running!");
            return;
        }

        // Keep camera manager
        mCameraManager = std::move(cameraManager);
        mSessionListener = std::move(sessionListener);
        mCameraDescription = std::move(cameraDescription);
        mImageConsumer = std::make_shared<RawImageConsumer>(mCameraDescription, mSessionListener, maxMemoryUsageBytes);

        // Create new session context and set up callbacks
        mSessionContext = std::make_shared<CameraCaptureSessionContext>();

        mSessionContext->outputConfig = rawOutputConfig;
        mSessionContext->previewOutputConfig = previewOutputConfig;
        mSessionContext->nativeWindow = std::move(previewOutputWindow);

        setupCallbacks();

        // Create event loop and start
        mEventLoopThread = std::make_unique<std::thread>(std::thread(&CameraSession::doEventLoop, this));

        json11::Json::object data = {
            { "setupForRawPreview", setupForRawPreview },
            { "cameraStartupSettings", cameraStartupSettings }
        };

        mAcceptEvents = true;
        pushEvent(EventAction::ACTION_OPEN_CAMERA, data);
    }

    void CameraSession::closeCamera() {
        if(!mSessionContext)
            return;

        // TODO:
        // There is a bug here where the camera sends an active event after the close event leaving us in a
        // deadlock. This happens if the camera is started/stopped very quickly.

        pushEvent(EventAction::ACTION_CLOSE_CAMERA);
        pushEvent(EventAction::STOP);

        if(mEventLoopThread && mEventLoopThread->joinable()) {
            mEventLoopThread->join();
        }

        mEventLoopThread = nullptr;
        mSessionContext = nullptr;
        mCameraDescription = nullptr;
        mImageConsumer = nullptr;
        mSessionListener = nullptr;
        mCameraStateManager = nullptr;
    }

    void CameraSession::getEstimatedPostProcessSettings(PostProcessSettings& outSettings) {
        if(mImageConsumer)
            mImageConsumer->getEstimatedSettings(outSettings);
    }

    void CameraSession::updateRawPreviewSettings(
            float shadows, float contrast, float saturation, float blackPoint, float whitePoint, float tempOffset, float tintOffset, bool useVideoPreview)
    {
        if(!mSessionContext)
            return;

        json11::Json::object data = {
                { "shadows", shadows },
                { "contrast", contrast },
                { "saturation", saturation },
                { "blackPoint", blackPoint },
                { "whitePoint", whitePoint }
        };

        pushEvent(EventAction::ACTION_UPDATE_PREVIEW, data);
    }

    void CameraSession::enableRawPreview(std::shared_ptr<RawPreviewListener> listener, const int previewQuality) {
        if(mImageConsumer)
            mImageConsumer->enableRawPreview(listener, previewQuality);
    }

    void CameraSession::disableRawPreview() {
        if(mImageConsumer)
            mImageConsumer->disableRawPreview();
    }

    void CameraSession::growMemory(uint64_t memoryBytes) {
        if(mImageConsumer)
            mImageConsumer->grow(memoryBytes);
    }

    void CameraSession::pauseCapture() {
        pushEvent(EventAction::ACTION_PAUSE_CAPTURE);
    }

    void CameraSession::resumeCapture() {
        pushEvent(EventAction::ACTION_RESUME_CAPTURE);
    }

    void CameraSession::setManualExposure(int32_t iso, int64_t exposureTime) {
        json11::Json::object data = {
                { "iso", iso },
                { "exposureTime", std::to_string(exposureTime) }
        };

        pushEvent(EventAction::ACTION_SET_MANUAL_EXPOSURE, data);
    }

    void CameraSession::setAutoExposure() {
        pushEvent(EventAction::ACTION_SET_AUTO_EXPOSURE);
    }

    void CameraSession::setExposureCompensation(float value) {
        json11::Json::object data = { { "value", value } };
        pushEvent(EventAction::ACTION_SET_EXPOSURE_COMP_VALUE, data);
    }

    void CameraSession::setFrameRate(int frameRate) {
        json11::Json::object data = { { "value", frameRate } };
        pushEvent(EventAction::ACTION_SET_FRAME_RATE, data);
    }

    void CameraSession::setAWBLock(bool lock) {
        json11::Json::object data = { { "value", lock } };
        pushEvent(EventAction::ACTION_SET_AWB_LOCK, data);
    }

    void CameraSession::setAELock(bool lock) {
        json11::Json::object data = { { "value", lock } };
        pushEvent(EventAction::ACTION_SET_AE_LOCK, data);
    }

    void CameraSession::setOIS(bool on) {
        json11::Json::object data = { { "value", on } };
        pushEvent(EventAction::ACTION_SET_OIS, data);
    }

    void CameraSession::setFocusDistance(float focusDistance) {
        json11::Json::object data = { { "value", focusDistance } };
        pushEvent(EventAction::ACTION_SET_FOCUS_DISTANCE, data);
    }

    void CameraSession::setFocusForVideo(bool focusForVideo) {
        json11::Json::object data = { { "value", focusForVideo } };
        pushEvent(EventAction::ACTION_SET_FOCUS_FOR_VIDEO, data);
    }

    void CameraSession::setLensAperture(float lensAperture) {
        json11::Json::object data = { { "value", lensAperture } };
        pushEvent(EventAction::ACTION_SET_LENS_APERTURE, data);
    }

    void CameraSession::activateCameraSettings() {
        pushEvent(EventAction::ACTION_ACTIVATE_CAMERA_SETTINGS);
    }

    void CameraSession::setFocusPoint(float focusX, float focusY, float exposureX, float exposureY) {
        json11::Json::object data = {
                { "focusX", focusX },
                { "focusY", focusY },
                { "exposureX", exposureX },
                { "exposureY", exposureY }
        };

        pushEvent(EventAction::ACTION_SET_FOCUS_POINT, data);
    }

    void CameraSession::setAutoFocus() {
        pushEvent(EventAction::ACTION_SET_AUTO_FOCUS);
    }

    void CameraSession::captureHdr(
        int numImages,
        int baseIso,
        int64_t baseExposure,
        int hdrIso,
        int64_t hdrExposure,
        const PostProcessSettings& postprocessSettings,
        const std::string& outputPath) {

        if(!mHdrCaptureSequenceCompleted) {
            LOGW("HDR capture already in progress, ignoring request");
            return;
        }

        mHdrCaptureSequenceCompleted = false;
        mLongHdrCaptureInProgress = true;
        mHdrCaptureOutputPath = outputPath;
        mHdrCaptureSettings = postprocessSettings;

        json11::Json::object data = {
                { "numImages", numImages },
                { "baseIso", baseIso },
                { "baseExposure", std::to_string(baseExposure) },
                { "hdrIso", hdrIso },
                { "hdrExposure", std::to_string(hdrExposure) }
        };

        pushEvent(EventAction::ACTION_CAPTURE_HDR, data);
    }

    void CameraSession::captureHdr(
            int numImages,
            const PostProcessSettings& postprocessSettings,
            const std::string& outputPath) {

        mHdrCaptureOutputPath = outputPath;
        mHdrCaptureSettings = postprocessSettings;

        json11::Json::object data = {
            { "numImages", numImages }
        };

        pushEvent(EventAction::EVENT_SAVE, data);
    }

    void CameraSession::prepareHdr(int iso, int64_t exposure) {
        if(!mHdrCaptureSequenceCompleted) {
            LOGW("HDR capture already in progress, ignoring request");
            return;
        }

        mHdrCaptureSequenceCompleted = false;
        mLongHdrCaptureInProgress = false;

        json11::Json::object data = {
            { "iso", iso },
            { "exposure", std::to_string(exposure) }
        };

        pushEvent(EventAction::ACTION_PRECAPTURE_HDR, data);
    }

    ACaptureRequest* CameraSession::createCaptureRequest(const ACameraDevice_request_template requestTemplate) {
        ACaptureRequest* captureRequest = nullptr;

        if(ACameraDevice_createCaptureRequest(mSessionContext->activeCamera.get(), requestTemplate, &captureRequest) != ACAMERA_OK)
            throw CameraSessionException("Failed to create capture request");

        const uint8_t tonemapMode           = ACAMERA_TONEMAP_MODE_FAST;
        const uint8_t shadingMode           = ACAMERA_SHADING_MODE_HIGH_QUALITY;
        const uint8_t colorCorrectionMode   = ACAMERA_COLOR_CORRECTION_MODE_FAST;
        const uint8_t lensShadingMapStats   = ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_ON;
        const uint8_t lensShadingMapApplied = ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED_FALSE;
        const uint8_t antiBandingMode       = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO;
        const uint8_t noiseReduction        = ACAMERA_NOISE_REDUCTION_MODE_FAST;
        const uint8_t edgeMode              = ACAMERA_EDGE_MODE_FAST;

        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_TONEMAP_MODE, 1, &tonemapMode);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_SHADING_MODE, 1, &shadingMode);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE, 1, &lensShadingMapStats);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED, 1, &lensShadingMapApplied);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_CONTROL_AE_ANTIBANDING_MODE, 1, &antiBandingMode);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_NOISE_REDUCTION_MODE, 1, &noiseReduction);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_COLOR_CORRECTION_MODE, 1, &colorCorrectionMode);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_EDGE_MODE, 1, &edgeMode);

        uint8_t aeMode  = ACAMERA_CONTROL_AE_MODE_ON;
        uint8_t afMode  = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        uint8_t awbMode = ACAMERA_CONTROL_AWB_MODE_AUTO;

        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_CONTROL_AWB_MODE, 1, &awbMode);

        uint8_t afTrigger = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
        uint8_t aeTrigger = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;

        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &afTrigger);
        ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_CONTROL_AF_TRIGGER, 1, &aeTrigger);

        return captureRequest;
    }

    void CameraSession::setupPreviewCaptureOutput(CameraCaptureSessionContext& state, bool setupForRawPreview) {
        ACaptureSessionOutput* sessionOutput = nullptr;
        ACameraOutputTarget* outputTarget = nullptr;

        // Keep the native window
        if (ACaptureSessionOutput_create(state.nativeWindow.get(), &sessionOutput) != ACAMERA_OK)
            throw CameraSessionException("Failed to create preview window session output");

        state.previewSessionOutput = std::shared_ptr<ACaptureSessionOutput>(sessionOutput, ACaptureSessionOutput_free);

        if (ACameraOutputTarget_create(state.nativeWindow.get(), &outputTarget) != ACAMERA_OK)
            throw CameraSessionException("Failed to create preview target");

        state.previewOutputTarget = std::shared_ptr<ACameraOutputTarget>(outputTarget, ACameraOutputTarget_free);

        if (ACaptureSessionOutputContainer_add(state.captureSessionContainer.get(), sessionOutput) != ACAMERA_OK)
            throw CameraSessionException("Failed to add preview output to session container");

        if(!setupForRawPreview) {
            if (ACaptureRequest_addTarget(state.repeatCaptureRequest->captureRequest, outputTarget) != ACAMERA_OK)
                throw CameraSessionException("Failed to add RAW output target");
        }
    }

    void CameraSession::setupRawCaptureOutput(CameraCaptureSessionContext& state) {
        AImageReader* imageReader = nullptr;

        media_status_t result =
                AImageReader_new(
                        state.outputConfig.outputSize.originalWidth(),
                        state.outputConfig.outputSize.originalHeight(),
                        state.outputConfig.format,
                        MAX_BUFFERED_RAW_IMAGES,
                        &imageReader);

        if (result != AMEDIA_OK) {
            throw CameraSessionException(std::string("Failed to create RAW image reader") + " (" + std::to_string(result) + ")");
        }

        state.rawImageReader = std::shared_ptr<AImageReader>(imageReader, AImageReader_delete);

        state.rawImageListener.context = this;
        state.rawImageListener.onImageAvailable = OnImageAvailable;

        // Set up image listener callback
        AImageReader_setImageListener(state.rawImageReader.get(), &state.rawImageListener);

        // Set up RAW output
        ANativeWindow* nativeWindow = nullptr;
        AImageReader_getWindow(state.rawImageReader.get(), &nativeWindow);

        ACaptureSessionOutput* sessionOutput = nullptr;
        ACameraOutputTarget* outputTarget = nullptr;

        if (ACaptureSessionOutput_create(nativeWindow, &sessionOutput) != ACAMERA_OK)
            throw CameraSessionException("Failed to create raw image reader capture session output");

        state.rawSessionOutput = std::shared_ptr<ACaptureSessionOutput>(sessionOutput, ACaptureSessionOutput_free);

        if (ACameraOutputTarget_create(nativeWindow, &outputTarget) != ACAMERA_OK)
            throw CameraSessionException("Failed to create raw target");

        state.rawOutputTarget = std::shared_ptr<ACameraOutputTarget>(outputTarget, ACameraOutputTarget_free);

        if (ACaptureSessionOutputContainer_add(state.captureSessionContainer.get(), state.rawSessionOutput.get()) != ACAMERA_OK)
            throw CameraSessionException("Failed to add raw session output to container");

        // Add all RAW output captures
        if (ACaptureRequest_addTarget(mSessionContext->repeatCaptureRequest->captureRequest, outputTarget) != ACAMERA_OK)
            throw CameraSessionException("Failed to add RAW output target");

        for(auto & hdrCaptureRequest : mSessionContext->hdrCaptureRequests)
            if (ACaptureRequest_addTarget(hdrCaptureRequest->captureRequest, outputTarget) != ACAMERA_OK)
                throw CameraSessionException("Failed to add HDR RAW output target");
    }

    void CameraSession::doOpenCamera(bool setupForRawPreview, const json11::Json& startupSettings) {
        if(mState != CameraCaptureSessionState::CLOSED) {
            LOGE("Trying to open camera that isn't closed");
            return;
        }

        // Open the camera
        LOGD("Opening camera");
        ACameraDevice* device = nullptr;

        if (ACameraManager_openCamera(
                mCameraManager.get(),
                mCameraDescription->id.c_str(),
                &mSessionContext->deviceStateCallbacks,
                &device) != ACAMERA_OK)
        {
            throw CameraSessionException("Failed to open camera");
        }

        mSessionContext->activeCamera = std::shared_ptr<ACameraDevice>(device, ACameraDevice_close);

        LOGD("Camera has opened");

        // Create output container
        ACaptureSessionOutputContainer* container = nullptr;

        if (ACaptureSessionOutputContainer_create(&container) != ACAMERA_OK)
            throw CameraSessionException("Failed to create session container");

        mSessionContext->captureSessionContainer = std::shared_ptr<ACaptureSessionOutputContainer>(container, ACaptureSessionOutputContainer_free);

        // Create capture request
        mSessionContext->repeatCaptureRequest = std::make_shared<CaptureRequest>(createCaptureRequest(TEMPLATE_PREVIEW), true);

        // Create HDR requests
        mSessionContext->hdrCaptureRequests[0] = std::make_shared<CaptureRequest>(createCaptureRequest(TEMPLATE_STILL_CAPTURE), false);
        mSessionContext->hdrCaptureRequests[1] = std::make_shared<CaptureRequest>(createCaptureRequest(TEMPLATE_STILL_CAPTURE), false);

        // Set up a JPEG output that we don't use. For some reason without it the camera auto
        // focus does not work properly
        //setupJpegCaptureOutput(*mSessionContext);

        // Set up output for capture
        setupRawCaptureOutput(*mSessionContext);

        // Set up output for preview
        setupPreviewCaptureOutput(*mSessionContext, setupForRawPreview);

        // Finally create and start the session
        ACameraCaptureSession* captureSession = nullptr;

        LOGD("Creating capture session");
        if (ACameraDevice_createCaptureSession(
                mSessionContext->activeCamera.get(),
                mSessionContext->captureSessionContainer.get(),
                &mSessionContext->sessionStateCallbacks,
                &captureSession) != ACAMERA_OK)
        {
            throw CameraSessionException("Failed to create capture session");
        }

        mSessionContext->captureSession =
                std::shared_ptr<ACameraCaptureSession>(captureSession, ACameraCaptureSession_close);

        mImageConsumer->start();

        // Start capture
        LOGD("Starting capture");

        mCameraStateManager = std::unique_ptr<CameraStateManager>(new CameraStateManager(*mSessionContext, *mCameraDescription));
        mCameraStateManager->start(startupSettings);

        mSessionListener->onCameraStarted();
    }

    void CameraSession::doCloseCamera() {
        // Stop accepting events when we are about to close the camera
        mAcceptEvents = false;

        // Stop all captures
        if(mSessionContext->captureSession != nullptr) {
            ACameraCaptureSession_abortCaptures(mSessionContext->captureSession.get());
        }

        // Close capture session
        LOGD("Closing capture session");
        mSessionContext->captureSession = nullptr;

        // Close active camera
        LOGD("Closing camera device");
        mSessionContext->activeCamera = nullptr;

        // Stop image consumer before closing RAW image reader
        LOGD("Stopping image consumer");
        mImageConsumer->stop();
        mImageConsumer = nullptr;

        LOGD("Closing RAW image reader");
        mSessionContext->rawImageReader = nullptr;

        // Free capture request
        if(mSessionContext->previewOutputTarget && mSessionContext->repeatCaptureRequest->isPreviewOutput)
            ACaptureRequest_removeTarget(mSessionContext->repeatCaptureRequest->captureRequest, mSessionContext->previewOutputTarget.get());

        if(mSessionContext->rawOutputTarget)
            ACaptureRequest_removeTarget(mSessionContext->repeatCaptureRequest->captureRequest, mSessionContext->rawOutputTarget.get());

        mSessionContext->previewOutputTarget    = nullptr;
        mSessionContext->rawOutputTarget        = nullptr;

        // Clear session container
        if(mSessionContext->captureSessionContainer) {
            if(mSessionContext->previewSessionOutput)
                ACaptureSessionOutputContainer_remove(mSessionContext->captureSessionContainer.get(), mSessionContext->previewSessionOutput.get());

            if(mSessionContext->rawSessionOutput)
                ACaptureSessionOutputContainer_remove(mSessionContext->captureSessionContainer.get(), mSessionContext->rawSessionOutput.get());
        }

        mSessionContext->captureSessionContainer    = nullptr;
        mSessionContext->previewSessionOutput       = nullptr;
        mSessionContext->rawSessionOutput           = nullptr;
        mSessionContext->nativeWindow               = nullptr;

        LOGD("Camera closed");
    }

    void CameraSession::doPauseCapture() {
        if(mState != CameraCaptureSessionState::ACTIVE) {
            LOGW("Cannot pause capture, invalid state.");
            return;
        }

        mCameraStateManager->requestPause();
    }

    void CameraSession::doResumeCapture() {
        if(mState != CameraCaptureSessionState::READY) {
            LOGW("Cannot resume capture, invalid state.");
            return;
        }

        mCameraStateManager->requestResume();
    }

    void CameraSession::doUpdatePreview(float shadows, float contrast, float blackPoint, float whitePoint) {
        if (mState == CameraCaptureSessionState::CLOSED) {
            LOGW("Cannot update preview, invalid state");
            return;
        }

        if (mState == CameraCaptureSessionState::ACTIVE) {
            generateTonemapCurve(shadows, 1.0f, contrast, blackPoint, whitePoint, mCameraDescription->maxTonemapCurvePts, mTonemapCurvePts);

            mCameraStateManager->requestUpdatePreview(std::move(mTonemapCurvePts));
            mCameraStateManager->activate();
        }
    }

    void CameraSession::doSetAutoExposure() {
        if (mState == CameraCaptureSessionState::CLOSED) {
            LOGW("Cannot set auto exposure, invalid state");
            return;
        }

        mCameraStateManager->requestExposureMode(CameraMode::AUTO);
    }

    void CameraSession::doSetManualExposure(int32_t iso, int64_t exposureTime) {
        if (mState == CameraCaptureSessionState::CLOSED) {
            LOGW("Cannot set manual exposure, invalid state");
            return;
        }

        if (mState == CameraCaptureSessionState::ACTIVE) {
            mCameraStateManager->requestUserExposure(iso, exposureTime);
        }
    }

    void CameraSession::doSetFocusPoint(double focusX, double focusY, double exposureX, double exposureY) {
        if(mState == CameraCaptureSessionState::CLOSED) {
            LOGW("Cannot set focus, invalid state");
            return;
        }

        // Need at least one AE region
        if(mCameraDescription->maxAfRegions <= 0) {
            LOGI("Can't set focus, zero AF regions");
            return;
        }

        mCameraStateManager->requestUserFocus(focusX, focusY);
    }

    void CameraSession::doSetAutoFocus() {
        if(mState == CameraCaptureSessionState::CLOSED) {
            LOGW("Cannot set auto focus, invalid state");
            return;
        }

        mCameraStateManager->requestAutoFocus();
    }

    void CameraSession::doPrecaptureCaptureHdr(int iso, int64_t exposure) {
        // Don't capture image if iso/exposure not set
        if(iso < 0 || exposure < 0)
            return;

        uint8_t mode  = ACAMERA_CONTROL_MODE_OFF_KEEP_STATE;

        ACaptureRequest_setEntry_u8(mSessionContext->hdrCaptureRequests[0]->captureRequest, ACAMERA_CONTROL_MODE, 1, &mode);
        ACaptureRequest_setEntry_i32(mSessionContext->hdrCaptureRequests[0]->captureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
        ACaptureRequest_setEntry_i64(mSessionContext->hdrCaptureRequests[0]->captureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposure);

        int32_t lastIso = mLastIso;
        int64_t lastExposureTime = mLastExposureTime;

        ACaptureRequest_setEntry_u8(mSessionContext->hdrCaptureRequests[1]->captureRequest, ACAMERA_CONTROL_MODE, 1, &mode);
        ACaptureRequest_setEntry_i32(mSessionContext->hdrCaptureRequests[1]->captureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &lastIso);
        ACaptureRequest_setEntry_i64(mSessionContext->hdrCaptureRequests[1]->captureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &lastExposureTime);

        std::vector<ACaptureRequest*> captureRequests(2);

        // Set up list of capture requests
        captureRequests[0] = mSessionContext->hdrCaptureRequests[0]->captureRequest;

        // Capture last image using previous exposure settings. This helps the AE behave a bit better
        captureRequests[1] = mSessionContext->hdrCaptureRequests[1]->captureRequest;

        LOGI("Initiating HDR precapture (hdrIso=%d, hdrExposure=%ld)", iso, exposure);

        // Keep timestamp of latest buffer as our reference
        mRequestHdrCaptureTimestamp = RawBufferManager::get().latestTimeStamp();
        mRequestedHdrCaptures = 1;

        ACameraCaptureSession_capture(
                mSessionContext->captureSession.get(),
                &mSessionContext->captureCallbacks[CaptureEvent::HDR_CAPTURE]->callbacks,
                captureRequests.size(),
                captureRequests.data(),
                &mSessionContext->captureCallbacks[CaptureEvent::HDR_CAPTURE]->sequenceId);
    }

    void CameraSession::doCaptureHdr(int numImages, int baseIso, int64_t baseExposure, int hdrIso, int64_t hdrExposure) {
        if(numImages < 1) {
            LOGE("Invalid HDR capture requested (numImages < 1)");
            return;
        }

        uint8_t mode  = ACAMERA_CONTROL_MODE_OFF_KEEP_STATE;

        float focusDistance = mLastFocusDistance;

        ACaptureRequest_setEntry_u8(mSessionContext->hdrCaptureRequests[0]->captureRequest, ACAMERA_CONTROL_MODE, 1, &mode);
        ACaptureRequest_setEntry_float(mSessionContext->hdrCaptureRequests[0]->captureRequest, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDistance);
        ACaptureRequest_setEntry_i32(mSessionContext->hdrCaptureRequests[0]->captureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &baseIso);
        ACaptureRequest_setEntry_i64(mSessionContext->hdrCaptureRequests[0]->captureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &baseExposure);

        ACaptureRequest_setEntry_u8(mSessionContext->hdrCaptureRequests[1]->captureRequest, ACAMERA_CONTROL_MODE, 1, &mode);
        ACaptureRequest_setEntry_float(mSessionContext->hdrCaptureRequests[1]->captureRequest, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDistance);
        ACaptureRequest_setEntry_i32(mSessionContext->hdrCaptureRequests[1]->captureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &hdrIso);
        ACaptureRequest_setEntry_i64(mSessionContext->hdrCaptureRequests[1]->captureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &hdrExposure);

        std::vector<ACaptureRequest*> captureRequests;

        // Allocate enough for numImages + 1 underexposed images
        mRequestedHdrCaptures = numImages + 1;

        captureRequests.resize(mRequestedHdrCaptures);

        for (int i = 0; i < mRequestedHdrCaptures; i++)
            captureRequests[i] = mSessionContext->hdrCaptureRequests[0]->captureRequest;

        // Second capture is underexposed image
        captureRequests[1] = mSessionContext->hdrCaptureRequests[1]->captureRequest;

        LOGI("Initiating HDR capture (numImages=%d, baseIso=%d, baseExposure=%ld, hdrIso=%d, hdrExposure=%ld)",
                numImages, baseIso, baseExposure, hdrIso, hdrExposure);

        // Keep timestamp of latest buffer as our reference
        mRequestHdrCaptureTimestamp = RawBufferManager::get().latestTimeStamp();

        ACameraCaptureSession_capture(
            mSessionContext->captureSession.get(),
            &mSessionContext->captureCallbacks[CaptureEvent::HDR_CAPTURE]->callbacks,
            captureRequests.size(),
            captureRequests.data(),
            &mSessionContext->captureCallbacks[CaptureEvent::HDR_CAPTURE]->sequenceId);
    }

    void CameraSession::doSave(int numImages) {
        int hdrBufferCount = RawBufferManager::get().numHdrBuffers();
        bool waitForImage = true;

        // Don't wait for HDR image if we have already captured it but
        // can't find it. This is likely because the user has kept the shutter button
        // pressed for a long time before releasing it.
        if(mHdrCaptureSequenceCompleted) {
            double timeSinceSequenceCompleted =
                    std::chrono::duration <double, std::milli>(std::chrono::steady_clock::now() - mHdrSequenceCompletedTimePoint).count();

            if(timeSinceSequenceCompleted > 250) {
                waitForImage = false;
                LOGI("Not waiting for HDR image");
            }
        }

        if(waitForImage && (hdrBufferCount < mRequestedHdrCaptures)) {
            // Continue waiting for HDR image
            json11::Json::object data = { { "numImages", numImages } };
            pushEvent(EventAction::EVENT_SAVE, data);
            return;
        }

        RawBufferManager::get().saveHdr(
                numImages + mRequestedHdrCaptures,
                mRequestHdrCaptureTimestamp,
                mCameraDescription->metadata,
                mHdrCaptureSettings,
                mHdrCaptureOutputPath);

        mSessionListener->onCameraHdrImageCaptureCompleted();
    }

    void CameraSession::doAttemptSaveHdrData() {
        // Check how long it has been since the capture sequence has complete
        if(mHdrCaptureSequenceCompleted) {
            double timeSinceSequenceCompleted =
                std::chrono::duration <double, std::milli>(std::chrono::steady_clock::now() - mHdrSequenceCompletedTimePoint).count();

            // Fail if we haven't gotten the images in a reasonable amount of time
            if(timeSinceSequenceCompleted > 5000) {
                mLongHdrCaptureInProgress = false;
                mHdrCaptureSequenceCompleted = false;

                mSessionListener->onCameraHdrImageCaptureFailed();

                return;
            }
        }

        // If we don't have the right number of images
        int hdrBufferCount = RawBufferManager::get().numHdrBuffers();
        if(hdrBufferCount < mRequestedHdrCaptures) {
            mSessionListener->onCameraHdrImageCaptureProgress(hdrBufferCount / (float) mRequestedHdrCaptures * 100.0f);
            return;
        }

        mSessionListener->onCameraHdrImageCaptureProgress(100);

        // Save HDR capture
        mLongHdrCaptureInProgress = false;

        LOGI("HDR capture completed. Saving data.");
        RawBufferManager::get().saveHdr(
                mRequestedHdrCaptures,
                mRequestHdrCaptureTimestamp,
                mCameraDescription->metadata,
                mHdrCaptureSettings,
                mHdrCaptureOutputPath);

        mSessionListener->onCameraHdrImageCaptureCompleted();
    }

    void CameraSession::doSetExposureCompensation(float value) {
        value = std::min(1.0f, std::max(0.0f, value));

        double range = mCameraDescription->exposureCompensationRange[1] - mCameraDescription->exposureCompensationRange[0];
        int exposureComp = static_cast<int>(std::round(value*range + mCameraDescription->exposureCompensationRange[0]));

        mCameraStateManager->requestExposureCompensation(exposureComp);
    }

    void CameraSession::doSetFrameRate(int frameRate) {
        mCameraStateManager->requestFrameRate(frameRate);
    }

    void CameraSession::doSetAWBLock(bool lock) {
        mCameraStateManager->requestAwbLock(lock);
    }

    void CameraSession::doSetAELock(bool lock) {
        mCameraStateManager->requestAELock(lock);
    }

    void CameraSession::doSetOIS(bool on) {
        mCameraStateManager->requestOis(on);
    }

    void CameraSession::doSetFocusDistance(float focusDistance) {
        mCameraStateManager->requestManualFocus(focusDistance);
    }

    void CameraSession::doSetFocusForVideo(bool focusForVideo) {
        mCameraStateManager->requestFocusForVideo(focusForVideo);
    }

    void CameraSession::doSetLensAperture(float lensAperture) {
        mCameraStateManager->requestAperture(lensAperture);
    }

    void CameraSession::doActivateCameraSettings() {
        mCameraStateManager->activate();
    }

    void CameraSession::updateOrientation(ScreenOrientation orientation) {
        mScreenOrientation = orientation;
    }

    std::shared_ptr<CaptureCallbackContext> CameraSession::createCaptureCallbacks(const CaptureEvent event) {
        std::shared_ptr<CaptureCallbackContext> context = std::make_shared<CaptureCallbackContext>();

        context->cameraSession = this;
        context->event = event;

        context->callbacks.context                      = static_cast<void*>(context.get());

        context->callbacks.onCaptureStarted             = OnCameraCaptureStarted;
        context->callbacks.onCaptureCompleted           = OnCameraCaptureCompleted;
        context->callbacks.onCaptureFailed              = OnCameraCaptureFailed;
        context->callbacks.onCaptureProgressed          = OnCameraCaptureProgressed;
        context->callbacks.onCaptureBufferLost          = OnCameraCaptureBufferLost;
        context->callbacks.onCaptureSequenceCompleted   = OnCameraCaptureSequenceCompleted;
        context->callbacks.onCaptureSequenceAborted     = OnCameraCaptureSequenceAborted;

        return context;
    }

    void CameraSession::setupCallbacks() {
        mSessionContext->deviceStateCallbacks.context                  = this;

        mSessionContext->deviceStateCallbacks.onError                  = OnCameraError;
        mSessionContext->deviceStateCallbacks.onDisconnected           = OnCameraDisconnected;

        mSessionContext->sessionStateCallbacks.context                 = this;

        mSessionContext->sessionStateCallbacks.onActive                = OnCameraSessionActive;
        mSessionContext->sessionStateCallbacks.onReady                 = OnCameraSessionReady;
        mSessionContext->sessionStateCallbacks.onClosed                = OnCameraSessionClosed;

        mSessionContext->captureCallbacks[CaptureEvent::REPEAT]        = createCaptureCallbacks(CaptureEvent::REPEAT);
        mSessionContext->captureCallbacks[CaptureEvent::HDR_CAPTURE]   = createCaptureCallbacks(CaptureEvent::HDR_CAPTURE);
    }

    void CameraSession::onCameraCaptureStarted(const CaptureCallbackContext& context, const ACaptureRequest* request, int64_t timestamp) {
    }

    void CameraSession::onCameraCaptureCompleted(const CaptureCallbackContext& context, const ACameraMetadata* metadata) {
        ACameraMetadata_const_entry metadataEntry;

        if(context.event == CaptureEvent::REPEAT) {
            mImageConsumer->queueMetadata(metadata, mScreenOrientation, RawType::ZSL);
        }
        else if(context.event == CaptureEvent::HDR_CAPTURE) {
            mImageConsumer->queueMetadata(metadata, mScreenOrientation, RawType::HDR);
        }

        // Read the ISO/shutter speed values.
        int iso = 0;
        int64_t exposure = 0;
        float focusDistance = 0;

        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_SENSITIVITY, &metadataEntry) == ACAMERA_OK) {
            iso = metadataEntry.data.i32[0];
        }

        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_EXPOSURE_TIME, &metadataEntry) == ACAMERA_OK) {
            exposure = (double) metadataEntry.data.i64[0];
        }

        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FOCUS_DISTANCE, &metadataEntry) == ACAMERA_OK) {
            focusDistance = (float) metadataEntry.data.f[0];
        }

        mLastFocusDistance = focusDistance;

        if (iso != mLastIso || exposure != mLastExposureTime) {
            json11::Json::object data = {
                    {"iso",          iso},
                    {"exposureTime", std::to_string(exposure)}
            };

            pushEvent(EventAction::EVENT_CAMERA_EXPOSURE_STATUS_CHANGED, data);

            mLastIso = iso;
            mLastExposureTime = exposure;
        }

        // Focus state
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_CONTROL_AF_STATE, &metadataEntry) == ACAMERA_OK) {
            auto afState = static_cast<acamera_metadata_enum_android_control_af_state_t>(metadataEntry.data.u8[0]);

            auto focusState = GetFocusState(afState);
            if(focusState != mLastFocusState) {
                json11::Json::object data = {
                        { "state", static_cast<int>(focusState) }
                };

                pushEvent(EventAction::EVENT_CAMERA_AUTO_FOCUS_STATE_CHANGED, data);
            }

            mLastFocusState = focusState;
        }

        // Exposure state
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_CONTROL_AE_STATE, &metadataEntry) == ACAMERA_OK) {
            auto aeState = static_cast<acamera_metadata_enum_android_control_ae_state_t>(metadataEntry.data.u8[0]);
            auto exposureState = GetExposureState(aeState);

            if(exposureState != mLastExposureState) {
                json11::Json::object data = {
                        { "state", static_cast<int>(exposureState) }
                };

                pushEvent(EventAction::EVENT_CAMERA_AUTO_EXPOSURE_STATE_CHANGED, data);
            }

            mLastExposureState = exposureState;
        }
    }

    void CameraSession::onCameraCaptureProgressed(const CaptureCallbackContext& context, const ACameraMetadata* result) {
    }

    void CameraSession::onCameraCaptureBufferLost(const CaptureCallbackContext& context, int64_t frameNumber) {
    }

    void CameraSession::onCameraCaptureFailed(const CaptureCallbackContext& context, ACameraCaptureFailure* failure) {
    }

    void CameraSession::onCameraCaptureSequenceCompleted(const CaptureCallbackContext& context, const int sequenceId) {
        if(context.event == CaptureEvent::HDR_CAPTURE) {
            LOGD("HDR capture sequence completed");
            mHdrSequenceCompletedTimePoint = std::chrono::steady_clock::now();
            mHdrCaptureSequenceCompleted = true;
        }
        else {
            json11::Json::object data = {
                { "sequenceId", sequenceId }
            };

            pushEvent(EventAction::EVENT_CAMERA_SEQUENCE_COMPLETED, data);
        }
    }

    void CameraSession::onCameraCaptureSequenceAborted(const CaptureCallbackContext& context, int sequenceId) {
        if(context.event == CaptureEvent::HDR_CAPTURE) {
            LOGD("HDR capture sequence aborted");
            mHdrSequenceCompletedTimePoint = std::chrono::steady_clock::now();
            mHdrCaptureSequenceCompleted = true;
        }
    }

    void CameraSession::onCameraError(int error) {
        LOGE("Camera has failed with error %d", error);
        json11::Json::object data = {
            { "error", error }
        };

        pushEvent(EventAction::EVENT_CAMERA_ERROR, data);
    }

    void CameraSession::onCameraDisconnected() {
        pushEvent(EventAction::EVENT_CAMERA_DISCONNECTED);
    }

    void CameraSession::onCameraSessionStateActive() {
        json11::Json::object data = {
            { "state", static_cast<int>(CameraCaptureSessionState::ACTIVE) }
        };

        pushEvent(EventAction::EVENT_CAMERA_SESSION_CHANGED, data);
    }

    void CameraSession::onCameraSessionStateReady() {
        json11::Json::object data = {
            { "state", static_cast<int>(CameraCaptureSessionState::READY) }
        };

        pushEvent(EventAction::EVENT_CAMERA_SESSION_CHANGED, data);
    }

    void CameraSession::onCameraSessionStateClosed() {
        json11::Json::object data = {
            { "state", static_cast<int>(CameraCaptureSessionState::CLOSED) }
        };

        pushEvent(EventAction::EVENT_CAMERA_SESSION_CHANGED, data);
    }

    //

    void CameraSession::onRawImageAvailable(AImageReader *imageReader) {
        AImage *image = nullptr;

        while (AImageReader_acquireNextImage(imageReader, &image) == AMEDIA_OK) {
            mImageConsumer->queueImage(image);
        }

        if(mLongHdrCaptureInProgress) {
            pushEvent(EventAction::EVENT_SAVE_HDR_DATA);
        }
    }

    //

    void CameraSession::doOnCameraError(int error) {
        LOGE("Camera has encountered an error (%d)", error);

        mSessionListener->onCameraError(error);
    }

    void CameraSession::doOnCameraDisconnected() {
        LOGI("Camera has disconnected");

        mSessionListener->onCameraDisconnected();
    }

    void CameraSession::doOnCameraSessionStateChanged(const CameraCaptureSessionState state) {
        LOGD("Camera session has changed state (%d)", state);

        mState = state;
        mCameraStateManager->onCameraSessionStateChanged(state);

        mSessionListener->onCameraStateChanged(mState);
    }

    void CameraSession::doOnCameraExposureStatusChanged(int32_t iso, int64_t exposureTime) {
        mSessionListener->onCameraExposureStatus(iso, exposureTime);
    }

    void CameraSession::doCameraAutoExposureStateChanged(CameraExposureState state) {
        mSessionListener->onCameraAutoExposureStateChanged(state);
    }

    void CameraSession::doCameraAutoFocusStateChanged(CameraFocusState state) {
        mSessionListener->onCameraAutoFocusStateChanged(state, mLastFocusDistance);
    }

    void CameraSession::doOnInternalError(const std::string& e) {
        LOGE("Internal error: %s", e.c_str());
        pushEvent(EventAction::ACTION_CLOSE_CAMERA);
    }

    //

    void CameraSession::pushEvent(const EventAction eventAction, const json11::Json& data) {
        if(!mSessionContext) {
            LOGW("Failed to queue event, event loop is gone (%d)", eventAction);
            return;
        }

        auto eventLoopData = std::make_shared<EventLoopData>(eventAction, data);

        mEventLoopQueue.enqueue(eventLoopData);
    }

    void CameraSession::pushEvent(const EventAction eventAction) {
        if(!mSessionContext) {
            LOGW("Failed to queue event, event loop is gone (%d)", eventAction);
            return;
        }

        json11::Json data;
        auto eventLoopData = std::make_shared<EventLoopData>(eventAction, data);

        mEventLoopQueue.enqueue(eventLoopData);
    }

    void CameraSession::doProcessEvent(const EventLoopDataPtr& eventLoopData) {
        switch(eventLoopData->eventAction) {
            //
            // Actions
            //

            case EventAction::ACTION_OPEN_CAMERA: {
                auto setupForRawPreview = eventLoopData->data["setupForRawPreview"].bool_value();
                auto startupSettings = eventLoopData->data["cameraStartupSettings"];

                doOpenCamera(setupForRawPreview, startupSettings);
                break;
            }

            case EventAction::ACTION_CLOSE_CAMERA: {
                doCloseCamera();
                break;
            }

            case EventAction::ACTION_PAUSE_CAPTURE: {
                doPauseCapture();
                break;
            }

            case EventAction::ACTION_RESUME_CAPTURE: {
                doResumeCapture();
                break;
            }

            case EventAction::ACTION_UPDATE_PREVIEW: {
                float shadows = static_cast<float>(eventLoopData->data["shadows"].number_value());
                float contrast = static_cast<float>(eventLoopData->data["contrast"].number_value());
                float blackPoint = static_cast<float>(eventLoopData->data["blackPoint"].number_value());
                float whitePoint = static_cast<float>(eventLoopData->data["whitePoint"].number_value());

                doUpdatePreview(shadows, contrast, blackPoint, whitePoint);
                break;
            }

            case EventAction::ACTION_SET_AUTO_EXPOSURE: {
                doSetAutoExposure();
                break;
            }

            case EventAction::ACTION_SET_EXPOSURE_COMP_VALUE: {
                float expComp = static_cast<float>(eventLoopData->data["value"].number_value());
                doSetExposureCompensation(expComp);
                break;
            }

            case EventAction::ACTION_SET_FRAME_RATE: {
                int frameRate = eventLoopData->data["value"].int_value();
                doSetFrameRate(frameRate);
                break;
            }

            case EventAction::ACTION_SET_AWB_LOCK: {
                bool value = eventLoopData->data["value"].bool_value();
                doSetAWBLock(value);
                break;
            }

            case EventAction::ACTION_SET_AE_LOCK: {
                bool value = eventLoopData->data["value"].bool_value();
                doSetAELock(value);
                break;
            }

            case EventAction::ACTION_SET_OIS: {
                bool value = eventLoopData->data["value"].bool_value();
                doSetOIS(value);
                break;
            }

            case EventAction::ACTION_SET_FOCUS_DISTANCE: {
                float value = eventLoopData->data["value"].number_value();
                doSetFocusDistance(value);
                break;
            }

            case EventAction::ACTION_SET_FOCUS_FOR_VIDEO: {
                bool value = eventLoopData->data["value"].bool_value();
                doSetFocusForVideo(value);
                break;
            }

            case EventAction::ACTION_SET_LENS_APERTURE: {
                float value = eventLoopData->data["value"].number_value();
                doSetLensAperture(value);
                break;
            }

            case EventAction::ACTION_ACTIVATE_CAMERA_SETTINGS: {
                doActivateCameraSettings();
                break;
            }

            case EventAction::ACTION_SET_MANUAL_EXPOSURE: {
                auto exposureTimeStr = eventLoopData->data["exposureTime"].string_value();
                auto exposureTime = std::stol(exposureTimeStr);

                doSetManualExposure(eventLoopData->data["iso"].int_value(), exposureTime);
                break;
            }

            case EventAction::ACTION_SET_AUTO_FOCUS: {
                doSetAutoFocus();
                break;
            }

            case EventAction::ACTION_CAPTURE_HDR: {
                int numImages = eventLoopData->data["numImages"].int_value();
                int baseIso = eventLoopData->data["baseIso"].int_value();
                int64_t baseExposure = std::stol(eventLoopData->data["baseExposure"].string_value());
                int hdrIso = eventLoopData->data["hdrIso"].int_value();
                int64_t hdrExposure = std::stol(eventLoopData->data["hdrExposure"].string_value());

                doCaptureHdr(numImages, baseIso, baseExposure, hdrIso, hdrExposure);
                break;
            }

            case EventAction::ACTION_PRECAPTURE_HDR: {
                int iso = eventLoopData->data["iso"].int_value();
                int64_t exposure = std::stol(eventLoopData->data["exposure"].string_value());

                doPrecaptureCaptureHdr(iso, exposure);
                break;
            }

            case EventAction::ACTION_SET_FOCUS_POINT: {
                doSetFocusPoint(
                        eventLoopData->data["focusX"].number_value(),
                        eventLoopData->data["focusY"].number_value(),
                        eventLoopData->data["exposureX"].number_value(),
                        eventLoopData->data["exposureY"].number_value());
                break;
            }

            //
            // Events
            //

            case EventAction::EVENT_SAVE: {
                int numImages = eventLoopData->data["numImages"].int_value();

                doSave(numImages);
                break;
            }

            case EventAction::EVENT_SAVE_HDR_DATA: {
                doAttemptSaveHdrData();
                break;
            }

            case EventAction::EVENT_CAMERA_ERROR: {
                doOnCameraError(eventLoopData->data["error"].int_value());
                break;
            }

            case EventAction::EVENT_CAMERA_DISCONNECTED: {
                doOnCameraDisconnected();
                break;
            }

            case EventAction::EVENT_CAMERA_SESSION_CHANGED: {
                doOnCameraSessionStateChanged(static_cast<CameraCaptureSessionState>(eventLoopData->data["state"].int_value()));
                break;
            }

            case EventAction::EVENT_CAMERA_EXPOSURE_STATUS_CHANGED: {
                auto iso = eventLoopData->data["iso"].int_value();
                auto exposureTimeStr = eventLoopData->data["exposureTime"].string_value();
                auto exposureTime = std::stol(exposureTimeStr);

                doOnCameraExposureStatusChanged(iso, exposureTime);
                break;
            }

            case EventAction::EVENT_CAMERA_AUTO_EXPOSURE_STATE_CHANGED: {
                doCameraAutoExposureStateChanged(static_cast<CameraExposureState>(eventLoopData->data["state"].int_value()));
                break;
            }

            case EventAction::EVENT_CAMERA_AUTO_FOCUS_STATE_CHANGED: {
                doCameraAutoFocusStateChanged(static_cast<CameraFocusState>(eventLoopData->data["state"].int_value()));
                break;
            }

            case EventAction::EVENT_CAMERA_SEQUENCE_COMPLETED: {
                mCameraStateManager->onCameraCaptureSequenceCompleted(eventLoopData->data["sequenceId"].int_value());
                break;
            }

            default:
                LOGW("Unknown event in event loop!");
                break;
        }
    }

    void CameraSession::doEventLoop() {
        bool eventLoopRunning = true;
        bool recvdStop = false;

        while(eventLoopRunning) {
            EventLoopDataPtr eventLoopData;

            // Try to get event
            if(!mEventLoopQueue.wait_dequeue_timed(eventLoopData, std::chrono::milliseconds(100))) {
                continue;
            }

            if(eventLoopData->eventAction == EventAction::STOP) {
                eventLoopRunning = false;
            }
            else {
                try {
                    if(mAcceptEvents)
                        doProcessEvent(eventLoopData);
                }
                catch (std::exception &e) {
                    doOnInternalError(e.what());
                }
            }
        }

        LOGD("Event loop has stopped");
    }
 }
