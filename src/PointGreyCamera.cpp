#include "PointGreyCamera.h"
#include "PointGreyInterface.h"

using namespace lima;
using namespace lima::PointGrey;
using namespace std;

//---------------------------
//- _AcqThread class
//---------------------------
class Camera::_AcqThread : public Thread
{
    DEB_CLASS_NAMESPC(DebModCamera, "Camera", "_AcqThread");
public:
    _AcqThread(Camera &aCam);
    virtual ~_AcqThread();
protected:
    virtual void threadFunction();
private:
    Camera &m_cam;
};

//---------------------------
//
//---------------------------
Camera::Camera(const std::string& camera_ip, const int camera_serial_no)
    : m_nb_frames(1)
    , m_status(Ready)
    , m_quit(false)
    , m_acq_started(false)
    , m_thread_running(true)
    , m_image_number(0)
    , m_video(NULL)
    , m_camera(NULL)
    // camera properties
    , m_frame_rate_property(FlyCapture2::FRAME_RATE)
    , m_exp_time_property(FlyCapture2::SHUTTER)
    , m_gain_property(FlyCapture2::GAIN)
    // property info
    , m_frame_rate_property_info(FlyCapture2::FRAME_RATE)
    , m_exp_time_property_info(FlyCapture2::SHUTTER)
    , m_gain_property_info(FlyCapture2::GAIN)
{
    DEB_CONSTRUCTOR();

    FlyCapture2::BusManager busmgr;
    FlyCapture2::PGRGuid pgrguid;

    unsigned int nb_cameras;
    m_camera = new FlyCapture2::Camera();

    m_error = busmgr.GetNumOfCameras(&nb_cameras);
    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Failed to create bus manager: " << m_error.GetDescription();

    if (nb_cameras < 1)
    	THROW_HW_ERROR(Error) << "No cameras found";

    if (camera_serial_no)
    {
    	m_error = busmgr.GetCameraFromSerialNumber(camera_serial_no, &pgrguid);
    	if (m_error != FlyCapture2::PGRERROR_OK)
	    THROW_HW_ERROR(Error) << "Camera not found: " << m_error.GetDescription();
    }
    else
        // TODO: lookup via ip address
    	THROW_HW_ERROR(Error) << "Camera lookup via IP address has not been implemented";

    m_error = m_camera->Connect(&pgrguid);
    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Failed to connect to camera: " << m_error.GetDescription();

    m_error = m_camera->GetCameraInfo(&m_camera_info);
    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Failed to get camera info: "<<  m_error.GetDescription();

    bool fmt7_supported;
    m_fmt7_info.mode = FlyCapture2::MODE_0;
    m_error = m_camera->GetFormat7Info( &m_fmt7_info, &fmt7_supported );

    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Failed to get Format7 info: "<<  m_error.GetDescription();

    if (!fmt7_supported)
    	THROW_HW_ERROR(Error) << "Format7 is not supported: ";

    // Set image format settings
    m_fmt7_image_settings.mode = m_fmt7_info.mode;
    m_fmt7_image_settings.offsetX = 0;
    m_fmt7_image_settings.offsetY = 0;
    m_fmt7_image_settings.width = m_fmt7_info.maxWidth;
    m_fmt7_image_settings.height = m_fmt7_info.maxHeight;
    m_fmt7_image_settings.pixelFormat = FlyCapture2::PIXEL_FORMAT_MONO8;

    bool valid;

    m_error = m_camera->ValidateFormat7Settings(&m_fmt7_image_settings,
						&valid,
						&m_fmt7_packet_info);
    if ( m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to validate image format settings:" << m_error.GetDescription();
    if ( !valid )
    	THROW_HW_ERROR(Error) << "Unsupported image format settings";

    m_error = m_camera->SetFormat7Configuration(&m_fmt7_image_settings,
						m_fmt7_packet_info.recommendedBytesPerPacket);

    if ( m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to apply image format settings:" << m_error.GetDescription();

    // Force the camera to PGR's Y16 endianness
    _forcePGRY16Mode();

    // get frame rate property
    _getPropertyInfo(&m_frame_rate_property_info);
    _getProperty(&m_frame_rate_property);

    // set auto frame rate, if supported
    // control off means that the current frame rate will not limit
    // the range of valid values for the exposure time property. the
    // frame rate will be adjusted to allow the requested exposure time.
    m_frame_rate_property.onOff = false;
    m_frame_rate_property.autoManualMode = m_frame_rate_property_info.autoSupported;

    _setProperty(&m_frame_rate_property);

    // get exp time property
    _getPropertyInfo(&m_exp_time_property_info);
    _getProperty(&m_exp_time_property);

    // set auto exp time, if supported
    m_exp_time_property.onOff = true;
    m_exp_time_property.absControl = true;
    m_exp_time_property.autoManualMode = m_exp_time_property_info.autoSupported;

    _setProperty(&m_exp_time_property);

    // Get gain property
    _getPropertyInfo(&m_gain_property_info);
    _getProperty(&m_gain_property);

    // set auto gain, if supported
    m_gain_property.onOff = true;
    m_gain_property.absControl = true;
    m_gain_property.autoManualMode = m_gain_property_info.autoSupported;

    _setProperty(&m_gain_property);

    //Acquisition  Thread
    m_acq_thread = new _AcqThread(*this);
    m_acq_thread->start();
}

//---------------------------
//
//---------------------------
Camera::~Camera()
{
    DEB_DESTRUCTOR();
    delete m_acq_thread;
    m_camera->Disconnect();
    delete m_camera;
}

void Camera::prepareAcq()
{
    DEB_MEMBER_FUNCT();
    m_image_number = 0;
}

//---------------------------
//
//---------------------------
void Camera::startAcq()
{
    DEB_MEMBER_FUNCT();

    DEB_TRACE() << "Start acquisition";
    m_video->getBuffer().setStartTimestamp(Timestamp::now());

    m_error = m_camera->StartCapture();
    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to start image capture" << m_error.GetDescription();

    // Start acquisition thread
    AutoMutex lock(m_cond.mutex());
    m_acq_started = true;
    m_cond.broadcast();
}

//---------------------------
//
//---------------------------
void Camera::stopAcq()
{
    DEB_MEMBER_FUNCT();
    _stopAcq(false);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getDetectorImageSize(Size& size)
{
    DEB_MEMBER_FUNCT();
    size = Size(m_fmt7_info.maxWidth, m_fmt7_info.maxHeight);
    DEB_RETURN() << DEB_VAR1(size);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getDetectorType(string& type)
{
    DEB_MEMBER_FUNCT();
    type = m_camera_info.vendorName;;
    DEB_RETURN() << DEB_VAR1(type);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getDetectorModel(string& model)
{
    DEB_MEMBER_FUNCT();
    model = m_camera_info.modelName;
    DEB_RETURN() << DEB_VAR1(model);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getTrigMode(TrigMode& mode)
{
    DEB_MEMBER_FUNCT();

    cout << "getTrigMode" << endl;

    // Get current trigger settings
    FlyCapture2::TriggerMode triggerMode;
    m_error = m_camera->GetTriggerMode( &triggerMode );
    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to get trigger mode settings" << m_error.GetDescription();

    mode = triggerMode.onOff ? ExtTrigSingle : IntTrig;

    DEB_RETURN() << DEB_VAR1(mode);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setTrigMode(TrigMode mode)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(mode);

    // Check for external trigger support
    FlyCapture2::TriggerModeInfo triggerModeInfo;
    m_error = m_camera->GetTriggerModeInfo( &triggerModeInfo );
    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to get trigger mode info from camera" << m_error.GetDescription();

    if ( not triggerModeInfo.present)
    {
    	DEB_ERROR() << "Camera does not support external trigger";
        return;
    }

    // Get current trigger settings
    FlyCapture2::TriggerMode triggerMode;
    m_error = m_camera->GetTriggerMode( &triggerMode );
    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to get trigger mode settings" << m_error.GetDescription();

    switch(mode)
    {
    case IntTrig:
    	triggerMode.onOff = false;
    	break;
    case ExtTrigSingle:
    	triggerMode.onOff = true;
    	triggerMode.mode = 0;
    	triggerMode.source = 0;
    	break;
    default:
    	THROW_HW_ERROR(Error) << "Trigger mode " << mode << " is not supported";
    }

    m_error = m_camera->SetTriggerMode( &triggerMode );
    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to set trigger mode settings" << m_error.GetDescription();
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getExpTime(double& exp_time)
{
    DEB_MEMBER_FUNCT();

    if (m_exp_time_property.autoManualMode)
        // value might have changed
        _getProperty(&m_exp_time_property);

    exp_time = 1.0E-3 * m_exp_time_property.absValue;
    DEB_RETURN() << DEB_VAR1(exp_time);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setExpTime(double exp_time)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(exp_time);

    m_exp_time_property.autoManualMode = false;
    m_exp_time_property.absValue = 1.0E3 * exp_time;

    _setProperty(&m_exp_time_property);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getAutoExpTime(bool &auto_exp_time) const
{
    DEB_MEMBER_FUNCT();

    auto_exp_time = m_exp_time_property.autoManualMode;

    DEB_RETURN() << DEB_VAR1(auto_exp_time);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setAutoExpTime(bool auto_exp_time)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(auto_exp_time);

    if (!m_exp_time_property_info.autoSupported )
    {
        DEB_WARNING() << "Auto exposure is not supported";
        return;
    }

    m_exp_time_property.autoManualMode = auto_exp_time;

    // do a "one push" adjustment, if supported
    if (m_exp_time_property_info.onePushSupported)
    	m_exp_time_property.onePush = auto_exp_time;

    _setProperty(&m_exp_time_property);

    m_exp_time_property.onePush = false;
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getLatTime(double& lat_time)
{
    // latency is not managed
    DEB_MEMBER_FUNCT();
    lat_time = 0;
    DEB_RETURN() << DEB_VAR1(lat_time);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setLatTime(double lat_time)
{
    // latency is not managed
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(lat_time);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getExpTimeRange(double& min_exp_time, double& max_exp_time) const
{
    DEB_MEMBER_FUNCT();
    min_exp_time = 1E-3 * m_exp_time_property_info.absMin;
    max_exp_time = 1E-3 * m_exp_time_property_info.absMax;
    DEB_RETURN() << DEB_VAR2(min_exp_time, max_exp_time);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getLatTimeRange(double& min_lat_time, double& max_lat_time) const
{
    DEB_MEMBER_FUNCT();
    min_lat_time = 0;
    max_lat_time = 0;
    DEB_RETURN() << DEB_VAR2(min_lat_time, max_lat_time);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getNbFrames(int& nb_frames)
{
    DEB_MEMBER_FUNCT();
    nb_frames = m_nb_frames;
    DEB_RETURN() << DEB_VAR1(nb_frames);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setNbFrames(int nb_frames)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(nb_frames);
    m_nb_frames = nb_frames;
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getNbHwAcquiredFrames(int &nb_acq_frames)
{ 
    DEB_MEMBER_FUNCT();
    nb_acq_frames = m_image_number;
}
  
//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getStatus(Camera::Status& status)
{
    DEB_MEMBER_FUNCT();
    AutoMutex lock(m_cond.mutex());
    status = m_status;
    DEB_RETURN() << DEB_VAR1(DEB_HEX(status));
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::checkRoi(const Roi& set_roi, Roi& hw_roi)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(set_roi);
    DEB_RETURN() << DEB_VAR1(hw_roi);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getRoi(Roi& hw_roi)
{
    DEB_MEMBER_FUNCT();
    DEB_RETURN() << DEB_VAR1(hw_roi);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setRoi(const Roi& ask_roi)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(ask_roi);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::checkBin(Bin &aBin)
{
    DEB_MEMBER_FUNCT();
    DEB_RETURN() << DEB_VAR1(aBin);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getBin(Bin &aBin)
{
    DEB_MEMBER_FUNCT();
    DEB_RETURN() << DEB_VAR1(aBin);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setBin(const Bin &aBin)
{
    DEB_MEMBER_FUNCT();
    DEB_RETURN() << DEB_VAR1(aBin);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getVideoMode(VideoMode& mode) const
{
    DEB_MEMBER_FUNCT();
    switch(m_fmt7_image_settings.pixelFormat)
    {
    case FlyCapture2::PIXEL_FORMAT_MONO8:
    	mode = Y8;
    	break;
    case FlyCapture2::PIXEL_FORMAT_MONO16:
    	mode = Y16;
    	break;
    default:
    	THROW_HW_ERROR(Error) << "Unable to determine the video mode";
    }
    DEB_RETURN() << DEB_VAR1(mode);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setVideoMode(VideoMode mode)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(mode);

    FlyCapture2::Format7ImageSettings fmt7_image_settings(m_fmt7_image_settings);
    FlyCapture2::Format7PacketInfo fmt7_packet_info;
    ImageType image_type;
    bool valid;

    switch(mode)
    {
    case Y8:
    	fmt7_image_settings.pixelFormat = FlyCapture2::PIXEL_FORMAT_MONO8;
    	image_type = Bpp8;
    	break;
    case Y16:
    	fmt7_image_settings.pixelFormat = FlyCapture2::PIXEL_FORMAT_MONO16;
    	image_type = Bpp16;
    	break;
    default:
    	THROW_HW_ERROR(Error) << "Unsupported video mode";
    }

    if (fmt7_image_settings.pixelFormat == m_fmt7_image_settings.pixelFormat)
    	// no changes
    	return;

    AutoMutex lock(m_cond.mutex());
    bool pause_acq = m_acq_started;
    lock.unlock();

    if (pause_acq)
    	stopAcq();

    m_error = m_camera->ValidateFormat7Settings(&fmt7_image_settings,
						&valid,
						&fmt7_packet_info);

    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to validate image format settings:" << m_error.GetDescription();

    if (!valid)
    	THROW_HW_ERROR(Error) << "Unsupported image format settings";

    m_error = m_camera->SetFormat7Configuration(&fmt7_image_settings,
						fmt7_packet_info.recommendedBytesPerPacket);

    if (m_error != FlyCapture2::PGRERROR_OK)
    	THROW_HW_ERROR(Error) << "Unable to apply image format settings:" << m_error.GetDescription();

    if (pause_acq)
    	startAcq();

    m_fmt7_image_settings = fmt7_image_settings;

    maxImageSizeChanged(Size(m_fmt7_info.maxWidth, m_fmt7_info.maxHeight), image_type);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getGain(double& gain)
{
    DEB_MEMBER_FUNCT();

    if (m_gain_property.autoManualMode)
        // value might have changed
        _getProperty(&m_gain_property);

    gain = m_gain_property.absValue / m_gain_property_info.absMax;

    DEB_RETURN() << DEB_VAR1(gain);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setGain(double gain)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(gain);

    m_gain_property.autoManualMode = false;
    m_gain_property.absValue = gain * m_gain_property_info.absMax;

    _setProperty(&m_gain_property);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::getAutoGain(bool& auto_gain) const
{
    DEB_MEMBER_FUNCT();
    auto_gain = m_gain_property.autoManualMode;
    DEB_RETURN() << DEB_VAR1(auto_gain);
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::setAutoGain(bool auto_gain)
{
    DEB_MEMBER_FUNCT();
    DEB_PARAM() << DEB_VAR1(auto_gain);

    if (!m_gain_property_info.autoSupported)
    {
    	DEB_WARNING() << "Auto gain is not supported";
    	return;
    }

    m_gain_property.autoManualMode = auto_gain;

    // do a "one push" adjustment, if supported
    if (m_gain_property_info.onePushSupported)
        m_gain_property.onePush = auto_gain;

    _setProperty(&m_gain_property);

    m_gain_property.onePush = false;
}

void Camera::_getPropertyInfo(FlyCapture2::PropertyInfo *property_info)
{
    DEB_MEMBER_FUNCT();
    m_error = m_camera->GetPropertyInfo(property_info);
    if (m_error != FlyCapture2::PGRERROR_OK)
        THROW_HW_ERROR(Error) << "Failed to get camera property info: " << m_error.GetDescription();
}

void Camera::_getProperty(FlyCapture2::Property *property)
{
    DEB_MEMBER_FUNCT();
    m_error = m_camera->GetProperty(property);
    if (m_error != FlyCapture2::PGRERROR_OK)
        THROW_HW_ERROR(Error) << "Failed to get camera property: " << m_error.GetDescription();
}

void Camera::_setProperty(FlyCapture2::Property *property)
{
    DEB_MEMBER_FUNCT();
    m_error = m_camera->SetProperty(property);
    if (m_error != FlyCapture2::PGRERROR_OK)
        THROW_HW_ERROR(Error) << "Failed to set camera property: " << m_error.GetDescription();
}

//-----------------------------------------------------
//
//-----------------------------------------------------
void Camera::_setStatus(Camera::Status status,bool force)
{
    DEB_MEMBER_FUNCT();
    AutoMutex alock(m_cond.mutex());
    if(force || m_status != Camera::Fault)
        m_status = status;
}

//---------------------------
//- Camera::_stopAcq()
//---------------------------
void Camera::_stopAcq(bool internalFlag)
{
    DEB_MEMBER_FUNCT();
    AutoMutex lock(m_cond.mutex());

    if(m_status == Camera::Ready)
        return;

    m_acq_started = false;

	// wait for the acq thread to finish
    while(!internalFlag && m_thread_running)
        m_cond.wait();

    lock.unlock();

    //Let the acq thread stop the acquisition
    if(!internalFlag)
        return;

    DEB_TRACE() << "Stop acquisition";
    m_error = m_camera->StopCapture();
    if (m_error != FlyCapture2::PGRERROR_OK)
        THROW_HW_ERROR(Error) << "Unable to stop image capture: " << m_error.GetDescription();

    _setStatus(Camera::Ready, false);
}

void Camera::_forcePGRY16Mode()
{
    DEB_MEMBER_FUNCT();
    const unsigned int k_imageDataFmtReg = 0x1048;
    unsigned int value = 0;
    m_error = m_camera->ReadRegister(k_imageDataFmtReg, &value);
    if (m_error != FlyCapture2::PGRERROR_OK)
    {
        THROW_HW_ERROR(Error) << "Failed to read camera register: " << m_error.GetDescription();
    }

    value &= ~0x1;

    m_error = m_camera->WriteRegister(k_imageDataFmtReg, value);
    if (m_error != FlyCapture2::PGRERROR_OK)
    {
        THROW_HW_ERROR(Error) << "Failed to write camera register: " << m_error.GetDescription();
    }
}

//---------------------------
//	Acquisition thread
//---------------------------
Camera::_AcqThread::_AcqThread(Camera &cam) : m_cam(cam)
{
    pthread_attr_setscope(&m_thread_attr,PTHREAD_SCOPE_PROCESS);
}

Camera::_AcqThread::~_AcqThread()
{
    AutoMutex lock(m_cam.m_cond.mutex());
    m_cam.m_quit = true;
    m_cam.m_cond.broadcast();
    lock.unlock();

    join();
}

void Camera::_AcqThread::threadFunction()
{
    DEB_MEMBER_FUNCT();
    FlyCapture2::Error error;
    FlyCapture2::Image image;
    VideoMode mode;

    sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &param))
    {
    	DEB_ERROR() << "Could not set FIFO scheduling for acquisition thread";
    }

    AutoMutex lock(m_cam.m_cond.mutex());

    while(true)
    {
        while(!m_cam.m_acq_started && !m_cam.m_quit)
        {
		    DEB_TRACE() << "wait";
		    m_cam.m_thread_running = false;
		    m_cam.m_cond.broadcast();
		    m_cam.m_cond.wait();
        }
        if (m_cam.m_quit) return;

        m_cam.m_thread_running = true;
        m_cam.m_status = Camera::Exposure;
        lock.unlock();

        DEB_TRACE() << "Run";
        bool continue_acq = true;

        continue_acq = true;

        while(continue_acq && (!m_cam.m_nb_frames || m_cam.m_image_number < m_cam.m_nb_frames))
        {
            error = m_cam.m_camera->RetrieveBuffer(&image);
            if (error == FlyCapture2::PGRERROR_OK)
            {
                // check if acquisition has been stopped
        	    if (!m_cam.m_acq_started)
        	        break;

                // Grabbing was successful, process image
                m_cam._setStatus(Camera::Readout, false);

                DEB_TRACE() << "image# " << m_cam.m_image_number << " acquired";

                m_cam.m_video->getVideoMode(mode);

                continue_acq = m_cam.m_video->callNewImage((char *)image.GetData(), image.GetCols(), image.GetRows(), mode);
                m_cam.m_image_number++;
            }
            else if (error == FlyCapture2::PGRERROR_IMAGE_CONSISTENCY_ERROR)
            {
                DEB_WARNING() << "No image acquired: " << error.GetDescription();
            }
            else
            {
                DEB_ERROR() << "No image acquired: " << error.GetDescription();
                m_cam._setStatus(Camera::Fault, false);
                continue_acq = false;
            }
        }
        m_cam._stopAcq(true);
        lock.lock();
    }
}

