#include <util_raytracing/object.hpp>
#include <util_raytracing/mesh.hpp>
#include <util_raytracing/camera.hpp>
#include <util_raytracing.hpp>
#include <util_raytracing/scene.hpp>
#include <util_raytracing/shader.hpp>
#include <util_image.hpp>
#include <util_image_buffer.hpp>
#include <sharedutils/util_command_manager.hpp>
#include <sharedutils/util.h>
#include <sharedutils/util_file.h>
#include <sharedutils/util_string.h>
#include <sharedutils/datastream.h>
#include <sharedutils/util_parallel_job.hpp>
#include <sharedutils/util_path.hpp>
#include <fsys/filesystem.h>
#include <queue>
#include <cstdlib>

#pragma optimize("",off)
static void print_usage()
{
	std::cout<<"Usage: render_raytracing -job <jobFile>"<<std::endl;
}

class RTJobManager
{
public:
	struct DeviceInfo
	{
		DeviceInfo(raytracing::Scene::DeviceType deviceType)
			: deviceType{deviceType}
		{}
		raytracing::Scene::DeviceType deviceType {};
		std::optional<util::ParallelJob<std::shared_ptr<uimg::ImageBuffer>>> job {};
		std::shared_ptr<raytracing::Scene> rtScene = nullptr;
		std::chrono::high_resolution_clock::time_point startTime {};
		util::Path outputPath {};
	};
	static std::shared_ptr<RTJobManager> Launch(int argc,char *argv[]);
	RTJobManager(const RTJobManager&)=delete;
	RTJobManager(RTJobManager&&)=delete;
	RTJobManager &operator=(const RTJobManager&)=delete;
	RTJobManager &operator=(RTJobManager&&)=delete;
	~RTJobManager();

	bool StartNextJob();
	bool IsComplete() const;
	uint32_t GetNumSucceeded() const {return m_numSucceeded;}
	uint32_t GetNumFailed() const {return m_numFailed;}
	uint32_t GetNumSkipped() const {return m_numSkipped;}
	void Update();
private:
	RTJobManager(std::unordered_map<std::string,std::string> &&launchParams,const std::string &inputFileName);
	void UpdateJob(DeviceInfo &devInfo);
	void PrintHeader(const raytracing::Scene::CreateInfo &createInfo,const raytracing::Scene::SceneInfo &sceneInfo);
	bool StartJob(const std::string &job,DeviceInfo &devInfo);
	void CollectJobs();

	std::chrono::high_resolution_clock::time_point m_startTime {};
	std::unordered_map<std::string,std::string> m_launchParams {};
	std::vector<DeviceInfo> m_devices {};
	std::queue<std::string> m_jobQueue {};
	uint32_t m_numJobs = 0;

	std::string m_inputFileName;
	uint32_t m_numSucceeded = 0;
	uint32_t m_numFailed = 0;
	uint32_t m_numSkipped = 0;
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
	auto kernelPath = util::Path::CreatePath(util::get_program_path());
	kernelPath.PopBack();
	kernelPath += "modules/cycles";
	auto itKernelPath = m_launchParams.find("-kernel_path");
	if(itKernelPath != m_launchParams.end())
		kernelPath = itKernelPath->second;
	raytracing::Scene::SetKernelPath(kernelPath.GetString());

	auto itVerbose = m_launchParams.find("-verbose");
	raytracing::Scene::SetVerbose(itVerbose != m_launchParams.end());

	auto itDeviceType = m_launchParams.find("-device_type");
	if(itDeviceType != m_launchParams.end())
	{
		auto &strDeviceType = itDeviceType->second;
		if(ustring::compare(strDeviceType,"cpu",false))
			m_devices.push_back(raytracing::Scene::DeviceType::CPU);
		else if(ustring::compare(strDeviceType,"gpu",false))
			m_devices.push_back(raytracing::Scene::DeviceType::GPU);
		else if(ustring::compare(strDeviceType,"combined",false))
		{
			m_devices.push_back(raytracing::Scene::DeviceType::GPU);
			m_devices.push_back(raytracing::Scene::DeviceType::CPU);
		}
	}
	if(m_devices.empty())
		m_devices.push_back(raytracing::Scene::DeviceType::GPU);
	for(auto &devInfo : m_devices)
	{
		std::cout<<"Using device: ";
		switch(devInfo.deviceType)
		{
		case raytracing::Scene::DeviceType::GPU:
			std::cout<<"GPU";
			break;
		case raytracing::Scene::DeviceType::CPU:
			std::cout<<"CPU";
			break;
		}
		std::cout<<std::endl;
	}

	util::minimize_window_to_tray();

	util::CommandManager::StartAsync();
	CollectJobs();

	std::cout<<"Executing "<<m_jobQueue.size()<<" jobs..."<<std::endl;
	m_startTime = std::chrono::high_resolution_clock::now();
}

RTJobManager::~RTJobManager()
{
	util::CommandManager::Join();
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

		for(auto &l : lines)
		{
			std::vector<std::string> ljobs {};
			FileManager::FindSystemFiles(l.c_str(),&ljobs,nullptr);

			// Sort by name
			std::sort(ljobs.begin(),ljobs.end());

			// Jobs are sorted by input string and THEN by name
			jobs.reserve(jobs.size() +ljobs.size());
			auto path = ufile::get_path_from_filename(l);
			for(auto &job : ljobs)
				jobs.push_back(path +job);
		}
	}

	for(auto &job : jobs)
		m_jobQueue.push(job);
	m_numJobs = m_jobQueue.size();

	if(m_jobQueue.empty())
	{
		std::cout<<"No jobs specified! ";
		print_usage();
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
		auto fImg = FileManager::OpenSystemFile(devInfo.outputPath.GetString().c_str(),"wb");
		if(fImg)
		{
			if(uimg::save_image(fImg,*imgBuf,uimg::ImageFormat::HDR) == false)
				std::cout<<"Unable to save image as '"<<devInfo.outputPath.GetString()<<"'!"<<std::endl;
			else
				++m_numSucceeded;
		}
	}
	devInfo.rtScene = nullptr;
	devInfo.job = {};
}

void RTJobManager::PrintHeader(const raytracing::Scene::CreateInfo &createInfo,const raytracing::Scene::SceneInfo &sceneInfo)
{
	if(createInfo.samples.has_value())
		std::cout<<"Samples: "<<*createInfo.samples<<std::endl;
	std::cout<<"HDR Output: "<<createInfo.hdrOutput<<std::endl;
	std::cout<<"Device: "<<((createInfo.deviceType == raytracing::Scene::DeviceType::CPU) ? "CPU" : "GPU")<<std::endl;
	std::cout<<"Denoise: "<<createInfo.denoise<<std::endl;

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

	raytracing::Scene::RenderMode renderMode;
	raytracing::Scene::CreateInfo createInfo;
	raytracing::Scene::SerializationData serializationData;
	raytracing::Scene::SceneInfo sceneInfo;
	auto success = raytracing::Scene::ReadHeaderInfo(ds,renderMode,createInfo,serializationData,&sceneInfo);
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
		fileName += ".hdr";
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
		PrintHeader(createInfo,sceneInfo);
		std::cout<<std::endl;

		auto itRenderMode = m_launchParams.find("-render_mode");
		if(itRenderMode != m_launchParams.end())
		{
			auto &strRenderMode = itRenderMode->second;
			if(ustring::compare(strRenderMode,"albedo",false))
				renderMode = raytracing::Scene::RenderMode::SceneAlbedo;
			else if(ustring::compare(strRenderMode,"depth",false))
				renderMode = raytracing::Scene::RenderMode::SceneDepth;
			else if(ustring::compare(strRenderMode,"normals",false))
				renderMode = raytracing::Scene::RenderMode::SceneNormals;
			else if(ustring::compare(strRenderMode,"image",false))
				renderMode = raytracing::Scene::RenderMode::RenderImage;
		}

		auto itSamples = m_launchParams.find("-samples");
		if(itSamples != m_launchParams.end())
			createInfo.samples = ustring::to_int(itSamples->second);

		auto itDenoise = m_launchParams.find("-denoise");
		if(itDenoise != m_launchParams.end())
			createInfo.denoise = util::to_boolean(itDenoise->second);

		createInfo.deviceType = devInfo.deviceType;

		auto itTonemapped = m_launchParams.find("-tonemapped");
		if(itTonemapped != m_launchParams.end())
			createInfo.hdrOutput = false;
	}
	auto rtScene = success ? raytracing::Scene::Create(ds,renderMode,createInfo) : nullptr;
	if(rtScene == nullptr)
	{
		std::cout<<"Unable to create scene from serialized data!"<<std::endl;
		++m_numFailed;
		return false;
	}

	uint32_t width,height;
	rtScene->GetCamera().GetResolution(width,height);
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

#if 0
	{
		auto r = 50.f;
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
		auto mesh = raytracing::Mesh::Create(*rtScene,"testBox",numVerts,numTris,raytracing::Mesh::Flags::None);
		for(auto &v : verts)
			mesh->AddVertex(v,{},{},{});
		for(auto i=0;i<verts.size();i+=3)
			mesh->AddTriangle(i,i +1,i +2,0);
		auto shader = raytracing::Shader::Create<raytracing::ShaderColorTest>(*rtScene,"testBoxShader");
		mesh->AddSubMeshShader(*shader);

		auto o = raytracing::Object::Create(*rtScene,*mesh);

		{
			auto &o = rtScene->GetObjects().front();
			auto &mesh = o->GetMesh();
			uint32_t idx = 0;
			for(auto &v : verts)
				mesh.AddVertex(v,{1.f,0.f,0.f},{0.f,0.f,1.f},uvs.at(idx++));
			for(auto i=0;i<verts.size();i+=3)
				mesh.AddTriangle(i,i +1,i +2,0);
		}
	}
#endif

	devInfo.startTime = std::chrono::high_resolution_clock::now();

	devInfo.job = rtScene->Finalize();
	devInfo.job->Start();
	return true;
}

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
