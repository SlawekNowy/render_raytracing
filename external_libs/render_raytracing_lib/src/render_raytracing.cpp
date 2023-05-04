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
#include <spdlog/spdlog.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <fsys/filesystem.h>
#include <fsys/ifile.hpp>
#include <util_ocio.hpp>
#include <sstream>
#include <queue>
#include <cstdlib>

#pragma optimize("", off)
static std::shared_ptr<spdlog::logger> g_logger = nullptr;
class RTJobManager {
  public:
	enum class ToneMapping : uint8_t {
		None = 0u, // Image will be saved with original HDR colors
		FilmicBlender
	};
	struct DeviceInfo {
		DeviceInfo(unirender::Scene::DeviceType deviceType) : deviceType {deviceType} {}
		unirender::Scene::DeviceType deviceType {};
		std::optional<util::ParallelJob<uimg::ImageLayerSet>> job {};
		std::shared_ptr<unirender::Renderer> renderer = nullptr;
		std::shared_ptr<unirender::Scene> rtScene = nullptr;
		std::chrono::high_resolution_clock::time_point startTime {};
		util::Path outputPath {};
	};
	static std::shared_ptr<RTJobManager> Launch(int argc, char *argv[]);
	RTJobManager(const RTJobManager &) = delete;
	RTJobManager(RTJobManager &&) = delete;
	RTJobManager &operator=(const RTJobManager &) = delete;
	RTJobManager &operator=(RTJobManager &&) = delete;
	~RTJobManager();

	void SetExposure(float exposure);
	void SetGamma(float gamma);
	void SetSaveAsHdr(bool saveAsHdr) { m_saveAsHdr = saveAsHdr; }

	bool StartNextJob();
	bool IsComplete() const;
	bool ShouldShutDownOnCompletion() const { return m_shutdownOnCompletion; }
	bool ShouldAutoCloseOnCompletion() const { return !m_dontCloseOnCompletion; }
	uint32_t GetNumSucceeded() const { return m_numSucceeded; }
	uint32_t GetNumFailed() const { return m_numFailed; }
	uint32_t GetNumSkipped() const { return m_numSkipped; }
	void Update();
  private:
	RTJobManager(std::unordered_map<std::string, std::string> &&launchParams, const std::string &inputFileName);
	void UpdateJob(DeviceInfo &devInfo);
	void PrintHeader(const unirender::Scene::CreateInfo &createInfo, const unirender::Scene::SceneInfo &sceneInfo);
	void PrintHelp();
	void PrintCommandHelp();
	bool StartJob(const std::string &job, DeviceInfo &devInfo);
	void CollectJobs();

	std::chrono::high_resolution_clock::time_point m_startTime {};
	std::unordered_map<std::string, std::string> m_launchParams {};
	std::vector<DeviceInfo> m_devices {};
	std::queue<std::string> m_jobQueue {};
	uint32_t m_numJobs = 0;

	unirender::Scene::RenderMode m_renderMode;
	bool m_shutdownOnCompletion = false;
	bool m_dontCloseOnCompletion = true;
	float m_exposure = 0.f;
	float m_gamma = 2.2f;
	bool m_saveAsHdr = false;
	std::string m_inputFileName;
	uint32_t m_numSucceeded = 0;
	uint32_t m_numFailed = 0;
	uint32_t m_numSkipped = 0;
	ToneMapping m_toneMapping = ToneMapping::FilmicBlender;
};

std::shared_ptr<RTJobManager> RTJobManager::Launch(int argc, char *argv[])
{
	// Remove the exe-path
	--argc;
	++argv;

	auto launchParams = util::get_launch_parameters(argc, argv);
	auto itJob = launchParams.find("-job");
	return std::shared_ptr<RTJobManager> {new RTJobManager {std::move(launchParams), (itJob != launchParams.end()) ? itJob->second : ""}};
}

RTJobManager::RTJobManager(std::unordered_map<std::string, std::string> &&launchParams, const std::string &inputFileName) : m_launchParams {std::move(launchParams)}, m_inputFileName {inputFileName}
{
	auto conSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	conSink->set_level(spdlog::level::info);

	auto logFilePath = ufile::get_path_from_filename(inputFileName) + "log.txt";
	auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath, true);
	fileSink->set_level(spdlog::level::trace);

	auto logger = std::make_shared<spdlog::logger>("renderer", spdlog::sinks_init_list {conSink, fileSink});
	logger->set_level(spdlog::level::info);
	spdlog::register_logger(logger);
	unirender::set_logger(logger);
	g_logger = logger;

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

	auto itHdr = m_launchParams.find("-hdr");
	if(itHdr != m_launchParams.end())
		SetSaveAsHdr(true);

	auto itLog = m_launchParams.find("-log");
	if(itLog != m_launchParams.end()) {
		unirender::set_log_handler([](std::string msg) { g_logger->info(msg); });
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
	if(itDeviceType != m_launchParams.end()) {
		auto &strDeviceType = itDeviceType->second;
		if(ustring::compare<std::string>(strDeviceType, "cpu", false))
			m_devices.push_back(unirender::Scene::DeviceType::CPU);
		else if(ustring::compare<std::string>(strDeviceType, "gpu", false))
			m_devices.push_back(unirender::Scene::DeviceType::GPU);
		else if(ustring::compare<std::string>(strDeviceType, "combined", false)) {
			m_devices.push_back(unirender::Scene::DeviceType::GPU);
			m_devices.push_back(unirender::Scene::DeviceType::CPU);
		}
	}
	if(m_devices.empty())
		m_devices.push_back(unirender::Scene::DeviceType::GPU);

	util::minimize_window_to_tray();
	util::CommandManager::StartAsync();

	if(m_launchParams.find("-help") != m_launchParams.end()) {
		PrintHelp();
		return;
	}

	util::CommandManager::RegisterCommand("pause", [this](std::vector<std::string> args) {
		uint32_t numPaused = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices) {
			if(dev.renderer == nullptr)
				continue;
			if(dev.renderer->Pause())
				++numPaused;
			else
				++numFailed;
		}
		g_logger->info("Paused {} render processess!", numPaused);
		if(numFailed > 0)
			g_logger->error("Failed to pause {} render processes!", numFailed);
	});
	util::CommandManager::RegisterCommand("resume", [this](std::vector<std::string> args) {
		uint32_t numResumed = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices) {
			if(dev.renderer == nullptr)
				continue;
			if(dev.renderer->Resume())
				++numResumed;
			else
				++numFailed;
		}
		g_logger->info("Resumed {} render processess!", numResumed);
		if(numFailed > 0)
			g_logger->error("Failed to resume {} render processes!", numFailed);
	});
	util::CommandManager::RegisterCommand("stop", [this](std::vector<std::string> args) {
		uint32_t numStopped = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices) {
			if(dev.renderer == nullptr)
				continue;
			if(dev.renderer->Stop())
				++numStopped;
			else
				++numFailed;
		}
		g_logger->info("Stopped {} render processess!", numStopped);
		if(numFailed > 0)
			g_logger->error("Failed to stop {} render processes!", numFailed);
	});
	util::CommandManager::RegisterCommand("preview", [this](std::vector<std::string> args) {
		uint32_t numStopped = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices) {
			if(dev.renderer == nullptr)
				continue;
			std::string err;
			auto filePath = dev.renderer->SaveRenderPreview("temp/", err);
			if(filePath.has_value())
				util::open_file_in_default_program(*filePath);
			else
				g_logger->error("Unable to save preview image: {}", err);
		}
	});
	util::CommandManager::RegisterCommand("suspend", [this](std::vector<std::string> args) {
		uint32_t numSuspended = 0;
		uint32_t numFailed = 0;
		for(auto &dev : m_devices) {
			if(dev.renderer == nullptr)
				continue;
			if(dev.renderer->Suspend())
				++numSuspended;
			else
				++numFailed;
		}
		g_logger->info("Suspended {} render processess!", numSuspended);
		if(numFailed > 0)
			g_logger->error("Failed to suspend {} render processes!", numFailed);
	});
	util::CommandManager::RegisterCommand("export", [this](std::vector<std::string> args) {
		uint32_t numExported = 0;
		uint32_t numFailed = 0;
		uint32_t devIdx = 0;
		for(auto &dev : m_devices) {
			if(dev.renderer != nullptr) {
				if(dev.renderer->Export("render/export/"))
					++numExported;
				else
					++numFailed;
			}
			++devIdx;
		}
		g_logger->info("Exported {} render processess!", numExported);
		if(numFailed > 0)
			g_logger->error("Failed to export {} render processes!", numFailed);
	});
	util::CommandManager::RegisterCommand("shutdown", [this](std::vector<std::string> args) {
		m_shutdownOnCompletion = args.empty() ? !m_shutdownOnCompletion : util::to_boolean(args[0]);
		if(m_shutdownOnCompletion)
			g_logger->info("Auto-Shutdown enabled! Operating system will shut down when rendering has been completed.");
		else
			g_logger->info("Auto-Shutdown disabled");
	});
	util::CommandManager::RegisterCommand("autoclose", [this](std::vector<std::string> args) {
		m_dontCloseOnCompletion = args.empty() ? !m_dontCloseOnCompletion : !util::to_boolean(args[0]);
		if(m_dontCloseOnCompletion)
			g_logger->info("Auto-Close disabled!");
		else
			g_logger->info("Auto-Close enabled!");
	});
	util::CommandManager::RegisterCommand("help", [this](std::vector<std::string> args) { PrintCommandHelp(); });

	PrintCommandHelp();

	for(auto &devInfo : m_devices)
		g_logger->info("Using device: {}", magic_enum::enum_name(devInfo.deviceType));

	CollectJobs();

	g_logger->info("Executing {} jobs...", m_jobQueue.size());
	m_startTime = std::chrono::high_resolution_clock::now();
}

RTJobManager::~RTJobManager()
{
	util::CommandManager::Join();
	m_devices.clear();
	unirender::Renderer::Close();

	g_logger = nullptr;
	spdlog::shutdown();
}

bool RTJobManager::IsComplete() const
{
	if(m_jobQueue.empty() == false)
		return false;
	auto itDev = std::find_if(m_devices.begin(), m_devices.end(), [](const DeviceInfo &devInfo) { return devInfo.job.has_value(); });
	return itDev == m_devices.end();
}

void RTJobManager::CollectJobs()
{
	std::vector<std::string> lines {};

	std::vector<std::string> jobs {};
	for(auto &param : m_launchParams) {
		if(param.first.empty() || param.first.front() == '-')
			continue;
		jobs.push_back(param.first);
	}

	if(m_inputFileName.empty() == false) {
		std::string ext;
		ufile::get_extension(m_inputFileName, &ext);
		if(ustring::compare<std::string>(ext, "txt", false)) {
			auto f = FileManager::OpenSystemFile(m_inputFileName.c_str(), "r");
			if(f == nullptr)
				return;
			auto contents = f->ReadString();
			ustring::explode(contents, "\n", lines);
		}
		else
			lines.push_back(m_inputFileName);

		//for(auto &l : lines)
		{
			std::vector<std::string> ljobs = std::move(lines);
			// FileManager::FindSystemFiles(l.c_str(),&ljobs,nullptr);

			// Sort by name
			std::sort(ljobs.begin(), ljobs.end());

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
			if(ljobs.size() > 1) {
				auto start = 0u;
				auto end = ljobs.size();
				for(uint32_t i = 2; i <= 32; i *= 2) {
					if(ljobs.size() < i)
						break;
					auto pos = (end - start) / i;
					while(start + pos < end) {
						if(ljobs.at(start + pos).empty() == false)
							orderedJobs.emplace_back(std::move(ljobs.at(start + pos)));
						pos += (end - start) / i;
					}
				}
			}

			for(auto &ljob : ljobs) {
				if(ljob.empty())
					continue;
				orderedJobs.emplace_back(std::move(ljob));
			}

			// Jobs are sorted by input string and THEN by name
			jobs.reserve(jobs.size() + orderedJobs.size());
			auto path = ufile::get_path_from_filename(m_inputFileName);
			for(auto &job : orderedJobs)
				jobs.push_back(path + job);
		}
	}

	for(auto &job : jobs)
		m_jobQueue.push(job);
	m_numJobs = m_jobQueue.size();

	if(m_jobQueue.empty()) {
		g_logger->warn("No jobs specified!");
		PrintHelp();
		std::this_thread::sleep_for(std::chrono::seconds {5});
	}
}

void RTJobManager::Update()
{
	util::CommandManager::PollEvents();
	if(util::CommandManager::ShouldExit()) {
		while(m_jobQueue.empty() == false)
			m_jobQueue.pop();
	}
	StartNextJob();
	auto allBusy = true;
	for(auto &devInfo : m_devices) {
		UpdateJob(devInfo);
		if(devInfo.job.has_value() == false)
			allBusy = false;
	}
	if(allBusy)
		std::this_thread::sleep_for(std::chrono::seconds {5});
}

void RTJobManager::UpdateJob(DeviceInfo &devInfo)
{
	if(devInfo.job.has_value() == false)
		return;
	auto &job = *devInfo.job;
	if(job.IsComplete() == false) {
		if(util::CommandManager::ShouldExit())
			job.Cancel();
		else {
			auto progress = job.GetProgress();
			auto tDelta = std::chrono::high_resolution_clock::now() - devInfo.startTime;
			double tDeltaD = tDelta.count() / static_cast<double>(progress) * static_cast<double>(1.f - progress);
			auto strTime = util::get_pretty_duration(tDeltaD / 1'000'000.0);
			std::stringstream ss;
			ss << "Progress for job '" << ufile::get_file_from_filename(devInfo.outputPath.GetString()) << "': " << util::round_string(progress * 100.f, 2) << " %";
			if(progress > 0.f)
				ss << " Time remaining: " << strTime << ".";

			auto numCompleted = m_numSucceeded + m_numFailed + m_numSkipped;
			auto totalProgress = (numCompleted + progress) / static_cast<float>(m_numJobs);
			ss << " Total progress: " << util::round_string(totalProgress * 100.f, 2.f) << "%";

			auto tDeltaAll = std::chrono::high_resolution_clock::now() - m_startTime;
			auto tDeltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(tDeltaAll);
			auto timePassed = util::get_pretty_duration(tDeltaMs.count());
			ss << " Total time passed: " << timePassed;

			auto numComplete = m_numSucceeded + progress;
			auto numLeft = m_numJobs - m_numFailed - m_numSkipped - numComplete;
			auto tRemainingMs = (tDeltaMs / numComplete) * numLeft;
			auto timeRemaining = util::get_pretty_duration(tRemainingMs.count());
			ss << " Total time remaining: " << timeRemaining;
			g_logger->info(ss.str());
		}
		return;
	}

	if(job.IsCancelled())
		g_logger->info("Job has been cancelled!");
	else if(job.IsSuccessful() == false)
		g_logger->error("Job has failed!");
	else {
		g_logger->info("Job has been completed successfully!");
		auto images = job.GetResult().images;
		auto imgBuf = images.begin()->second;

		g_logger->info("Saving images...");
		std::optional<std::string> errMsg {};
		if(m_renderMode == unirender::Scene::RenderMode::BakeDiffuseLighting || m_renderMode == unirender::Scene::RenderMode::BakeDiffuseLightingSeparate) {
			struct OutputImageInfo {
				std::string suffix = "";
				std::shared_ptr<uimg::ImageBuffer> imgBuf;
			};
			std::vector<OutputImageInfo> outputImageInfos;
			if(m_renderMode == unirender::Scene::RenderMode::BakeDiffuseLighting)
				outputImageInfos.push_back({"", imgBuf});
			else {
				outputImageInfos.push_back({"_direct", images["DiffuseDirect"]});
				outputImageInfos.push_back({"_indirect", images["DiffuseIndirect"]});
			}
			for(auto &outputImgInfo : outputImageInfos) {
				auto path = devInfo.outputPath;
				path.RemoveFileExtension(std::vector<std::string> {"hdr", "dds"});
				path += outputImgInfo.suffix;

				if(m_saveAsHdr) {
					path += ".hdr";
					auto f = filemanager::open_system_file(path.GetString(), filemanager::FileMode::Write | filemanager::FileMode::Binary);
					if(!f) {
						errMsg = "Failed to open output file '" + path.GetString() + "'!";
						continue;
					}
					fsys::File fp {f};
					if(!uimg::save_image(fp, *outputImgInfo.imgBuf, uimg::ImageFormat::HDR))
						errMsg = "Unable to save image as '" + devInfo.outputPath.GetString() + "'!";
					continue;
				}
				path += ".dds";
				uimg::TextureInfo texInfo {};
				texInfo.containerFormat = uimg::TextureInfo::ContainerFormat::DDS;
				texInfo.inputFormat = uimg::TextureInfo::InputFormat::R16G16B16A16_Float;
				texInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC6;
				texInfo.flags = uimg::TextureInfo::Flags::GenerateMipmaps;
				uimg::TextureSaveInfo saveInfo {};
				saveInfo.texInfo = texInfo;
				if(uimg::save_texture(path.GetString(), *outputImgInfo.imgBuf, saveInfo, nullptr, true) == false)
					errMsg = "Unable to save image as '" + devInfo.outputPath.GetString() + "'!";
			}
		}
		else {
			auto fImg = FileManager::OpenSystemFile(devInfo.outputPath.GetString().c_str(), "wb");
			if(fImg) {
				/*auto ocioConfigLocation = util::Path::CreatePath(util::get_program_path());
				ocioConfigLocation += "../modules/open_color_io/configs/";
				ocioConfigLocation.Canonicalize();*/
				switch(m_toneMapping) {
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

				if(errMsg.has_value() == false) {
					auto result = false;
					imgBuf->Convert(uimg::Format::RGB_LDR);

					//if(imgBuf->IsHDRFormat() || imgBuf->IsFloatFormat())
					//	result = uimg::save_image(fImg,*imgBuf,uimg::ImageFormat::HDR);
					//else
					fsys::File fp {fImg};
					result = uimg::save_image(fp, *imgBuf, uimg::ImageFormat::PNG);
					if(result == false)
						errMsg = "Unable to save image as '" + devInfo.outputPath.GetString() + "'!";
				}
			}
			if(errMsg.has_value()) {
				g_logger->error(*errMsg);
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

void RTJobManager::PrintCommandHelp()
{
	std::stringstream ss;
	ss << "Available commands:\n";
	for(auto &cmd : util::CommandManager ::GetCommands())
		ss << cmd << "\n";
	ss << "\n";
	g_logger->info(ss.str());
}

void RTJobManager::PrintHelp()
{
	std::stringstream ss;
	ss << "Usage: render_raytracing <jobFile> [-option0 -option1=value -option2 ...]\n";
	ss << "Available options:\n";
	ss << "-print_header: Prints the header information for the specified job (e.g. sample count, max bounces, etc.)\n";
	ss << "-help: Prints this help.\n";
	ss << "-width=<width>: Overrides the width of the rendered image\n";
	ss << "-height=<height>: Overrides the height of the rendered image\n";
	ss << "-render_mode=albedo/depth/normals/image: Overrides the render mode. \"image\" renders with full lighting, otherwise only albedo colors, depth values or normals are rendered.\n";
	ss << "-samples=<sampleCount>: Overrides the number of samples per pixel. Higher values result in a higher quality image with less artifacts, at the cost of rendering time.\n";
	ss << "-denoise=<1/0>: Enables or disables denoising.\n";
	ss << "-tonemapped=<1/0>: If disabled, a HDR image will be generated as output, otherwise the image will be gamma corrected and saved as a PNG image instead.\n";
	ss << "-sky=<pathToSkyTexture>: Overrides the sky texture to use for the scene. Only DDS formats are supported!\n";
	ss << "-sky_strength=<skyStrength>: Overrides the strength of the sky.\n";
	ss << "-sky_angle=<skyAngle>: Overrides the yaw-angle of the sky.\n";
	ss << "-camera_type=orthographic/perspective/panorama: Overrides the camera type.\n";
	ss << "-panorama_type=equirectangular/fisheye_equidistant/fisheye_equisolid/mirrorball: Overrides the panorama type. Only has an effect if the camera type is \"panorama\".\n";
	ss << "-stereoscopic=<1/0>: Renders a stereoscopic image, i.e. one image for the left eye and one image for the right eye for use in VR. This option should only be used with the \"panorama\" camera type!\n";
	ss << "-horizontal_camera_range=(0,360]: The horizontal range in degrees to use if the camera type is set to \"panorama\".\n";
	ss << "-vertical_camera_range=(0,360]: The vertical range in degrees to use if the camera type is set to \"panorama\".\n";
	ss << "-color_transform: The color transform to apply to the image.\n";
	ss << "-color_transform_look: The look of the color transform to apply to the image.\n";
	g_logger->info(ss.str());
}

void RTJobManager::PrintHeader(const unirender::Scene::CreateInfo &createInfo, const unirender::Scene::SceneInfo &sceneInfo)
{
	g_logger->info("Header Info:");
	if(createInfo.samples.has_value())
		g_logger->info("Samples: {}", *createInfo.samples);
	g_logger->info("Renderer: {}", createInfo.renderer);
	g_logger->info("HDR Output: {}", createInfo.hdrOutput);
	g_logger->info("Device: {}", magic_enum::enum_name(createInfo.deviceType));
	g_logger->info("Denoise: {}", magic_enum::enum_name(createInfo.denoiseMode));

	if(createInfo.colorTransform.has_value()) {
		g_logger->info("Color transform: {}", createInfo.colorTransform->config);
		if(createInfo.colorTransform->lookName.has_value())
			g_logger->info("Transform look: {}", *createInfo.colorTransform->lookName);
	}

	g_logger->info("Sky angles: {} {} {}", sceneInfo.skyAngles.p, sceneInfo.skyAngles.y, sceneInfo.skyAngles.r);
	g_logger->info("Sky: {}", sceneInfo.sky);
	g_logger->info("Sky strength: {}", sceneInfo.skyStrength);
	g_logger->info("Emission strength: {}", sceneInfo.emissionStrength);
	g_logger->info("Light intensity factor: {}", sceneInfo.lightIntensityFactor);
	g_logger->info("Motion blur strength: {}", sceneInfo.motionBlurStrength);
	g_logger->info("Max transparency bounces: {}", sceneInfo.maxTransparencyBounces);
	g_logger->info("Max bounces: {}", sceneInfo.maxBounces);
	g_logger->info("Max diffuse bounces: {}", sceneInfo.maxDiffuseBounces);
	g_logger->info("Max glossy bounces: {}", sceneInfo.maxGlossyBounces);
	g_logger->info("Max transmission bounces: {}", sceneInfo.maxTransmissionBounces);
}

static unirender::PMesh create_test_box_mesh(unirender::Scene &rtScene, float r = 50.f)
{
	Vector3 cmin {-r, -r, -r};
	Vector3 cmax {r, r, r};
	auto min = cmin;
	auto max = cmax;
	uvec::to_min_max(min, max);
	std::vector<Vector3> uniqueVertices {
	  min,                          // 0
	  Vector3(max.x, min.y, min.z), // 1
	  Vector3(max.x, min.y, max.z), // 2
	  Vector3(max.x, max.y, min.z), // 3
	  max,                          // 4
	  Vector3(min.x, max.y, min.z), // 5
	  Vector3(min.x, min.y, max.z), // 6
	  Vector3(min.x, max.y, max.z)  // 7
	};
	std::vector<Vector3> verts {
	  uniqueVertices[0], uniqueVertices[6], uniqueVertices[7], // 1
	  uniqueVertices[0], uniqueVertices[7], uniqueVertices[5], // 1
	  uniqueVertices[3], uniqueVertices[0], uniqueVertices[5], // 2
	  uniqueVertices[3], uniqueVertices[1], uniqueVertices[0], // 2
	  uniqueVertices[2], uniqueVertices[0], uniqueVertices[1], // 3
	  uniqueVertices[2], uniqueVertices[6], uniqueVertices[0], // 3
	  uniqueVertices[7], uniqueVertices[6], uniqueVertices[2], // 4
	  uniqueVertices[4], uniqueVertices[7], uniqueVertices[2], // 4
	  uniqueVertices[4], uniqueVertices[1], uniqueVertices[3], // 5
	  uniqueVertices[1], uniqueVertices[4], uniqueVertices[2], // 5
	  uniqueVertices[4], uniqueVertices[3], uniqueVertices[5], // 6
	  uniqueVertices[4], uniqueVertices[5], uniqueVertices[7], // 6
	};
	std::vector<Vector3> faceNormals {Vector3(-1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 0, -1), Vector3(0, 0, -1), Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 1, 0)};
	std::vector<::Vector2> uvs {
	  ::Vector2(0, 1), ::Vector2(1, 1), ::Vector2(1, 0), // 1
	  ::Vector2(0, 1), ::Vector2(1, 0), ::Vector2(0, 0), // 1
	  ::Vector2(0, 0), ::Vector2(1, 1), ::Vector2(1, 0), // 2
	  ::Vector2(0, 0), ::Vector2(0, 1), ::Vector2(1, 1), // 2
	  ::Vector2(0, 1), ::Vector2(1, 0), ::Vector2(0, 0), // 3
	  ::Vector2(0, 1), ::Vector2(1, 1), ::Vector2(1, 0), // 3
	  ::Vector2(0, 0), ::Vector2(0, 1), ::Vector2(1, 1), // 4
	  ::Vector2(1, 0), ::Vector2(0, 0), ::Vector2(1, 1), // 4
	  ::Vector2(0, 0), ::Vector2(1, 1), ::Vector2(1, 0), // 5
	  ::Vector2(1, 1), ::Vector2(0, 0), ::Vector2(0, 1), // 5
	  ::Vector2(1, 1), ::Vector2(1, 0), ::Vector2(0, 0), // 6
	  ::Vector2(1, 1), ::Vector2(0, 0), ::Vector2(0, 1)  // 6
	};

	auto numVerts = verts.size();
	auto numTris = verts.size() / 3;
	auto mesh = unirender::Mesh::Create("testBox", numVerts, numTris, unirender::Mesh::Flags::None);
	for(auto &v : verts)
		mesh->AddVertex(v, {}, {}, {});
	for(auto i = 0; i < verts.size(); i += 3)
		mesh->AddTriangle(i, i + 1, i + 2, 0);

	uint32_t idx = 0;
	for(auto &v : verts)
		mesh->AddVertex(v, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 1.f}, uvs.at(idx++));
	for(auto i = 0; i < verts.size(); i += 3)
		mesh->AddTriangle(i, i + 1, i + 2, 0);

	return mesh;
}

bool RTJobManager::StartJob(const std::string &jobName, DeviceInfo &devInfo)
{
	auto jobFileName = jobName;
	auto f = FileManager::OpenSystemFile(jobFileName.c_str(), "rb");
	if(f == nullptr) {
		g_logger->error("Job file '{}' not found!", jobFileName);
		++m_numFailed;
		return false;
	}
	auto sz = f->GetSize();
	DataStream ds {static_cast<uint32_t>(sz)};
	ds->SetOffset(0);
	f->Read(ds->GetData(), sz);

	unirender::Scene::RenderMode renderMode;
	unirender::Scene::CreateInfo createInfo;
	unirender::Scene::SerializationData serializationData;
	unirender::Scene::SceneInfo sceneInfo;
	uint32_t version;
	auto success = unirender::Scene::ReadHeaderInfo(ds, renderMode, createInfo, serializationData, version, &sceneInfo);
	if(success) {
		auto printHeader = (m_launchParams.find("-print_header") != m_launchParams.end());
		if(printHeader) {
			g_logger->info("Header information for job '{}':", ufile::get_file_from_filename(jobFileName));
			PrintHeader(createInfo, sceneInfo);
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

		if(FileManager::ExistsSystem(outputPath.GetString())) {
			g_logger->info("Output file '{}' for job '{}' already exists! Skipping...", outputPath.GetString(), ufile::get_file_from_filename(jobFileName));
			++m_numSkipped;
			return false;
		}

		g_logger->info("Initializing job '{}'...", jobFileName);

		auto itRenderMode = m_launchParams.find("-render_mode");
		if(itRenderMode != m_launchParams.end()) {
			auto &strRenderMode = itRenderMode->second;
			if(ustring::compare<std::string>(strRenderMode, "albedo", false))
				renderMode = unirender::Scene::RenderMode::SceneAlbedo;
			else if(ustring::compare<std::string>(strRenderMode, "depth", false))
				renderMode = unirender::Scene::RenderMode::SceneDepth;
			else if(ustring::compare<std::string>(strRenderMode, "normals", false))
				renderMode = unirender::Scene::RenderMode::SceneNormals;
			else if(ustring::compare<std::string>(strRenderMode, "image", false))
				renderMode = unirender::Scene::RenderMode::RenderImage;
		}

		auto itSamples = m_launchParams.find("-samples");
		if(itSamples != m_launchParams.end())
			createInfo.samples = ustring::to_int(itSamples->second);

		auto itColorTransform = m_launchParams.find("-color_transform");
		if(itColorTransform != m_launchParams.end()) {
			createInfo.colorTransform = unirender::Scene::ColorTransformInfo {};
			createInfo.colorTransform->config = itColorTransform->second;

			auto itLook = m_launchParams.find("-color_transform_look");
			if(itLook != m_launchParams.end())
				createInfo.colorTransform->lookName = itLook->second;
		}

		auto itDenoise = m_launchParams.find("-denoise");
		if(itDenoise != m_launchParams.end())
			createInfo.denoiseMode = util::to_boolean(itDenoise->second) ? unirender::Scene::DenoiseMode::AutoDetailed : unirender::Scene::DenoiseMode::AutoFast;

		auto itAdaptiveSampling = m_launchParams.find("-adaptiveSampling");
		if(itAdaptiveSampling != m_launchParams.end()) {
			auto &enabled = sceneInfo.useAdaptiveSampling;
			float &adaptiveSamplingThreshold = sceneInfo.adaptiveSamplingThreshold;
			uint32_t &adaptiveMinSamples = sceneInfo.adaptiveMinSamples;
			enabled = true;
			adaptiveSamplingThreshold = 0.01f;
			adaptiveMinSamples = 0;

			std::vector<std::string> args;
			ustring::explode_whitespace(itAdaptiveSampling->second, args);
			if(args.size() > 0) {
				enabled = util::to_boolean(args[0]);
				if(args.size() > 1) {
					adaptiveSamplingThreshold = util::to_float(args[1]);
					if(args.size() > 2)
						adaptiveMinSamples = util::to_uint(args[2]);
				}
			}
		}

		createInfo.deviceType = devInfo.deviceType;

		auto itTonemapped = m_launchParams.find("-tonemapped");
		if(itTonemapped != m_launchParams.end())
			createInfo.hdrOutput = false;

		auto itLog = m_launchParams.find("-log");
		if(itLog == m_launchParams.end() || util::to_boolean(itLog->second) == false)
			unirender::set_log_handler();
		else {
			unirender::set_log_handler([](const std::string &msg) { g_logger->info(msg); });
		}

		auto itRenderer = m_launchParams.find("-renderer");
		if(itRenderer != m_launchParams.end())
			createInfo.renderer = itRenderer->second;
	}
	// We'll disable these since we don't have a preview anyway
	createInfo.progressive = false;
	createInfo.progressiveRefine = false;

	PrintHeader(createInfo, sceneInfo);

	auto nodeManager = unirender::NodeManager::Create(); // Unused, since we only use shaders from serialized data
	auto rtScene = success ? unirender::Scene::Create(*nodeManager, ds, ufile::get_path_from_filename(jobFileName), renderMode, createInfo) : nullptr;
	if(rtScene == nullptr) {
		g_logger->error("Unable to create scene from serialized data!");
		++m_numFailed;
		return false;
	}
	m_renderMode = renderMode;

	uint32_t width, height;
	rtScene->GetCamera().GetResolution(width, height);

	auto itSky = m_launchParams.find("-sky");
	if(itSky != m_launchParams.end())
		rtScene->SetSky(itSky->second);
	auto itSkyStrength = m_launchParams.find("-sky_strength");
	if(itSkyStrength != m_launchParams.end())
		rtScene->SetSkyStrength(util::to_float(itSkyStrength->second));
	auto itSkyAngle = m_launchParams.find("-sky_angle");
	if(itSkyAngle != m_launchParams.end())
		rtScene->SetSkyAngles(EulerAngles {0.f, util::to_float(itSkyAngle->second), 0.f});

	auto itCamType = m_launchParams.find("-camera_type");
	if(itCamType != m_launchParams.end()) {
		auto &strCamType = itCamType->second;
		std::optional<unirender::Camera::CameraType> camType {};
		if(ustring::compare<std::string>(strCamType, "orthographic", false))
			camType = unirender::Camera::CameraType::Orthographic;
		else if(ustring::compare<std::string>(strCamType, "perspective", false))
			camType = unirender::Camera::CameraType::Perspective;
		else if(ustring::compare<std::string>(strCamType, "panorama", false))
			camType = unirender::Camera::CameraType::Panorama;
		if(camType.has_value())
			rtScene->GetCamera().SetCameraType(*camType);
	}

	auto itPanoramaType = m_launchParams.find("-panorama_type");
	if(itPanoramaType != m_launchParams.end()) {
		auto &strPanoramaType = itPanoramaType->second;
		std::optional<unirender::Camera::PanoramaType> panoramaType {};
		if(ustring::compare<std::string>(strPanoramaType, "equirectangular", false))
			panoramaType = unirender::Camera::PanoramaType::Equirectangular;
		else if(ustring::compare<std::string>(strPanoramaType, "fisheye_equidistant", false))
			panoramaType = unirender::Camera::PanoramaType::FisheyeEquidistant;
		else if(ustring::compare<std::string>(strPanoramaType, "fisheye_equisolid", false))
			panoramaType = unirender::Camera::PanoramaType::FisheyeEquisolid;
		else if(ustring::compare<std::string>(strPanoramaType, "mirrorball", false))
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
	if((width % 2) != 0)
		width += 1;
	if((height % 2) != 0)
		height += 1;
	rtScene->GetCamera().SetResolution(width, height);

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
	std::string errMsg;
	devInfo.renderer = unirender::Renderer::Create(*rtScene, createInfo.renderer, errMsg, unirender::Renderer::Flags::DisableDisplayDriver);
	if(devInfo.renderer == nullptr) {
		g_logger->error("Failed to create renderer: {}!", errMsg);
		return false;
	}
	if(devInfo.renderer == nullptr)
		return false;
	devInfo.job = devInfo.renderer->StartRender();
	devInfo.job->Start();
	return true;
}

void RTJobManager::SetExposure(float exposure) { m_exposure = exposure; }
void RTJobManager::SetGamma(float gamma) { m_gamma = gamma; }

bool RTJobManager::StartNextJob()
{
	if(m_jobQueue.empty())
		return false;
	auto itDev = std::find_if(m_devices.begin(), m_devices.end(), [](const DeviceInfo &devInfo) { return devInfo.job.has_value() == false; });
	if(itDev == m_devices.end())
		return false; // All devices in use
	auto &job = m_jobQueue.front();
	auto success = StartJob(job, *itDev);
	m_jobQueue.pop();
	return success;
}

#ifdef __linux__
#define DLLEXPORT __attribute__((visibility("default")))
#else
#define DLLEXPORT __declspec(dllexport)
#endif

extern "C" {
DLLEXPORT int render_raytracing(int argc, char *argv[])
{
	auto rtManager = RTJobManager::Launch(argc, argv);
	if(rtManager == nullptr)
		return EXIT_FAILURE;
	while(rtManager->IsComplete() == false)
		rtManager->Update();

	util::flash_window();
	g_logger->info("{} succeeded, {} skipped and {} failed!", rtManager->GetNumSucceeded(), rtManager->GetNumSkipped(), rtManager->GetNumFailed());

	auto shutDown = rtManager->ShouldShutDownOnCompletion();
	auto waitBeforeExit = true;
	if(!rtManager->ShouldAutoCloseOnCompletion() && !shutDown) {
		g_logger->info("Press enter to exit...");
		util::CommandManager::Stop();
		waitBeforeExit = false;
	}

	rtManager = nullptr;
	if(waitBeforeExit)
		std::this_thread::sleep_for(std::chrono::seconds {5});
	if(shutDown) {
		g_logger->info("Shutting down...");
		std::this_thread::sleep_for(std::chrono::seconds {5});
		util::shutdown_os();
	}
	return EXIT_SUCCESS;
}
};
#pragma optimize("", on)
