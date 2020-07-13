#include <util_raytracing/object.hpp>
#include <util_raytracing/mesh.hpp>
#include <util_raytracing.hpp>
#include <util_raytracing/scene.hpp>
#include <util_raytracing/shader.hpp>
#include <util_image.hpp>
#include <util_image_buffer.hpp>
#include <sharedutils/util_command_manager.hpp>
#include <sharedutils/util.h>
#include <sharedutils/datastream.h>
#include <sharedutils/util_parallel_job.hpp>
#include <sharedutils/util_path.hpp>
#include <fsys/filesystem.h>
#include <cstdlib>

#pragma optimize("",off)
static void print_usage()
{
	std::cout<<"Usage: render_raytracing -job <jobFile>"<<std::endl;
}

extern "C"
{
__declspec(dllexport) int render_raytracing(int argc,char *argv[])
{
	auto rootPath = util::Path{FileManager::GetRootPath()};
	rootPath.PopBack();
	FileManager::SetRootPath(rootPath.GetString());

	auto launchParams = util::get_launch_parameters(argc,argv);
	auto itJob = launchParams.find("-job");
	if(itJob == launchParams.end())
	{
		std::cout<<"No job specified! ";
		print_usage();
		std::this_thread::sleep_for(std::chrono::seconds{5});
		return EXIT_FAILURE;
	}
	auto &jobFileName = itJob->second;
	auto f = FileManager::OpenSystemFile(jobFileName.c_str(),"rb");
	if(f == nullptr)
	{
		std::cout<<"Job file '"<<jobFileName<<"' not found!"<<std::endl;
		std::this_thread::sleep_for(std::chrono::seconds{5});
		return EXIT_FAILURE;
	}
	auto sz = f->GetSize();
	DataStream ds {static_cast<uint32_t>(sz)};
	ds->SetOffset(0);
	f->Read(ds->GetData(),sz);

	auto rtScene = raytracing::Scene::Create(ds);
	if(rtScene == nullptr)
	{
		std::cout<<"Unable to create scene from serialized data!"<<std::endl;
		std::this_thread::sleep_for(std::chrono::seconds{5});
		return EXIT_FAILURE;
	}

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

	auto job = rtScene->Finalize();
	job.Start();
	for(;;)
	{
		if(job.IsComplete() == false)
		{
			auto progress = job.GetProgress();
			std::cout<<"Progress: "<<progress<<std::endl;
			std::this_thread::sleep_for(std::chrono::seconds{1});
		}
		else
		{
			if(job.IsCancelled())
				std::cout<<"Job has been cancelled!"<<std::endl;
			else if(job.IsSuccessful() == false)
				std::cout<<"Job has failed!"<<std::endl;
			else
			{
				std::cout<<"Job has been completed successfully!"<<std::endl;
				auto imgBuf = job.GetResult();
				imgBuf = imgBuf->ApplyToneMapping(uimg::ImageBuffer::ToneMapping::Aces);
				auto fImg = FileManager::OpenSystemFile("E:/projects/pragma/build_winx64/tools/render_raytracing/RelWithDebInfo/output.png","wb");
				if(fImg)
				{
					uimg::save_image(fImg,*imgBuf,uimg::ImageFormat::PNG);//HDR);
				}
			}
			break;
		}
	}
	return EXIT_SUCCESS;
}
};
#pragma optimize("",on)
