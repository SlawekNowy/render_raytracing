#define UIMG_ENABLE_NVTT
#include <util_raytracing/object.hpp>
#include <util_raytracing/mesh.hpp>
#include <util_raytracing/camera.hpp>
#include <util_raytracing.hpp>
#include <util_raytracing/scene.hpp>
#include <util_raytracing/renderer.hpp>
#include <util_raytracing/shader.hpp>
#include <util_image.hpp>
#include <util_image_buffer.hpp>
#include <util_texture_info.hpp>
#include <sharedutils/util_command_manager.hpp>
#include <sharedutils/util.h>
#include <sharedutils/util_file.h>
#undef __UTIL_STRING_H__
#include <sharedutils/util_string.h>
#include <sharedutils/datastream.h>
#include <sharedutils/util_parallel_job.hpp>
#include <sharedutils/util_path.hpp>
#include <fsys/filesystem.h>
#include <util_ocio.hpp>
#include <sstream>
#include <queue>
#include <cstdlib>

#pragma optimize("",off)
class RTJobManager
{
public:
	enum class ToneMapping : uint8_t
	{
		None = 0u, // Image will be saved with original HDR colors
		FilmicBlender
	};
	struct DeviceInfo
	{
		DeviceInfo(unirender::Scene::DeviceType deviceType)
			: deviceType{deviceType}
		{}
		unirender::Scene::DeviceType deviceType {};
		std::optional<util::ParallelJob<std::shared_ptr<uimg::ImageBuffer>>> job {};
		std::shared_ptr<unirender::Renderer> renderer = nullptr;
		std::shared_ptr<unirender::Scene> rtScene = nullptr;
		std::chrono::high_resolution_clock::time_point startTime {};
		util::Path outputPath {};
	};
	static std::shared_ptr<RTJobManager> Launch(int argc,char *argv[]);
	RTJobManager(const RTJobManager&)=delete;
	RTJobManager(RTJobManager&&)=delete;
	RTJobManager &operator=(const RTJobManager&)=delete;
	RTJobManager &operator=(RTJobManager&&)=delete;
	~RTJobManager();

	void SetExposure(float exposure);
	void SetGamma(float gamma);

	bool StartNextJob();
	bool IsComplete() const;
	uint32_t GetNumSucceeded() const {return m_numSucceeded;}
	uint32_t GetNumFailed() const {return m_numFailed;}
	uint32_t GetNumSkipped() const {return m_numSkipped;}
	void Update();
private:
	RTJobManager(std::unordered_map<std::string,std::string> &&launchParams,const std::string &inputFileName);
	void UpdateJob(DeviceInfo &devInfo);
	void PrintHeader(const unirender::Scene::CreateInfo &createInfo,const unirender::Scene::SceneInfo &sceneInfo);
	void PrintHelp();
	bool StartJob(const std::string &job,DeviceInfo &devInfo);
	void CollectJobs();

	std::chrono::high_resolution_clock::time_point m_startTime {};
	std::unordered_map<std::string,std::string> m_launchParams {};
	std::vector<DeviceInfo> m_devices {};
	std::queue<std::string> m_jobQueue {};
	uint32_t m_numJobs = 0;

	unirender::Scene::RenderMode m_renderMode;
	float m_exposure = 0.f;
	float m_gamma = 2.2f;
	std::string m_inputFileName;
	uint32_t m_numSucceeded = 0;
	uint32_t m_numFailed = 0;
	uint32_t m_numSkipped = 0;
	ToneMapping m_toneMapping = ToneMapping::FilmicBlender;
};

std::shared_ptr<RTJobManager> RTJobManager::Launch(int argc,char *argv[])
{
	// Remove the exe-path
	--argc;
	++argv;

	auto launchParams = util::get_launch_parameters(argc,argv);
	auto itJob = launchParams.find("-job");
	return std::shared_ptr<RTJobManager>{new RTJobManager{std::move(launchParams),(itJob != launchParams.end()) ? itJob->second : ""}};
}

RTJobManager::RTJobManager(std::unordered_map<std::string,std::string> &&launchParams,const std::string &inputFileName)
	: m_launchParams{std::move(launchParams)},m_inputFileName{inputFileName}
{
	/*auto kernelPath = util::Path::CreatePath(util::get_program_path());
	kernelPath += "modules/cycles";
	auto itKernelPath = m_launchParams.find("-kernel_path");
	if(itKernelPath != m_launchParams.end())
		kernelPath = itKernelPath->second;
	unirender::Scene::SetKernelPath(kernelPath.GetString());*/
	unirender::set_module_lookup_location("modules/unirender/");

	auto itVerbose = m_launchParams.find("-verbose");
	unirender::Scene::SetVerbose(itVerbose != m_launchParams.end());

	auto itExposure = m_launchParams.find("-exposure");
	if(itExposure != m_launchParams.end())
		SetExposure(util::to_float(itExposure->second));

	auto itGamma = m_launchParams.find("-gamma");
	if(itGamma != m_launchParams.end())
		SetGamma(util::to_float(itGamma->second));

	auto itLog = m_launchParams.find("-log");
	if(itLog != m_launchParams.end())
	{
		unirender::set_log_handler([](std::string msg) {
			std::cout<<"Unirender: "<<msg<<std::endl;
		});
	}

#if 0
	auto itToneMapping = m_launchParams.find("-tone_mapping");
	if(itToneMapping != m_launchParams.end())
	{
		auto &toneMapping = itToneMapping->second;
		if(ustring::compare(toneMapping,"none",false))
			m_toneMapping = ToneMapping::None;
	}
#endif

	auto itDeviceType = m_launchParams.find("-device_type");
	if(itDeviceType != m_launchParams.end())
	{
		auto &strDeviceType = itDeviceType->second;
		if(ustring::compare(strDeviceType,"cpu",false))
			m_devices.push_back(unirender::Scene::DeviceType::CPU);
		else if(ustring::compare(strDeviceType,"gpu",false))
			m_devices.push_back(unirender::Scene::DeviceType::GPU);
		else if(ustring::compare(strDeviceType,"combined",false))
		{
			m_devices.push_back(unirender::Scene::DeviceType::GPU);
			m_devices.push_back(unirender::Scene::DeviceType::CPU);
		}
	}
	if(m_devices.empty())
		m_devices.push_back(unirender::Scene::DeviceType::GPU);
	for(auto &devInfo : m_devices)
	{
		std::cout<<"Using device: ";
		switch(devInfo.deviceType)
		{
		case unirender::Scene::DeviceType::GPU:
			std::cout<<"GPU";
			break;
		case unirender::Scene::DeviceType::CPU:
			std::cout<<"CPU";
			break;
		}
		std::cout<<std::endl;
	}

	util::minimize_window_to_tray();
	util::CommandManager::StartAsync();

	if(m_launchParams.find("-help") != m_launchParams.end())
	{
		PrintHelp();
		return;
	}

	util::CommandManager::RegisterCommand("pause",[this](std::vector<std::string> args) {
		uint32_t numPaused = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices)
		{
			if(dev.renderer == nullptr)
				continue;
			if(dev.renderer->Pause())
				++numPaused;
			else
				++numFailed;
		}
		std::cout<<"Paused "<<numPaused<<" render processess!"<<std::endl;
		if(numFailed > 0)
			std::cout<<"Failed to pause "<<numFailed<<" render processes!"<<std::endl;
	});
	util::CommandManager::RegisterCommand("resume",[this](std::vector<std::string> args) {
		uint32_t numResumed = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices)
		{
			if(dev.renderer == nullptr)
				continue;
			if(dev.renderer->Resume())
				++numResumed;
			else
				++numFailed;
		}
		std::cout<<"Resumed "<<numResumed<<" render processess!"<<std::endl;
		if(numFailed > 0)
			std::cout<<"Failed to resume "<<numFailed<<" render processes!"<<std::endl;
	});
	util::CommandManager::RegisterCommand("stop",[this](std::vector<std::string> args) {
		uint32_t numStopped = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices)
		{
			if(dev.renderer == nullptr)
				continue;
			if(dev.renderer->Stop())
				++numStopped;
			else
				++numFailed;
		}
		std::cout<<"Stopped "<<numStopped<<" render processess!"<<std::endl;
		if(numFailed > 0)
			std::cout<<"Failed to stop "<<numFailed<<" render processes!"<<std::endl;
	});
	util::CommandManager::RegisterCommand("preview",[this](std::vector<std::string> args) {
		uint32_t numStopped = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices)
		{
			if(dev.renderer == nullptr)
				continue;
			std::string err;
			auto filePath = dev.renderer->SaveRenderPreview("temp/",err);
			if(filePath.has_value())
				util::open_file_in_default_program(*filePath);
			else
				std::cout<<"Unable to save preview image: "<<err<<std::endl;
		}
	});
	util::CommandManager::RegisterCommand("suspend",[this](std::vector<std::string> args) {
		uint32_t numSuspended = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices)
		{
			if(dev.renderer == nullptr)
				continue;
			if(dev.renderer->Suspend())
				++numSuspended;
			else
				++numFailed;
		}
		std::cout<<"Suspended "<<numSuspended<<" render processess!"<<std::endl;
		if(numFailed > 0)
			std::cout<<"Failed to suspend "<<numFailed<<" render processes!"<<std::endl;
	});
	util::CommandManager::RegisterCommand("export",[this](std::vector<std::string> args) {
		uint32_t numExported = 0;
		uint32_t numFailed = 0;
		uint32_t devIdx = 0;
		for(auto &dev : m_devices)
		{
			if(dev.renderer != nullptr)
			{
				if(dev.renderer->Export("render/export/"))
					++numExported;
				else
					++numFailed;
			}
			++devIdx;
		}
		std::cout<<"Exported "<<numExported<<" render processess!"<<std::endl;
		if(numFailed > 0)
			std::cout<<"Failed to export "<<numFailed<<" render processes!"<<std::endl;
	});

	CollectJobs();

	std::cout<<"Executing "<<m_jobQueue.size()<<" jobs..."<<std::endl;
	m_startTime = std::chrono::high_resolution_clock::now();
}

RTJobManager::~RTJobManager()
{
	util::CommandManager::Join();
	m_devices.clear();
	unirender::Renderer::Close();
}

bool RTJobManager::IsComplete() const
{
	if(m_jobQueue.empty() == false)
		return false;
	auto itDev = std::find_if(m_devices.begin(),m_devices.end(),[](const DeviceInfo &devInfo) {
		return devInfo.job.has_value();
	});
	return itDev == m_devices.end();
}

void RTJobManager::CollectJobs()
{
	std::vector<std::string> lines {};

	std::vector<std::string> jobs {};
	for(auto &param : m_launchParams)
	{
		if(param.first.empty() || param.first.front() == '-')
			continue;
		jobs.push_back(param.first);
	}

	if(m_inputFileName.empty() == false)
	{
		std::string ext;
		ufile::get_extension(m_inputFileName,&ext);
		if(ustring::compare(ext,"txt",false))
		{
			auto f = FileManager::OpenSystemFile(m_inputFileName.c_str(),"r");
			if(f == nullptr)
				return;
			auto contents = f->ReadString();
			ustring::explode(contents,"\n",lines);
		}
		else
			lines.push_back(m_inputFileName);

		//for(auto &l : lines)
		{
			std::vector<std::string> ljobs = std::move(lines);
			// FileManager::FindSystemFiles(l.c_str(),&ljobs,nullptr);

			// Sort by name
			std::sort(ljobs.begin(),ljobs.end());

			// Test
			// ljobs = {};
			// for(uint32_t i=0;i<100;++i)
			// 	ljobs.push_back("frame00" +std::to_string(i));

			// In many cases it can be useful to render a few random samples of the animation
			// before rendering it in its entirety.
			// For this reason we'll put the frames in such an order that we'll render a handful
			// of equidistant frames of the animation first (so the animator can make sure everything is in order),
			// and then render the rest of them sequentially.
			std::vector<std::string> orderedJobs {};
			orderedJobs.reserve(ljobs.size());
			if(ljobs.size() > 1)
			{
				auto start = 0u;
				auto end = ljobs.size();
				for(uint32_t i=2;i<=32;i*=2)
				{
					if(ljobs.size() < i)
						break;
					auto pos = (end -start) /i;
					while(start +pos < end)
					{
						if(ljobs.at(start +pos).empty() == false)
							orderedJobs.emplace_back(std::move(ljobs.at(start +pos)));
						pos += (end -start) /i;
					}
				}
			}

			for(auto &ljob : ljobs)
			{
				if(ljob.empty())
					continue;
				orderedJobs.emplace_back(std::move(ljob));
			}

			// Jobs are sorted by input string and THEN by name
			jobs.reserve(jobs.size() +orderedJobs.size());
			auto path = ufile::get_path_from_filename(m_inputFileName);
			for(auto &job : orderedJobs)
				jobs.push_back(path +job);
		}
	}

	for(auto &job : jobs)
		m_jobQueue.push(job);
	m_numJobs = m_jobQueue.size();

	if(m_jobQueue.empty())
	{
		std::cout<<"No jobs specified! ";
		PrintHelp();
		std::this_thread::sleep_for(std::chrono::seconds{5});
	}
}

void RTJobManager::Update()
{
	util::CommandManager::PollEvents();
	if(util::CommandManager::ShouldExit())
	{
		while(m_jobQueue.empty() == false)
			m_jobQueue.pop();
	}
	StartNextJob();
	auto allBusy = true;
	for(auto &devInfo : m_devices)
	{
		UpdateJob(devInfo);
		if(devInfo.job.has_value() == false)
			allBusy = false;
	}
	if(allBusy)
		std::this_thread::sleep_for(std::chrono::seconds{5});
}

void RTJobManager::UpdateJob(DeviceInfo &devInfo)
{
	if(devInfo.job.has_value() == false)
		return;
	auto &job = *devInfo.job;
	if(job.IsComplete() == false)
	{
		if(util::CommandManager::ShouldExit())
			job.Cancel();
		else
		{
			auto progress = job.GetProgress();
			auto tDelta = std::chrono::high_resolution_clock::now() -devInfo.startTime;
			double tDeltaD = tDelta.count() /static_cast<double>(progress) *static_cast<double>(1.f -progress);
			auto strTime = util::get_pretty_duration(tDeltaD /1'000'000.0);
			std::cout<<"Progress for job '"<<ufile::get_file_from_filename(devInfo.outputPath.GetString())<<"': "<<util::round_string(progress *100.f,2)<<" %";
			if(progress > 0.f)
				std::cout<<" Time remaining: "<<strTime<<".";

			auto numCompleted = m_numSucceeded +m_numFailed +m_numSkipped;
			auto totalProgress = (numCompleted +progress) /static_cast<float>(m_numJobs);
			std::cout<<" Total progress: "<<util::round_string(totalProgress *100.f,2.f)<<"%";

			auto tDeltaAll = std::chrono::high_resolution_clock::now() -m_startTime;
			auto tDeltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(tDeltaAll);
			auto timePassed = util::get_pretty_duration(tDeltaMs.count());
			std::cout<<" Total time passed: "<<timePassed;

			auto numComplete = m_numSucceeded +progress;
			auto numLeft = m_numJobs -m_numFailed -m_numSkipped -numComplete;
			auto tRemainingMs = (tDeltaMs /numComplete) *numLeft;
			auto timeRemaining = util::get_pretty_duration(tRemainingMs.count());
			std::cout<<" Total time remaining: "<<timeRemaining<<std::endl;

			std::cout<<std::endl;
		}
		return;
	}

	if(job.IsCancelled())
		std::cout<<"Job has been cancelled!"<<std::endl;
	else if(job.IsSuccessful() == false)
		std::cout<<"Job has failed!"<<std::endl;
	else
	{
		std::cout<<"Job has been completed successfully!"<<std::endl;
		auto imgBuf = job.GetResult();
		
		std::optional<std::string> errMsg {};
		if(m_renderMode == unirender::Scene::RenderMode::BakeDiffuseLighting)
		{
			auto path = devInfo.outputPath;
			path.RemoveFileExtension();
			path += ".dds";
			uimg::TextureInfo texInfo {};
			texInfo.containerFormat = uimg::TextureInfo::ContainerFormat::DDS;
			texInfo.inputFormat = uimg::TextureInfo::InputFormat::R16G16B16A16_Float;
			texInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC6;
			texInfo.flags = uimg::TextureInfo::Flags::GenerateMipmaps;
			if(uimg::save_texture(path.GetString(),*imgBuf,texInfo,false,nullptr,true) == false)
				errMsg = "Unable to save image as '" +devInfo.outputPath.GetString() +"'!";
		}
		else
		{
			auto fImg = FileManager::OpenSystemFile(devInfo.outputPath.GetString().c_str(),"wb");
			if(fImg)
			{
				/*auto ocioConfigLocation = util::Path::CreatePath(util::get_program_path());
				ocioConfigLocation += "../modules/open_color_io/configs/";
				ocioConfigLocation.Canonicalize();*/
				switch(m_toneMapping)
				{
				case ToneMapping::FilmicBlender:
				{
					//std::string err;
					//if(util::ocio::apply_color_transform(*imgBuf,util::ocio::Config::FilmicBlender,ocioConfigLocation.GetString(),err,m_exposure,m_gamma) == false)
					//	errMsg = "Unable to apply OCIO color transform: " +err;
					//imgBuf->ToLDR();
					//imgBuf->ClearAlpha(std::numeric_limits<uint8_t>::max());
					break;
				}
				}

				if(errMsg.has_value() == false)
				{
					auto result = false;
					imgBuf->Convert(uimg::ImageBuffer::Format::RGB_LDR);

					//if(imgBuf->IsHDRFormat() || imgBuf->IsFloatFormat())
					//	result = uimg::save_image(fImg,*imgBuf,uimg::ImageFormat::HDR);
					//else
						result = uimg::save_image(fImg,*imgBuf,uimg::ImageFormat::PNG);
					if(result == false)
						errMsg = "Unable to save image as '" +devInfo.outputPath.GetString() +"'!";
				}
			}
			if(errMsg.has_value())
			{
				std::cout<<*errMsg<<std::endl;
				fImg = nullptr;
				FileManager::RemoveSystemFile(devInfo.outputPath.GetString().c_str());
			}
			else
				++m_numSucceeded;
		}
	}
	devInfo.rtScene = nullptr;
	devInfo.job = {};
}

void RTJobManager::PrintHelp()
{
	std::stringstream ss;
	ss<<"Usage: render_raytracing <jobFile> [-option0 -option1=value -option2 ...]\n";
	ss<<"Available options:\n";
	ss<<"-print_header: Prints the header information for the specified job (e.g. sample count, max bounces, etc.)\n";
	ss<<"-help: Prints this help.\n";
	ss<<"-width=<width>: Overrides the width of the rendered image\n";
	ss<<"-height=<height>: Overrides the height of the rendered image\n";
	ss<<"-render_mode=albedo/depth/normals/image: Overrides the render mode. \"image\" renders with full lighting, otherwise only albedo colors, depth values or normals are rendered.\n";
	ss<<"-samples=<sampleCount>: Overrides the number of samples per pixel. Higher values result in a higher quality image with less artifacts, at the cost of rendering time.\n";
	ss<<"-denoise=<1/0>: Enables or disables denoising.\n";
	ss<<"-tonemapped=<1/0>: If disabled, a HDR image will be generated as output, otherwise the image will be gamma corrected and saved as a PNG image instead.\n";
	ss<<"-sky=<pathToSkyTexture>: Overrides the sky texture to use for the scene. Only DDS formats are supported!\n";
	ss<<"-sky_strength=<skyStrength>: Overrides the strength of the sky.\n";
	ss<<"-sky_angle=<skyAngle>: Overrides the yaw-angle of the sky.\n";
	ss<<"-camera_type=orthographic/perspective/panorama: Overrides the camera type.\n";
	ss<<"-panorama_type=equirectangular/fisheye_equidistant/fisheye_equisolid/mirrorball: Overrides the panorama type. Only has an effect if the camera type is \"panorama\".\n";
	ss<<"-stereoscopic=<1/0>: Renders a stereoscopic image, i.e. one image for the left eye and one image for the right eye for use in VR. This option should only be used with the \"panorama\" camera type!\n";
	ss<<"-horizontal_camera_range=(0,360]: The horizontal range in degrees to use if the camera type is set to \"panorama\".\n";
	ss<<"-vertical_camera_range=(0,360]: The vertical range in degrees to use if the camera type is set to \"panorama\".\n";
	ss<<"-color_transform: The color transform to apply to the image.\n";
	ss<<"-color_transform_look: The look of the color transform to apply to the image.\n";
	std::cout<<ss.str();
}

void RTJobManager::PrintHeader(const unirender::Scene::CreateInfo &createInfo,const unirender::Scene::SceneInfo &sceneInfo)
{
	if(createInfo.samples.has_value())
		std::cout<<"Samples: "<<*createInfo.samples<<std::endl;
	std::cout<<"Renderer: "<<createInfo.renderer<<std::endl;
	std::cout<<"HDR Output: "<<createInfo.hdrOutput<<std::endl;
	std::cout<<"Device: "<<((createInfo.deviceType == unirender::Scene::DeviceType::CPU) ? "CPU" : "GPU")<<std::endl;
	std::cout<<"Denoise: ";
	switch(createInfo.denoiseMode)
	{
	case unirender::Scene::DenoiseMode::None:
		std::cout<<"None";
		break;
	case unirender::Scene::DenoiseMode::Fast:
		std::cout<<"Fast";
		break;
	case unirender::Scene::DenoiseMode::Detailed:
		std::cout<<"Detailed";
		break;
	}
	std::cout<<std::endl;

	if(createInfo.colorTransform.has_value())
	{
		std::cout<<"Color transform: "<<createInfo.colorTransform->config<<std::endl;
		if(createInfo.colorTransform->lookName.has_value())
			std::cout<<"Transform look: "<<*createInfo.colorTransform->lookName<<std::endl;
	}

	std::cout<<"Sky angles: "<<sceneInfo.skyAngles<<std::endl;
	std::cout<<"Sky: "<<sceneInfo.sky<<std::endl;
	std::cout<<"Sky strength: "<<sceneInfo.skyStrength<<std::endl;
	std::cout<<"Emission strength: "<<sceneInfo.emissionStrength<<std::endl;
	std::cout<<"Light intensity factor: "<<sceneInfo.lightIntensityFactor<<std::endl;
	std::cout<<"Motion blur strength: "<<sceneInfo.motionBlurStrength<<std::endl;
	std::cout<<"Max transparency bounces: "<<sceneInfo.maxTransparencyBounces<<std::endl;
	std::cout<<"Max bounces: "<<sceneInfo.maxBounces<<std::endl;
	std::cout<<"Max diffuse bounces: "<<sceneInfo.maxDiffuseBounces<<std::endl;
	std::cout<<"Max glossy bounces: "<<sceneInfo.maxGlossyBounces<<std::endl;
	std::cout<<"Max transmission bounces: "<<sceneInfo.maxTransmissionBounces<<std::endl;
}

static unirender::PMesh create_test_box_mesh(unirender::Scene &rtScene,float r=50.f)
{
	Vector3 cmin {-r,-r,-r};
	Vector3 cmax {r,r,r};
	auto min = cmin;
	auto max = cmax;
	uvec::to_min_max(min,max);
	std::vector<Vector3> uniqueVertices {
		min, // 0
		Vector3(max.x,min.y,min.z), // 1
		Vector3(max.x,min.y,max.z), // 2
		Vector3(max.x,max.y,min.z), // 3
		max, // 4
		Vector3(min.x,max.y,min.z), // 5
		Vector3(min.x,min.y,max.z), // 6
		Vector3(min.x,max.y,max.z) // 7
	};
	std::vector<Vector3> verts {
		uniqueVertices[0],uniqueVertices[6],uniqueVertices[7], // 1
		uniqueVertices[0],uniqueVertices[7],uniqueVertices[5], // 1
		uniqueVertices[3],uniqueVertices[0],uniqueVertices[5], // 2
		uniqueVertices[3],uniqueVertices[1],uniqueVertices[0], // 2
		uniqueVertices[2],uniqueVertices[0],uniqueVertices[1], // 3
		uniqueVertices[2],uniqueVertices[6],uniqueVertices[0], // 3
		uniqueVertices[7],uniqueVertices[6],uniqueVertices[2], // 4
		uniqueVertices[4],uniqueVertices[7],uniqueVertices[2], // 4
		uniqueVertices[4],uniqueVertices[1],uniqueVertices[3], // 5
		uniqueVertices[1],uniqueVertices[4],uniqueVertices[2], // 5
		uniqueVertices[4],uniqueVertices[3],uniqueVertices[5], // 6
		uniqueVertices[4],uniqueVertices[5],uniqueVertices[7], // 6
	};
	std::vector<Vector3> faceNormals {
		Vector3(-1,0,0),Vector3(-1,0,0),
		Vector3(0,0,-1),Vector3(0,0,-1),
		Vector3(0,-1,0),Vector3(0,-1,0),
		Vector3(0,0,1),Vector3(0,0,1),
		Vector3(1,0,0),Vector3(1,0,0),
		Vector3(0,1,0),Vector3(0,1,0)
	};
	std::vector<::Vector2> uvs {
		::Vector2(0,1),::Vector2(1,1),::Vector2(1,0), // 1
		::Vector2(0,1),::Vector2(1,0),::Vector2(0,0), // 1
		::Vector2(0,0),::Vector2(1,1),::Vector2(1,0), // 2
		::Vector2(0,0),::Vector2(0,1),::Vector2(1,1), // 2
		::Vector2(0,1),::Vector2(1,0),::Vector2(0,0), // 3
		::Vector2(0,1),::Vector2(1,1),::Vector2(1,0), // 3
		::Vector2(0,0),::Vector2(0,1),::Vector2(1,1), // 4
		::Vector2(1,0),::Vector2(0,0),::Vector2(1,1), // 4
		::Vector2(0,0),::Vector2(1,1),::Vector2(1,0), // 5
		::Vector2(1,1),::Vector2(0,0),::Vector2(0,1), // 5
		::Vector2(1,1),::Vector2(1,0),::Vector2(0,0), // 6
		::Vector2(1,1),::Vector2(0,0),::Vector2(0,1) // 6
	};

	auto numVerts = verts.size();
	auto numTris = verts.size() /3;
	auto mesh = unirender::Mesh::Create("testBox",numVerts,numTris,unirender::Mesh::Flags::None);
	for(auto &v : verts)
		mesh->AddVertex(v,{},{},{});
	for(auto i=0;i<verts.size();i+=3)
		mesh->AddTriangle(i,i +1,i +2,0);

	uint32_t idx = 0;
	for(auto &v : verts)
		mesh->AddVertex(v,{1.f,0.f,0.f},{0.f,0.f,1.f,1.f},uvs.at(idx++));
	for(auto i=0;i<verts.size();i+=3)
		mesh->AddTriangle(i,i +1,i +2,0);

	return mesh;
}

bool RTJobManager::StartJob(const std::string &jobName,DeviceInfo &devInfo)
{
	auto jobFileName = jobName;
	auto f = FileManager::OpenSystemFile(jobFileName.c_str(),"rb");
	if(f == nullptr)
	{
		std::cout<<"Job file '"<<jobFileName<<"' not found!"<<std::endl;
		++m_numFailed;
		return false;
	}
	auto sz = f->GetSize();
	DataStream ds {static_cast<uint32_t>(sz)};
	ds->SetOffset(0);
	f->Read(ds->GetData(),sz);

	unirender::Scene::RenderMode renderMode;
	unirender::Scene::CreateInfo createInfo;
	unirender::Scene::SerializationData serializationData;
	unirender::Scene::SceneInfo sceneInfo;
	uint32_t version;
	auto success = unirender::Scene::ReadHeaderInfo(ds,renderMode,createInfo,serializationData,version,&sceneInfo);
	if(success)
	{
		auto printHeader = (m_launchParams.find("-print_header") != m_launchParams.end());
		if(printHeader)
		{
			std::cout<<"Header information for job '"<<ufile::get_file_from_filename(jobFileName)<<"':"<<std::endl;
			PrintHeader(createInfo,sceneInfo);
			return true;
		}

		std::string fileName = serializationData.outputFileName;
		ufile::remove_extension_from_filename(fileName);
		//if(m_toneMapping == ToneMapping::None)
		//	fileName += ".hdr";
		//else
			fileName += ".png";
		auto &outputPath = devInfo.outputPath;
		outputPath = util::Path::CreatePath(ufile::get_path_from_filename(jobFileName));
		outputPath += ufile::get_file_from_filename(fileName); // TODO: Only write file name in the first place

		if(FileManager::ExistsSystem(outputPath.GetString()))
		{
			std::cout<<"Output file '"<<outputPath.GetString()<<"' for job '"<<ufile::get_file_from_filename(jobFileName)<<"' already exists! Skipping..."<<std::endl;
			++m_numSkipped;
			return false;
		}

		std::cout<<"Initializing job '"<<jobFileName<<"'..."<<std::endl;

		auto itRenderMode = m_launchParams.find("-render_mode");
		if(itRenderMode != m_launchParams.end())
		{
			auto &strRenderMode = itRenderMode->second;
			if(ustring::compare(strRenderMode,"albedo",false))
				renderMode = unirender::Scene::RenderMode::SceneAlbedo;
			else if(ustring::compare(strRenderMode,"depth",false))
				renderMode = unirender::Scene::RenderMode::SceneDepth;
			else if(ustring::compare(strRenderMode,"normals",false))
				renderMode = unirender::Scene::RenderMode::SceneNormals;
			else if(ustring::compare(strRenderMode,"image",false))
				renderMode = unirender::Scene::RenderMode::RenderImage;
		}

		auto itSamples = m_launchParams.find("-samples");
		if(itSamples != m_launchParams.end())
			createInfo.samples = ustring::to_int(itSamples->second);

		auto itColorTransform = m_launchParams.find("-color_transform");
		if(itColorTransform != m_launchParams.end())
		{
			createInfo.colorTransform = unirender::Scene::ColorTransformInfo{};
			createInfo.colorTransform->config = itColorTransform->second;

			auto itLook = m_launchParams.find("-color_transform_look");
			if(itLook != m_launchParams.end())
				createInfo.colorTransform->lookName = itLook->second;
		}

		auto itDenoise = m_launchParams.find("-denoise");
		if(itDenoise != m_launchParams.end())
			createInfo.denoiseMode = util::to_boolean(itDenoise->second) ? unirender::Scene::DenoiseMode::Detailed : unirender::Scene::DenoiseMode::Fast;

		createInfo.deviceType = devInfo.deviceType;

		auto itTonemapped = m_launchParams.find("-tonemapped");
		if(itTonemapped != m_launchParams.end())
			createInfo.hdrOutput = false;

		auto itLog = m_launchParams.find("-log");
		if(itLog == m_launchParams.end() || util::to_boolean(itLog->second) == false)
			unirender::set_log_handler();
		else
		{
			unirender::set_log_handler([](const std::string &msg) {
				std::cout<<"Log: "<<msg<<std::endl;
			});
		}

		auto itRenderer = m_launchParams.find("-renderer");
		if(itRenderer != m_launchParams.end())
			createInfo.renderer = itRenderer->second;
	}
	// We'll disable these since we don't have a preview anyway
	createInfo.progressive = false;
	createInfo.progressiveRefine = false;

	// TODO: We *should* disable progressive rendering, however there is currently a slight difference in rendering behavior when
	// not rendering with the progressive flag, so we'll keep it enabled for now.
	createInfo.progressive = true;

	PrintHeader(createInfo,sceneInfo);
	std::cout<<std::endl;

	auto nodeManager = unirender::NodeManager::Create(); // Unused, since we only use shaders from serialized data
	auto rtScene = success ? unirender::Scene::Create(*nodeManager,ds,ufile::get_path_from_filename(jobFileName),renderMode,createInfo) : nullptr;
	if(rtScene == nullptr)
	{
		std::cout<<"Unable to create scene from serialized data!"<<std::endl;
		++m_numFailed;
		return false;
	}
	m_renderMode = renderMode;

	uint32_t width,height;
	rtScene->GetCamera().GetResolution(width,height);

	auto itSky = m_launchParams.find("-sky");
	if(itSky != m_launchParams.end())
		rtScene->SetSky(itSky->second);
	auto itSkyStrength = m_launchParams.find("-sky_strength");
	if(itSkyStrength != m_launchParams.end())
		rtScene->SetSkyStrength(util::to_float(itSkyStrength->second));
	auto itSkyAngle = m_launchParams.find("-sky_angle");
	if(itSkyAngle != m_launchParams.end())
		rtScene->SetSkyAngles(EulerAngles{0.f,util::to_float(itSkyAngle->second),0.f});

	auto itCamType = m_launchParams.find("-camera_type");
	if(itCamType != m_launchParams.end())
	{
		auto &strCamType = itCamType->second;
		std::optional<unirender::Camera::CameraType> camType {};
		if(ustring::compare(strCamType,"orthographic",false))
			camType = unirender::Camera::CameraType::Orthographic;
		else if(ustring::compare(strCamType,"perspective",false))
			camType = unirender::Camera::CameraType::Perspective;
		else if(ustring::compare(strCamType,"panorama",false))
			camType = unirender::Camera::CameraType::Panorama;
		if(camType.has_value())
			rtScene->GetCamera().SetCameraType(*camType);
	}

	auto itPanoramaType = m_launchParams.find("-panorama_type");
	if(itPanoramaType != m_launchParams.end())
	{
		auto &strPanoramaType = itPanoramaType->second;
		std::optional<unirender::Camera::PanoramaType> panoramaType {};
		if(ustring::compare(strPanoramaType,"equirectangular",false))
			panoramaType = unirender::Camera::PanoramaType::Equirectangular;
		else if(ustring::compare(strPanoramaType,"fisheye_equidistant",false))
			panoramaType = unirender::Camera::PanoramaType::FisheyeEquidistant;
		else if(ustring::compare(strPanoramaType,"fisheye_equisolid",false))
			panoramaType = unirender::Camera::PanoramaType::FisheyeEquisolid;
		else if(ustring::compare(strPanoramaType,"mirrorball",false))
			panoramaType = unirender::Camera::PanoramaType::Mirrorball;
		if(panoramaType.has_value())
			rtScene->GetCamera().SetPanoramaType(*panoramaType);
	}

	auto itStereoscopic = m_launchParams.find("-stereoscopic");
	if(itStereoscopic != m_launchParams.end())
		rtScene->GetCamera().SetStereoscopic(util::to_boolean(itStereoscopic->second));

	auto itHorizontalRange = m_launchParams.find("-horizontal_camera_range");
	if(itHorizontalRange != m_launchParams.end())
		rtScene->GetCamera().SetEquirectangularHorizontalRange(util::to_float(itHorizontalRange->second));

	auto itVerticalRange = m_launchParams.find("-vertical_camera_range");
	if(itVerticalRange != m_launchParams.end())
		rtScene->GetCamera().SetEquirectangularVerticalRange(util::to_float(itVerticalRange->second));

	// TODO: Add options for applying tone-mapping directly, as well as outputting to non-HDR formats

	auto itWidth = m_launchParams.find("-width");
	if(itWidth != m_launchParams.end())
		width = util::to_int(itWidth->second);
	auto itHeight = m_launchParams.find("-height");
	if(itHeight != m_launchParams.end())
		height = util::to_int(itHeight->second);
	if((width %2) != 0)
		width += 1;
	if((height %2) != 0)
		height += 1;
	rtScene->GetCamera().SetResolution(width,height);

	/*{
		// Cube test
		auto mesh = create_test_box_mesh(*rtScene,1500.f);
		//auto shader = unirender::Shader::Create<unirender::ShaderColorTest>(*rtScene,"testBoxShader");;
		auto shader = unirender::Shader::Create<unirender::ShaderVolumeScatter>(*rtScene,"testVolScatter");
		
		mesh->AddSubMeshShader(*shader);
		auto o = unirender::Object::Create(*rtScene,*mesh);
		o->SetPos(Vector3{0,50,0});
	}*/

	devInfo.startTime = std::chrono::high_resolution_clock::now();

	rtScene->Finalize();
	devInfo.renderer = unirender::Renderer::Create(*rtScene,createInfo.renderer);
	if(devInfo.renderer == nullptr)
		return false;
	devInfo.job = devInfo.renderer->StartRender();
	devInfo.job->Start();
	return true;
}

void RTJobManager::SetExposure(float exposure) {m_exposure = exposure;}
void RTJobManager::SetGamma(float gamma) {m_gamma = gamma;}

bool RTJobManager::StartNextJob()
{
	if(m_jobQueue.empty())
		return false;
	auto itDev = std::find_if(m_devices.begin(),m_devices.end(),[](const DeviceInfo &devInfo) {
		return devInfo.job.has_value() == false;
	});
	if(itDev == m_devices.end())
		return false; // All devices in use
	auto &job = m_jobQueue.front();
	auto success = StartJob(job,*itDev);
	m_jobQueue.pop();
	return success;
}

extern "C"
{
__declspec(dllexport) int render_raytracing(int argc,char *argv[])
{
	auto rtManager = RTJobManager::Launch(argc,argv);
	if(rtManager == nullptr)
		return EXIT_FAILURE;
	while(rtManager->IsComplete() == false)
		rtManager->Update();

	util::flash_window();
	std::cout<<rtManager->GetNumSucceeded()<<" succeeded, "<<rtManager->GetNumSkipped()<<" skipped and "<<rtManager->GetNumFailed()<<" failed!"<<std::endl;
	rtManager = nullptr;
	std::this_thread::sleep_for(std::chrono::seconds{5});
	return EXIT_SUCCESS;
}
};
#pragma optimize("",on)